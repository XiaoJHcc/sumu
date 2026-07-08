# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Ported (照搬) from lada-realtime lada/restorationpipeline/__init__.py.
# Only load_models + _maybe_build_trt_split_forward are carried over (the model
# loading/cache-producing path, port item 5). Imports repointed lada -> sumu.ai.
from __future__ import annotations

import logging
import os

from sumu.ai import LOG_LEVEL, ModelFiles
from sumu.ai.restorationpipeline.progress import report_load_progress

logger = logging.getLogger(__name__)
logging.basicConfig(level=LOG_LEVEL)

# ── TensorRT acceleration for BasicVSR++ (optional) ────────────────────────────
# When enabled (and device is cuda + fp16), the BasicVSR++ restorer runs as 6
# precompiled TensorRT sub-engines instead of the PyTorch model — ~3x restorer
# throughput. Engines are bound to the GPU architecture / precision / clip-size
# upper bound and are compiled on first use (a one-time, multi-minute, blocking
# step), then cached under model_weights/<stem>_sub_engines/. Non-Nvidia / fp32
# users never trigger this and keep the PyTorch path unchanged.
#
# Default on for the realtime fork's purpose; set LADA_BASICVSRPP_TRT=0 to force
# the PyTorch path (e.g. to skip the first-run compile, or when debugging).
BASICVSRPP_TRT_ENABLED = os.environ.get("LADA_BASICVSRPP_TRT", "1") not in ("0", "false", "False", "")
# Single fixed engine upper bound. Must be >= the largest clip ever fed. Changing
# this triggers a recompile.
BASICVSRPP_TRT_MAX_CLIP_SIZE = int(os.environ.get("LADA_BASICVSRPP_TRT_MAX_CLIP", "180"))


def load_models(
    device: torch.device,
    mosaic_restoration_model_name: str,
    mosaic_restoration_model_path: str,
    mosaic_restoration_config_path: str | None,
    mosaic_detection_model_path: str,
    fp16: bool,
    detect_face_mosaics: bool,
    allow_trt_compile: bool = True):
    if mosaic_restoration_model_name.startswith("basicvsrpp"):
        from sumu.ai.models.basicvsrpp.inference import load_model
        from sumu.ai.restorationpipeline.basicvsrpp_mosaic_restorer import BasicvsrppMosaicRestorer
        report_load_progress(_("Loading restoration model…"))
        _model = load_model(mosaic_restoration_config_path, mosaic_restoration_model_path, device, fp16)
        split_forward = _maybe_build_trt_split_forward(_model, mosaic_restoration_model_path, device, fp16,
                                                       allow_compile=allow_trt_compile)
        mosaic_restoration_model = BasicvsrppMosaicRestorer(_model, device, fp16, split_forward=split_forward)
        pad_mode = 'zero'
    else:
        raise NotImplementedError()
    # setting classes=[0] will consider only detections of class id = 0 (nsfw mosaics) therefore filtering out sfw mosaics (heads, faces)
    if detect_face_mosaics:
        classes = [0]
        detection_model_name = ModelFiles.get_detection_model_by_path(mosaic_detection_model_path)
        if detection_model_name and detection_model_name == "v2":
            logger.info("Mosaic detection model v2 does not support detecting face mosaics. Use detection models v3 or newer. Ignoring...")
    else:
        classes = None
    report_load_progress(_("Loading detection model…"))
    from sumu.ai.models.yolo.yolo11_segmentation_model import Yolo11SegmentationModel
    mosaic_detection_model = Yolo11SegmentationModel(mosaic_detection_model_path, device, classes=classes, conf=0.15, fp16=fp16)

    # Pay the one-time CUDA/cuDNN init cost now (at model-load time, while the user is already
    # waiting for the model to load) instead of on the first real clip. Without this the first
    # BasicVSR++ forward is several times slower than steady state, which the realtime path
    # reads as the AI failing to keep up -> it falls back to the original (and, with reposition
    # on, repeatedly restarts and re-pays the cost). Best-effort: a warmup failure must not
    # block model loading.
    if hasattr(mosaic_restoration_model, "warmup"):
        try:
            report_load_progress(_("Warming up models…"))
            mosaic_restoration_model.warmup()
        except Exception as e:
            logger.warning(f"restoration model warmup skipped: {e}")
    # The YOLO detector already warms up batch=1 in its __init__; the realtime detector feeds
    # batch_size=4, whose first inference can still trigger a cuDNN autotune. Warm that shape.
    try:
        import torch as _torch
        dummy_batch = [_torch.randint(0, 256, (mosaic_detection_model.imgsz[0], mosaic_detection_model.imgsz[1], 3), dtype=_torch.uint8) for _ in range(4)]
        preprocessed = mosaic_detection_model.preprocess(dummy_batch)
        mosaic_detection_model.inference_and_postprocess(preprocessed, dummy_batch)
    except Exception as e:
        logger.warning(f"detection model batch warmup skipped: {e}")

    return mosaic_detection_model, mosaic_restoration_model, pad_mode


