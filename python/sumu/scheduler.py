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

from sumu.ai.restorationpipeline.blend import blend_back_frame, blend_regions_into_frame, restore_clip
from sumu.ai.restorationpipeline.scene_clip import (
    Clip,
    Scene,
    append_or_create_scenes,
    materialize_completed_clips,
)
from sumu.ai.utils import image_utils
from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor
from sumu.ai.utils.video_utils import _nv12_to_bgr_hwc_gpu
import cv2

logger = logging.getLogger(__name__)

# Fallback when Player has no ring_capacity/decode_ahead_max (older builds / tests).
# Live values: PT ring is resolution-aware (1080p/4K both aim ~180 NV12); AI display ring
# may be shorter at 4K (sparse crop stockpile + JIT push). See player.cpp pick_ring_capacities.
_DECODE_AHEAD_SAFE_FALLBACK = 50
# DESIGN.md I8 / lookahead_frames: stockpile ~180 frames of AI work for hard segments.
_DEFAULT_LEAD_FRAMES = 180
# How far ahead of present head we JIT-push into the (possibly short) native AI ring.
# Leave margin under ai_ring_capacity so present can still hit the slot.
_AI_PUSH_MARGIN = 8

# H4: backoff between consecutive unexpected producer exceptions (e.g. CUDA OOM on one
# pathological clip) so the playhead can advance past the failing region instead of
# tight-looping the same exception.
_ERROR_BACKOFF_BASE_S = 0.1
_ERROR_BACKOFF_MAX_S = 1.0

