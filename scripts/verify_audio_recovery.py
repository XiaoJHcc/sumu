# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# WASAPI 音频端点恢复（player_audio.cpp）：真实初始化 + 真实重建 + pause / seek / 关闭重开。
#     .venv\Scripts\python.exe scripts\verify_audio_recovery.py
#     .venv\Scripts\python.exe scripts\verify_audio_recovery.py --dry-run   (只造素材 + 校验判据，不 import sumu_core)
#
# 验证的是「真实 WASAPI 客户端」而不是打桩：audio_stats() 的 device_id / sample_rate / channels /
# buffer_frames 全部来自 IMMDevice::GetId 与 GetMixFormat，write_frames 只在 render client 真的吃下
# PCM 时增长，last_peak 是出厂前最后一个样本块的峰值（静音补零不计入）。
#
# 未覆盖（诚实说明）：物理热插拔（拔线/插拔 USB 声卡）与「Windows 里切换默认输出设备」不在脚本内
# 自动执行 —— 那会改动用户机器的默认输出设备且需要硬件。脚本用 Player.request_audio_device_rebuild()
# 走与真实热插拔完全相同的实现路径（close_endpoint() -> open_endpoint()，同一个 COM 生命周期内的
# 重建），并额外确认稳态播放中 ~500ms 的默认设备轮询不会误判（device_changes/invalidations 不增长）。
import faulthandler
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRACE_DIR = ROOT / "scripts" / "trace"

W, H, FPS = 320, 180, 30
DUR_S = 40.0
TONE_HZ = 440
SR = 48000
MIN_WRITE_FRAMES = 2000        # 播放 ~0.5s 后设备应当已吃下这么多帧（48k 下 ≈40ms）
SETTLE_S = 0.3


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)],
                   check=True, timeout=180)


def ffprobe_audio(path):
    run = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=codec_name,channels,sample_rate:format=duration", "-of", "json", str(path)],
        capture_output=True, text=True, timeout=180)
    if run.returncode != 0:
        return None
    info = json.loads(run.stdout or "{}")
    return info if info.get("streams") else None


def build_media(tmp):
    """带音轨的测试片：testsrc2 画面 + 440Hz 正弦，AAC 立体声。"""
    path = tmp / "audio.mp4"
    ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={FPS}",
           "-f", "lavfi", "-i", f"sine=frequency={TONE_HZ}:sample_rate={SR}",
           "-t", DUR_S,
           "-c:v", "libx264", "-preset", "ultrafast", "-g", FPS, "-pix_fmt", "yuv420p",
           "-c:a", "aac", "-b:a", "128k", "-ac", "2",
           "-movflags", "+faststart", "-shortest", path)
    return path


def check_media(path):
    info = ffprobe_audio(path)
    assert info, f"{path.name}: ffprobe found no audio stream"
    stream = info["streams"][0]
    assert stream["codec_name"] == "aac", stream
    assert int(stream["channels"]) == 2, stream
    assert int(stream["sample_rate"]) == SR, stream
    print(f"media: {path.name} audio={stream['codec_name']} "
          f"{stream['channels']}ch {stream['sample_rate']}Hz", flush=True)


