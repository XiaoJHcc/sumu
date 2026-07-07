# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# sumu AI-core package. Ported (照搬) from lada-realtime's AI compute core:
#   - item 1: YOLO detection (models/yolo + utils/torch_letterbox)
#   - item 3: BasicVSR++ restore wrapper (restorationpipeline + vendored models/basicvsrpp)
#   - item 5: model loading/cache + TRT sub-engine assembly (restorationpipeline + trt)
#
# This top-level module replaces the pieces of lada's `lada/__init__.py` that the
# ported code depends on (`from lada import LOG_LEVEL, ModelFiles`) plus lada's
# gettext install (the ported code uses the `_()` translation builtin). Everything
# else in lada/__init__.py (version/git, flatpak, OS-language detection) is GUI/
# packaging plumbing that the AI core does not need and is intentionally dropped.

import os
import sys
import gettext
from dataclasses import dataclass
from functools import cache

# ── gettext `_` builtin ────────────────────────────────────────────────────────
# lada installs a gettext domain at import time so that `_("…")` is available as a
# builtin across the codebase (ModelFiles descriptions, load-progress messages).
# We install a domain with no compiled catalogs, so `_` is the identity function
# (NullTranslations fallback). This keeps the ported code verbatim.
gettext.install("sumu")

# ── env knobs ultralytics expects (mirrors lada/__init__.py) ────────────────────
os.environ.setdefault("ALBUMENTATIONS_OFFLINE", "1")
os.environ.setdefault("ALBUMENTATIONS_NO_TELEMETRY", "1")
os.environ.setdefault("YOLO_VERBOSE", "false")

LOG_LEVEL = os.environ.get("LOG_LEVEL", "WARNING")

# ── model weights directory ─────────────────────────────────────────────────────
# Per the sumu port plan, weights are referenced from lada by path and NOT copied
# into sumu (they are large + gitignored). Resolution order:
#   1) SUMU_MODEL_WEIGHTS_DIR  2) LADA_MODEL_WEIGHTS_DIR
#   3) (frozen only) <dir of the built exe>/model_weights
#   4) lada-realtime/model_weights next to this checkout  5) "model_weights" (CWD-relative)
# Override via env for other layouts.
def _default_model_weights_dir() -> str:
    for var in ("SUMU_MODEL_WEIGHTS_DIR", "LADA_MODEL_WEIGHTS_DIR"):
        if var in os.environ and os.environ[var]:
            return os.environ[var]
    # Frozen (PyInstaller) builds: __file__ lives under the _MEIPASS bundle temp dir, so
    # the sibling-repo computation below is meaningless. Packaging ships weights in a
    # `model_weights/` folder next to the built executable instead (writable, since
    # TensorRT engine caches get written under <weights_dir>/<stem>_sub_engines/). Return
    # this path unconditionally when frozen -- it's the intended canonical location
    # regardless of whether weights are placed there yet; callers already check per-file
    # existence (ModelFiles._get_well_known_* guard with os.path.exists).
    if getattr(sys, "frozen", False):
        return os.path.join(os.path.dirname(sys.executable), "model_weights")
    # <repo parent>/lada-realtime/model_weights (sumu and lada-realtime are siblings)
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    sibling = os.path.join(os.path.dirname(here), "lada-realtime", "model_weights")
    return sibling if os.path.isdir(sibling) else "model_weights"

MODEL_WEIGHTS_DIR = _default_model_weights_dir()


# ── ModelFiles (ported verbatim from lada/__init__.py) ──────────────────────────
# Used by load_models (detect_face_mosaics branch) and the model cache provider
# (name -> path resolution). Filenames match lada's HuggingFace weights.
@dataclass(frozen=True)
class ModelFile:
    name: str
    description: str | None
    path: str


