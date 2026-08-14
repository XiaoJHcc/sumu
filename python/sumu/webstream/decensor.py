# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# DecensorProcessor: the sequential AI decensor producer for the transcode pipeline. Mirrors
# python/sumu/scheduler.py's per-frame AI logic (_process_frame / _restore_and_push /
# _flush_pending_to_native) EXACTLY, but decoupled from the present head: frames are ingested in
# decode order (0,1,2,...) and emitted strictly in order, as fast as the GPU allows.
#
# The one semantic difference from the player's present path: the encoder consumes every frame
# exactly once, so (a) frames WITHOUT any mosaic region are emitted as the original
# passthrough BGR frame, and (b) a frame is only emitted once the ingest frontier has advanced
# a full clip_length past it -- this guarantees every clip that contributes a (possibly
# multi-region) restoration to that frame has completed, since clips are <= clip_length long.
from __future__ import annotations

import time
from collections import OrderedDict

import cv2
import torch

from sumu.ai.restorationpipeline.blend import blend_regions_into_frame, restore_clip
from sumu.ai.restorationpipeline.scene_clip import (
    append_or_create_scenes,
    materialize_completed_clips,
)
from sumu.ai.utils import image_utils
from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor
from sumu.ai.utils.video_utils import _nv12_to_bgr_hwc_gpu


class DecensorProcessor:
    """Sequential NV12-CUDA-frame -> final-BGR-frame decensor, reusing the AI pure functions.

    `config` is a scheduler.SchedulerConfig (its clip_length / clip_size / max_regions_per_frame /
    bt709 / full_range / model_name fields drive the pipeline). Callers feed frames via
    `ingest(...)` and drain finished frames via `emit()` after each ingest; call `flush_eof()`
    once the decoder reports EOF, then `emit()` again to drain the tail.
    """

    def __init__(self, det_model, res_model, pad_mode: str, video_meta_data, config):
        self.det_model = det_model
        self.res_model = res_model
        self.pad_mode = pad_mode
        self.video_meta_data = video_meta_data
        self.config = config

        self.scenes = []
        self.clip_counter = 0
        self.frame_cache: "OrderedDict[int, torch.Tensor]" = OrderedDict()
        self.pending_regions: "OrderedDict[int, list]" = OrderedDict()

        # Emit lag: a frame is only handed out once the ingest frontier is a full clip_length
        # past it (see module docstring). Set to 0 by flush_eof() to drain the tail.
        self.emit_lag = max(1, int(config.clip_length))
        self.frames_ingested = 0
        self.frames_emitted = 0

        # Diagnostics (best-effort, no locking -- single producer thread).
        self.clips_restored = 0
        self.restore_frames = 0
        self.restore_seconds = 0.0

    # ---- ingest ------------------------------------------------------------------------

    def ingest(self, n: int, dev_ptr: int, width: int, height: int, pitch_bytes: int) -> None:
        cfg = self.config
        nv12 = wrap_nv12_cuda_buffer_as_tensor(dev_ptr, width, height, pitch_bytes)
        bgr = _nv12_to_bgr_hwc_gpu(nv12, height, width, bt709=cfg.bt709,
                                   full_range=cfg.full_range)
        frame = bgr.clone()  # survive the native single-buffer reuse
        self.frame_cache[n] = frame

        pre = self.det_model.preprocess([frame])
        results = self.det_model.inference_and_postprocess(pre, [frame])[0]

        self.scenes = append_or_create_scenes(
            results, self.scenes, n, self.video_meta_data, cfg.max_regions_per_frame
        )
        self.scenes, clips, self.clip_counter = materialize_completed_clips(
            self.scenes, n, False, cfg.clip_length, cfg.clip_size, self.pad_mode,
            self.clip_counter,
        )
        for clip in clips:
            self._restore_clip(clip)

        self.frames_ingested = n + 1

    def flush_eof(self) -> None:
        """Materialize trailing clips at end-of-stream (mirrors the scheduler's eof=True branch)
        and open the emit gate so `emit()` drains every remaining frame."""
        if self.frames_ingested > 0:
            n = self.frames_ingested - 1
            self.scenes, clips, self.clip_counter = materialize_completed_clips(
                self.scenes, n, True, self.config.clip_length, self.config.clip_size,
                self.pad_mode, self.clip_counter,
            )
            for clip in clips:
                self._restore_clip(clip)
        self.emit_lag = 0

    # ---- emit --------------------------------------------------------------------------

    def emit(self):
        """Yield (frame_num, final_bgr_cuda_uint8) in strict order for frames whose full
        restoration (if any) is guaranteed complete. Frames with no mosaic region are yielded
        as the original passthrough BGR frame. Generator: call after each ingest() and after
        flush_eof()."""
        limit = self.frames_ingested - self.emit_lag
        while self.frames_emitted < limit:
            k = self.frames_emitted
            regions = self.pending_regions.pop(k, None)
            orig = self.frame_cache.pop(k, None)
            if orig is None:
                # frame_cache shallower than emit_lag -- should not happen; bail rather than
                # emit out of order.
                break
            if regions:
                final_bgr = blend_regions_into_frame(orig, regions, self.res_model)
            else:
                final_bgr = orig  # passthrough (no mosaic on this frame)
            yield k, final_bgr
            self.frames_emitted = k + 1

    # ---- internals ----------------------------------------------------------------------

    def _restore_clip(self, clip) -> None:
        frame_start, frame_end = clip.frame_start, clip.frame_end
        n_frames = frame_end - frame_start + 1
        if torch.cuda.is_available():
            torch.cuda.synchronize()
        t0 = time.perf_counter()
        restore_clip(self.res_model, self.config.model_name, clip)
        if torch.cuda.is_available():
            torch.cuda.synchronize()
        dt = time.perf_counter() - t0
        if dt > 0.0 and n_frames > 0:
            self.restore_frames += n_frames
            self.restore_seconds += dt
        self.clips_restored += 1

        for fnum in range(frame_start, frame_end + 1):
            if fnum not in self.frame_cache:
                clip.pop()
                continue
            clip_img, clip_mask, orig_clip_box, orig_crop_shape, pad_after_resize = clip.pop()
            clip_img = image_utils.unpad_image(clip_img, pad_after_resize)
            clip_mask = image_utils.unpad_image(clip_mask, pad_after_resize)
            clip_img = image_utils.resize(clip_img, orig_crop_shape[:2])
            clip_mask = image_utils.resize(
                clip_mask, orig_crop_shape[:2], interpolation=cv2.INTER_NEAREST
            )
            region = (clip_img.contiguous(), clip_mask.contiguous(), orig_clip_box)
            bucket = self.pending_regions.get(fnum)
            if bucket is None:
                self.pending_regions[fnum] = [region]
            else:
                bucket.append(region)

    @property
    def restore_fps(self) -> float | None:
        return (self.restore_frames / self.restore_seconds) if self.restore_seconds > 0 else None
