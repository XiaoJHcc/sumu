# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Ported (照搬) from lada-realtime lada/restorationpipeline/mosaic_detector.py
# (porting_manifest.md item 2). Imports repointed lada -> sumu.ai.
#
# `Scene`, `Clip` and `_NoDetectionResult` below are verbatim (only the import block
# changed). `append_or_create_scenes` / `materialize_completed_clips` are the method
# *bodies* of `MosaicDetector._create_or_append_scenes_based_on_prediction_result`
# (mosaic_detector.py:376-396) and `MosaicDetector._create_clips_for_completed_scenes`
# (mosaic_detector.py:355-375) extracted into standalone pure functions - the
# `MosaicDetector` class itself (worker threads, `PipelineQueue`, `StopMarker`/
# `EofMarker` handshake) is NOT ported; see porting_manifest.md "二、重写/剥离".
# Differences from the original method bodies, deliberately:
#   - `self.xxx` constructor params (`max_regions_per_frame`, `max_clip_length`,
#     `clip_size`, `pad_mode`, `video_meta_data`) become explicit function
#     parameters.
#   - `self.mosaic_clip_queue.put(clip)` + `self.clip_counter += 1` become
#     "append to a returned list" + "return the advanced counter" - no queue, no
#     mid-loop STOP_MARKER short-circuit (that was PipelineQueue backpressure
#     unwinding a producer thread; a pure function has no thread to unwind).
#   - `eof` here just means "flush every remaining scene into a clip regardless of
#     completion state" (same meaning as upstream's `eof=True` call site), not an
#     EOF_MARKER sentinel flowing through a queue.
from __future__ import annotations

import logging
from typing import List, Tuple

import cv2
import torch

from sumu.ai import LOG_LEVEL
from sumu.ai.utils import Box, VideoMetadata, ImageTensor, MaskTensor, Pad
from sumu.ai.utils import image_utils
from sumu.ai.utils.box_utils import box_overlap
from sumu.ai.utils.scene_utils import crop_to_box_v3
from sumu.ai.utils.ultralytics_utils import convert_yolo_box, convert_yolo_mask_tensor, UltralyticsResults

logger = logging.getLogger(__name__)
logging.basicConfig(level=LOG_LEVEL)


class _NoDetectionResult:
    """Stand-in for a YOLO result when detection is skipped for a frame (see
    lada's MosaicDetector.detection_start_frame - sumu's future scheduler will have its own
    name for this, YOLO frame-skipping itself is out of scope for this port). Only `.boxes` is
    ever read on this path (`append_or_create_scenes` short-circuits on an empty list before
    touching masks/orig_img), so nothing else needs to be modeled."""
    boxes = []


class Scene:
    def __init__(self, file_path: str, video_meta_data: VideoMetadata):
        self.file_path = file_path
        self.video_meta_data = video_meta_data
        self.frames: list[ImageTensor] = []
        self.masks: list[MaskTensor] = []
        self.boxes: list[Box] = []
        self.frame_start: int | None = None
        self.frame_end: int | None = None
        self._index: int = 0

    def __len__(self):
        return len(self.frames)

    def add_frame(self, frame_num: int, img: ImageTensor, mask: MaskTensor, box: Box):
        if self.frame_start is None:
            self.frame_start = frame_num
            self.frame_end = frame_num
        else:
            assert frame_num == self.frame_end + 1
            self.frame_end = frame_num

        self.frames.append(img)
        self.masks.append(mask)
        self.boxes.append(box)

    def merge_mask_box(self, mask: MaskTensor, box: Box):
        assert self.belongs(box)
        current_box = self.boxes[-1]
        t = min(current_box[0], box[0])
        l = min(current_box[1], box[1])
        b = max(current_box[2], box[2])
        r = max(current_box[3], box[3])
        new_box = (t, l, b, r)
        self.boxes[-1] = new_box
        self.masks[-1] = torch.maximum(self.masks[-1], mask)

    def belongs(self, box: Box):
        if len(self.boxes) == 0:
            return False
        last_scene_box = self.boxes[-1]
        return box_overlap(last_scene_box, box)

    def __iter__(self):
        return self

    def __next__(self):
        if self._index < len(self):
            item = self.frames[self._index], self.masks[self._index], self.boxes[self._index]
            self._index += 1
            return item
        else:
            raise StopIteration


