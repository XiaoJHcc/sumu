# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# 不完整媒体：打开失败 / 声明全长的半截文件 / 零填缺损 / 播放中追加数据 / TS 推断时长。
#     .venv\Scripts\python.exe scripts\verify_partial_media.py
#     .venv\Scripts\python.exe scripts\verify_partial_media.py --dry-run   (只校验素材，不 import sumu_core)
import faulthandler
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

W, H = 320, 180
FPS = 30
FULL_S = 12.0
LONG_FPS = 120
LONG_S = 6.0
HOLE_FRAC = (0.40, 0.60)        # 字节占比
LONG_HOLE_FRAC = (0.20, 0.80)
PACKET_ERROR_BUDGET = 256       # decoder.cpp pump_one_raw_frame 的 kMaxRecoverableErrors
FRESH = 0                       # Source::PassthroughFresh


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)],
                   check=True, timeout=180)


def _zeroed(src, frac):
    data = bytearray(src.read_bytes())
    lo, hi = int(len(data) * frac[0]), int(len(data) * frac[1])
    data[lo:hi] = bytes(hi - lo)
    out = src.with_name(f"hole-{src.stem}.part")
    out.write_bytes(bytes(data))
    return out


class Media:
    """12s 30fps 主片 + 6s 120fps 长洞片，全落在临时目录。"""

    def __init__(self, tmp):
        self.full = self._encode(tmp / "full.mp4", FPS, FULL_S)
        data = self.full.read_bytes()

        # 声明全长，只有前半段字节能解。
        self.half_mp4 = tmp / "half.mp4.part"
        self.half_mp4.write_bytes(data[: len(data) // 2])
        self.half_mp4_suffix = data[len(data) // 2:]

        # moov 在尾部，切一半后必然打不开。
        tail = tmp / "tail.mp4"
        ffmpeg("-i", self.full, "-c", "copy", tail)
        self.no_moov = tmp / "no-moov.mp4.part"
        self.no_moov.write_bytes(tail.read_bytes()[: tail.stat().st_size // 2])

        self.invalid = tmp / "invalid.mp4.part"
        self.invalid.write_bytes(b"not a video" * 100)

        self.hole_mp4 = _zeroed(self.full, HOLE_FRAC)

        full_mkv = tmp / "full.mkv"
        ffmpeg("-i", self.full, "-c", "copy", full_mkv)
        mkv = full_mkv.read_bytes()
        self.half_mkv = tmp / "half.mkv.part"
        self.half_mkv.write_bytes(mkv[: len(mkv) // 2])
        self.half_mkv_suffix = mkv[len(mkv) // 2:]

        # TS 只能从现有字节推断时长。
        full_ts = tmp / "full.ts"
        ffmpeg("-i", self.full, "-c", "copy", full_ts)
        ts = full_ts.read_bytes()
        self.half_ts = tmp / "half.ts.part"
        self.half_ts.write_bytes(ts[: len(ts) // 2])

        # 120fps 让 256 个坏包只占 ~2s 内容时间。
        self.long = self._encode(tmp / "long.mp4", LONG_FPS, LONG_S)
        self.long_hole = _zeroed(self.long, LONG_HOLE_FRAC)

    @staticmethod
    def _encode(path, fps, seconds):
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={fps}", "-t", seconds,
               "-c:v", "libx264", "-preset", "ultrafast", "-g", "15", "-bf", "2",
               "-pix_fmt", "yuv420p", "-movflags", "+faststart", path)
        return path

    @staticmethod
    def info(path):
        """首条视频流的 ffprobe JSON；读不到时为 None。"""
        run = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
             "-show_entries",
             "stream=nb_frames,nb_read_frames,duration,avg_frame_rate:format=duration",
             "-of", "json", str(path)],
            capture_output=True, text=True, timeout=180)
        if run.returncode != 0:
            return None
        info = json.loads(run.stdout or "{}")
        return info if info.get("streams") else None

    @classmethod
    def declared(cls, path, fps):
        """复算容器声明的总帧数（同 Decoder::open 的规则）。"""
        info = cls.info(path)
        assert info, f"{path.name}: ffprobe found no video stream"
        stream = info["streams"][0]
        if int(stream.get("nb_frames") or 0) > 0:
            return int(stream["nb_frames"])
        duration = stream.get("duration") or info.get("format", {}).get("duration")
        assert duration, f"{path.name}: no duration to derive a frame count from"
        return round(float(duration) * fps)

    @classmethod
    def decodable(cls, path):
        """实际能解出来的帧数。"""
        info = cls.info(path)
        assert info, f"{path.name}: ffprobe found no video stream"
        return int(info["streams"][0]["nb_read_frames"])


class Trace:
    """present trace 的增量视图。"""

    def __init__(self, player, path):
        self.player = player
        self.path = path
        self.seen = 0

    def delta(self):
        """自上次调用以来新上屏的 (source, frame_num)。"""
        self.player.dump_present_trace(str(self.path))
        rows = []
        for line in self.path.read_text(encoding="utf-8").splitlines()[1:]:
            _, source, frame = line.rsplit(",", 2)
            rows.append((int(source), int(frame)))
        # 原生只留最近 64K tick，被裁过就退化为全部现存行。
        new = rows[self.seen:] if len(rows) >= self.seen else rows
        self.seen = len(rows)
        return new


class Play:
    """每 tick 守两条不变量：时钟只进不退、声明末尾之前不暂停。"""

    def __init__(self, player, label):
        self.player = player
        self.label = label
        self.declared = player.frame_count()
        self.frame = int(player.current_frame())
        assert self.declared > 0, f"{label}: native reports an unknown frame count"

    def step(self):
        self.player.pump_messages()
        self.player.ui_tick()
        stats = self.player.stats()
        frame = int(stats["current_frame"])
        if frame < self.frame:
            raise AssertionError(f"{self.label}: clock went backwards {self.frame} -> {frame}")
        # 末尾暂停时 playing_ 先落、clock_frame_ 后落，留几帧容差。
        if frame < self.declared - 3 and not stats["is_playing"]:
            raise AssertionError(f"{self.label}: paused early at frame {frame}/{self.declared}")
        self.frame = frame
        return stats

    def run(self, seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.step()
            time.sleep(0.01)

    def until(self, done, timeout):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            self.step()
            if done():
                return True
            time.sleep(0.01)
        return False

    def wait_end(self, timeout):
        """等到暂停，再多 tick 一拍取样（末尾帧晚一拍才落到 clock_frame_）。"""
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if not self.step()["is_playing"]:
                self.run(0.15)
                return self.frame
            time.sleep(0.01)
        raise AssertionError(f"{self.label}: still playing at {self.frame}/{self.declared}")


def wait_fresh(play, trace, min_frame, timeout):
    """等 >= min_frame 的 fresh 上屏。"""
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        play.run(0.05)
        if play.frame >= min_frame and any(
                source == FRESH and frame >= min_frame for source, frame in trace.delta()):
            return True
        time.sleep(0.15)
    return False


def held_span(rows, lo, hi):
    """洞窗口内非 fresh 上屏覆盖的帧区间（必须卡满整个窗口）。"""
    held = [frame for source, frame in rows if source != FRESH and lo <= frame <= hi]
    assert held, f"frames {lo}-{hi} decoded anyway, the case proves nothing"
    span = (min(held), max(held))
    assert span[0] <= lo + 12 and span[1] >= hi - 12, (span, lo, hi)
    return span


def check_media(media):
    """校验素材与判据里的数字。"""
    full = media.declared(media.full, FPS)
    assert full == round(FULL_S * FPS), full
    assert media.decodable(media.full) >= full - 2
    print(f"PASS: full mp4 declares {full} frames and decodes all of them", flush=True)

    for path, label in ((media.half_mp4, "half mp4"), (media.half_mkv, "half mkv")):
        declared, tail = media.declared(path, FPS), media.decodable(path)
        assert declared == full, (label, declared)
        assert tail < declared - 2 * FPS, (label, tail, declared)
        print(f"PASS: {label} declares {declared} frames but only ~{tail} decode", flush=True)

    ts_declared, ts_tail = media.declared(media.half_ts, FPS), media.decodable(media.half_ts)
    assert ts_declared <= ts_tail + 2 * FPS, (ts_declared, ts_tail)
    print(f"PASS: half ts declares only what it holds ({ts_declared}, {ts_tail} decoded)", flush=True)

    hole_declared, hole_tail = media.declared(media.hole_mp4, FPS), media.decodable(media.hole_mp4)
    assert hole_tail < hole_declared - 20, (hole_tail, hole_declared)
    print(f"PASS: mp4 hole drops {hole_declared - hole_tail} frames mid-file", flush=True)

    long_declared = media.declared(media.long_hole, LONG_FPS)
    long_tail = media.decodable(media.long_hole)
    window = (int(long_declared * LONG_HOLE_FRAC[0]), int(long_declared * LONG_HOLE_FRAC[1]))
    assert window[1] - window[0] > PACKET_ERROR_BUDGET, window
    assert long_declared - long_tail >= PACKET_ERROR_BUDGET, (long_declared, long_tail)
    print(f"PASS: long hole drops {long_declared - long_tail} frames "
          f"(window {window[0]}-{window[1]} > {PACKET_ERROR_BUDGET} packet budget)", flush=True)

    for path in (media.invalid, media.no_moov):
        assert media.info(path) is None, f"{path.name}: unexpectedly readable"
        print(f"PASS: {path.name} has no readable video stream (open must reject it)", flush=True)


def case_open_rejects(player, media):
    for path in (media.invalid, media.no_moov):
        try:
            player.open(str(path))
        except RuntimeError as exc:
            print(f"PASS: rejected {path.name}: {exc}", flush=True)
        else:
            raise AssertionError(f"unexpectedly opened {path.name}")
        for _ in range(10):  # 让 present 吃掉这次失败的 open
            player.pump_messages()
            player.ui_tick()
            time.sleep(0.01)


def case_full(player, trace, media):
    """正常全长片：播到声明末尾暂停。"""
    player.open(str(media.full))
    assert player.stats()["decode_frame_count"] > 0
    play = Play(player, "full mp4")
    player.play()
    frame = play.wait_end(play.declared / FPS + 15)
    assert frame >= play.declared - 2, (frame, play.declared)
    assert player.seek(0) <= 2
    print(f"PASS: full mp4 paused at its declared end {frame}/{play.declared}", flush=True)
    player.close_current_session()


def case_unavailable_seek(player, trace, media):
    """缺失区段的 seek 是可恢复错误，随后能 seek 回去。"""
    player.open(str(media.half_mp4))
    target = player.frame_count() - 20
    assert target > media.decodable(media.half_mp4) + 10, target
    try:
        landed = player.seek(target)
    except RuntimeError as exc:
        print(f"PASS: seek into the missing region is a recoverable error: {exc}", flush=True)
    else:
        assert landed < target, (landed, target)
        assert player.current_frame() < target, player.current_frame()
        print(f"PASS: seek into the missing region lands short ({landed} < {target})", flush=True)
    assert player.seek(0) <= 2
    player.close_current_session()


def case_truncated_append(player, trace, path, suffix, label):
    """越过可解码尾仍继续走时钟；播放中追加剩余数据后自行恢复。"""
    player.open(str(path))
    assert player.stats()["decode_frame_count"] > 0
    play = Play(player, label)
    tail = Media.decodable(path)
    assert tail < play.declared - 2 * FPS, (tail, play.declared)
    player.play()
    trace.delta()
    assert play.until(lambda: play.frame > tail + 12, (tail + 12) / FPS + 15), \
        f"{label}: clock never passed the decodable tail {tail}/{play.declared}"
    assert player.stats()["is_playing"], f"{label}: paused past the decodable tail"
    before = play.frame
    play.run(0.4)
    assert play.frame > before + 4, f"{label}: clock stalled {before} -> {play.frame}"
    trace.delta()
    # 不 seek / 不 reopen / 不 play。
    with path.open("ab") as f:
        f.write(suffix)
        f.flush()
        os.fsync(f.fileno())
    assert wait_fresh(play, trace, tail + 12, timeout=10.0), \
        f"{label}: no fresh frame after appending the rest"
    frame = play.wait_end((play.declared - play.frame) / FPS + 10)
    assert frame >= play.declared - 2, (frame, play.declared)
    print(f"PASS: {label} kept playing past its decodable tail ({tail}/{play.declared}), "
          f"resumed fresh frames after the append, paused at {frame}", flush=True)
    player.close_current_session()


def case_ts(player, trace, media):
    """TS 推断时长即现有部分，不要求越过（既有局限）。"""
    player.open(str(media.half_ts))
    play = Play(player, "ts half")
    player.play()
    frame = play.wait_end(play.declared / FPS + 15)
    landed = player.seek(0)
    assert landed <= 30, landed
    print(f"PASS: ts half paused at its inferred end {frame}/{play.declared}, "
          f"seek(0) -> {landed}", flush=True)
    player.close_current_session()


def case_hole(player, trace, media):
    """中间零填缺损：洞内不停，洞后 fresh 帧恢复。"""
    player.open(str(media.hole_mp4))
    play = Play(player, "mp4 hole")
    lo, hi = int(play.declared * HOLE_FRAC[0]), int(play.declared * HOLE_FRAC[1])
    player.play()
    trace.delta()
    assert play.until(lambda: play.frame > hi + 12, (hi + 12) / FPS + 15), \
        f"mp4 hole: clock never crossed the hole ({play.frame}/{play.declared})"
    assert player.stats()["is_playing"], "mp4 hole: paused inside the hole"
    span = held_span(trace.delta(), lo, hi)
    assert wait_fresh(play, trace, hi, timeout=4.0), "mp4 hole: no fresh frame after the hole"
    frame = play.wait_end((play.declared - play.frame) / FPS + 10)
    assert frame >= play.declared - 2, (frame, play.declared)
    print(f"PASS: mp4 hole held the picture over frames {span[0]}-{span[1]} without pausing, "
          f"fresh frames resumed, paused at {frame}/{play.declared}", flush=True)
    player.close_current_session()


def case_long_hole(player, trace, media):
    """长洞跨过 256 坏包预算：打满上限也不结束播放。"""
    player.open(str(media.long_hole))
    play = Play(player, "mp4 long hole")
    lo, hi = int(play.declared * LONG_HOLE_FRAC[0]), int(play.declared * LONG_HOLE_FRAC[1])
    landed = player.seek(max(0, lo - 20))  # 跳过洞前内容，压短墙钟
    assert landed <= lo, (landed, lo)
    player.play()
    trace.delta()
    assert play.until(lambda: play.frame > hi + 8, (hi + 8 - landed) / LONG_FPS + 20), \
        f"mp4 long hole: playback died inside the hole ({play.frame}/{play.declared})"
    span = held_span(trace.delta(), lo, hi)
    assert wait_fresh(play, trace, hi, timeout=10.0), \
        "mp4 long hole: no fresh frame after the hole"
    frame = play.wait_end((play.declared - play.frame) / LONG_FPS + 10)
    assert frame >= play.declared - 2, (frame, play.declared)
    print(f"PASS: mp4 long hole held the picture over frames {span[0]}-{span[1]} "
          f"({span[1] - span[0]} frames > {PACKET_ERROR_BUDGET} packet error budget) "
          f"without pausing, resumed fresh frames, paused at {frame}/{play.declared}",
          flush=True)
    player.close_current_session()


def main():
    faulthandler.enable()
    dry_only = "--dry-run" in sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="sumu-partial-", dir=ROOT / "scripts" / "trace") as tmp:
        tmp = Path(tmp)
        media = Media(tmp)
        check_media(media)
        if dry_only:
            print("dry run: media and criteria checks passed (no player opened)")
            return
        sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu")]
        import sumu_core

        player = sumu_core.Player(640, 400)
        try:
            trace = Trace(player, tmp / "present.csv")
            case_open_rejects(player, media)
            case_full(player, trace, media)
            case_unavailable_seek(player, trace, media)  # 必须在追加之前
            case_truncated_append(player, trace, media.half_mp4, media.half_mp4_suffix, "half mp4")
            case_truncated_append(player, trace, media.half_mkv, media.half_mkv_suffix, "half mkv")
            case_ts(player, trace, media)
            case_hole(player, trace, media)
            case_long_hole(player, trace, media)
            player.open(str(media.full))
            player.play()
            for _ in range(50):
                player.pump_messages()
                player.ui_tick()
                time.sleep(0.01)
            assert player.current_frame() > 0, player.stats()
            print("PASS: normal playback after the failure cases", flush=True)
        finally:
            player.close()
    print("All partial-media checks passed.")


if __name__ == "__main__":
    main()
