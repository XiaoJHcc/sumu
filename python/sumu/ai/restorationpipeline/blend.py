# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Ported (照搬) from lada-realtime lada/restorationpipeline/frame_restorer.py
# (porting_manifest.md item 4). Imports repointed lada -> sumu.ai.
#
# `restore_clip_frames`, `restore_clip` and `blend_back_frame` below are the *bodies* of
# FrameRestorer._restore_clip_frames (frame_restorer.py:240-247), FrameRestorer._restore_clip
# (frame_restorer.py:290-303) and FrameRestorer._restore_frame (frame_restorer.py:249-288,
# including the `_blend_gpu`/`_blend_cpu` closures and the device-based dispatch between them)
# extracted into standalone pure functions - the `FrameRestorer` class itself (worker threads,
# `PipelineQueue`, start/stop teardown handshake) is NOT ported; see porting_manifest.md
# "二、重写/剥离".
#
# Deliberate differences from the original method bodies:
#   - `self.xxx` become explicit function parameters (`restoration_model` stands in for both
#     `self.mosaic_restoration_model` - used for its `.dtype`/`.device` - and `self.device`,
#     which were the *same* object in every real call site: FrameRestorer always constructs
#     its `mosaic_restoration_model` with `device=self.device`. Passing one object instead of
#     two is a naming simplification, not a behavior change).
#   - `_restore_clip` drops the `mosaic_detection` debug-visualization branch
#     (`visualization_utils.draw_mosaic_detections`) - out of scope for this port (not listed in
#     porting_manifest.md item 4, and `visualization_utils` was never vendored into sumu).
#   - `blend_back_frame`'s upstream body scans `restored_clips` for `c.frame_start == frame_num`
#     (a linear list scan) to find which buffered clip(s) cover this frame. That scan is dropped:
#     the caller now passes in `matched_clips`, the already-resolved list of clips that start at
#     `frame_num`. Finding them is the future ready-map scheduler's O(1) job, not this function's
#     - see porting_manifest.md item 4 risk note. The blend math/tensor ops themselves
#     (`_blend_gpu` / `_blend_cpu`, the CPU/GPU dispatch) are otherwise verbatim.
from __future__ import annotations

import cv2
import numpy as np
import torch

from sumu.ai.utils import image_utils, mask_utils, ImageTensor
from sumu.ai.restorationpipeline.scene_clip import Clip


def restore_clip_frames(restoration_model, mosaic_restoration_model_name: str, images: list[ImageTensor]) -> list[ImageTensor]:
    """Extracted from FrameRestorer._restore_clip_frames (frame_restorer.py:240-247).
    Dispatches to the restoration model's `.restore(images)` - currently only the
    "basicvsrpp*" family is ported (BasicvsrppMosaicRestorer, porting_manifest.md item 3)."""
    if mosaic_restoration_model_name.startswith("basicvsrpp"):
        from sumu.ai.restorationpipeline.basicvsrpp_mosaic_restorer import BasicvsrppMosaicRestorer
        assert isinstance(restoration_model, BasicvsrppMosaicRestorer)
        restored_clip_images = restoration_model.restore(images)
    else:
        raise NotImplementedError()
    return restored_clip_images


def restore_clip(restoration_model, mosaic_restoration_model_name: str, clip: Clip) -> None:
    """Extracted from FrameRestorer._restore_clip (frame_restorer.py:290-303), minus the
    `mosaic_detection` debug-visualization branch (see module docstring). Restores every frame
    of `clip` in place (temporal restoration runs over the whole clip at once, per
    BasicvsrppMosaicRestorer.restore's ImageTensor(H,W,C) list -> ImageTensor(H,W,C) list
    contract) and writes the results back into `clip.frames`."""
    restored_clip_images = restore_clip_frames(restoration_model, mosaic_restoration_model_name, clip.frames)
    assert len(restored_clip_images) == len(clip.frames)

    for i in range(len(restored_clip_images)):
        assert clip.frames[i].shape == restored_clip_images[i].shape
        clip.frames[i] = restored_clip_images[i]


