# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# decoder.cpp 可恢复媒体数据日志限频：first: 示例 + summary: 计数，其余日志与 Decoder 作用域外
# 的消息原样转发。每个场景跑独立子进程，父进程按 fd 抓 stdout/stderr 数行数。
#     .venv\Scripts\python.exe scripts\verify_decoder_logging.py              (真实 GPU，需先 build)
#     .venv\Scripts\python.exe scripts\verify_decoder_logging.py --dry-run    (合成素材 + avutil 探针)
#     .venv\Scripts\python.exe scripts\verify_decoder_logging.py --self-test  (用合成 stderr 校验判据)
import argparse
import ctypes
import faulthandler
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"

RECOVER = "[sumu] recoverable media data:"
FIRST = "first:"
SUMMARY = "summary:"
COUNTS = re.compile(r"ffmpeg_messages=(\d+)\s+packet_errors=(\d+)")

STDERR_LINE_BUDGET = 100    # 不限频时洞里是上千行原始坏包日志
MIN_AGGREGATED = 100        # 折叠掉的条数下限，必须远多于打印出的行数
SUMMARY_CAP = 6             # ~10s 播放：5s 节拍 + 终结摘要 + 可能的第二个 Decoder
CHILD_TIMEOUT = 300

FLOOD_FPS, FLOOD_S = 120, 12.0
FLOOD_HOLE = (0.18, 0.90)
CLEAN_S = 5.0

AV_LOG_FATAL, AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_INFO = 8, 16, 24, 32
AV_LOG_VERBOSE, AV_LOG_DEBUG, AV_LOG_TRACE = 40, 48, 56

# 作用域外必须原样可见的哨兵；文案不能含 '%'（av_log 是 printf 风格，且不给可变参数）。
SENTINEL_BODIES = (
    (AV_LOG_FATAL, "unknown fatal from a plain caller"),
    (AV_LOG_ERROR, "unknown error from a plain caller"),
    (AV_LOG_ERROR, "Invalid NAL unit size (9123 > 15)"),
    (AV_LOG_ERROR, "error while decoding MB 12 34, bytestream -5"),
    (AV_LOG_ERROR, "Packet corrupt (stream = 0, dts = 2048)"),
    (AV_LOG_WARNING, "concealing 100 DC, 100 AC, 100 MV errors in I frame"),
    (AV_LOG_INFO, "unknown info from a plain caller"),
    (AV_LOG_VERBOSE, "unknown verbose from a plain caller"),
    (AV_LOG_DEBUG, "unknown debug from a plain caller"),
    (AV_LOG_TRACE, "unknown trace from a plain caller"),
)


def sentinels(scope):
    return [(f"sumu-sentinel-{scope}-{i}", level, body)
            for i, (level, body) in enumerate(SENTINEL_BODIES)]


class Checks:
    def __init__(self, quiet=False):
        self.items = []
        self.quiet = quiet

    def check(self, name, ok, detail=""):
        ok = bool(ok)
        self.items.append((name, ok, detail))
        if not self.quiet:
            if ok:
                print(f"[PASS] {name}", flush=True)
            else:
                print(f"[FAIL] {name}" + (f" ({detail})" if detail else ""), flush=True)
        return ok

    def brief(self, text):
        if not self.quiet:
            print(f"       {text}", flush=True)

    @property
    def failures(self):
        return [item for item in self.items if not item[1]]


# --- 素材 ---------------------------------------------------------------

