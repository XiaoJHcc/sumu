# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Clock-driven AI producer that fills the native Player's ready-map ahead of the present
# head. This is the integration piece connecting the already-validated native core
# (native/src/player.cpp, contracts in docs/native_core.md / docs/native_ai_input.md) to the
# already-ported AI compute core (python/sumu/ai/, contracts exercised end-to-end in
# scripts/verify_scene_clip_blend.py).
#
# Architectural semantics this module must uphold (DESIGN.md):
#   I1 - present never blocks on AI. This module runs on its own daemon thread and every
#        native call it makes (get_cuda_nv12_by_frame / push_ai_frame) is designed by the
#        native layer itself to be non-blocking for the present thread; this module never
#        calls anything that could stall present.
#   I2 - present/AI are decoupled. Scheduler only talks to Player through its public,
#        already-validated API; it never touches present-loop internals.
#   I5 - frame number is the single source of truth. Every dict/list here is keyed by the
#        frame_num Player itself hands out (get_cuda_nv12_by_frame's echoed frame_num,
#        Player.current_frame(), Player.seek()'s returned actual frame).
#   I6 - seek = reposition. On a seek, this module resets its own in-flight AI state
#        (scenes/frame_cache/frontier) to the new position; it never tears down or recreates
#        threads/models.
#   I9 - degrade, never stall. If AI falls behind, the frontier is resynced to the present
#        head (dropping in-flight work) instead of trying to catch up frame-by-frame; if a
#        frame isn't decoded yet, the loop just sleeps and retries. Present always has a
#        clean passthrough fallback (native side), so under any of these conditions the only
#        visible effect is a lower ai_hit_rate, never a stutter.
#
# This is a *rewrite* of lada-realtime's worker/PipelineQueue orchestration (single daemon
# thread here, no queues, no STOP_MARKER/EOF_MARKER handshake - see DESIGN.md D "重写"), but
# it calls the *ported* pure functions verbatim (scene_clip.py / blend.py / video_utils.py /
# cuda_dlpack.py) exactly as scripts/verify_scene_clip_blend.py already exercised them.
from __future__ import annotations

import logging
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass
from typing import Optional

import torch

from sumu.ai.restorationpipeline.blend import blend_back_frame, restore_clip
from sumu.ai.restorationpipeline.scene_clip import (
    Clip,
    Scene,
    append_or_create_scenes,
    materialize_completed_clips,
)
from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor
from sumu.ai.utils.video_utils import _nv12_to_bgr_hwc_gpu

logger = logging.getLogger(__name__)

# Native passthrough ring is kRingCapacity=64 with kDecodeAheadMax=54 (player.cpp). AI can only
# pull frames that still sit in that ring ahead of present head -- so cold-start skip must not
# ask for a frame further ahead than this, or get_cuda_nv12_by_frame stays ready=False until the
# playhead itself advances (defeating the skip). Keep a small margin under 54.
_DECODE_AHEAD_SAFE = 50


def clamp_cold_start_s(value) -> float:
    """UI / settings range: 0–3 seconds. Non-numeric / NaN → default 1.0."""
    try:
        v = float(value)
    except (TypeError, ValueError):
        return 1.0
    if v != v:  # NaN
        return 1.0
    return max(0.0, min(3.0, v))


