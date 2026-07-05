# SPDX-FileCopyrightText: Lada Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Lightweight engine-path helpers for the BasicVSR++ TensorRT sub-engines.
# Ported from jasna's engine_paths.py, keeping only the BasicVSR++ helpers
# (the unet/sd15/yolo/_frozen parts are dropped).
#
# This module is the SINGLE SOURCE OF TRUTH for sub-engine file paths: both the
# compiler (which writes the .engine files) and the loader (which reads them)
# must call get_basicvsrpp_sub_engine_paths(), so the written names and the
# looked-up names can never drift apart.
#
# Engines live in ``<weights dir>/<weights stem>_sub_engines/`` next to the
# .pth weights — i.e. inside model_weights/. A TRT engine built with
# hardware_compatible=False is bound to its GPU architecture, TensorRT version,
# precision, OS, and shape bounds. ALL of these are encoded in the filename so
# that changing any of them is seen as a cache miss (triggering a recompile)
# rather than loading an incompatible engine:
#   loop_body_backward_1.sm89.trt1012.fp16.win.engine
#   preprocess_b180.sm89.trt1012.fp16.win.engine
# Without the arch/trt segments, upgrading torch-tensorrt or copying
# model_weights/ across machines would leave a stale engine whose name still
# matches -> every startup would re-attempt a doomed deserialize.
from __future__ import annotations

import logging
import os

logger = logging.getLogger(__name__)

BASICVSRPP_DIRECTIONS = ("backward_1", "forward_1", "backward_2", "forward_2")


def engine_system_suffix() -> str:
    return "win" if os.name == "nt" else "linux"


def engine_precision_name(*, fp16: bool) -> str:
    return "fp16" if bool(fp16) else "fp32"


def engine_arch_suffix(device) -> str:
    """GPU compute-capability tag, e.g. ``sm89`` for a 4080 (cc 8.9).

    A hardware_compatible=False engine only deserializes on the architecture it
    was built for, so the arch must be part of the cache key. Falls back to
    ``smunknown`` if the capability can't be read (keeps a deterministic name;
    a wrong-arch engine then fails to load and gets cleaned up by load_sub_engines).
    """
    try:
        import torch

        idx = None
        if device is not None and getattr(device, "type", None) == "cuda":
            idx = device.index
        major, minor = torch.cuda.get_device_capability(idx)
        return f"sm{major}{minor}"
    except Exception as e:  # noqa: BLE001 - any failure -> deterministic fallback tag
        logger.debug("engine_arch_suffix fallback (%s)", e)
        return "smunknown"


def engine_trt_suffix() -> str:
    """TensorRT major.minor tag, e.g. ``trt1012`` for TensorRT 10.12.

    An engine is bound to the TensorRT version that serialized it; upgrading
    torch-tensorrt must invalidate the cache. Falls back to ``trtunknown`` if
    tensorrt can't be imported (the engine then fails to load and is cleaned up).
    """
    try:
        import tensorrt  # type: ignore[import-not-found]

        parts = str(tensorrt.__version__).split(".")
        major = parts[0] if len(parts) > 0 else "0"
        minor = parts[1] if len(parts) > 1 else "0"
        return f"trt{major}{minor}"
    except Exception as e:  # noqa: BLE001 - any failure -> deterministic fallback tag
        logger.debug("engine_trt_suffix fallback (%s)", e)
        return "trtunknown"


def _basicvsrpp_sub_engine_dir(model_weights_path: str) -> str:
    stem = os.path.splitext(os.path.basename(model_weights_path))[0]
    return os.path.join(os.path.dirname(model_weights_path), f"{stem}_sub_engines")


def get_basicvsrpp_sub_engine_paths(
    model_weights_path: str, fp16: bool, max_clip_size: int = 60, device=None,
) -> dict[str, str]:
    """Return the {engine_key: absolute_path} map for all 6 sub-engines.

    The filename encodes the full cache key {precision, arch, trt version, OS,
    clip-size upper bound}. Pass the same ``device`` here and at compile time so
    writer and reader agree. loop_body engines are static batch=1 but still
    carry arch/trt/precision; preprocess/upsample additionally carry the clip
    upper bound (their dynamic batch max).
    """
    engine_dir = _basicvsrpp_sub_engine_dir(model_weights_path)
    prec = engine_precision_name(fp16=fp16)
    arch = engine_arch_suffix(device)
    trt = engine_trt_suffix()
    suf = engine_system_suffix()
    tag = f"{arch}.{trt}.{prec}.{suf}"
    paths: dict[str, str] = {}
    for d in BASICVSRPP_DIRECTIONS:
        paths[f"loop_body_{d}"] = os.path.join(engine_dir, f"loop_body_{d}.{tag}.engine")
    paths["preprocess"] = os.path.join(engine_dir, f"preprocess_b{max_clip_size}.{tag}.engine")
    paths["upsample"] = os.path.join(engine_dir, f"upsample_dyn_b{max_clip_size}.{tag}.engine")
    return paths


def all_basicvsrpp_sub_engines_exist(
    model_weights_path: str, fp16: bool, max_clip_size: int = 60, device=None,
) -> bool:
    return all(
        os.path.isfile(p)
        for p in get_basicvsrpp_sub_engine_paths(model_weights_path, fp16, max_clip_size, device).values()
    )