class ModelFiles:
    _WELL_KNOWN_RESTORATION_MODELS = [
        ModelFile('basicvsrpp-v1.0', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_restoration_model_generic.pth')),
        ModelFile('basicvsrpp-v1.1', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_restoration_model_generic_v1.1.pth')),
        ModelFile('basicvsrpp-v1.2', _("Latest Lada restoration model. Recommended"), os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_restoration_model_generic_v1.2.pth')),
    ]
    _WELL_KNOWN_DETECTION_MODELS = [
        ModelFile('v2', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v2.pt')),
        ModelFile('v3', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v3.pt')),
        ModelFile('v3.1-fast', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v3.1_fast.pt')),
        ModelFile('v3.1-accurate', None, os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v3.1_accurate.pt')),
        ModelFile('v4-fast', _("Fast and efficient. Recommended"), os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v4_fast.pt')),
        ModelFile('v4-accurate', _("Can be slightly more accurate than v4-fast but slower"), os.path.join(MODEL_WEIGHTS_DIR, 'lada_mosaic_detection_model_v4_accurate.pt')),
    ]

    @staticmethod
    def _get_custom_detection_models() -> list[ModelFile]:
        models = []
        if not os.path.exists(MODEL_WEIGHTS_DIR):
            return models
        well_known_filenames = [os.path.basename(model.path) for model in ModelFiles._WELL_KNOWN_DETECTION_MODELS]
        for filename in os.listdir(MODEL_WEIGHTS_DIR):
            if filename.endswith('.pt') and filename.startswith('lada_mosaic_detection_model_') and filename not in well_known_filenames:
                model_name = os.path.splitext(filename)[0].split("lada_mosaic_detection_model_")[1]
                if len(model_name) == 0:
                    continue
                model_path = os.path.join(MODEL_WEIGHTS_DIR, filename)
                models.append(ModelFile(model_name, None, model_path))
        return models

    @staticmethod
    def _get_custom_restoration_models() -> list[ModelFile]:
        models = []
        if not os.path.exists(MODEL_WEIGHTS_DIR):
            return models
        well_known_filenames = [os.path.basename(model.path) for model in ModelFiles._WELL_KNOWN_RESTORATION_MODELS]
        for filename in os.listdir(MODEL_WEIGHTS_DIR):
            if filename.endswith('.pth') and filename.startswith('lada_mosaic_restoration_model_') and filename not in well_known_filenames:
                model_name = os.path.splitext(filename)[0].split("lada_mosaic_restoration_model_")[1]
                if len(model_name) == 0:
                    continue
                if not model_name.startswith("basicvsrpp"):
                    model_name = f"basicvsrpp-{model_name}"
                model_path = os.path.join(MODEL_WEIGHTS_DIR, filename)
                models.append(ModelFile(model_name, None, model_path))
        return models

    @staticmethod
    def _get_well_known_detection_models():
        return [m for m in ModelFiles._WELL_KNOWN_DETECTION_MODELS if os.path.exists(m.path)]

    @staticmethod
    def _get_well_known_restoration_models():
        return [m for m in ModelFiles._WELL_KNOWN_RESTORATION_MODELS if os.path.exists(m.path)]

    @staticmethod
    @cache
    def get_detection_models() -> list[ModelFile]:
        return ModelFiles._get_well_known_detection_models() + ModelFiles._get_custom_detection_models()

    @staticmethod
    @cache
    def get_restoration_models() -> list[ModelFile]:
        return ModelFiles._get_well_known_restoration_models() + ModelFiles._get_custom_restoration_models()

    @staticmethod
    def get_restoration_model_by_name(model_name: str) -> ModelFile | None:
        for model in ModelFiles.get_restoration_models():
            if model.name == model_name:
                return model
        return None

    @staticmethod
    def get_detection_model_by_name(model_name: str) -> ModelFile | None:
        for model in ModelFiles.get_detection_models():
            if model.name == model_name:
                return model
        return None

    @staticmethod
    def get_detection_model_by_path(model_path: str) -> ModelFile | None:
        for model in ModelFiles.get_detection_models():
            if model.path == model_path:
                return model
        return None
