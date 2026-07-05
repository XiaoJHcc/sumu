# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Ported (照搬) from lada-realtime lada/gui/frame_restorer_provider.py, keeping
# ONLY the model loading/cache-producing path (port item 5): the cache-key/miss
# logic of _ensure_loaded -> load_models plus _clear_cache. The parts of the
# original that build a FrameRestorer / PassthroughFrameRestorer (get(), the
# GStreamer/GTK production wiring, VideoReader, decoder_slot) are the "rewrite"
# boundary and are intentionally NOT ported — sumu's scheduler/ready-map owns
# frame production. This class hands back two ready + warmed model objects
# (Yolo11SegmentationModel, BasicvsrppMosaicRestorer) exactly as load_models
# produces them; it does not know or care whether TRT is in use.
from __future__ import annotations

import gc
import logging
import threading

from sumu.ai import LOG_LEVEL, ModelFiles

logger = logging.getLogger(__name__)
logging.basicConfig(level=LOG_LEVEL)


class ModelCache:
    """Lazily loads and caches the (detection, restoration) model pair.

    Thread-safe (single lock). A change to any cache-key field
    (restoration/detection model name, fp16, detect_face_mosaics) drops the cache
    and reloads. Mirrors lada's FrameRestorerProvider._ensure_loaded / _clear_cache.
    """

    def __init__(self):
        self.models_cache: None | dict = None
        self._lock = threading.Lock()

    def get_models(self, *, device: str, mosaic_restoration_model_name: str,
                   mosaic_detection_model_name: str, fp16_enabled: bool,
                   detect_face_mosaics: bool, allow_trt_compile: bool = True) -> dict:
        """Return the cached models dict, loading it if necessary. Keys:
        mosaic_detection_model, mosaic_restoration_model,
        mosaic_restoration_model_preferred_pad_mode (+ the cache-key fields)."""
        with self._lock:
            self._ensure_loaded(device, mosaic_restoration_model_name, mosaic_detection_model_name,
                                fp16_enabled, detect_face_mosaics, allow_trt_compile=allow_trt_compile)
            return self.models_cache

    def warmup(self, *, device: str, mosaic_restoration_model_name: str, mosaic_detection_model_name: str,
               fp16_enabled: bool, detect_face_mosaics: bool) -> None:
        """Eagerly load + cache models without compiling TRT engines (load-only).

        allow_trt_compile=False loads existing engines but never compiles new ones,
        so eager preload honours a deferred TRT build (lada's "Later" contract)."""
        with self._lock:
            self._ensure_loaded(device, mosaic_restoration_model_name, mosaic_detection_model_name,
                                fp16_enabled, detect_face_mosaics, allow_trt_compile=False)

    def _ensure_loaded(self, device: str, mosaic_restoration_model_name: str, mosaic_detection_model_name: str,
                       fp16_enabled: bool, detect_face_mosaics: bool, allow_trt_compile: bool = True) -> None:
        """Load models for the given settings into self.models_cache if not already cached.
        Caller must hold self._lock."""
        is_empty_cache = self.models_cache is None
        cache_miss = False
        if is_empty_cache:
            cache_miss = True
        else:
            if self.models_cache["mosaic_restoration_model_name"] != mosaic_restoration_model_name:
                cache_miss = True
                logger.info(f"model {mosaic_restoration_model_name} not found in cache. Reloading models...")
            if self.models_cache["mosaic_detection_model_name"] != mosaic_detection_model_name:
                cache_miss = True
                logger.info(f"model {mosaic_detection_model_name} not found in cache. Reloading models...")
            if self.models_cache.get("fp16_enabled") != fp16_enabled:
                cache_miss = True
                logger.info(f"FP16 setting changed from {self.models_cache.get('fp16_enabled')} to {fp16_enabled}. Reloading models...")
            if self.models_cache.get("detect_face_mosaics") != detect_face_mosaics:
                cache_miss = True
                logger.info(f"Detect Face Mosaics setting changed from {self.models_cache.get('detect_face_mosaics')} to {detect_face_mosaics}. Reloading models...")

        if not cache_miss:
            return

        self._clear_cache()

        import torch
        from sumu.ai.restorationpipeline import load_models

        mosaic_restoration_model_path = ModelFiles.get_restoration_model_by_name(mosaic_restoration_model_name).path
        mosaic_detection_path = ModelFiles.get_detection_model_by_name(mosaic_detection_model_name).path
        mosaic_detection_model, mosaic_restoration_model, mosaic_restoration_model_preferred_pad_mode = load_models(
            torch.device(device), mosaic_restoration_model_name, mosaic_restoration_model_path, None,
            mosaic_detection_path, fp16=fp16_enabled, detect_face_mosaics=detect_face_mosaics,
            allow_trt_compile=allow_trt_compile,
        )

        self.models_cache = dict(mosaic_restoration_model_name=mosaic_restoration_model_name,
                                 mosaic_detection_model_name=mosaic_detection_model_name,
                                 fp16_enabled=fp16_enabled,
                                 mosaic_detection_model=mosaic_detection_model,
                                 mosaic_restoration_model=mosaic_restoration_model,
                                 mosaic_restoration_model_preferred_pad_mode=mosaic_restoration_model_preferred_pad_mode,
                                 detect_face_mosaics=detect_face_mosaics)

    def _clear_cache(self):
        if self.models_cache is None:
            return
        if "mosaic_detection_model" in self.models_cache: del self.models_cache["mosaic_detection_model"]
        if "mosaic_restoration_model" in self.models_cache: del self.models_cache["mosaic_restoration_model"]
        gc.collect()
        try:
            import torch
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            elif hasattr(torch, 'xpu') and torch.xpu.is_available():
                torch.xpu.empty_cache()
            elif getattr(torch, 'mps', None) is not None:
                torch.mps.empty_cache()
        except Exception:
            pass
        self.models_cache = None