def build_media(tmp):
    sys.path.insert(0, str(SCRIPTS))
    import verify_partial_media as vpm

    clean = vpm.Media._encode(tmp / "clean.mp4", 30, CLEAN_S)
    flood_src = vpm.Media._encode(tmp / "flood.mp4", FLOOD_FPS, FLOOD_S)
    flood = vpm._zeroed(flood_src, FLOOD_HOLE)      # 洞在 moov 之后，容器仍可读

    # 不给 -movflags +faststart：moov 落在尾部，切一半后必然打不开。
    tail = tmp / "tail.mp4"
    vpm.ffmpeg("-i", clean, "-c", "copy", tail)
    no_moov = tmp / "no-moov.mp4.part"
    no_moov.write_bytes(tail.read_bytes()[: tail.stat().st_size // 2])

    media = {
        "tmp": str(tmp),
        "clean": str(clean),
        "flood": str(flood),
        "flood_fps": FLOOD_FPS,
        "flood_hole": list(FLOOD_HOLE),
        "no_moov": str(no_moov),
        "info": {
            "clean": [vpm.Media.declared(clean, 30), vpm.Media.decodable(clean)],
            "flood": [vpm.Media.declared(flood, FLOOD_FPS), vpm.Media.decodable(flood)],
            "no_moov_readable": vpm.Media.info(no_moov) is not None,
        },
    }
    (tmp / "media.json").write_text(json.dumps(media, indent=2), encoding="utf-8")
    return media


def check_media(checks, media):
    info = media["info"]
    declared, decodable = info["clean"]
    checks.check("media: clean clip decodes completely", declared - 2 <= decodable <= declared + 2,
                 f"{decodable}/{declared}")
    declared, decodable = info["flood"]
    lo, hi = int(declared * FLOOD_HOLE[0]), int(declared * FLOOD_HOLE[1])
    checks.check("media: flood clip really loses its hole", declared - decodable >= MIN_AGGREGATED,
                 f"{declared - decodable} of {declared} frames undecodable")
    checks.check("media: flood hole spans enough frames", hi - lo >= MIN_AGGREGATED,
                 f"window {lo}-{hi}")
    checks.check("media: no-moov clip is unreadable", info["no_moov_readable"] is False)


# --- avutil ------------------------------------------------------------

def load_avutil():
    """优先按基名命中 sumu_core 已载入的那一份，保证是同一个模块。"""
    names = sorted(p.name for p in (ROOT / "python" / "sumu").glob("avutil-*.dll"))
    if not names:
        raise RuntimeError("no avutil-*.dll next to sumu_core")
    errors = []
    for spec in names[:1] + [str(ROOT / "python" / "sumu" / name) for name in names]:
        try:
            lib = ctypes.CDLL(spec)
        except OSError as exc:      # 基名找不到就换绝对路径
            errors.append(f"{spec}: {exc}")
            continue
        if not hasattr(lib, "av_log") or not hasattr(lib, "av_log_get_level"):
            errors.append(f"{spec}: no av_log/av_log_get_level export")
            continue
        lib.av_log.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        lib.av_log.restype = None
        lib.av_log_get_level.restype = ctypes.c_int
        lib.av_log_set_level.argtypes = [ctypes.c_int]
        lib.av_log_set_level.restype = None
        return lib, spec, _module_path(lib._handle)
    raise RuntimeError("cannot load avutil: " + "; ".join(errors))


def _module_path(handle):
    try:
        kernel32 = ctypes.WinDLL("kernel32.dll")
        buf = ctypes.create_unicode_buffer(1024)
        if kernel32.GetModuleFileNameW(ctypes.c_void_p(handle), buf, 1024):
            return buf.value
    except (OSError, AttributeError):
        pass
    return ""


def loaded_from_sumu(path):
    return bool(path) and os.path.normcase(path).startswith(
        os.path.normcase(str((ROOT / "python" / "sumu")) + os.sep))


# --- 子进程场景 ---------------------------------------------------------

def _emit(payload):
    print("RESULT " + json.dumps(payload), flush=True)


def _player_imports():
    sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu"), str(SCRIPTS)]
    import verify_partial_media as vpm
    import sumu_core
    return vpm, sumu_core


def child_flood(media):
    vpm, sumu_core = _player_imports()
    path, fps, hole = Path(media["flood"]), media["flood_fps"], media["flood_hole"]
    result = {"case": "flood"}
    player = sumu_core.Player(640, 400)
    try:
        player.open(str(path))
        play = vpm.Play(player, "flood")
        lo, hi = int(play.declared * hole[0]), int(play.declared * hole[1])
        landed = player.seek(max(0, lo - 25))       # 跳过洞前内容，压短墙钟
        assert landed <= lo, (landed, lo)
        player.play()
        trace = vpm.Trace(player, Path(media["tmp"]) / "present.csv")
        trace.delta()
        started = time.monotonic()
        assert play.until(lambda: play.frame > hi + 8, (hi + 8 - landed) / fps + 25), \
            f"clock died inside the hole ({play.frame}/{play.declared})"
        assert player.stats()["is_playing"], f"paused inside the hole at {play.frame}"
        span = vpm.held_span(trace.delta(), lo, hi)
        assert vpm.wait_fresh(play, trace, hi, timeout=10.0), "no fresh frame after the hole"
        frame = play.wait_end((play.declared - play.frame) / fps + 10)
        assert frame >= play.declared - 2, (frame, play.declared)
        result.update(span=[span[0], span[1]], fresh=True, end=frame,
                      declared=play.declared, seconds=round(time.monotonic() - started, 2))
    finally:
        player.close_current_session()              # Decoder 析构 -> 终结摘要
        player.close()
    _emit(result)


def child_clean(media):
    vpm, sumu_core = _player_imports()
    result = {"case": "clean"}
    player = sumu_core.Player(640, 400)
    try:
        player.open(media["clean"])
        play = vpm.Play(player, "clean")
        player.play()
        play.run(1.5)
        result.update(frame=play.frame, declared=play.declared,
                      playing=bool(player.stats()["is_playing"]))
        assert play.frame > 0, play.frame
    finally:
        player.close_current_session()
        player.close()
    _emit(result)


def child_sentinel(media):
    _, sumu_core = _player_imports()
    lib, spec, dll_path = load_avutil()
    fired = []

    def fire(tag, level, body, emit_level):
        lib.av_log(None, level, f"{tag}: {body}\n".encode("utf-8"))
        fired.append([tag, level, emit_level, body])

    level0 = lib.av_log_get_level()
    fire(*sentinels("before")[0], level0)           # 还没有任何 Decoder：callback 未安装
    player = sumu_core.Player(640, 400)
    try:
        player.open(media["clean"])                 # 建出 Decoder，全局 callback 装好
        for _ in range(20):                         # 让解码线程填满起播缓冲后静止
            player.pump_messages()
            player.ui_tick()
            time.sleep(0.01)
        lib.av_log_set_level(AV_LOG_DEBUG)          # 抬到 DEBUG：更啰嗦的 level 不能被丢
        for item in sentinels("main"):
            fire(*item, AV_LOG_DEBUG)
        thread = threading.Thread(target=lambda: [fire(*item, AV_LOG_DEBUG)
                                                  for item in sentinels("thread")])
        thread.start()
        thread.join()
        player.close_current_session()              # Decoder 已销毁，callback 仍应在
        lib.av_log_set_level(level0)
        fire("sumu-sentinel-after-close", AV_LOG_ERROR,
             "unknown error after the decoder was closed", level0)
    finally:
        lib.av_log_set_level(level0)
        player.close()
    _emit({"case": "sentinel", "avutil": spec, "avutil_path": dll_path,
           "level0": level0, "sentinels": fired})


def child_no_moov(media):
    _, sumu_core = _player_imports()
    raised, message = False, ""
    player = sumu_core.Player(640, 400)
    try:
        try:
            player.open(media["no_moov"])
        except RuntimeError as exc:
            raised, message = True, str(exc)
        for _ in range(10):                         # 让 present 吃掉这次失败的 open
            player.pump_messages()
            player.ui_tick()
            time.sleep(0.01)
    finally:
        player.close()
    _emit({"case": "no_moov", "raised": raised, "message": message})
    assert raised, "opening a clip without moov did not raise"


CHILDREN = {"flood": child_flood, "clean": child_clean,
            "sentinel": child_sentinel, "no_moov": child_no_moov}


def run_child(case, tmp):
    cmd = [sys.executable, str(Path(__file__).resolve()), "--child", case, "--tmp", str(tmp)]
    run = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, timeout=CHILD_TIMEOUT)
    return (run.returncode,
            run.stdout.decode("utf-8", "replace"),
            run.stderr.decode("utf-8", "replace"))


def parse_result(stdout):
    for line in reversed(stdout.splitlines()):
        if line.startswith("RESULT "):
            try:
                return json.loads(line[len("RESULT "):])
            except ValueError:
                return None
    return None


# --- 判据 ---------------------------------------------------------------

def _stderr_lines(stderr):
    return [line for line in stderr.splitlines() if line.strip()]


def _recover_lines(stderr):
    return [line for line in stderr.splitlines() if RECOVER in line]


def _tail(stderr, n=12):
    return " | ".join(_stderr_lines(stderr)[-n:])[:1200]


def _leak_profile(lines, n=4):
    """漏网原始行按前缀归一后的 top-N，用来定位哪一类文案没被收掉。"""
    seen = {}
    for line in lines:
        key = re.sub(r"0x[0-9a-fA-F]+|@\s*\S+|\d+", "#", line.strip())[:70]
        seen[key] = seen.get(key, 0) + 1
    return "; ".join(f"{count}x {key}" for key, count in
                     sorted(seen.items(), key=lambda kv: -kv[1])[:n])


def check_flood(checks, res, stderr):
    lines = _stderr_lines(stderr)
    recover = _recover_lines(stderr)
    firsts = [line for line in recover if FIRST in line]
    summaries = [line for line in recover if SUMMARY in line]
    # 计数按 Decoder 实例独立：跨实例只取最大的那一份，不假设先后顺序。
    counts = [(int(a), int(b)) for a, b in COUNTS.findall("\n".join(recover))]
    summary_counts = [(int(a), int(b)) for a, b in COUNTS.findall("\n".join(summaries))]
    biggest = max(counts, key=sum) if counts else None

    checks.check("flood: child finished the hole and paused at the declared end",
                 res is not None and res.get("fresh") is True
                 and res.get("end", -1) >= res.get("declared", 1) - 2,
                 f"res={res}")
    checks.check("flood: one bad-data message kept as the sample", len(firsts) >= 1,
                 f"first_lines={len(firsts)}: {firsts[:1]}")
    checks.check("flood: the sample is a summary-free first line",
                 bool(firsts) and all(SUMMARY not in line for line in firsts))
    checks.check("flood: summary carries 'ffmpeg_messages=N packet_errors=M'",
                 bool(summary_counts), f"summaries={summaries[:3]}")
    checks.check("flood: collapsed far more messages than it printed",
                 biggest is not None and sum(biggest) >= MIN_AGGREGATED, f"counts={biggest}")
    checks.check("flood: summary count is rate limited", len(summaries) <= SUMMARY_CAP,
                 f"summaries={len(summaries)} first={len(firsts)}")
    checks.check("flood: recoverable lines stay few", len(recover) <= SUMMARY_CAP + 2,
                 f"recover_lines={len(recover)}")
    under = len(lines) < STDERR_LINE_BUDGET
    checks.check(f"flood: total stderr stayed under {STDERR_LINE_BUDGET} lines", under,
                 f"lines={len(lines)}, recover={len(recover)}, leaks: {_leak_profile(lines)}")
    checks.brief(f"flood: {len(lines)} stderr lines, {len(recover)} recoverable "
                 f"({len(firsts)} first / {len(summaries)} summary), "
                 f"max counts={biggest} (ffmpeg_messages, packet_errors)")


def check_sentinel(checks, res, stderr):
    if not checks.check("sentinel: child reported its sentinels", res is not None, _tail(stderr)):
        return
    lines = _stderr_lines(stderr)
    path = str(res.get("avutil_path") or res.get("avutil") or "")
    checks.check("sentinel: ctypes is talking to the avutil sumu_core loaded",
                 loaded_from_sumu(path), path or str(res.get("avutil")))
    missing, leaked, folded, mangled = [], [], [], []
    for tag, level, emit_level, body in res["sentinels"]:
        hits = [line for line in lines if tag in line]
        if level <= emit_level and not hits:
            missing.append(f"{tag}@{level}/emit{emit_level}")
        if level > emit_level and hits:
            leaked.append(f"{tag}@{level}/emit{emit_level}")
        if any(RECOVER in line for line in hits):
            folded.append(tag)
        if hits and not any(body in line for line in hits):
            mangled.append(tag)
    checks.check("sentinel: every message at or below the active level stayed raw",
                 not missing, f"missing={missing}")
    checks.check("sentinel: nothing above the active level got through", not leaked,
                 f"leaked={leaked}")
    checks.check("sentinel: no sentinel was folded into the summary", not folded,
                 f"folded={folded}")
    checks.check("sentinel: sentinel text printed verbatim", not mangled, f"mangled={mangled}")
    bad = [tag for tag, level, _e, body in res["sentinels"]
           if level <= AV_LOG_DEBUG and ("NAL unit" in body or "decoding MB" in body
                                         or "Packet corrupt" in body)]
    checks.check("sentinel: bad-data-looking text exercised outside the decoder scope",
                 len(bad) >= 3, str(bad))
    debug_tags = [tag for tag, level, emit_level, _body in res["sentinels"]
                  if level == AV_LOG_DEBUG and emit_level >= AV_LOG_DEBUG]
    checks.check("sentinel: DEBUG-level messages were not lost",
                 len(debug_tags) >= 2 and not any(tag in group for tag in debug_tags
                                                  for group in missing),
                 f"tags={debug_tags}")
    checks.brief(f"sentinel: {len(res['sentinels'])} sentinels fired at av_log level "
                 f"{res.get('level0')}/{AV_LOG_DEBUG}")


def check_no_moov(checks, res, stderr):
    moov = [line for line in _stderr_lines(stderr) if "moov atom not found" in line]
    checks.check("no_moov: open() rejected the truncated clip",
                 res is not None and res.get("raised") is True, f"res={res}")
    checks.check("no_moov: the key 'moov atom not found' error is still printed",
                 bool(moov), f"stderr_tail={_tail(stderr)}")
    checks.check("no_moov: the key error was not folded into the summary bucket",
                 bool(moov) and not any(RECOVER in line for line in moov), f"moov={moov[:1]}")


def check_clean(checks, res, stderr):
    recover = _recover_lines(stderr)
    checks.check("clean: normal playback advanced",
                 res is not None and res.get("frame", 0) > 0 and res.get("playing") is True,
                 f"res={res}")
    checks.check("clean: no recoverable-media chatter on a clean file", not recover,
                 str(recover[:2]))


CASES = (
    ("flood", check_flood),
    ("sentinel", check_sentinel),
    ("no_moov", check_no_moov),
    ("clean", check_clean),
)


# --- 自检（不用 GPU）----------------------------------------------------

def self_test():
    ok = True

    def expect(name, condition):
        nonlocal ok
        ok = ok and bool(condition)
        print(f"[{'PASS' if condition else 'FAIL'}] self-test: {name}", flush=True)

    def verdict(checker, res, stderr):
        probe = Checks(quiet=True)
        checker(probe, res, stderr)
        return [item[0] for item in probe.failures]

    flood_res = {"case": "flood", "fresh": True, "end": 1438, "declared": 1440,
                 "span": [259, 1296], "seconds": 9.4}
    flood_stderr = "\n".join([
        "[sumu] pt_ring=32 ai_ring=48 decode_ahead_max=64 (320x180)",
        RECOVER + " first: h264 @ 0x0 error while decoding MB 12 34, bytestream -5",
        RECOVER + " summary: ffmpeg_messages=612 packet_errors=291",
        RECOVER + " summary: ffmpeg_messages=1204 packet_errors=577",
    ]) + "\n"
    expect("flood checker accepts a rate-limited log",
           not verdict(check_flood, flood_res, flood_stderr))
    noisy = "\n".join([f"h264 @ 0x0 error while decoding MB {i} 3, bytestream -5"
                       for i in range(150)]) + "\n" + flood_stderr
    expect("flood checker rejects a raw flood", verdict(check_flood, flood_res, noisy))
    expect("flood checker rejects a log without a summary",
           verdict(check_flood, flood_res, flood_stderr.replace("summary:", "folded:")))
    expect("flood checker rejects a log without the first sample",
           verdict(check_flood, flood_res, flood_stderr.replace("first:", "sample:")))
    tiny = "\n".join([
        RECOVER + " first: error while decoding MB 12 34",
        RECOVER + " summary: ffmpeg_messages=4 packet_errors=1",
    ]) + "\n"
    expect("flood checker rejects tiny aggregation", verdict(check_flood, flood_res, tiny))
    expect("flood checker rejects a dead playback",
           verdict(check_flood, dict(flood_res, fresh=False), flood_stderr))
    # 多 Decoder：一个已经收尾的大计数 + 另一个刚起步的小计数，不能用顺序判单调。
    two_decoders = "\n".join([
        RECOVER + " first: error while decoding MB 12 34",
        RECOVER + " summary: ffmpeg_messages=40760 packet_errors=10190",
        RECOVER + " summary: ffmpeg_messages=3 packet_errors=1 (session closed)",
    ]) + "\n"
    expect("flood checker tolerates per-instance counters from a second decoder",
           not verdict(check_flood, flood_res, two_decoders))

    clean_res = {"case": "clean", "frame": 42, "declared": 150, "playing": True}
    expect("clean checker accepts clean stderr",
           not verdict(check_clean, clean_res, "[sumu] UI DPI scale=1.00\n"))
    expect("clean checker rejects false positives",
           verdict(check_clean, clean_res, RECOVER + " first: noise\n"))

    fired = [[tag, level, AV_LOG_DEBUG, body] for tag, level, body in sentinels("main")]
    fired.append(["sumu-sentinel-thread-8", AV_LOG_DEBUG, AV_LOG_DEBUG,
                  "unknown debug from a plain caller"])
    fired.append(["sumu-sentinel-after-close", AV_LOG_ERROR, AV_LOG_INFO,
                  "unknown error after the decoder was closed"])
    sent_res = {"case": "sentinel", "avutil": "avutil-61.dll",
                "avutil_path": str(ROOT / "python" / "sumu" / "avutil-61.dll"),
                "level0": AV_LOG_INFO, "sentinels": fired}
    sent_stderr = "\n".join([f"{tag}: {body}" for tag, level, emit, body in fired
                             if level <= emit]) + "\n"
    expect("sentinel checker accepts raw pass-through",
           not verdict(check_sentinel, sent_res, sent_stderr))
    swallowed = sent_stderr.replace(
        "sumu-sentinel-main-3: error while decoding MB 12 34, bytestream -5\n", "")
    expect("sentinel checker catches a swallowed bad-data message",
           verdict(check_sentinel, sent_res, swallowed))
    expect("sentinel checker catches a filtered-out DEBUG message",
           verdict(check_sentinel, sent_res,
                   sent_stderr.replace("sumu-sentinel-main-8: unknown debug from a plain caller\n", "")))
    expect("sentinel checker catches a message above the active level leaking through",
           verdict(check_sentinel, sent_res,
                   sent_stderr + "sumu-sentinel-main-9: unknown trace from a plain caller\n"))
    expect("sentinel checker catches a sentinel folded into the summary",
           verdict(check_sentinel, sent_res, sent_stderr.replace(
               "sumu-sentinel-main-1: unknown error from a plain caller",
               RECOVER + " summary: ffmpeg_messages=1 packet_errors=0 "
               "sumu-sentinel-main-1: unknown error from a plain caller")))

    moov_res = {"case": "no_moov", "raised": True, "message": "open failed"}
    expect("no_moov checker accepts the retained key error",
           not verdict(check_no_moov, moov_res,
                       "[mov,mp4,m4a,3gp,3g2,mj2 @ 0x1] moov atom not found\n"))
    expect("no_moov checker rejects a swallowed key error",
           verdict(check_no_moov, moov_res, "[sumu] open failed\n"))
    expect("no_moov checker rejects the key error folded into a summary",
           verdict(check_no_moov, moov_res,
                   RECOVER + " first: moov atom not found\n"))

    print("self-test passed." if ok else "self-test FAILED.")
    return 0 if ok else 1


# --- 主流程 -------------------------------------------------------------

def main():
    faulthandler.enable()
    ap = argparse.ArgumentParser(description="decoder recoverable-log rate-limit checks")
    ap.add_argument("--dry-run", action="store_true",
                    help="synthesize media and probe avutil, never open a player")
    ap.add_argument("--self-test", action="store_true",
                    help="validate the checkers against synthetic stderr")
    ap.add_argument("--child", choices=sorted(CHILDREN), help=argparse.SUPPRESS)
    ap.add_argument("--tmp", help=argparse.SUPPRESS)
    ap.add_argument("--keep", action="store_true", help="keep the synthesized media directory")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.child:
        media = json.loads((Path(args.tmp) / "media.json").read_text(encoding="utf-8"))
        CHILDREN[args.child](media)
        return 0

    tmp = Path(tempfile.mkdtemp(prefix="sumu-declog-", dir=str(SCRIPTS / "trace")))
    checks = Checks()
    try:
        media = build_media(tmp)
        check_media(checks, media)
        if args.dry_run:
            lib, spec, dll_path = load_avutil()
            level = lib.av_log_get_level()
            checks.check("dry-run: avutil exposes av_log/av_log_get_level", bool(spec), spec)
            checks.check("dry-run: the loaded avutil is the one next to sumu_core",
                         loaded_from_sumu(dll_path), dll_path or spec)
            checks.check("dry-run: av_log default level is readable", 0 <= level <= AV_LOG_TRACE,
                         f"level={level} ({spec})")
            print(f"dry run: media synthesized and checked; cases "
                  f"{[name for name, _ in CASES]} not run (add --keep to inspect the files)")
            return 1 if checks.failures else 0

        for case, checker in CASES:
            code, out, err = run_child(case, tmp)
            checks.check(f"{case}: child exited 0", code == 0,
                         f"exit={code} stderr_tail={_tail(err)}")
            checker(checks, parse_result(out), err)
    finally:
        if args.keep:
            print(f"media kept in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    failed = checks.failures
    print()
    if failed:
        print(f"{len(failed)} check(s) failed:")
        for name, _ok, _detail in failed:
            print(f"  - {name}")
        return 1
    print("all decoder logging checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