class Clip:
    def __init__(self, scene: Scene, size, pad_mode, id):
        self.id = id
        self.file_path = scene.file_path
        self.frame_start = scene.frame_start
        self.frame_end = scene.frame_end
        assert self.frame_start <= self.frame_end
        self.size = size
        self.pad_mode = pad_mode
        self.frames: list[ImageTensor] = []
        self.masks: list[MaskTensor] = []
        self.boxes: list[Box] = []
        self.crop_shapes: List[Tuple[int, int]] = []
        self.pad_after_resizes: List[Pad] = []
        self._index: int = 0

        # crop scene
        for i in range(len(scene)):
            img, mask, box = scene.frames[i], scene.masks[i], scene.boxes[i]
            cropped_img, cropped_mask, cropped_box, _ = crop_to_box_v3(box, img, mask, (size, size), max_box_expansion_factor=1., border_size=0.06)
            self.frames.append(cropped_img)
            self.masks.append(cropped_mask)
            self.boxes.append(cropped_box)
            self.crop_shapes.append(cropped_img.shape)

        # resize crops to out_size
        max_width, max_height = self.get_max_width_height()
        scale_width, scale_height = size/max_width, size/max_height

        for i, (cropped_img, cropped_mask, cropped_box) in enumerate(zip(self.frames, self.masks, self.boxes)):
            crop_shape = cropped_img.shape

            resize_shape = (int(crop_shape[0] * scale_height), int(crop_shape[1] * scale_width))
            cropped_img = image_utils.resize(cropped_img, resize_shape, interpolation=cv2.INTER_LINEAR)
            cropped_mask = image_utils.resize(cropped_mask, resize_shape, interpolation=cv2.INTER_NEAREST)
            assert cropped_mask.shape[:2] == cropped_img.shape[:2], f"{cropped_mask.shape[:2]}, {cropped_img.shape[:2]}"
            assert cropped_img.shape[0] <= size or cropped_img.shape[1] <= size

            cropped_img, pad_after_resize = image_utils.pad_image(cropped_img, size, size, mode=self.pad_mode)
            cropped_mask, _ = image_utils.pad_image(cropped_mask, size, size, mode='zero')

            self.frames[i] = cropped_img
            self.masks[i] = cropped_mask
            self.boxes[i] = cropped_box
            self.crop_shapes[i] = crop_shape
            self.pad_after_resizes.append(pad_after_resize)

    def get_max_width_height(self):
        max_width = 0
        max_height = 0
        for box in self.boxes:
            t, l, b, r = box
            width, height = r - l + 1, b - t + 1
            if height > max_height:
                max_height = height
            if width > max_width:
                max_width = width
        return max_width, max_height

    def pop(self):
        """Stateful, one-shot consume: pops the oldest buffered frame off this clip and shrinks
        the frame_start/frame_end range accordingly. This mirrors lada's queue-consumer usage
        (each clip is drained frame-by-frame as the single restoration worker advances). sumu's
        ready-map scheduler may end up reading clips differently (e.g. keep the clip around and
        index into it instead of destructively popping) - see porting_manifest.md item 2 risk
        note. Not changed here; kept verbatim so the math/semantics stay a known quantity."""
        self.frame_start += 1
        if self.frame_start > self.frame_end:
            self.frame_start = None
            self.frame_end = None

        return self.frames.pop(0), self.masks.pop(0), self.boxes.pop(0), self.crop_shapes.pop(0), self.pad_after_resizes.pop(0)

    def __len__(self):
        return len(self.frames)

    def __iter__(self):
        return self

    def __next__(self):
        if self._index < len(self):
            item = self.frames[self._index], self.masks[self._index], self.boxes[self._index], self.crop_shapes[self._index], self.pad_after_resizes[self._index]
            self._index += 1
            return item
        else:
            raise StopIteration

    def __getitem__(self, item):
        return self.frames[item], self.masks[item], self.boxes[item]