def blend_back_frame(frame: ImageTensor, frame_num: int, matched_clips: list[Clip], restoration_model) -> ImageTensor:
    """Extracted from FrameRestorer._restore_frame (frame_restorer.py:249-288). Blends restored
    content from `matched_clips` back into `frame` (mutated in place via ROI tensor/ndarray
    views, and also returned for convenience), matching lada's blend math exactly.

    `matched_clips` must already be exactly the Clip(s) covering `frame_num` (upstream matched
    these itself via `[c for c in restored_clips if c.frame_start == frame_num]` - a linear scan
    over every buffered clip; sumu's ready-map scheduler resolves this in O(1) and passes the
    result in directly, see module docstring). Each matched clip has `.pop()` called on it,
    which is a stateful, one-shot consume (see Clip.pop docstring).

    `restoration_model` supplies `.dtype` and `.device`, matching upstream's
    `self.mosaic_restoration_model.dtype` / `self.device` (always the same device in every real
    FrameRestorer call site).
    """
    is_cpu_input = frame.device.type == 'cpu'
    target_dtype = torch.float32 if is_cpu_input else restoration_model.dtype

    def _blend_gpu(blend_mask: torch.Tensor, clip_img: torch.Tensor, orig_clip_box: tuple[int, int, int, int]):
        t, l, b, r = orig_clip_box
        frame_roi = frame[t:b + 1, l:r + 1, :]
        roi_f = frame_roi.to(dtype=restoration_model.dtype)
        temp = clip_img.to(dtype=restoration_model.dtype, device=frame_roi.device)
        temp.sub_(roi_f)
        temp.mul_(blend_mask.unsqueeze(-1))
        temp.add_(roi_f)
        temp.round_().clamp_(0, 255)
        frame_roi[:] = temp

    def _blend_cpu(blend_mask: torch.Tensor, clip_img: torch.Tensor, orig_clip_box: tuple[int, int, int, int]):
        blend_mask = blend_mask.cpu().numpy()
        clip_img = clip_img.cpu().numpy()
        t, l, b, r = orig_clip_box
        frame_roi = frame[t:b + 1, l:r + 1, :].numpy()
        temp_buffer = np.empty_like(frame_roi, dtype=np.float32)
        np.subtract(clip_img, frame_roi, out=temp_buffer, dtype=np.float32)
        np.multiply(temp_buffer, blend_mask[..., None], out=temp_buffer)
        np.add(temp_buffer, frame_roi, out=temp_buffer)
        frame_roi[:] = temp_buffer.astype(np.uint8)

    blend = _blend_cpu if is_cpu_input else _blend_gpu

    for buffered_clip in matched_clips:
        assert buffered_clip.frame_start == frame_num, \
            "blend_back_frame: caller passed a clip that doesn't start at frame_num - matching " \
            "clips to frame_num is the scheduler's job, not this function's (see docstring)"
        clip_img, clip_mask, orig_clip_box, orig_crop_shape, pad_after_resize = buffered_clip.pop()
        clip_img = image_utils.unpad_image(clip_img, pad_after_resize)
        clip_mask = image_utils.unpad_image(clip_mask, pad_after_resize)
        clip_img = image_utils.resize(clip_img, orig_crop_shape[:2])
        clip_mask = image_utils.resize(clip_mask, orig_crop_shape[:2], interpolation=cv2.INTER_NEAREST)
        blend_mask = mask_utils.create_blend_mask(clip_mask.to(device=restoration_model.device).float()).to(device=clip_img.device, dtype=target_dtype)

        blend(blend_mask, clip_img, orig_clip_box)

    return frame


def blend_regions_into_frame(
    frame: ImageTensor,
    regions: list[tuple[torch.Tensor, torch.Tensor, tuple[int, int, int, int]]],
    restoration_model,
) -> ImageTensor:
    """Blend a list of (restored_crop_bgr, mask, orig_box) into `frame` (mutated, returned).

    Same math as blend_back_frame's ROI path, but without Clip.pop() -- used by the scheduler's
    sparse pending store so multiple regions per frame can accumulate before a JIT push_ai_frame.
    Each crop/mask is already unpadded and resized to the original crop shape (scheduler job).
    """
    if not regions:
        return frame
    is_cpu_input = frame.device.type == "cpu"
    target_dtype = torch.float32 if is_cpu_input else restoration_model.dtype

    def _blend_gpu(blend_mask: torch.Tensor, clip_img: torch.Tensor, orig_clip_box: tuple[int, int, int, int]):
        t, l, b, r = orig_clip_box
        frame_roi = frame[t:b + 1, l:r + 1, :]
        roi_f = frame_roi.to(dtype=restoration_model.dtype)
        temp = clip_img.to(dtype=restoration_model.dtype, device=frame_roi.device)
        temp.sub_(roi_f)
        temp.mul_(blend_mask.unsqueeze(-1))
        temp.add_(roi_f)
        temp.round_().clamp_(0, 255)
        frame_roi[:] = temp

    def _blend_cpu(blend_mask: torch.Tensor, clip_img: torch.Tensor, orig_clip_box: tuple[int, int, int, int]):
        blend_mask = blend_mask.cpu().numpy()
        clip_img = clip_img.cpu().numpy()
        t, l, b, r = orig_clip_box
        frame_roi = frame[t:b + 1, l:r + 1, :].numpy()
        temp_buffer = np.empty_like(frame_roi, dtype=np.float32)
        np.subtract(clip_img, frame_roi, out=temp_buffer, dtype=np.float32)
        np.multiply(temp_buffer, blend_mask[..., None], out=temp_buffer)
        np.add(temp_buffer, frame_roi, out=temp_buffer)
        frame_roi[:] = temp_buffer.astype(np.uint8)

    blend = _blend_cpu if is_cpu_input else _blend_gpu
    for clip_img, clip_mask, orig_clip_box in regions:
        blend_mask = mask_utils.create_blend_mask(
            clip_mask.to(device=restoration_model.device).float()
        ).to(device=clip_img.device, dtype=target_dtype)
        blend(blend_mask, clip_img, orig_clip_box)
    return frame