def compile_and_activate_trt(res_model, mosaic_restoration_model_path: str, device, fp16: bool):
    """On-demand TensorRT compile driven by the startup-UX prompt (not the passive load path).

    Blocks (minutes) compiling the 6 BasicVSR++ sub-engines for THIS machine's GPU arch /
    TRT version / precision, then builds a split forward bound to the already-loaded eager
    model's generator and returns it. The caller (app.py) attaches it via
    res_model.activate_trt(...) on the main thread.

    Uses the SAME max_clip_size (BASICVSRPP_TRT_MAX_CLIP_SIZE) the loader looks up, so the
    engines this writes are exactly the ones a subsequent load-only startup will find. Per-engine
    progress ("Compiling sub-engine i/6…") flows through report_load_progress() to whatever
    callback the caller registered. Returns the split forward, or None if compilation was skipped
    (e.g. non-cuda / fp32 / VRAM too low) or the freshly-written engines failed to load.
    """
    from sumu.ai.restorationpipeline.basicvsrpp_trt_compilation import basicvsrpp_startup_policy
    from sumu.ai.restorationpipeline.basicvsrpp_sub_engines import create_split_forward

    ok = basicvsrpp_startup_policy(
        restoration_model_path=mosaic_restoration_model_path,
        device=device, fp16=fp16, compile_basicvsrpp=True,
        max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE, optimization_level=5,
    )
    if not ok:
        logger.warning("On-demand TRT compile did not produce usable engines; staying on PyTorch path.")
        return None
    return create_split_forward(
        res_model.model, mosaic_restoration_model_path, device, fp16,
        max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE,
    )


def _maybe_build_trt_split_forward(model, mosaic_restoration_model_path: str, device: torch.device, fp16: bool,
                                   allow_compile: bool = True):
    """Compile (if needed) and load the BasicVSR++ TensorRT split forward.

    Returns a BasicVSRPlusPlusNetSplit to run instead of the PyTorch model, or
    None to keep the PyTorch path. None is returned whenever TRT is disabled,
    the device isn't cuda, fp16 is off, or compilation/loading fails — in every
    such case the caller falls back to the unchanged PyTorch model. A failure
    here must never block model loading.

    allow_compile=False switches to load-only: existing engines are used if
    present, but engines are never compiled.
    """
    if not BASICVSRPP_TRT_ENABLED:
        return None
    if device.type != "cuda" or not fp16:
        if device.type != "cuda":
            logger.info("BasicVSR++ TRT disabled: device is %s, not cuda.", device.type)
        else:
            logger.info("BasicVSR++ TRT disabled: requires fp16.")
        return None

    try:
        from sumu.ai.restorationpipeline.basicvsrpp_trt_compilation import basicvsrpp_startup_policy
        from sumu.ai.restorationpipeline.basicvsrpp_sub_engines import create_split_forward, all_sub_engines_exist

        if allow_compile:
            report_load_progress(_("Building TensorRT acceleration engines (first run only, may take several minutes)…"))
            use_trt = basicvsrpp_startup_policy(
                restoration_model_path=mosaic_restoration_model_path,
                device=device, fp16=fp16, compile_basicvsrpp=True,
                max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE, optimization_level=5,
            )
        else:
            # Load-only: use engines if they already exist, but never compile.
            use_trt = all_sub_engines_exist(
                mosaic_restoration_model_path, fp16, BASICVSRPP_TRT_MAX_CLIP_SIZE, device,
            )
            if not use_trt:
                logger.info("BasicVSR++ TRT engines not present and compile deferred; using PyTorch path.")
        if not use_trt:
            logger.info("BasicVSR++ TRT engines unavailable; using PyTorch path.")
            return None

        split_forward = create_split_forward(
            model, mosaic_restoration_model_path, device, fp16,
            max_clip_size=BASICVSRPP_TRT_MAX_CLIP_SIZE,
        )
        if split_forward is None:
            logger.warning("BasicVSR++ TRT engines reported present but failed to load; using PyTorch path.")
            return None
        logger.info("BasicVSR++ TensorRT split forward active (max_clip_size=%d).", BASICVSRPP_TRT_MAX_CLIP_SIZE)
        return split_forward
    except Exception as e:
        logger.warning("BasicVSR++ TRT setup failed (%s); falling back to PyTorch path.", e)
        return None