# DESIGN.md I8 (VRAM is a first-class constraint): frame_cache holds full-resolution BGR HWC
# uint8 CUDA tensors (~6.2MB/frame at 1080p, ~24.9MB/frame at 4K). The frame-count cap
# (lead + clip_length + margin ~= 226 frames) is resolution-blind: ~1.4GB at 1080p but
# ~5.6GB at 4K. So the cap is additionally clamped by this byte budget once the real
# per-frame size is known from the first cached frame (see _frame_cache_cap).
_FRAME_CACHE_BUDGET_BYTES = 2 * 1024**3


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
    # [head, head + lead]. Default ~180 (DESIGN.md lookahead) so easy segments can stockpile
    # restored frames for hard multi-region segments. Runtime-clamped to native decode-ahead
    # (see Scheduler._effective_lead) so AI never races past the passthrough ring.
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
            # Prefer DESIGN.md ~180 lookahead; never below clip_length so a full clip can finish.
            self.lead = max(self.clip_length, _DEFAULT_LEAD_FRAMES)
        else:
            try:
                self.lead = int(self.lead)
            except (TypeError, ValueError):
                self.lead = _DEFAULT_LEAD_FRAMES
            self.lead = max(1, min(_DEFAULT_LEAD_FRAMES, self.lead))
            # A lead shorter than one clip still works but starves restore; keep at least clip_length.
            self.lead = max(self.clip_length, self.lead)
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
        # Session generation (guarded by _seek_lock): bumped by every start(). The producer
        # thread captures its own generation and re-validates it at every native push/query
        # point -- a thread orphaned by a stop() join timeout must never push frames from the
        # old timeline into a reopened session (H5).
        self._generation = 0

        # Producer-thread-owned state (only ever mutated inside _run/_process_frame, which
        # both execute on the same single daemon thread - no lock needed for these).
        self.scenes: list[Scene] = []
        self.clip_counter = 0
        self.frame_cache: "OrderedDict[int, torch.Tensor]" = OrderedDict()
        # Sparse restored regions: fnum -> list of (crop_bgr, mask, box) ready to blend.
        # Full-frame RGBA stays out of VRAM until JIT push into the short native AI ring.
        self.pending_regions: "OrderedDict[int, list]" = OrderedDict()
        self.ai_frontier = 0
        self._last_head = 0
        self._eof_flushed_at: Optional[int] = None
        # P1: restore timing via CUDA event pairs instead of torch.cuda.synchronize()
        # (a device-wide sync per clip serializes YOLO/blend/push/next-clip submission and
        # breaks the GPU pipeline). One pair is in flight at a time; settled (read) lazily on
        # the next _restore_and_push once the end event has completed - elapsed_time() before
        # completion raises, so it is only ever called after query() says True. Events are
        # recycled through this pool to avoid re-creation churn.
        self._restore_ev_pair: list = []  # FIFO of (start_ev, end_ev, n_frames) awaiting settle
        self._restore_ev_pool: list = []
        # P3: per-frame byte size of cached tensors, learned from the first _cache_put (the
        # byte-budget clamp in _frame_cache_cap only kicks in afterwards).
        self._frame_bytes = 0
        self._frame_cache_cap_warned = False

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
        # New session: bump the generation so a still-running thread orphaned by a previous
        # stop() join timeout can never push into this session, and clear the stop event so a
        # same-instance restart actually runs (the orphaned thread's stale-generation check is
        # what keeps it from coming back to life -- see _session_ok).
        with self._seek_lock:
            self._generation += 1
            gen = self._generation
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run, args=(gen,), name="sumu-ai-scheduler", daemon=True
        )
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
            self._thread = None

    def _session_ok(self, gen: int) -> bool:
        """True while this producer generation is still the live session: not stop()ed and no
        newer start() has superseded it. Checked before every native push (and at the top of
        every producer iteration) so an orphaned thread can never leak old-timeline frames
        into a new session (H5)."""
        if self._stop_event.is_set():
            return False
        with self._seek_lock:
            return self._generation == gen

    def get_stats(self) -> dict:
        d = self.stats.as_dict()
        d["ai_frontier"] = self.ai_frontier
        d["scenes_open"] = len(self.scenes)
        d["frame_cache_size"] = len(self.frame_cache)
        d["pending_frames"] = len(self.pending_regions)
        d["cold_start_skip_frames"] = self._cold_start_frames()
        d["effective_lead"] = self._effective_lead()
        d["decode_ahead_safe"] = self._decode_ahead_safe()
        d["ai_push_window"] = self._ai_push_window()
        return d

    # ---- cold-start helpers ----------------------------------------------------------------

    def _decode_ahead_safe(self) -> int:
        """Max frames AI may sit ahead of present head and still hit the passthrough ring.

        Reads live Player.decode_ahead_max() (resolution-aware ring); falls back to a
        conservative constant if the binding is missing. Keeps a small margin under the
        native throttle so get_cuda_nv12_by_frame is ready before the slot is overwritten.
        """
        try:
            if hasattr(self.player, "decode_ahead_max"):
                n = int(self.player.decode_ahead_max())
                if n > 0:
                    return max(0, n - 4)
            if hasattr(self.player, "ring_capacity"):
                n = int(self.player.ring_capacity())
                if n > 0:
                    return max(0, n - 14)
        except Exception:  # noqa: BLE001 -- never fail the producer on a stats/probe path
            pass
        return _DECODE_AHEAD_SAFE_FALLBACK

    def _ai_push_window(self) -> int:
        """Max frames ahead of present head that may sit in the native AI ready-map.

        At 4K the AI ring is intentionally short (~64); restored crops live in pending_regions
        until the playhead approaches, then we blend+push. 1080p keeps a deep AI ring so the
        window can match the PT lead.
        """
        try:
            if hasattr(self.player, "ai_ring_capacity"):
                n = int(self.player.ai_ring_capacity())
                if n > 0:
                    return max(1, n - _AI_PUSH_MARGIN)
        except Exception:  # noqa: BLE001
            pass
        # Symmetric legacy / missing binding: use PT-safe lead.
        return self._decode_ahead_safe()

    def _cold_start_frames(self) -> int:
        """Frames to skip after open/seek before AI starts. Fixed seconds × fps, clamped to the
        native decode-ahead ring so the first AI pull can hit a ring slot immediately."""
        fps = float(self.player.fps() or 0.0)
        if fps <= 0.0:
            fps = 30.0
        s = clamp_cold_start_s(self.config.cold_start_s)
        raw = int(round(s * fps))
        return max(0, min(raw, self._decode_ahead_safe()))

    def _effective_lead(self) -> int:
        """Frontier gate upper bound: config lead (default ~180), never below cold-start skip,
        and never past the native decode-ahead ring (otherwise AI spins on ready=False)."""
        base = int(self.config.lead or 0)
        lead = max(base, self._cold_start_frames())
        safe = self._decode_ahead_safe()
        if safe <= 0:
            return lead
        return min(lead, max(self._cold_start_frames(), safe))


    def _frame_cache_cap(self) -> int:
        cfg = self.config
        frames_cap = max(
            int(cfg.frame_cache_capacity or 0),
            self._effective_lead() + cfg.clip_length + cfg.frame_cache_margin,
        )
        # P3 / I8: clamp by the VRAM byte budget once the real per-frame size is known (first
        # _cache_put). The frame-count cap alone is resolution-blind: ~1.4GB at 1080p but
        # ~5.6GB at 4K. Behaviour change: at 4K the cache now holds fewer frames than the lead
        # span, so long clips far ahead of the head may find their early frames evicted before
        # blend (counted as frame_cache_misses, regions dropped) - accepted trade-off: VRAM is
        # a first-class constraint and 4K AI is best-effort anyway.
        if self._frame_bytes > 0:
            budget_frames = _FRAME_CACHE_BUDGET_BYTES // self._frame_bytes
            # Floor: always room for at least one full in-flight clip plus margin, even if
            # that alone exceeds the byte budget.
            floor = cfg.clip_length + cfg.frame_cache_margin
            byte_cap = max(floor, budget_frames)
            if byte_cap < frames_cap:
                if not self._frame_cache_cap_warned:
                    self._frame_cache_cap_warned = True
                    logger.warning(
                        "scheduler: frame_cache cap clamped by VRAM budget: %d -> %d frames "
                        "(%d B/frame, budget %d MiB, floor %d)",
                        frames_cap, byte_cap, self._frame_bytes,
                        _FRAME_CACHE_BUDGET_BYTES >> 20, floor,
                    )
                return byte_cap
        return frames_cap

    # ---- internals ------------------------------------------------------------------------

    def _anchor_at(self, frame_num: int) -> None:
        """Drop in-flight AI state and re-anchor frontier at frame_num + cold-start skip.
        Used on start and on every seek/discontinuity (I6). Mid-stream backlog resync does NOT
        use this -- catch-up jumps to head with no extra skip."""
        self.scenes = []
        self.frame_cache.clear()
        self.pending_regions.clear()
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

    def _run(self, gen: int) -> None:
        """Producer thread body (H4): every iteration is wrapped so an unexpected exception
        (CUDA OOM inside restore, native calls throwing under a half-closed session, ...) can
        never kill the thread silently. Expected teardown exceptions (stop()/superseded
        generation) exit quietly; anything else drops in-flight state and resyncs the
        frontier to the present head, with backoff so a deterministic failure at one position
        doesn't tight-loop."""
        consecutive_errors = 0
        while not self._stop_event.is_set():
            if not self._session_ok(gen):
                # Orphaned by a stop() join timeout + a newer start(): exit quietly and never
                # touch the new session again (H5).
                logger.info("scheduler: stale producer generation %d exiting", gen)
                return
            try:
                self._run_iteration(gen)
                consecutive_errors = 0
            except Exception:  # noqa: BLE001 -- the producer thread must never die silently
                if not self._session_ok(gen):
                    # stop()/reopen tore the session down under us (e.g. player calls throwing
                    # mid-teardown) - expected, exit quietly.
                    logger.info("scheduler: producer exiting during session teardown")
                    return
                consecutive_errors += 1
                logger.exception(
                    "scheduler: producer iteration failed (%d in a row); dropping in-flight "
                    "state and resyncing frontier to head",
                    consecutive_errors,
                )
                self._resync_after_error()
                time.sleep(
                    min(_ERROR_BACKOFF_BASE_S * consecutive_errors, _ERROR_BACKOFF_MAX_S)
                )

    def _resync_after_error(self) -> None:
        """H4 recovery: drop every piece of in-flight AI state and resync the frontier to the
        present head (same degrade-don't-stall idea as the I9 backlog resync; no cold-start
        re-skip). Also releases cached CUDA blocks once: the hot path deliberately never calls
        empty_cache() (it stalls the torch allocator for everyone), but after e.g. a CUDA OOM
        this recovery path is the one place where reclaiming cached blocks is worth it."""
        if torch.cuda.is_available():
            try:
                torch.cuda.empty_cache()
            except Exception:  # noqa: BLE001 -- recovery must never throw
                pass
        try:
            head = int(self.player.current_frame())
        except Exception:  # noqa: BLE001 -- session may be half-closed; use last known head
            head = self._last_head
        self.scenes = []
        self.frame_cache.clear()
        self.pending_regions.clear()
        self.ai_frontier = head
        self._last_head = head
        self._eof_flushed_at = None
        # Drop unsettled restore-timing pairs: after e.g. a CUDA OOM their measurement is
        # meaningless (and touching events on a possibly-broken context is not worth it).
        self._restore_ev_pair.clear()
        self.stats.backlog_resyncs += 1

    def _run_iteration(self, gen: int) -> None:
        cfg = self.config
        pending_seek = None
        with self._seek_lock:
            if self._pending_seek is not None:
                pending_seek = self._pending_seek
                self._pending_seek = None
        if pending_seek is not None:
            self._reset_state(pending_seek)
            self.stats.seek_resets += 1
            return

        head = self.player.current_frame()

        # Backup discontinuity heuristic (see SchedulerConfig.seek_jump_threshold
        # docstring) - only fires if the caller drove player.seek()/looped without going
        # through notify_seek().
        if head < self._last_head or (head - self._last_head) > cfg.seek_jump_threshold:
            self._reset_state(head)
            self.stats.seek_resets += 1
            return
        self._last_head = head

        if self.ai_frontier < head:
            # Fell behind: don't try to catch up frame-by-frame (that would just dig the
            # hole deeper while present has long since moved on) - jump straight to head
            # and drop whatever was in flight (I9: degrade, don't stall). No cold-start
            # re-skip here -- that only applies to explicit open/seek anchors.
            self.scenes = []
            self.ai_frontier = head
            self.stats.backlog_resyncs += 1
            # Still JIT-push any pending stockpile that is now in the near-head window.
            self._flush_pending_to_native(head, gen)
            return

        # Push stockpiled restorations that have entered the short AI display window.
        self._flush_pending_to_native(head, gen)

        if self.ai_frontier > head + self._effective_lead():
            time.sleep(cfg.sleep_step_s)
            return

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
            return

        # Content-position eof: n's position *within the current loop* (n % frame_count),
        # not n itself, marks the loop boundary. This fires once per loop (every
        # frame_count frames) instead of only once at first-pass end, so scenes get
        # flushed at every content discontinuity - the tail of one loop and the head of
        # the next are not temporally continuous, so Scene/BasicVSR++ state must not
        # bridge across it. Only the eof flag/materialize call uses the wrapped position;
        # get_cuda_nv12_by_frame/push_ai_frame above and ai_frontier below still use the
        # raw monotonic n, matching present/ring's own frame numbering.
        eof = bool(frame_count > 0 and (n % frame_count) == frame_count - 1)
        self._process_frame(n, g, eof, gen)
        self.ai_frontier = n + 1

    def _process_frame(self, n: int, g: dict, eof: bool, gen: int) -> None:
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
            self._restore_and_push(clip, gen)

        if eof and self._eof_flushed_at != n:
            self._eof_flushed_at = n
            self.scenes, clips, self.clip_counter = materialize_completed_clips(
                self.scenes, n, True, cfg.clip_length, cfg.clip_size, self.pad_mode, self.clip_counter
            )
            for clip in clips:
                self._restore_and_push(clip, gen)

    def _restore_and_push(self, clip: Clip, gen: int) -> None:
        frame_start, frame_end = clip.frame_start, clip.frame_end  # Clip.pop() mutates these
        n_frames = frame_end - frame_start + 1
        # P1: time restore with a CUDA event pair, NOT torch.cuda.synchronize(). A device-wide
        # sync per clip drains the whole GPU pipeline every clip_length frames, serializing
        # YOLO/blend/push/next-clip submission (same hazard webstream/decensor.py documents).
        # Events record on the current stream, so the pair brackets everything the producer
        # submitted in between - same measurement scope as the old sync-based wall time. The
        # pair is settled lazily (next call, once the end event completes), so restore_fps
        # lags reality by at most one clip and a clip in flight at stop() is simply not
        # counted. CPU-only fallback keeps the old perf_counter measure (approximate: it
        # includes submission overhead).
        self._settle_restore_timing()
        if torch.cuda.is_available():
            start_ev = self._restore_ev_pool.pop() if self._restore_ev_pool else torch.cuda.Event(enable_timing=True)
            end_ev = self._restore_ev_pool.pop() if self._restore_ev_pool else torch.cuda.Event(enable_timing=True)
            start_ev.record()
            restore_clip(self.res_model, self.config.model_name, clip)
            end_ev.record()
            self._restore_ev_pair.append((start_ev, end_ev, n_frames))
        else:
            t0 = time.perf_counter()
            restore_clip(self.res_model, self.config.model_name, clip)
            dt = time.perf_counter() - t0
            if dt > 0.0 and n_frames > 0:
                self.stats.restore_frames += n_frames
                self.stats.restore_seconds += dt
        self.stats.clips_restored += 1

        # The restore above can easily outlive a stop() join timeout (4K/long clips): if this
        # generation is no longer the live session, drop the whole clip right here instead of
        # draining old-timeline regions into a reopened session's ready-map (H5).
        if not self._session_ok(gen):
            return

        # Drain clip into sparse pending (not full-frame RGBA in the native AI ring yet).
        # Multi-region: multiple clips may contribute regions to the same fnum; JIT flush
        # blends them all onto the cached original before push_ai_frame.
        for fnum in range(frame_start, frame_end + 1):
            if fnum not in self.frame_cache:
                logger.warning(
                    "scheduler: frame_cache miss for fnum=%d before sparse store - dropping region",
                    fnum,
                )
                clip.pop()
                self.stats.frame_cache_misses += 1
                continue
            clip_img, clip_mask, orig_clip_box, orig_crop_shape, pad_after_resize = clip.pop()
            clip_img = image_utils.unpad_image(clip_img, pad_after_resize)
            clip_mask = image_utils.unpad_image(clip_mask, pad_after_resize)
            clip_img = image_utils.resize(clip_img, orig_crop_shape[:2])
            clip_mask = image_utils.resize(
                clip_mask, orig_crop_shape[:2], interpolation=cv2.INTER_NEAREST
            )
            # Contiguous clones so pending outlives clip buffer reuse.
            region = (clip_img.contiguous(), clip_mask.contiguous(), orig_clip_box)
            bucket = self.pending_regions.get(fnum)
            if bucket is None:
                self.pending_regions[fnum] = [region]
            else:
                bucket.append(region)

        try:
            head = int(self.player.current_frame())
        except Exception:  # noqa: BLE001
            head = 0
        self._flush_pending_to_native(head, gen)

    def _settle_restore_timing(self) -> None:
        """Read out completed restore-timing event pairs (P1). elapsed_time() raises if the
        end event has not completed, so it is only called after query() says done; the stream
        is FIFO, so the first incomplete pair means everything behind it is incomplete too.
        Completed events go back to the pool for reuse."""
        pending = self._restore_ev_pair
        while pending:
            start_ev, end_ev, n_frames = pending[0]
            if not end_ev.query():
                break
            pending.pop(0)
            dt = start_ev.elapsed_time(end_ev) / 1000.0  # ms -> s
            if dt > 0.0 and n_frames > 0:
                self.stats.restore_frames += n_frames
                self.stats.restore_seconds += dt
            self._restore_ev_pool.extend((start_ev, end_ev))

    def _flush_pending_to_native(self, head: int, gen: int) -> None:
        """Blend pending sparse regions for frames in [head, head+ai_push_window] and push.

        Frames behind head are dropped (present already passed). Frames beyond the AI display
        window stay in pending until the playhead approaches (4K short AI ring).

        Every push is gated on this generation still being the live session: after a stop()
        join timeout the orphaned thread may still finish its current iteration, but it must
        never land old-timeline frames in a reopened session's ready-map (H5).
        """
        if not self._session_ok(gen):
            return
        if not self.pending_regions:
            return
        window = self._ai_push_window()
        hi = head + window
        # Drop anything already behind the playhead.
        while self.pending_regions:
            fnum = next(iter(self.pending_regions))
            if fnum >= head:
                break
            self.pending_regions.popitem(last=False)
            self.frame_cache.pop(fnum, None)

        # Push in frame order within the near-head window.
        to_push = [f for f in self.pending_regions if f <= hi]
        to_push.sort()
        for fnum in to_push:
            if not self._session_ok(gen):
                return
            regions = self.pending_regions.pop(fnum, None)
            if not regions:
                continue
            orig = self.frame_cache.get(fnum)
            if orig is None:
                logger.warning(
                    "scheduler: frame_cache miss for fnum=%d at JIT push - skip", fnum,
                )
                self.stats.frame_cache_misses += 1
                continue
            original_for_capture = orig.clone() if self._capture_budget > 0 else None
            final_bgr = blend_regions_into_frame(orig, regions, self.res_model)
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
            # Keep frame_cache until head advances so a later multi-region clip can re-blend
            # onto the already-restored frame and re-push (same fnum).

            self.stats.frames_pushed += 1
            if self.stats.first_push_at is None:
                self.stats.first_push_at = time.monotonic()

    def _cache_put(self, n: int, frame: torch.Tensor) -> None:
        if self._frame_bytes == 0:
            # First cached frame: learn the real per-frame byte size for the P3 byte-budget
            # clamp, and log the resulting cap once (also fires the clamp warning here if the
            # budget already binds at this resolution).
            self._frame_bytes = int(frame.nelement()) * int(frame.element_size())
            logger.info(
                "scheduler: frame_cache frame=%d B (%dx%d) -> cap=%d frames (budget %d MiB)",
                self._frame_bytes, frame.shape[1], frame.shape[0],
                self._frame_cache_cap(), _FRAME_CACHE_BUDGET_BYTES >> 20,
            )
        self.frame_cache[n] = frame
        # Drop frames already behind the present head (no clip can still need them for blend).
        head = 0
        try:
            head = int(self.player.current_frame())
        except Exception:  # noqa: BLE001
            pass
        while self.frame_cache:
            oldest = next(iter(self.frame_cache))
            if oldest >= head:
                break
            self.frame_cache.popitem(last=False)
        # Bound pending + cache to lead span (sparse crops are cheap; full BGR is not).
        while self.pending_regions:
            oldest = next(iter(self.pending_regions))
            if oldest >= head:
                break
            self.pending_regions.popitem(last=False)
        cap = self._frame_cache_cap()
        while len(self.frame_cache) > cap:
            evicted_n, _ = self.frame_cache.popitem(last=False)
            self.pending_regions.pop(evicted_n, None)
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