class Run:
    """驱动主线程 tick（present/UI），顺带守住「视频时钟只进不退」。"""

    def __init__(self, player):
        self.player = player
        self.check_clock = True   # case_reopen 换会话期间关掉（帧号会回到 0）
        self.frame = int(player.current_frame())

    def tick(self, dt=0.01):
        self.player.pump_messages()
        self.player.ui_tick()
        stats = self.player.stats()
        frame = int(stats["current_frame"])
        if self.check_clock:
            assert frame >= self.frame, f"video clock went backwards {self.frame} -> {frame}"
        self.frame = frame
        return stats

    def rebase(self):
        """会话切换（open / close_session）之后重新对齐单调性基线。"""
        self.player.pump_messages()
        self.player.ui_tick()
        self.frame = int(self.player.current_frame())
        self.check_clock = True

    def run(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.tick()
            time.sleep(0.01)

    def until(self, done, timeout, label):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.tick()
            if done():
                return True
            time.sleep(0.01)
        raise AssertionError(f"{label}: not satisfied within {timeout}s")


def audio_stats(player):
    return player.audio_stats()


def case_open(player, run, video):
    """打开带音轨文件：音频线程真实建起 WASAPI 端点（不是沿用上一条会话）。"""
    player.open(str(video))
    assert player.has_audio(), "native reports no audio stream for the test file"
    run.until(lambda: audio_stats(player)["active"], 15.0, "WASAPI endpoint init")
    st = audio_stats(player)
    assert st["device_id"], st
    assert st["sample_rate"] > 0, st
    assert st["channels"] in (1, 2, 6, 8), st
    assert st["buffer_frames"] > 0, st
    assert st["builds"] == 1, st
    assert st["failures"] == 0, st
    print(f"PASS: real WASAPI endpoint opened: id={st['device_id']} "
          f"{st['sample_rate']}Hz {st['channels']}ch buffer={st['buffer_frames']} frames", flush=True)


def case_play(player, run):
    """播放：视频推进 + 真实 PCM 持续写入设备（write_frames/last_peak 是证据）。"""
    frame0 = int(player.current_frame())
    player.play()
    w0 = audio_stats(player)["write_frames"]
    run.until(lambda: audio_stats(player)["write_frames"] >= w0 + MIN_WRITE_FRAMES, 15.0,
              "audio frames reaching the device")
    st = audio_stats(player)
    assert st["active"], st
    assert st["last_peak"] > 1e-4, f"no real PCM left the audio thread: {st}"
    run.run(2.0)
    st2 = audio_stats(player)
    frame1 = int(player.current_frame())
    assert st2["write_frames"] > st["write_frames"], (st, st2)
    assert frame1 > frame0 + 10, (frame0, frame1)
    # 稳态播放中不得因 ~500ms 默认设备轮询而误判重建。
    assert st2["invalidations"] == 0, st2
    assert st2["device_changes"] == 0, st2
    assert st2["builds"] == 1, st2
    print(f"PASS: playing -> write_frames {st2['write_frames']} peak={st2['last_peak']:.4f} "
          f"frame {frame0}->{frame1}, no spurious rebuild "
          f"(invalidations=0 device_changes=0)", flush=True)


def case_rebuild(player, run):
    """显式重建：真实 teardown + 真实 re-open 同一默认端点，输出随即恢复，视频不受影响。"""
    st0 = audio_stats(player)
    frame0 = int(player.current_frame())
    writes0 = st0["write_frames"]
    player.request_audio_device_rebuild()
    run.until(lambda: audio_stats(player)["builds"] > st0["builds"], 15.0,
              "endpoint rebuilt after request")
    run.until(lambda: audio_stats(player)["active"], 15.0, "rebuilt endpoint active")
    st1 = audio_stats(player)
    assert st1["rebuild_requests"] == st0["rebuild_requests"] + 1, (st0, st1)
    assert st1["builds"] == st0["builds"] + 1, (st0, st1)
    # 重建的必须是同一个默认端点、同样的混音格式：脚本没有动过用户的默认设备。
    assert st1["device_id"] == st0["device_id"], (st0["device_id"], st1["device_id"])
    assert st1["sample_rate"] == st0["sample_rate"], (st0, st1)
    assert st1["channels"] == st0["channels"], (st0, st1)
    assert st1["failures"] == st0["failures"], (st0, st1)
    # 新端点开始吃 PCM：重建后输出真的恢复了（不是只有 active 标志）。
    run.until(lambda: (audio_stats(player)["write_frames"] > writes0 + MIN_WRITE_FRAMES
                       and int(player.current_frame()) > frame0), 15.0,
              "audio output and video resumed on the rebuilt endpoint")
    frame1 = int(player.current_frame())
    assert frame1 > frame0, (frame0, frame1)
    print(f"PASS: endpoint rebuilt (builds {st0['builds']}->{st1['builds']}, same id) "
          f"output resumed write_frames {writes0}->{audio_stats(player)['write_frames']}, "
          f"video kept advancing {frame0}->{frame1}", flush=True)


def case_pause(player, run):
    """pause：写静音但视频时钟冻结；恢复播放后输出继续（音频不因暂停而丢端点）。"""
    player.pause()
    run.run(SETTLE_S)
    st_paused = audio_stats(player)
    frame_paused = int(player.current_frame())
    run.run(0.7)
    st_paused2 = audio_stats(player)
    assert int(player.current_frame()) <= frame_paused + 1, "video clock advanced while paused"
    assert st_paused2["active"], st_paused2
    assert st_paused2["builds"] == st_paused["builds"], (st_paused, st_paused2)
    runs_paused = st_paused2["write_frames"] - st_paused["write_frames"]
    assert runs_paused == 0, f"paused player still pushed PCM: {runs_paused} frames"

    player.play()
    w0 = audio_stats(player)["write_frames"]
    run.until(lambda: audio_stats(player)["write_frames"] > w0 + MIN_WRITE_FRAMES, 15.0,
              "output resumes after pause")
    assert int(player.current_frame()) > frame_paused, "video did not resume"
    print(f"PASS: pause froze video at frame {frame_paused} and pushed silence only "
          f"(write_frames flat), resume restored output and video", flush=True)


def case_seek(player, run):
    """seek：音频按 seek_version 丢旧状态后重新对齐 master，端点不重建。"""
    st0 = audio_stats(player)
    target = int(player.frame_count() * 0.6)
    landed = int(player.seek(target))
    assert abs(landed - target) <= 2, (landed, target)
    w0 = audio_stats(player)["write_frames"]
    run.until(lambda: audio_stats(player)["write_frames"] > w0 + MIN_WRITE_FRAMES, 15.0,
              "output after seek")
    st1 = audio_stats(player)
    assert st1["active"], st1
    assert st1["builds"] == st0["builds"], (st0, st1)
    assert st1["invalidations"] == st0["invalidations"], (st0, st1)
    assert int(player.current_frame()) >= landed - 3, (landed, player.current_frame())
    print(f"PASS: seek {landed} kept the same endpoint (builds={st1['builds']}) and resumed "
          f"output write_frames {w0}->{st1['write_frames']}", flush=True)


def case_reopen(player, run, video):
    """关闭会话会 join 音频线程（端点释放）；重新打开后重新初始化并恢复输出。"""
    st0 = audio_stats(player)
    run.check_clock = False  # 换会话期间帧号会回到 0，单调性检查先让开
    player.close_current_session()
    run.until(lambda: not audio_stats(player)["active"], 10.0, "endpoint released on close_session")
    st_closed = audio_stats(player)
    assert st_closed["builds"] == st0["builds"], (st0, st_closed)

    player.open(str(video))
    run.until(lambda: audio_stats(player)["active"], 15.0, "endpoint re-init on reopen")
    run.rebase()
    st1 = audio_stats(player)
    assert st1["builds"] == st0["builds"] + 1, (st0, st1)
    assert st1["device_id"] == st0["device_id"], (st0["device_id"], st1["device_id"])

    player.play()
    w0 = audio_stats(player)["write_frames"]
    run.until(lambda: audio_stats(player)["write_frames"] > w0 + MIN_WRITE_FRAMES, 15.0,
              "output after reopen")
    print(f"PASS: close_session released the endpoint, reopen re-initialized it "
          f"(builds {st0['builds']}->{st1['builds']}) and output resumed", flush=True)


def main():
    faulthandler.enable()
    dry_only = "--dry-run" in sys.argv[1:]
    TRACE_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sumu-audio-", dir=TRACE_DIR) as tmp:
        tmp = Path(tmp)
        video = build_media(tmp)
        check_media(video)
        if dry_only:
            print("dry run: media and criteria checks passed (no player opened)")
            return

        sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu")]
        import sumu_core

        player = sumu_core.Player(640, 400)
        try:
            run = Run(player)
            case_open(player, run, video)
            case_play(player, run)
            case_rebuild(player, run)
            case_pause(player, run)
            case_seek(player, run)
            case_reopen(player, run, video)
            st = audio_stats(player)
            print(f"final: {json.dumps(st, sort_keys=True)}", flush=True)
        finally:
            player.close()

    print("NOTE: physical hot-plug / Windows default-output switching was NOT performed "
          "(it would change this machine's default render device); the rebuild path above is "
          "the same code a real device switch takes.", flush=True)
    print("All audio recovery checks passed.")


if __name__ == "__main__":
    main()