def append_or_create_scenes(
    results: UltralyticsResults,
    scenes: list[Scene],
    frame_num: int,
    video_meta_data: VideoMetadata,
    max_regions_per_frame: int = 1,
) -> list[Scene]:
    """Extracted from MosaicDetector._create_or_append_scenes_based_on_prediction_result
    (mosaic_detector.py:376-396). For each YOLO detection box on this frame (capped at
    `max_regions_per_frame` - kept, NOT a free simplification to drop per porting_manifest.md),
    either merges it into the last frame of an existing Scene it overlaps (`Scene.belongs`), or
    appends it as a new frame to that scene, or starts a brand-new Scene. Mutates and returns
    `scenes` (mutate-in-place, matching upstream's list.append/mutation calling convention)."""
    num_boxes = min(len(results.boxes), max_regions_per_frame) if max_regions_per_frame > 0 else len(results.boxes)
    for i in range(num_boxes):
        mask = convert_yolo_mask_tensor(results.masks[i], results.orig_shape).to(device=results.orig_img.device)
        box = convert_yolo_box(results.boxes[i], results.orig_shape)

        current_scene = None
        for scene in scenes:
            if scene.belongs(box):
                current_scene = scene
                if scene.frame_end == frame_num:
                    current_scene.merge_mask_box(mask, box)
                else:
                    current_scene.add_frame(frame_num, results.orig_img, mask, box)
                break
        if current_scene is None:
            current_scene = Scene(video_meta_data.video_file, video_meta_data)
            scenes.append(current_scene)
            current_scene.add_frame(frame_num, results.orig_img, mask, box)
    return scenes


def materialize_completed_clips(
    scenes: list[Scene],
    frame_num: int,
    eof: bool,
    max_clip_length: int,
    clip_size: int,
    pad_mode: str,
    clip_counter: int,
) -> tuple[list[Scene], list[Clip], int]:
    """Extracted from MosaicDetector._create_clips_for_completed_scenes
    (mosaic_detector.py:355-375). A Scene is "completed" (ready to become a Clip) once no new
    detection can extend it (`frame_end < frame_num`), it's hit the length cap
    (`max_clip_length`), or the caller is flushing at end-of-stream (`eof=True`). Completing a
    scene also force-completes any *other* scene that started earlier (`other_scene.frame_start
    < current_scene.frame_start`) - this preserves clip output ordering by frame_start, verbatim
    from upstream.

    Unlike upstream, completed clips are returned in a list instead of pushed onto
    `mosaic_clip_queue`, and there is no `StopMarker` return value / mid-loop stop-request
    short-circuit (that was queue-producer backpressure unwinding a worker thread; here the
    caller decides what to do with `completed_clips`, including stopping early - EOF/stop
    semantics are expressed purely through the boolean `eof` argument and normal return, not a
    sentinel type).

    Returns (scenes, completed_clips, clip_counter) - clip_counter is threaded through
    explicitly (was `self.clip_counter` in upstream) since this is a pure function with no
    instance state to increment."""
    completed_scenes = []
    for current_scene in scenes:
        if (current_scene.frame_end < frame_num or len(current_scene) >= max_clip_length or eof) and current_scene not in completed_scenes:
            completed_scenes.append(current_scene)
            other_scenes = [other for other in scenes if other != current_scene]
            for other_scene in other_scenes:
                if other_scene.frame_start < current_scene.frame_start and other_scene not in completed_scenes:
                    completed_scenes.append(other_scene)

    completed_clips: list[Clip] = []
    for completed_scene in sorted(completed_scenes, key=lambda s: s.frame_start):
        clip = Clip(completed_scene, clip_size, pad_mode, clip_counter)
        completed_clips.append(clip)
        scenes.remove(completed_scene)
        clip_counter += 1

    return scenes, completed_clips, clip_counter