@dataclass
class SchedulerConfig:
    """All knobs deliberately exposed and tunable (DESIGN.md I9's downgrade levers). Defaults
    match the values called out in the task brief."""

    clip_length: int = 30          # BasicVSR++ clip length in frames (<= TRT engine max, 180)
    clip_size: int = 256           # square crop/resize size fed to BasicVSR++
    max_regions_per_frame: int = 1  # cap on YOLO detections turned into scenes per frame

    # Cold-start skip (seconds, not frames/clips): after open/seek, present plays passthrough from
    # the landing frame while AI starts at landing + round(cold_start_s * fps). Fixed wall time so
    # UX is consistent across fps/clip_length; clamped at runtime to the native decode-ahead ring.
    # 0 = previous behaviour (AI starts at the landing frame). UI range 0–3.
    cold_start_s: float = 1.0

    # AI frontier gate (README mechanism "处理前沿闸门"): keep ai_frontier in
    # [head, head + lead]. lead is a frame count, tied to clip_length so a clip has a chance
    # to complete and restore before playback catches up to its start. At runtime the gate also
    # never sits below cold-start skip frames (see Scheduler._effective_lead).
    lead: Optional[int] = None  # computed in __post_init__ if left None

    # Bounded frame_cache: holds CUDA-resident BGR frames from get_cuda_nv12_by_frame until
    # blend_back_frame consumes them. Sized to comfortably outlive one full lead+clip_length
    # span (worst case: a clip starts right at the frontier's trailing edge and needs every
    # frame back to head still cached when it completes).
    frame_cache_capacity: Optional[int] = None  # computed in __post_init__ if left None
    frame_cache_margin: int = 16

    # Throttle step used whenever the loop has nothing productive to do this iteration
    # (decode hasn't reached the requested frame yet, or the frontier is already far enough
    # ahead of head). Kept small per the brief (~1-2ms) so the producer reacts quickly once
    # work is available, without busy-spinning a full core.
    sleep_step_s: float = 0.0015

    # Discontinuity heuristic (backup to the explicit notify_seek() path, see module
    # docstring "seek/不连续检测"): current_frame() going backwards is unambiguous evidence of
    # a seek/loop. A *forward* jump only counts as a discontinuity once it is far larger than
    # anything one scheduler iteration's real-time playback advance could produce (the
    # producer loop only sleeps ~1-2ms at a time; even a slow clip-restore iteration measured
    # in the tens of ms at 60fps only advances current_frame() by a handful of frames) - a
    # jump of hundreds of frames is only explained by an actual seek.
    seek_jump_threshold: int = 500

    # Color conversion params for _nv12_to_bgr_hwc_gpu. Both sumu test videos are BT.709
    # limited-range (see CLAUDE.md); expose them here rather than hardcoding so a differently
    # tagged source can be wired up later without touching the loop body.
    bt709: bool = True
    full_range: bool = False

    model_name: str = "basicvsrpp-v1.2"

    def __post_init__(self):
        self.cold_start_s = clamp_cold_start_s(self.cold_start_s)
        if self.lead is None:
            self.lead = max(self.clip_length, round(1.2 * self.clip_length))
        if self.frame_cache_capacity is None:
            self.frame_cache_capacity = self.lead + self.clip_length + self.frame_cache_margin


class SchedulerStats:
    """Plain-int/float counters updated only from the producer thread, read (best-effort,
    unlocked - a torn read of a single int/float is not a correctness concern for a
    diagnostics counter) from any thread for periodic printing/logging."""

    def __init__(self):
        self.frames_detected = 0
        self.clips_restored = 0
        self.frames_pushed = 0
        self.frame_cache_misses = 0
        self.seek_resets = 0
        self.backlog_resyncs = 0
        self.started_at: Optional[float] = None
        self.first_push_at: Optional[float] = None
        # Net BasicVSR restore throughput only: wall time inside restore_clip(), excluding
        # frontier-gate sleeps / decode-not-ready waits / YOLO / blend. restore_fps =
        # restore_frames / restore_seconds (None until the first restore finishes).
        self.restore_frames = 0
        self.restore_seconds = 0.0

    def as_dict(self) -> dict:
        cold_start_s = (
            (self.first_push_at - self.started_at)
            if (self.started_at is not None and self.first_push_at is not None)
            else None
        )
        restore_fps = (
            (self.restore_frames / self.restore_seconds)
            if self.restore_seconds > 0.0
            else None
        )
        return {
            "frames_detected": self.frames_detected,
            "clips_restored": self.clips_restored,
            "frames_pushed": self.frames_pushed,
            "frame_cache_misses": self.frame_cache_misses,
            "seek_resets": self.seek_resets,
            "backlog_resyncs": self.backlog_resyncs,
            "cold_start_s": cold_start_s,
            "restore_frames": self.restore_frames,
            "restore_seconds": self.restore_seconds,
            "restore_fps": restore_fps,
        }


