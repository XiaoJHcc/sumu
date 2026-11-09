# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Headless check of app.py's partial-file tolerance: drives the real app.main() with a wrapper
# Player that injects UI intents (seeks) and records what the app did with them. Only the
# GPU/model/window/pipe edges are stubbed (sumu_core, torch, build_models, sumu.scheduler,
# sumu.webstream, single_instance); app.py, i18n, settings and VideoMetadata stay real, and
# settings.json is redirected to a temp APPDATA.
#
# Run: .venv\Scripts\python.exe scripts\verify_app_partial.py
# Native (real sumu_core.Player on a truncated file, opens a window):
#      .venv\Scripts\python.exe scripts\verify_app_partial.py --native scripts\trace\partial_tmp\half.download
import argparse
import os
import queue
import shutil
import sys
import tempfile
import types

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))
sys.path.insert(0, os.path.join(_REPO, "python", "sumu"))  # native module sumu_core.*.pyd

failures: list[str] = []


def check(name: str, condition, detail: str = "") -> bool:
    tail = f" ({detail})" if detail and not condition else ""
    print(f"[{'PASS' if condition else 'FAIL'}] {name}{tail}")
    if not condition:
        failures.append(name)
    return bool(condition)


class _Rec:
    """Per-scenario record shared by the scripted plan, the stubs and the asserts."""

    def __init__(self, plan, quit_at: int, make_inner):
        self.plan = plan              # (rec, inner_player, tick) -> {intent: value} | None
        self.quit_at = quit_at        # loop exits once tick >= quit_at
        self.make_inner = make_inner
        self.tick = 0
        self.injected: list[tuple[int, dict]] = []
        self.status_texts: list[str] = []
        self.sched_built = 0
        self.sched_meta: list = []
        self.sched_events: list[tuple] = []   # ("start"|"stop", tick) / ("seek", frame, tick)
        self.sched_start_raises = False
        self.notify_raises = False
        self.probe_called = False             # a second ffprobe/cv2 open must never happen
        self.native = None                    # (frame_count, fps, dims) seen once AI came up
        self.last_playing = None
        self.fail_seek_tick = None
        self.ok_seek_tick = None
        self.head_at_fail = None


def _player_factory(width, height, maximized):
    rec = _STATE["rec"]
    return _Wrapper(rec.make_inner(width, height, maximized), rec)


class _Wrapper:
    """The Player app.main() sees: forwards to the inner player, injects the scenario's intents
    once per tick, and records the status float + the app's tick count."""

    def __init__(self, inner, rec):
        self.inner = inner
        self.rec = rec

    def __getattr__(self, name):
        return getattr(self.inner, name)

    def should_quit(self):
        return self.rec.tick >= self.rec.quit_at

    def take_ui_intents(self):
        rec = self.rec
        rec.tick += 1
        intents = dict(self.inner.take_ui_intents())
        try:
            rec.last_playing = bool(self.inner.is_playing())
        except Exception:  # noqa: BLE001 -- diagnostics only
            pass
        injected = rec.plan(rec, self.inner, rec.tick) or {}
        if injected:
            rec.injected.append((rec.tick, dict(injected)))
            intents.update(injected)
        return intents

    def set_status_text(self, text):
        self.rec.status_texts.append(str(text))
        return self.inner.set_status_text(text)


# Keys of Player::take_ui_intents (player_session.cpp); all-None/falsy == "no intent".
_INTENT_KEYS = (
    "seek", "toggle_play", "clip_length", "max_regions", "cold_start_s", "lead", "target_fps",
    "open_path", "open_dialog", "close_request", "compile_engine", "stream_start", "stream_port",
    "stream_root", "stream_no_token", "stream_token", "stream_stop", "export_enter", "export_exit",
    "export_add_files", "export_start", "export_pick_global", "export_pick_custom", "export_remove",
    "export_cancel", "export_move_id", "export_move_to", "export_item_preset_id",
    "export_item_preset_idx", "export_item_out_id", "export_item_out_mode", "export_clip_length",
    "export_preset_save", "export_preset_delete", "export_preset_edit_idx", "export_preset_name",
    "export_preset_codec", "export_preset_cq_enabled", "export_preset_cq",
    "export_preset_vbr_enabled",
    "export_preset_bitrate", "export_preset_maxrate", "export_preset_quality",
    "export_preset_audio_copy", "export_preset_audio_bitrate", "export_preset_subtitle",
    "export_preset_suffix", "export_set_default", "export_drop_paths",
)


