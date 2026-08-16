# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# TranscodeEngine: the headless decensor->encode pipeline shared by the web-streaming server and
# the offline-export job. Wiring:
#
#   sumu_core.HeadlessDecode (d3d11va -> CUDA NV12, sequential, windowless)
#     -> DecensorProcessor (YOLO + BasicVSR + blend -> final BGR, in order)
#     -> NvencEncoder (ffmpeg h264_nvenc -> HLS / MP4)
#
# `run()` is blocking and is meant to be driven from a dedicated daemon thread (the HTTP server /
# export job each own one). Progress is reported via an optional callback(frame_num, frame_count);
# cancellation via `cancel()` (checked between frames and clips).
from __future__ import annotations

import threading
from fractions import Fraction

import sumu_core

from .decensor import DecensorProcessor
from .encoder import EncodeOptions, NvencEncoder


class TranscodeError(RuntimeError):
    """Transcode failed (decode / AI / encode) or was cancelled."""


class TranscodeEngine:
    """One reusable engine instance bound to loaded AI models + a scheduler config.

    A single engine runs one transcode at a time (run() is not re-entrant); build one engine per
    concurrent session if you need parallelism (the server queues sessions instead).
    """

    def __init__(self, det_model, res_model, pad_mode: str, config, video_meta=None):
        self.det_model = det_model
        self.res_model = res_model
        self.pad_mode = pad_mode
        self.config = config
        self.video_meta = video_meta
        self._cancel = threading.Event()
        # Serializes run(): the engine holds ONE shared AI model set and ONE GPU pipeline, so two
        # concurrent run() calls (web-streaming vs offline-export started together) would corrupt
        # each other's cancel state and share non-reentrant model state. This is a *conflict*
        # guard (state safety), NOT a throttle: the second caller simply blocks until the first
        # finishes. Mere GPU contention is deliberately left for the machine to handle.
        self._run_lock = threading.Lock()
        self.last_stats = None

    def cancel(self) -> None:
        self._cancel.set()

    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def run(self, source: str, out: str, mode: str, *, bitrate: str = "8M",
            encode=None, quality_first: bool = False, config=None,
            video_meta=None, audio_source: str | None = None,
            progress_cb=None, start_seconds: float = 0.0) -> int:
        """Transcode `source` (local path or http(s) URL) to HLS (`mode="hls"`, dir `out`) or
        MP4 (`mode="mp4"`, file `out`). Returns the number of frames emitted. Raises
        TranscodeError on failure/cancel.

        `start_seconds` re-anchors decode at that offset (I6 reposition: HeadlessDecode
        seek_to_frame) before transcoding to EOF -- the AI streaming server uses this for seek.

        `encode` is an EncodeOptions (offline export passes HEVC/CQ/p7 quality-first; None falls
        back to the live-streaming h264 profile built from `bitrate`). `quality_first` appends the
        always-on export quality flags (tune hq / AQ / B-frame refs / full lookahead). `config`
        overrides the engine's scheduler config for this run (export uses a longer clip_length +
        unlimited per-frame regions).

        Non-reentrant (see _run_lock): the second concurrent caller blocks here until the first
        run finishes, instead of corrupting shared model/cancel state."""
        if encode is None:
            encode = EncodeOptions(codec="h264", rate_mode="vbr", bitrate=bitrate,
                                   preset="p4", audio_copy=False, subtitle=False)
        with self._run_lock:
            return self._run_locked(source, out, mode, encode=encode,
                                    quality_first=quality_first, config=config,
                                    video_meta=video_meta, audio_source=audio_source,
                                    progress_cb=progress_cb, start_seconds=start_seconds)

    def _run_locked(self, source: str, out: str, mode: str, *, encode, quality_first: bool,
                    config, video_meta=None, audio_source: str | None = None,
                    progress_cb=None, start_seconds: float = 0.0) -> int:
        self._cancel.clear()
        cfg = config or self.config
        dec = sumu_core.HeadlessDecode()
        try:
            dec.open(source)
            w, h = dec.width(), dec.height()
            fps = float(dec.fps())
            fc = int(dec.frame_count()) if dec.frame_count() > 0 else 0

            # Re-anchor decode at start_seconds (seek = reposition). The processor's own frame
            # numbering is a fresh 0-based local counter (contiguity is all the scene/clip math
            # needs), so we only use the landed frame to derive the remaining-frame count for
            # progress reporting.
            start_frame = 0
            if start_seconds > 0.0:
                start_frame = int(dec.seek_to_frame(int(round(start_seconds * fps))))

            meta = video_meta or self.video_meta or self._build_meta(source, dec)
            proc = DecensorProcessor(self.det_model, self.res_model, self.pad_mode, meta, cfg)
            enc = NvencEncoder(w, h, fps, mode, out, encode=encode, quality_first=quality_first,
                               audio_source=audio_source if audio_source is not None else source,
                               start_seconds=start_seconds,
                               bt709=cfg.bt709, full_range=cfg.full_range)
            remaining = max(0, fc - start_frame) if fc > 0 else 0

            def _emit(proc, enc, remaining):
                for fnum, final_bgr in proc.emit():
                    if self._cancel.is_set():
                        return False
                    # final_bgr stays CUDA-resident; write_frame does the BGR->YUV420 conversion
                    # on the GPU and the single D2H copy + pipe write (no numpy round-trip here).
                    enc.write_frame(final_bgr)
                    if progress_cb is not None:
                        progress_cb(fnum, remaining)
                return True

            ingest_n = 0
            while True:
                if self._cancel.is_set():
                    enc.abort()
                    raise TranscodeError("transcode cancelled")
                g = dec.next_frame()
                if g["eof"]:
                    break
                proc.ingest(ingest_n, g["dev_ptr"], g["width"], g["height"], g["pitch_bytes"])
                ingest_n += 1
                if not _emit(proc, enc, remaining):
                    enc.abort()
                    raise TranscodeError("transcode cancelled")

            # EOF: drain the tail.
            proc.flush_eof()
            _emit(proc, enc, remaining)

            rc, err = enc.finish()
            if rc != 0:
                raise TranscodeError(f"ffmpeg exited rc={rc}:\n{err[-2000:]}")
            self.last_stats = {
                "frames_emitted": proc.frames_emitted,
                "frames_ingested": proc.frames_ingested,
                "clips_restored": proc.clips_restored,
                "restore_frames": proc.restore_frames,
                "restore_seconds": proc.restore_seconds,
                "restore_fps": proc.restore_fps,
            }
            return proc.frames_emitted
        except TranscodeError:
            raise
        except Exception as e:  # noqa: BLE001 -- wrap decode/AI/encode failures uniformly
            raise TranscodeError(f"transcode failed: {e!r}") from e
        finally:
            dec.close()

    # ---- helpers ------------------------------------------------------------------------

    @staticmethod
    def _build_meta(source, dec):
        """Build VideoMetadata from the open decoder for network sources (no second ffprobe open);
        local files use ffprobe (matches the scheduler's get_video_meta_data path)."""
        from sumu.ai.utils import VideoMetadata
        from sumu.ai.utils.video_utils import get_video_meta_data
        if dec.is_network():
            w, h = dec.width(), dec.height()
            fps = float(dec.fps())
            fc = int(dec.frame_count()) if dec.frame_count() > 0 else 0
            fps_exact = Fraction(fps).limit_denominator(1001)
            dur = (fc / fps) if fps > 0 and fc > 0 else 0.0
            return VideoMetadata(
                video_file=source, video_height=h, video_width=w, video_fps=fps,
                average_fps=fps, video_fps_exact=fps_exact, codec_name="unknown",
                frames_count=fc, duration=float(dur),
                time_base=Fraction(1, max(1, int(round(fps * 1001)))) if fps > 0 else Fraction(1, 30),
                start_pts=0,
            )
        return get_video_meta_data(source)