class Scheduler:
    """Clock-driven AI producer. One daemon thread, no queues. Every round: figure out where
    the present head is, decide whether the AI frontier needs resetting (seek) or resyncing
    (fell behind) or throttling (too far ahead), otherwise pull the next frame, run
    detection/clip aggregation/restoration/blend on it, and push finished frames into the
    native ready-map."""

    def __init__(
        self,
        player,
        det_model,
        res_model,
        pad_mode: str,
        video_meta_data,
        config: Optional[SchedulerConfig] = None,
        capture_correctness_samples: int = 0,
    ):
        self.player = player
        self.det_model = det_model
        self.res_model = res_model
        self.pad_mode = pad_mode
        self.video_meta_data = video_meta_data
        self.config = config or SchedulerConfig()

        self.stats = SchedulerStats()

        # Verification-only hook (default off, costs nothing when capture_correctness_samples
        # == 0): captures the first N (frame_num, original_bgr, final_bgr, rgba) tuples right
        # before each push_ai_frame call, moved to CPU immediately so they don't hold GPU
        # memory or compete with the production path. Consumed by scripts/run_player.py's
        # correctness check (channel-order + "mosaic actually changed" verification against
        # an independent CPU reference decode) - see docs/scheduler.md.
        self._capture_budget = capture_correctness_samples
        self.correctness_samples: list[dict] = []

        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._seek_lock = threading.Lock()
        self._pending_seek: Optional[int] = None

        # Producer-thread-owned state (only ever mutated inside _run/_process_frame, which
        # both execute on the same single daemon thread - no lock needed for these).
        self.scenes: list[Scene] = []
        self.clip_counter = 0
        self.frame_cache: "OrderedDict[int, torch.Tensor]" = OrderedDict()
        self.ai_frontier = 0
        self._last_head = 0
        self._eof_flushed_at: Optional[int] = None

    # ---- public control surface --------------------------------------------------------

    def notify_seek(self, frame_num: int) -> None:
        """Reliable seek notification: call this whenever the app calls player.seek(...) (in
        either order relative to the actual seek() call - the producer thread reads this
        before deciding anything else on its next iteration). This is the primary path;
        the current_frame()-regression/jump heuristic in _run() is only a backup for cases
        where a caller drives Player.seek() without going through this method."""
        with self._seek_lock:
            self._pending_seek = int(frame_num)

    def start(self) -> None:
        if self._thread is not None:
            raise RuntimeError("Scheduler already started")
        # Anchor AI at landing + cold-start skip (same path as seek) so open/play also skips the
        # first ~cold_start_s of content instead of racing the playhead from frame 0.
        self._anchor_at(self.player.current_frame())
        self.stats.started_at = time.monotonic()
        self._thread = threading.Thread(target=self._run, name="sumu-ai-scheduler", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None

    def get_stats(self) -> dict:
        d = self.stats.as_dict()
        d["ai_frontier"] = self.ai_frontier
        d["scenes_open"] = len(self.scenes)
        d["frame_cache_size"] = len(self.frame_cache)
        d["cold_start_skip_frames"] = self._cold_start_frames()
        return d

    # ---- cold-start helpers ----------------------------------------------------------------

    def _cold_start_frames(self) -> int:
        """Frames to skip after open/seek before AI starts. Fixed seconds × fps, clamped to the
        native decode-ahead ring so the first AI pull can hit a ring slot immediately."""
        fps = float(self.player.fps() or 0.0)
        if fps <= 0.0:
            fps = 30.0
        s = clamp_cold_start_s(self.config.cold_start_s)
        raw = int(round(s * fps))
        return max(0, min(raw, _DECODE_AHEAD_SAFE))

    def _effective_lead(self) -> int:
        """Frontier gate upper bound: base lead, but never below cold-start skip (otherwise
        ai_frontier = head + skip would immediately hit frontier > head+lead and sleep forever)."""
        base = int(self.config.lead or 0)
        return max(base, self._cold_start_frames())

    def _frame_cache_cap(self) -> int:
        cfg = self.config
        return max(
            int(cfg.frame_cache_capacity or 0),
            self._effective_lead() + cfg.clip_length + cfg.frame_cache_margin,
        )

    # ---- internals ------------------------------------------------------------------------

    def _anchor_at(self, frame_num: int) -> None:
        """Drop in-flight AI state and re-anchor frontier at frame_num + cold-start skip.
        Used on start and on every seek/discontinuity (I6). Mid-stream backlog resync does NOT
        use this -- catch-up jumps to head with no extra skip."""
        self.scenes = []
        self.frame_cache.clear()
        skip = self._cold_start_frames()
        self.ai_frontier = int(frame_num) + skip
        self._last_head = int(frame_num)
        self._eof_flushed_at = None

    def _reset_state(self, frame_num: int) -> None:
        """Seek/discontinuity handling (I6): drop every piece of in-flight AI state and
        re-anchor the frontier with cold-start skip. Scenes reference frame numbers that assert
        strict +1 contiguity (Scene.add_frame), so any discontinuity invalidates them
        outright; frame_cache entries are keyed to frame numbers on the *old* timeline and are
        equally invalid. clip_counter is left monotonically increasing (it is only ever used
        as an opaque id, not a correctness-relevant counter, so there's no need to reset it -
        avoids any chance of colliding with a clip id already in flight through restore/blend)."""
        self._anchor_at(frame_num)

    def _run(self) -> None:
        cfg = self.config
        while not self._stop_event.is_set():
            pending_seek = None
            with self._seek_lock:
                if self._pending_seek is not None:
                    pending_seek = self._pending_seek
                    self._pending_seek = None
            if pending_seek is not None:
                self._reset_state(pending_seek)
                self.stats.seek_resets += 1
                continue

            head = self.player.current_frame()

            # Backup discontinuity heuristic (see SchedulerConfig.seek_jump_threshold
            # docstring) - only fires if the caller drove player.seek()/looped without going
            # through notify_seek().
            if head < self._last_head or (head - self._last_head) > cfg.seek_jump_threshold:
                self._reset_state(head)
                self.stats.seek_resets += 1
                continue
            self._last_head = head

            if self.ai_frontier < head:
                # Fell behind: don't try to catch up frame-by-frame (that would just dig the
                # hole deeper while present has long since moved on) - jump straight to head
                # and drop whatever was in flight (I9: degrade, don't stall). No cold-start
                # re-skip here -- that only applies to explicit open/seek anchors.
                self.scenes = []
                self.ai_frontier = head
                self.stats.backlog_resyncs += 1
                continue

            if self.ai_frontier > head + self._effective_lead():
                time.sleep(cfg.sleep_step_s)
                continue

            n = self.ai_frontier
            frame_count = self.player.frame_count()

            g = self.player.get_cuda_nv12_by_frame(n)
            if not g["ready"]:
                # Decode head hasn't reached n yet (or it was overwritten - see
                # docs/native_ai_input.md's ring-overwrite caveat). Never block: just retry
                # next iteration. Note: frame numbers are monotonically increasing across
                # content loops (I5) - there is no "n >= frame_count -> stop producing" state;
                # the decode head keeps advancing past frame_count on every loop and n must
                # keep following it, forever.
                time.sleep(cfg.sleep_step_s)
                continue

            # Content-position eof: n's position *within the current loop* (n % frame_count),
            # not n itself, marks the loop boundary. This fires once per loop (every
            # frame_count frames) instead of only once at first-pass end, so scenes get
            # flushed at every content discontinuity - the tail of one loop and the head of
            # the next are not temporally continuous, so Scene/BasicVSR++ state must not
            # bridge across it. Only the eof flag/materialize call uses the wrapped position;
            # get_cuda_nv12_by_frame/push_ai_frame above and ai_frontier below still use the
            # raw monotonic n, matching present/ring's own frame numbering.
            eof = bool(frame_count > 0 and (n % frame_count) == frame_count - 1)
            self._process_frame(n, g, eof)
            self.ai_frontier = n + 1

    def _process_frame(self, n: int, g: dict, eof: bool) -> None:
        cfg = self.config

        nv12 = wrap_nv12_cuda_buffer_as_tensor(g["dev_ptr"], g["width"], g["height"], g["pitch_bytes"])
        bgr = _nv12_to_bgr_hwc_gpu(nv12, g["height"], g["width"], bt709=cfg.bt709, full_range=cfg.full_range)
        # Native's buffer is single-buffered and reused on the *next* get_cuda_nv12_by_frame
        # call (docs/native_ai_input.md) - clone now so this frame survives in frame_cache
        # across however many future iterations until blend_back_frame needs it.
        frame = bgr.clone()
        self._cache_put(n, frame)

        pre = self.det_model.preprocess([frame])
        results = self.det_model.inference_and_postprocess(pre, [frame])[0]
        self.stats.frames_detected += 1

        self.scenes = append_or_create_scenes(
            results, self.scenes, n, self.video_meta_data, cfg.max_regions_per_frame
        )
        self.scenes, clips, self.clip_counter = materialize_completed_clips(
            self.scenes, n, False, cfg.clip_length, cfg.clip_size, self.pad_mode, self.clip_counter
        )
        for clip in clips:
            self._restore_and_push(clip)

        if eof and self._eof_flushed_at != n:
            self._eof_flushed_at = n
            self.scenes, clips, self.clip_counter = materialize_completed_clips(
                self.scenes, n, True, cfg.clip_length, cfg.clip_size, self.pad_mode, self.clip_counter
            )
            for clip in clips:
                self._restore_and_push(clip)

    def _restore_and_push(self, clip: Clip) -> None:
        frame_start, frame_end = clip.frame_start, clip.frame_end  # Clip.pop() mutates these
        n_frames = frame_end - frame_start + 1
        # Net BasicVSR wall time only (no gate wait). Synchronize so async GPU work is fully
        # charged to this clip rather than leaking into the subsequent blend/push path.
        if torch.cuda.is_available():
            torch.cuda.synchronize()
        t0 = time.perf_counter()
        restore_clip(self.res_model, self.config.model_name, clip)
        if torch.cuda.is_available():
            torch.cuda.synchronize()
        dt = time.perf_counter() - t0
        if dt > 0.0 and n_frames > 0:
            self.stats.restore_frames += n_frames
            self.stats.restore_seconds += dt
        self.stats.clips_restored += 1

        for fnum in range(frame_start, frame_end + 1):
            orig = self.frame_cache.get(fnum)
            if orig is None:
                # Should not happen given frame_cache_capacity's sizing (see SchedulerConfig
                # docstring), but blend_back_frame's pop() must still be drained in lockstep
                # to keep the clip's internal bookkeeping consistent for the next fnum in this
                # loop - so degrade by skipping the push (frame stays passthrough for fnum)
                # rather than desyncing or throwing.
                logger.warning(
                    "scheduler: frame_cache miss for fnum=%d (evicted before blend) - skipping push, "
                    "frame stays on passthrough", fnum,
                )
                clip.pop()
                self.stats.frame_cache_misses += 1
                continue

            original_for_capture = orig.clone() if self._capture_budget > 0 else None
            final_bgr = blend_back_frame(orig, fnum, [clip], self.res_model)
            rgba = self._to_rgba(final_bgr)

            if self._capture_budget > 0:
                self.correctness_samples.append({
                    "frame_num": fnum,
                    "original_bgr": original_for_capture.cpu(),
                    "final_bgr": final_bgr.clone().cpu(),
                    "rgba": rgba.clone().cpu(),
                })
                self._capture_budget -= 1

            h, w = rgba.shape[0], rgba.shape[1]
            self.player.push_ai_frame(fnum, rgba.data_ptr(), w, h, w * 4)
            self.frame_cache.pop(fnum, None)

            self.stats.frames_pushed += 1
            if self.stats.first_push_at is None:
                self.stats.first_push_at = time.monotonic()

    def _cache_put(self, n: int, frame: torch.Tensor) -> None:
        self.frame_cache[n] = frame
        cap = self._frame_cache_cap()
        while len(self.frame_cache) > cap:
            evicted_n, _ = self.frame_cache.popitem(last=False)
            logger.debug("scheduler: frame_cache evicted frame %d before it was blended", evicted_n)

    @staticmethod
    def _to_rgba(bgr: torch.Tensor) -> torch.Tensor:
        """(H,W,3) BGR uint8 CUDA -> (H,W,4) RGBA8 uint8 CUDA, contiguous - the exact layout
        push_ai_frame's native contract requires (DXGI_FORMAT_R8G8B8A8_UNORM, byte0=R; see
        docs/native_ai_input.md / player.cpp's push_ai_frame). bgr's channel order is
        [B,G,R] (that's what _nv12_to_bgr_hwc_gpu's torch.stack([b,g,r], dim=2) produces), so
        R lives at index 2, not 0 - getting this backwards is exactly the "blue face" failure
        mode called out in the task brief."""
        h, w = bgr.shape[0], bgr.shape[1]
        rgba = torch.empty((h, w, 4), dtype=torch.uint8, device=bgr.device)
        rgba[..., 0] = bgr[..., 2]  # R
        rgba[..., 1] = bgr[..., 1]  # G
        rgba[..., 2] = bgr[..., 0]  # B
        rgba[..., 3] = 255
        return rgba.contiguous()