class _FakeNative:
    """sumu_core.Player stand-in: a container whose tail (frames >= playable) is undecodable --
    the half-downloaded file native seek() throws on -- while frame_count() still reports the
    full container count."""

    def __init__(self, width=1280, height=720, frames=500, playable=300, fps=30.0):
        self.width, self.height = width, height
        self.frames, self.playable, self.src_fps = frames, playable, fps
        self.head = 0
        self.opened = False
        self.playing = False
        self.div = 1

    def open(self, path):
        self.opened, self.head = True, 0

    def reopen(self, path):
        self.close_current_session()
        self.open(path)

    def close_current_session(self):
        self.opened = False
        self.playing = False

    def play(self):
        self.playing = True

    def pause(self):
        self.playing = False

    def is_playing(self):
        return self.playing

    def seek(self, frame_num):
        n = max(0, min(int(frame_num), self.frames - 1))
        if n >= self.playable:
            raise RuntimeError(
                "seek_to_frame failed: seek_to_frame: no frame could be decoded after seek")
        self.head = n
        # Native returns the frame it really landed on, which off a GOP boundary is not the
        # requested one -- the app must notify the scheduler with THIS value.
        return n - 1 if n > 0 else 0

    def current_frame(self):
        return self.head

    def frame_count(self):
        return self.frames

    def fps(self):
        return self.src_fps / self.div

    def source_fps(self):
        return self.src_fps

    def set_fps_div(self, div):
        self.div = max(1, int(div))

    def fps_div(self):
        return self.div

    def dims(self):
        return (self.width, self.height)

    def is_network(self):
        return False

    def decode_ahead_max(self):
        return 48

    def ring_capacity(self):
        return 64

    def take_ui_intents(self):
        return {key: None for key in _INTENT_KEYS}

    def __getattr__(self, name):
        # Anything else app.main() pokes (pump_messages / ui_tick / set_* / pick_*) is a no-op.
        return lambda *a, **k: None


class _FakeSchedulerConfig:
    def __init__(self, **kwargs):
        self.kwargs = kwargs


def _make_scheduler_cls(rec):
    """Scheduler stand-in that records the control calls app.main() makes on it."""

    class _FakeScheduler:
        def __init__(self, player, det_model, res_model, pad_mode, video_meta_data, config=None):
            rec.sched_built += 1
            rec.sched_meta.append(video_meta_data)

        def start(self):
            rec.sched_events.append(("start", rec.tick))
            if rec.sched_start_raises:
                raise RuntimeError("simulated scheduler start failure")

        def stop(self):
            rec.sched_events.append(("stop", rec.tick))

        def notify_seek(self, frame_num):
            if rec.notify_raises:
                raise RuntimeError("simulated notify_seek failure")
            rec.sched_events.append(("seek", int(frame_num), rec.tick))

        def get_stats(self):
            return {}

    return _FakeScheduler


def _module(name, **attrs):
    mod = types.ModuleType(name)
    for key, value in attrs.items():
        setattr(mod, key, value)
    sys.modules[name] = mod
    return mod


_STATE = {"rec": None}


def _install_stubs() -> None:
    import sumu.pipeline

    _module("sumu_core", Player=_player_factory)

    # torch: never imported for real (no models are built, no CUDA is touched).
    class _Device:
        def __init__(self, spec):
            self.type = "cuda" if str(spec).startswith("cuda") else "cpu"

    _module("torch", __version__="0.0.0-stub", device=_Device,
            cuda=types.SimpleNamespace(is_available=lambda: False,
                                       get_device_name=lambda idx=0: "stub"))

    sumu.pipeline.build_models = (
        lambda device, fp16, allow_trt_compile=True: (object(), object(), "bilateral"))
    sumu.pipeline.default_restoration_model_path = lambda: "<stub>/basicvsrpp-v1.2.pth"

    _module("sumu.scheduler", Scheduler=_make_scheduler_cls(_Rec(None, 0, None)),
            SchedulerConfig=_FakeSchedulerConfig)
    _module("sumu.ai.restorationpipeline", BASICVSRPP_TRT_MAX_CLIP_SIZE=180)
    _module("sumu.ai.restorationpipeline.trt_engine_paths",
            basicvsrpp_sub_engines_present_fast=lambda *a, **k: False)

    def _forbidden_probe(path):
        _STATE["rec"].probe_called = True
        raise AssertionError("app.py opened the file a second time for video meta")

    _module("sumu.ai.utils.video_utils", get_video_meta_data=_forbidden_probe)

    class _ExportQueue:
        def __init__(self, engine, runner):
            self.items = []
            self.engine = engine

        def load_persisted(self, *a, **k):
            pass

        def snapshot(self):
            return {}

        def to_persist(self):
            return []

        def cancel_all(self):
            pass

    class _TranscodeEngine:
        def __init__(self, *a, **k):
            pass

    _module("sumu.webstream", ExportQueue=_ExportQueue, TranscodeEngine=_TranscodeEngine)

    import sumu.single_instance

    class _NoPipeListener:
        def __init__(self):
            self.incoming = queue.Queue()

        def start(self):
            pass

        def stop(self):
            pass

    sumu.single_instance.try_forward_to_running = lambda path: False
    sumu.single_instance.PipeListener = _NoPipeListener


def _plan_seek_partial(rec, native, tick):
    """Wait for the AI scheduler, seek past the decodable end, then back into the playable
    region, then let the loop run a few more ticks and exit."""
    if rec.sched_built == 0:
        return None
    if rec.native is None:
        rec.native = (int(native.frame_count()), float(native.fps()), tuple(native.dims()))
    frames = int(native.frame_count())
    if rec.fail_seek_tick is None:
        rec.fail_seek_tick = tick
        rec.head_at_fail = int(native.current_frame())
        return {"seek": frames - 1}           # container last frame: undecodable in a partial file
    if rec.ok_seek_tick is None and tick >= rec.fail_seek_tick + 3:
        rec.ok_seek_tick = tick
        return {"seek": max(1, frames // 5)}  # back into the region that is actually on disk
    if rec.ok_seek_tick is not None and tick >= rec.ok_seek_tick + 5:
        rec.quit_at = tick
    return None


def _plan_seek_after_ai(rec, native, tick):
    """One successful seek, then a second one two ticks later: both must survive a scheduler
    whose notify_seek() throws."""
    if rec.sched_built == 0:
        return None
    if rec.fail_seek_tick is None:
        rec.fail_seek_tick = tick
        return {"seek": 40}
    if rec.ok_seek_tick is None and tick >= rec.fail_seek_tick + 2:
        rec.ok_seek_tick = tick
        return {"seek": 80}
    if rec.ok_seek_tick is not None and tick >= rec.ok_seek_tick + 5:
        rec.quit_at = tick
    return None


def _sched_seeks(rec):
    return [e[1] for e in rec.sched_events if e[0] == "seek"]


def _run_app(video_path: str):
    """Drive the real app.main() once. Returns None if it returned, else its exception."""
    import sumu.app

    saved_argv = sys.argv
    sys.argv = ["sumu-app-verify", video_path]
    try:
        sumu.app.main()
        return None
    except BaseException as e:  # noqa: BLE001 -- reported as a check failure, not a crash
        return e
    finally:
        sys.argv = saved_argv


def _activate(rec: _Rec) -> None:
    _STATE["rec"] = rec
    sys.modules["sumu.scheduler"].Scheduler = _make_scheduler_cls(rec)


def scenario_seek_partial(video_path: str, make_inner, label: str) -> None:
    import sumu.i18n as i18n_mod

    rec = _Rec(_plan_seek_partial, quit_at=200, make_inner=make_inner)
    _activate(rec)
    err = _run_app(video_path)

    hint = i18n_mod.t("seek_unavailable_partial")
    seeks = rec.injected
    fail_target = seeks[0][1]["seek"] if seeks else None
    ok_target = seeks[1][1]["seek"] if len(seeks) > 1 else None
    sched_frames = _sched_seeks(rec)

    check(f"{label}: app.main() returned instead of raising", err is None, repr(err))
    check(f"{label}: hint key is localized", hint != "seek_unavailable_partial", hint)
    check(f"{label}: scenario ran ({rec.tick} ticks)", rec.tick >= 10)
    check(f"{label}: two seeks injected (fail={fail_target}, recovery={ok_target})",
          fail_target is not None and ok_target is not None)
    check(f"{label}: seek hint shown on the status float",
          hint in rec.status_texts, str(rec.status_texts[-4:]))
    check(f"{label}: loop kept ticking after the failing seek "
          f"(fail tick={rec.fail_seek_tick}, quit tick={rec.tick})",
          rec.fail_seek_tick is not None and rec.tick >= rec.fail_seek_tick + 5)
    check(f"{label}: playback still running at the end", rec.last_playing is True)
    check(f"{label}: scheduler never told about the undecodable target {fail_target}",
          fail_target not in sched_frames, str(sched_frames))
    check(f"{label}: scheduler re-anchored after the failed seek (seeks={sched_frames})",
          len(sched_frames) >= 2)
    if len(sched_frames) >= 2:
        anchor, landed = sched_frames[0], sched_frames[1]
        check(f"{label}: re-anchor sits at the player's real frame "
              f"(anchor={anchor}, head={rec.head_at_fail})",
              abs(anchor - int(rec.head_at_fail)) <= 60)
        check(f"{label}: seek back into the playable region notified the scheduler "
              f"(landed={landed}, target={ok_target})",
              ok_target is not None and abs(landed - int(ok_target)) <= 5)
        if label == "fake":
            # The fake lands one frame before the target: proves the app forwards the frame
            # seek() reported instead of the requested one.
            check(f"{label}: notify_seek used the landed frame, not the target",
                  ok_target is not None and landed == int(ok_target) - 1)

    meta = rec.sched_meta[0] if rec.sched_meta else None
    if not check(f"{label}: scheduler built with video_meta", meta is not None):
        return
    frames_n, fps_n, dims_n = rec.native or (None, None, None)
    check(f"{label}: video_meta.frames_count from native ({meta.frames_count})",
          meta.frames_count == frames_n)
    check(f"{label}: video_meta dims from native (got {meta.video_width}x{meta.video_height})",
          (meta.video_width, meta.video_height) == dims_n, str(dims_n))
    check(f"{label}: video_meta fps from native ({meta.video_fps})",
          fps_n is not None and abs(meta.video_fps - fps_n) < 1e-6, str(fps_n))
    check(f"{label}: video_meta carries the played file", meta.video_file == video_path)
    check(f"{label}: no second ffprobe/cv2 open of the file", not rec.probe_called)


def scenario_scheduler_fail(video_path: str, make_inner, label: str) -> None:
    rec = _Rec(lambda r, n, t: None, quit_at=25, make_inner=make_inner)
    rec.sched_start_raises = True
    _activate(rec)
    err = _run_app(video_path)

    check(f"{label}: app.main() returned instead of raising", err is None, repr(err))
    check(f"{label}: app survived a failing Scheduler.start() ({rec.tick} ticks)",
          rec.tick >= rec.quit_at)
    check(f"{label}: build attempted exactly once, not per tick ({rec.sched_built})",
          rec.sched_built == 1)
    check(f"{label}: partially started scheduler was stopped before being dropped "
          f"(events={rec.sched_events})",
          any(e[0] == "stop" for e in rec.sched_events))
    check(f"{label}: playback kept running on passthrough", rec.last_playing is True)


def scenario_notify_seek_fail(video_path: str, make_inner, label: str) -> None:
    rec = _Rec(_plan_seek_after_ai, quit_at=60, make_inner=make_inner)
    rec.notify_raises = True
    _activate(rec)
    err = _run_app(video_path)

    check(f"{label}: app.main() returned instead of raising", err is None, repr(err))
    check(f"{label}: both seeks were issued despite notify_seek failing "
          f"({[i[1] for i in rec.injected]})", len(rec.injected) == 2)
    check(f"{label}: loop kept ticking ({rec.tick} ticks)", rec.tick >= 10)
    check(f"{label}: scheduler was stopped and dropped (events={rec.sched_events})",
          any(e[0] == "stop" for e in rec.sched_events))
    check(f"{label}: latched, so no rebuild after the failure ({rec.sched_built} builds)",
          rec.sched_built == 1)
    check(f"{label}: playback kept running on passthrough", rec.last_playing is True)


def main() -> int:
    ap = argparse.ArgumentParser(description="app.py partial-file tolerance checks")
    ap.add_argument("--native", metavar="PARTIAL_VIDEO",
                    help="use the real sumu_core.Player on this truncated file (opens a window)")
    args = ap.parse_args()

    # Isolated settings.json: app.main() saves on open/exit, the user's real
    # %APPDATA%/sumu/settings.json must not be touched by a verification run.
    tmp_root = tempfile.mkdtemp(prefix="sumu-verify-")
    os.environ["APPDATA"] = os.path.join(tmp_root, "appdata")
    os.makedirs(os.environ["APPDATA"], exist_ok=True)
    video_path = os.path.join(tmp_root, "partial.mp4")
    with open(video_path, "wb") as fh:      # the fake container only needs a path to report
        fh.write(b"\0" * 1024)

    real_cls = None
    if args.native:
        video_path = os.path.abspath(args.native)
        if not os.path.isfile(video_path):
            print(f"no such file: {video_path}")
            shutil.rmtree(tmp_root, ignore_errors=True)
            return 2
        import sumu_core
        real_cls = sumu_core.Player

    _install_stubs()  # replaces sys.modules["sumu_core"] either way; real_cls stays reachable

    try:
        if args.native:
            print(f"== native mode == {video_path}")
            scenario_seek_partial(video_path, lambda w, h, m: real_cls(w, h, m), "native")
        else:
            print("== fake-player mode == (no GPU, no window)")
            scenario_seek_partial(video_path, lambda w, h, m: _FakeNative(), "fake")
            scenario_scheduler_fail(video_path, lambda w, h, m: _FakeNative(), "fake")
            scenario_notify_seek_fail(video_path, lambda w, h, m: _FakeNative(), "fake")
    finally:
        os.environ.pop("APPDATA", None)
        shutil.rmtree(tmp_root, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} check(s) failed:")
        for name in failures:
            print(f"  - {name}")
        return 1
    print("all app partial-file checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
