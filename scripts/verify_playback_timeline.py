# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# VFR（可变帧率）时间轴：帧号仍是固定 fps 的时间网格，时间轴长度来自媒体 duration*fps
# 而不是物理 nb_frames；每张 GPU 帧带着自己的实际 PTS 持续区间，present 与 AI 输入按区间
# 查找——真实 VFR 间隙不能被当成解码缺损（否则持帧区间里全是 stale、时间轴还会提前结束）。
#     .venv\Scripts\python.exe scripts/verify_playback_timeline.py
#     .venv\Scripts\python.exe scripts/verify_playback_timeline.py --dry-run   (只校验素材/时间表)
#
# 素材是纯色轮转（geq 逐帧写 luma）：相邻物理帧内容必不同，同一物理帧在其持帧区间内逐字节
# 完全相同。因此「同一区间读到同一张图、换区间内容就变」可以直接用像素判，而不只看时钟推进。
#
# 覆盖：时间轴长度（duration*fps 而非 nb_frames）、全时长播放与末尾暂停、seek 落点与其内容、
# 持帧区间经 get_cuda_nv12_by_frame 的内容一致性、present 的 fresh 率、长持帧与解码前沿共存的
# 环槽选择、拼接流的时间戳回退与分段总长扩展、fps_div=2、短视频、普通 CFR 回归。
import faulthandler
import json
import math
import subprocess
import sys
import tempfile
import time
from bisect import bisect_right
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

W, H = 320, 180
VFR_PICS = 60                       # 物理帧数；三档间距 0.04/0.08/0.12 → 4.8s、网格 120 帧
VFR_DELTAS = (0.04, 0.08, 0.12)     # 25fps 网格下分别持 1/2/3 格
CFR_FPS, CFR_PICS = 30, 90          # 回归素材：每格正好一张图
SHORT_PICS = 9                      # 短视频：0.72s、网格 18 帧
Y_PALETTE = (81, 145, 41, 210)      # geq 写入的 luma，四色轮转（相邻帧必然不同）
LONG_HOLD_S = 10.0                  # 长持帧素材：首图持 10s（30fps 网格下占 300 格）
LONG_HOLD_TAIL_PICS = 120           # 之后 120 张 30fps 变色图 → 总时长 14s、时间轴上 121 张
LONG_HOLD_HEAD = 81                 # 首图 luma
LONG_HOLD_TAIL = (145, 41, 210)     # 后段三色轮转
FRESH = 0                           # Source::PassthroughFresh

_wrap_nv12 = None


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)],
                   check=True, timeout=180)


def _if_chain(var, values):
    """ffmpeg 表达式里的查表：if(eq(var,0),v0,if(eq(var,1),v1,...,v_last))。"""
    expr = str(values[-1])
    for i in range(len(values) - 2, -1, -1):
        expr = f"if(eq({var},{i}),{values[i]},{expr})"
    return expr


def _rate(text):
    """ffprobe 的 "25/1" 这类帧率字符串 -> float。"""
    num, _, den = str(text or "0/1").partition("/")
    den = float(den or 1.0)
    return float(num) / den if den else 0.0


def _long_hold_colour(index):
    """长持帧素材里第 index 张物理帧的 luma：首图 81，之后 145/41/210 轮转。"""
    if index == 0:
        return LONG_HOLD_HEAD
    return LONG_HOLD_TAIL[(index - 1) % len(LONG_HOLD_TAIL)]


class Media:
    """临时目录里的素材：VFR 主片、CFR 回归片、VFR 短视频、长持帧片、拼接流（PTS 回退）。"""

    def __init__(self, tmp):
        self.vfr = self._vfr(tmp / "vfr.mp4", VFR_PICS, VFR_DELTAS)
        self.cfr = self._cfr(tmp / "cfr.mp4")
        self.short = self._vfr(tmp / "short.mp4", SHORT_PICS, VFR_DELTAS)
        self.long_hold = self._long_hold(tmp / "long-hold.mp4")
        self.joined = self._joined(tmp / "joined.ts")

    @staticmethod
    def _palette_filter(filler_frame=None):
        """cb/cr 固定 128：单色，NV12 的 Y 平面即为 palette 原值。

        filler_frame 那张（收尾帧）用前一张的颜色：它落在时间轴之外，涂什么色都不该影响
        判据，同色之后「尾部那张图铺到末尾」的读法就只剩一种。
        """
        base = _if_chain(f"mod(N,{len(Y_PALETTE)})", Y_PALETTE)
        if filler_frame is None:
            return f"geq=lum='{base}':cb=128:cr=128"
        colour = Y_PALETTE[(filler_frame - 1) % len(Y_PALETTE)]
        return (f"geq=lum='if(eq(N,{filler_frame}),{colour},{base})'"
                f":cb=128:cr=128")

    @classmethod
    def _vfr(cls, path, pictures, deltas):
        """真 VFR：帧还是一批纯色图，时间戳按 deltas 循环铺开（settb 之后不再被量化）。

        x264 不给最后一包写时长，mp4 的 duration 就等于最后一帧的 pts——所以多编一张收尾帧
        （与上一张同色），让时间轴上最后一张图真的铺满自己的持帧区间。
        """
        n = len(deltas)
        offsets, acc = [], 0.0
        for d in deltas:
            offsets.append(round(acc, 6))
            acc += d
        period = round(acc, 6)
        setpts = f"(floor(N/{n})*{period}+{_if_chain(f'mod(N,{n})', offsets)})/TB"
        frames = pictures + 1
        ffmpeg("-f", "lavfi", "-i", f"color=black:s={W}x{H}:r=60",
               "-vf", f"{cls._palette_filter(frames - 1)},settb=1/90000,setpts='{setpts}'",
               "-frames:v", frames, "-fps_mode", "passthrough",
               "-c:v", "libx264", "-preset", "ultrafast", "-crf", "1",
               "-pix_fmt", "yuv420p", "-video_track_timescale", 90000,
               "-movflags", "+faststart", path)
        return path

    @classmethod
    def _cfr(cls, path):
        ffmpeg("-f", "lavfi", "-i", f"color=black:s={W}x{H}:r={CFR_FPS}",
               "-vf", cls._palette_filter(), "-frames:v", CFR_PICS,
               "-c:v", "libx264", "-preset", "ultrafast", "-crf", "1",
               "-pix_fmt", "yuv420p", "-movflags", "+faststart", path)
        return path

    @classmethod
    def _long_hold(cls, path):
        """首图持 10s（前 10 秒只有这一张）+ 之后 120 张 30fps 变色图。

        持帧区间取自容器给这张包声明的时长（stts 里 10s），也就是解码器 df.duration_seconds
        的可信来源；收尾那张只为让前一包的时长有定义，它自己落在时间轴之外。
        这个素材压的是「解码前沿已越过首图持帧区间、present head 仍停在区间内」的边界：旧实现
        按 frame_num % capacity 选槽，解到 360 时会覆盖首图那一格。
        """
        frames = LONG_HOLD_TAIL_PICS + 2  # 首图 + 120 张后段图 + 收尾帧
        cycle = _if_chain(f"mod(N-1,{len(LONG_HOLD_TAIL)})", LONG_HOLD_TAIL)
        luma = f"if(eq(N,0),{LONG_HOLD_HEAD},{cycle})"
        setpts = f"(if(eq(N,0),0,{LONG_HOLD_S}+(N-1)/{CFR_FPS}))/TB"
        ffmpeg("-f", "lavfi", "-i", f"color=black:s={W}x{H}:r=60",
               "-vf", f"geq=lum='{luma}':cb=128:cr=128,settb=1/90000,setpts='{setpts}'",
               "-frames:v", frames, "-fps_mode", "passthrough",
               "-c:v", "libx264", "-preset", "ultrafast", "-crf", "1",
               "-pix_fmt", "yuv420p", "-video_track_timescale", 90000,
               "-movflags", "+faststart", path)
        return path

    @staticmethod
    def frame_times(path):
        """解复用顺序的 (pts, duration)；duration 来自容器（stts），N/A 记 0。

        一张包的持帧区间就是它声明的时长——长持帧素材靠这里证明「首帧真的是 10s」。
        """
        run = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "frame=pts_time,duration_time", "-of", "csv=p=0", str(path)],
            capture_output=True, text=True, timeout=180)
        assert run.returncode == 0, f"{path.name}: ffprobe frame dump failed"
        rows = []
        for line in run.stdout.splitlines():
            parts = [part.strip() for part in line.split(",")]
            if not parts or not parts[0]:
                continue
            dur = parts[1] if len(parts) > 1 and parts[1] not in ("", "N/A") else "0"
            rows.append((float(parts[0]), float(dur)))
        assert rows, f"{path.name}: no frame timestamps"
        return rows

    @classmethod
    def _joined(cls, path):
        """两段 TS 直接首尾相接：第二段的 PTS 从 ~0 重新开始，拼接点是一次显著回退。

        两段内容刻意不同且可判：前段 testsrc2（逐帧变化），后段 smptebars（静止画面）。
        后段若被错误地映射到已经上屏的网格上，读回的前半段会混进大段逐字节相同的静止帧。
        """
        tmp = path.parent
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={CFR_FPS}",
               "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
               "-pix_fmt", "yuv420p", "-g", CFR_FPS, "-f", "mpegts", tmp / "seg1.ts")
        ffmpeg("-f", "lavfi", "-i", f"smptebars=size={W}x{H}:rate={CFR_FPS}",
               "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
               "-pix_fmt", "yuv420p", "-g", CFR_FPS, "-f", "mpegts", tmp / "seg2.ts")
        path.write_bytes((tmp / "seg1.ts").read_bytes() + (tmp / "seg2.ts").read_bytes())
        return path

    @staticmethod
    def info(path):
        """首条视频流的 ffprobe JSON；读不到时为 None。"""
        run = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries",
             "stream=nb_frames,duration,r_frame_rate,avg_frame_rate:format=duration",
             "-of", "json", str(path)],
            capture_output=True, text=True, timeout=180)
        if run.returncode != 0:
            return None
        info = json.loads(run.stdout or "{}")
        return info if info.get("streams") else None

    @staticmethod
    def pts_ordered(path):
        """按解复用顺序（文件里真实的呈现时间戳序列）返回到帧 PTS，秒。

        与 pts() 不同：不排序，所以拼接流里那次向后跳尚在（判据就是它）。
        """
        run = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "frame=pts_time", "-of", "csv=p=0", str(path)],
            capture_output=True, text=True, timeout=180)
        assert run.returncode == 0, f"{path.name}: ffprobe frame dump failed"
        vals = []
        for line in run.stdout.splitlines():
            field = line.split(",")[0].strip()
            if field:
                vals.append(float(field))
        assert vals, f"{path.name}: no frame timestamps"
        return vals

    @staticmethod
    def pts(path):
        """按呈现顺序（PTS 升序）排好的绝对时间戳，秒。"""
        return sorted(Media.pts_ordered(path))

    @classmethod
    def headers(cls, path):
        """容器声明（Decoder::open 的取法）。"""
        info = cls.info(path)
        assert info, f"{path.name}: ffprobe found no video stream"
        stream = info["streams"][0]
        duration = stream.get("duration") or info.get("format", {}).get("duration")
        return {
            "duration": float(duration),
            "nb_frames": int(stream.get("nb_frames") or 0),
            "r_rate": _rate(stream.get("r_frame_rate")),
            "avg_rate": _rate(stream.get("avg_frame_rate")),
        }

    @classmethod
    def deltas(cls, path):
        pts = cls.pts(path)
        return [round(pts[i + 1] - pts[i], 4) for i in range(len(pts) - 1)]


def _grid_index(seconds, fps):
    """秒 -> 固定 fps 网格帧号（同 C++ 的 llround；正数下取 floor(x+0.5)）。"""
    return int(math.floor(seconds * fps + 0.5))


class Timeline:
    """一张素材的物理帧 → 固定 fps 时间网格表（判据的唯一事实来源）。

    帧号 = 时间网格；物理帧 i 覆盖网格区间 [anchors[i], anchors[i+1])，最后一张铺到时间轴
    末尾。colours[i] 是该物理帧的 luma（纯色素材，可直接判像素）。

    VFR 素材会多出收尾帧（x264 不写最后一包时长，见 Media._vfr）：它的 anchor 落在时间轴
    之外，anchors[i] >= grid 的物理帧永远不会上屏。colour_of 给素材自定义 luma 序列时用。
    """

    def __init__(self, path, fps=None, colour_of=None):
        hdr = Media.headers(path)
        self.path = path
        self.duration = hdr["duration"]
        self.physical = hdr["nb_frames"] or len(Media.pts(path))
        self.r_rate = hdr["r_rate"]
        self.avg_rate = hdr["avg_rate"]
        self.fps = float(fps or self.r_rate)
        self.grid = max(1, _grid_index(self.duration, self.fps))

        pts = Media.pts(path)
        origin = pts[0]
        self.anchors = [_grid_index(p - origin, self.fps) for p in pts]
        self.holds = [self.anchors[i + 1] - self.anchors[i]
                      for i in range(len(self.anchors) - 1)]
        self.holds.append(max(1, self.grid - self.anchors[-1]))
        self.colours = [Y_PALETTE[i % len(Y_PALETTE)] for i in range(len(self.anchors))]
        if colour_of is not None:
            self.colours = [colour_of(i) for i in range(len(self.anchors))]

    @property
    def visible(self):
        """落在时间轴内的物理帧序号（收尾帧不算）。"""
        return [i for i, a in enumerate(self.anchors) if a < self.grid]

    @property
    def max_hold(self):
        return max(self.holds)

    def picture_at(self, frame):
        return max(0, bisect_right(self.anchors, frame) - 1)

    def colour_at(self, frame):
        return self.colours[self.picture_at(frame)]

    def pictures_between(self, lo, hi):
        """[lo, hi] 网格区间覆盖到的物理帧序号。"""
        return range(self.picture_at(lo), self.picture_at(hi) + 1)


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

    def step(self, allow_paused=False):
        """allow_paused 只给「自己主动 pause 之后再取样」的窗口用，其余路径仍守不早停。"""
        self.player.pump_messages()
        self.player.ui_tick()
        stats = self.player.stats()
        frame = int(stats["current_frame"])
        if frame < self.frame:
            raise AssertionError(f"{self.label}: clock went backwards {self.frame} -> {frame}")
        # 末尾暂停时 playing_ 先落、clock_frame_ 后落，留几帧容差。
        if not allow_paused and frame < self.declared - 3 and not stats["is_playing"]:
            raise AssertionError(f"{self.label}: paused early at frame {frame}/{self.declared}")
        self.frame = frame
        return stats

    def reset(self):
        """seek 是 reposition：之后的时钟不是回退。"""
        self.frame = int(self.player.current_frame())
        return self.frame

    def run(self, seconds, allow_paused=False):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.step(allow_paused)
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


def fresh_frames(rows):
    """窗口内实际 fresh 上屏的帧号集合（pass-through fresh；本测试不推 AI 帧）。"""
    return {frame for source, frame in rows if source == FRESH}


def read_y_plane(player, frame_num, timeout_s=0.6):
    """按帧号取 GPU NV12（区间查找），返回 Y 平面 clone；取不到返回 None。

    原生那张 CUDA 缓冲是单份复用内存，所以每次都必须 clone 出来才能比。
    """
    global _wrap_nv12
    if _wrap_nv12 is None:
        from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor
        _wrap_nv12 = wrap_nv12_cuda_buffer_as_tensor

    until = time.monotonic() + timeout_s
    while time.monotonic() < until:
        g = player.get_cuda_nv12_by_frame(frame_num)
        if g.get("ready"):
            y = _wrap_nv12(g["dev_ptr"], g["width"], g["height"], g["pitch_bytes"])
            y = y[:g["height"]].clone()
            player.pump_messages()
            player.ui_tick()
            return y
        player.pump_messages()
        player.ui_tick()
        time.sleep(0.01)
    return None


def probe_run(player, first, max_frames):
    """从 first 起连续按帧号读回 Y 平面，读到第一格未就绪为止。"""
    out = {}
    for n in range(first, first + max_frames):
        y = read_y_plane(player, n)
        if y is None:
            break
        out[n] = y
    return out


def mean_of(y):
    import torch
    return float(torch.mean(y.float()).item())


def same_bytes(a, b):
    import torch
    return bool(torch.equal(a, b))


def runs_of(planes):
    """连续帧按内容（逐字节相同）聚成持帧区间，返回 [(lo, hi, 均值)]。"""
    runs = []
    for n in sorted(planes):
        if runs and same_bytes(planes[n], runs[-1][2]):
            runs[-1][1] = n
        else:
            runs.append([n, n, planes[n]])
    return [(lo, hi, mean_of(plane)) for lo, hi, plane in runs]


def check_media(media):
    """校验素材与判据里的数字（dry-run 也跑这一段）。"""
    vfr = Timeline(media.vfr)
    tiers = sorted(set(Media.deltas(media.vfr)))
    assert len(tiers) >= 3, (tiers, VFR_DELTAS)
    assert vfr.r_rate / vfr.avg_rate > 1.5, (vfr.r_rate, vfr.avg_rate)
    assert abs(vfr.grid - vfr.physical) > 0.4 * vfr.grid, (vfr.grid, vfr.physical)
    assert set(vfr.holds) >= {1, 2, 3}, sorted(set(vfr.holds))
    # 时间轴上的相邻物理帧必须不同色；收尾帧落在时间轴之外（画面上与上一张同色，由生成端保证）。
    assert all(vfr.colours[i] != vfr.colours[i + 1] for i in vfr.visible[:-1])
    assert vfr.anchors[-1] >= vfr.grid
    assert len(vfr.visible) == vfr.physical - 1
    print(f"PASS: vfr mp4 is really variable-rate -- deltas {tiers}s, r_frame_rate "
          f"{vfr.r_rate:g} vs avg {vfr.avg_rate:.2f}, {vfr.physical} physical frames "
          f"({len(vfr.visible)} of them on the {vfr.grid}-frame grid, {vfr.duration:.2f}s), "
          f"holds {sorted(set(vfr.holds))} grid frames", flush=True)

    cfr = Timeline(media.cfr)
    assert set(Media.deltas(media.cfr)) == {round(1.0 / CFR_FPS, 4)}, Media.deltas(media.cfr)
    assert abs(cfr.grid - cfr.physical) <= 1, (cfr.grid, cfr.physical)
    assert set(cfr.holds) == {1}, sorted(set(cfr.holds))
    print(f"PASS: cfr mp4 is a plain {CFR_FPS}fps clip -- {cfr.physical} frames, grid "
          f"{cfr.grid}, every grid frame is its own picture", flush=True)

    short = Timeline(media.short)
    assert 4 <= short.grid <= 40, short.grid
    assert short.physical == SHORT_PICS + 1, short.physical  # 含收尾帧
    print(f"PASS: short vfr mp4 is {short.duration:.2f}s -- {short.physical - 1} physical frames "
          f"on a {short.grid}-frame grid", flush=True)

    # 长持帧：首帧自己声明 10s（持帧区间的可信来源就是它的 duration），整条时间轴 14s @30fps。
    long_hold = Timeline(media.long_hold, colour_of=_long_hold_colour)
    times = Media.frame_times(media.long_hold)
    assert abs(times[0][1] - LONG_HOLD_S) < 0.02, times[0]
    assert abs(times[1][0] - times[0][0] - LONG_HOLD_S) < 0.02, times[:2]
    assert abs(long_hold.duration - (LONG_HOLD_S + LONG_HOLD_TAIL_PICS / CFR_FPS)) < 0.05, \
        long_hold.duration
    assert abs(long_hold.fps - CFR_FPS) <= 0.01, long_hold.fps
    assert long_hold.holds[0] >= 290, long_hold.holds[0]
    assert len(long_hold.visible) == LONG_HOLD_TAIL_PICS + 1, len(long_hold.visible)
    assert long_hold.physical != long_hold.grid, (long_hold.physical, long_hold.grid)
    assert all(long_hold.colours[i] != long_hold.colours[i + 1]
               for i in long_hold.visible[:-1])
    print(f"PASS: long-hold mp4 -- the first picture declares {times[0][1]:.2f}s and owns grid "
          f"frames 0-{long_hold.holds[0] - 1} of a {long_hold.grid}-frame timeline; "
          f"{len(long_hold.visible)} pictures total over {long_hold.duration:.2f}s "
          f"(container claims {long_hold.physical} samples)", flush=True)

    # 拼接流：解复用顺序里必须真的有一次显著向后跳（这是异常 PTS 的实物证据），且容器初始
    # 只声明第一段时长——原生分段策略必须随后按真实图片扩展，而不是停在初始声明末尾。
    joined_pts = Media.pts_ordered(media.joined)
    jumps = [(i, joined_pts[i - 1] - joined_pts[i]) for i in range(1, len(joined_pts))
             if joined_pts[i] < joined_pts[i - 1]]
    assert len(jumps) == 1 and jumps[0][1] > 1.0, jumps
    real_span = len(joined_pts) / CFR_FPS
    declared = Media.headers(media.joined)["duration"]
    assert declared < 0.75 * real_span, (declared, real_span)
    print(f"PASS: joined ts really restarts its timestamps -- 1 backward jump of "
          f"{jumps[0][1]:.2f}s after frame {jumps[0][0]} of {len(joined_pts)}; container "
          f"declares {declared:.2f}s while the two segments hold {real_span:.2f}s of "
          f"{CFR_FPS}fps content", flush=True)


def case_timeline(player, media):
    """时间轴长度来自 duration*fps，不是物理 nb_frames。"""
    player.open(str(media.vfr))
    hdr = Media.headers(media.vfr)
    fps = player.source_fps()
    assert abs(fps - hdr["r_rate"]) <= 0.01 * hdr["r_rate"], \
        f"player fps {fps} != container r_frame_rate {hdr['r_rate']}: this clip only stresses " \
        f"the duration*fps rule when the player lays the timeline on the max-rate grid"
    timeline = Timeline(media.vfr, fps)
    declared = player.frame_count()
    assert abs(declared - timeline.grid) <= 2, (
        f"frame_count()={declared} but duration*fps={timeline.grid} "
        f"({timeline.duration:.2f}s x {fps:g}fps): the timeline length must come from the media "
        f"duration, not from the physical nb_frames={timeline.physical}")
    assert abs(declared - timeline.physical) > 0.4 * declared, (declared, timeline.physical)
    assert player.stats()["decode_frame_count"] > 0
    print(f"PASS: vfr timeline is {declared} grid frames (duration {timeline.duration:.2f}s x "
          f"{fps:g}fps) while the file holds {timeline.physical} physical frames", flush=True)
    player.close_current_session()
    return timeline


def case_full(player, trace, timeline, media):
    """全时长：播到时间轴末尾才暂停，末尾那张图铺满自己的持帧区间。"""
    player.open(str(media.vfr))
    play = Play(player, "vfr full")
    assert play.declared == timeline.grid, (play.declared, timeline.grid)
    trace.delta()
    started = time.monotonic()
    player.play()
    frame = play.wait_end(timeline.duration + 15)
    elapsed = time.monotonic() - started
    assert frame >= play.declared - 2, (frame, play.declared)
    wall = play.declared / timeline.fps
    assert abs(elapsed - wall) < 1.5, (elapsed, wall)
    fresh = fresh_frames(trace.delta())
    assert len(fresh) >= 0.55 * play.declared, (len(fresh), play.declared)

    # 末尾那张图必须铺到时间轴末尾（最后一段持帧区间不是缺损）。
    tail_frame = play.declared - 2
    y = read_y_plane(player, tail_frame)
    assert y is not None, \
        f"grid frame {tail_frame} never read back: the last picture must cover the tail"
    tail_mean = mean_of(y)
    expected = timeline.colour_at(tail_frame)
    assert abs(tail_mean - expected) <= 1.0, (tail_mean, expected, tail_frame)

    # 停住之后 present 仍在走，时钟冻结在末尾。
    present = player.stats()["present_count"]
    play.run(0.3)
    assert player.stats()["present_count"] > present, "present stopped while paused at the end"
    assert not player.stats()["is_playing"] and play.frame >= play.declared - 2
    print(f"PASS: vfr played the whole timeline in {elapsed:.2f}s (grid {wall:.2f}s), fresh "
          f"frames {len(fresh)}/{play.declared}, paused at {frame}, tail picture (luma "
          f"{tail_mean:.0f} == {expected}) covers frame {tail_frame}", flush=True)
    player.close_current_session()


def case_holds(player, timeline, media):
    """同一持帧区间读到的 CUDA 内容一致、换区间就变——按帧号区间查找，不看解码进度。"""
    player.open(str(media.vfr))
    play = Play(player, "vfr holds")
    player.seek(0)
    play.reset()
    player.play()
    assert play.until(lambda: play.frame >= 40, 40 / timeline.fps + 15), \
        f"vfr holds: clock stuck at {play.frame}"
    player.pause()
    play.run(0.05, allow_paused=True)  # 主动暂停后再取样：这里暂停是预期状态

    window = max(10, min(40, player.ring_capacity() - 16))
    planes = probe_run(player, play.frame, window)
    assert len(planes) >= 10, (
        f"only {len(planes)} of the {window} grid frames from {play.frame} on could be read "
        f"back by frame number: a hold must stay readable for every grid frame it covers")

    frames = sorted(planes)
    for n in frames:
        measured = mean_of(planes[n])
        expected = timeline.colour_at(n)
        assert abs(measured - expected) <= 1.0, (n, measured, expected)
    runs = runs_of(planes)
    for lo, hi, _ in runs:  # 同一区间逐字节一致
        for n in range(lo, hi + 1):
            assert same_bytes(planes[lo], planes[n]), (lo, n)
    for (_, _, a), (_, _, b) in zip(runs, runs[1:]):  # 换区间内容就变
        assert abs(a - b) >= 20.0, (a, b)
    expected_runs = len(timeline.pictures_between(frames[0], frames[-1]))
    assert len(runs) == expected_runs, (len(runs), expected_runs, frames[0], frames[-1])
    assert any(hi > lo for lo, hi, _ in runs), runs  # 真间隙被 hold 住，不是跳过
    assert any(hi == lo for lo, hi, _ in runs), runs  # 快区段一格一张图

    # 同一帧号读两次：同一份 CUDA 内容（单缓冲复用，clone 之后才比得出来）。
    mid = frames[len(frames) // 2]
    again = read_y_plane(player, mid)
    assert again is not None and same_bytes(again, planes[mid]), \
        f"reading grid frame {mid} twice gave different content"

    hold_max = player.stats().get("pt_hold_max")  # 原生 VFR 诊断（可选）：最长持帧区间
    if hold_max is not None:
        assert 2 <= hold_max <= timeline.max_hold, (hold_max, timeline.max_hold)

    view = " ".join(f"{lo}-{hi}:{m:.0f}" for lo, hi, m in runs[:8])
    print(f"PASS: vfr frames {frames[0]}-{frames[-1]} resolve to {len(runs)} held pictures "
          f"(expected {expected_runs}); same interval byte-identical, neighbour differs -- "
          f"{view}", flush=True)
    player.close_current_session()


def case_seek(player, timeline, media):
    """seek 落到目标时间附近，读回的内容就是该网格帧所在的那张图。"""
    player.open(str(media.vfr))
    play = Play(player, "vfr seek")
    tol = timeline.max_hold + 2
    targets = [timeline.grid // 4, timeline.grid // 2,
               (timeline.grid * 7) // 8, timeline.grid - 6]
    landed_list = []
    for target in targets:
        landed = player.seek(target)
        assert abs(landed - target) <= tol, (target, landed, tol)
        play.reset()
        assert abs(play.frame - landed) <= 1, (landed, play.frame)
        y = read_y_plane(player, landed)
        assert y is not None, f"seek landed at {landed} but that frame never read back"
        measured, expected = mean_of(y), timeline.colour_at(landed)
        assert abs(measured - expected) <= 1.0, (landed, measured, expected)
        player.play()
        assert play.until(lambda: play.frame > landed + 3, 3.0), \
            f"clock did not resume after seek to {target} (landed {landed})"
        player.pause()
        landed_list.append(landed)
    print(f"PASS: vfr seek landed {landed_list} for targets {targets} (max hold "
          f"{timeline.max_hold} grid frames) and every landed frame held the expected picture",
          flush=True)
    player.close_current_session()


def case_long_hold(player, media):
    """长持帧（首图 10s）与解码前沿共存：解到 360 时 270 仍须读出首图的真实像素。

    旧实现按 frame_num % ring_capacity 直接选槽：320x180 环深 180，首图占槽 0，解到 360 正好
    落回槽 0，把还在 present head 持帧区间内的首图覆盖掉。判据只看暂停后读回的 Y 像素。
    """
    player.open(str(media.long_hold))
    timeline = Timeline(media.long_hold, player.source_fps(), colour_of=_long_hold_colour)
    play = Play(player, "long hold")
    declared = play.declared
    assert abs(declared - timeline.grid) <= 2, (
        f"frame_count()={declared} but duration*fps={timeline.grid}: the timeline must come from "
        f"the container duration, not from nb_frames={timeline.physical}")
    hold = timeline.holds[0]
    cap = int(player.ring_capacity())
    first_pic = timeline.anchors[1]
    conflict = ((first_pic + cap - 1) // cap) * cap  # 第一个撞回首图槽位的网格帧
    assert hold >= 290, hold

    # 第 6 秒还在首图里：seek 必须落在这张图的持帧区间内，不能跳到 10s 处那张图。
    landed = player.seek(180)
    play.reset()
    assert 170 <= landed <= 200, (
        f"seek(180) landed at {landed}: a target inside a {hold}-frame hold must stay inside "
        f"that picture instead of jumping to the next one")
    player.play()
    assert play.until(lambda: play.frame >= 210, 15.0), \
        f"long hold: clock stuck at {play.frame}"
    player.pause()
    play.run(0.05, allow_paused=True)

    # 等解码前沿越过冲突点（旧实现就在这一步覆盖首图槽位）。暂停之后读取才不受环滚动打扰。
    probe_frame = conflict + 10
    probe = None
    deadline = time.monotonic() + 25.0
    while time.monotonic() < deadline:
        probe = read_y_plane(player, probe_frame, 0.25)
        if probe is not None:
            break
        play.run(0.1, allow_paused=True)
    assert probe is not None, (
        f"the decode head never reached grid frame {probe_frame} within 25s "
        f"(paused at frame {play.frame})")

    head = play.frame
    assert head < hold, (head, hold)
    hold_max = player.stats().get("pt_hold_max")
    if hold_max is not None:
        assert hold_max >= hold, (hold_max, hold)

    hold_means = []
    for n in (head, 270):
        y = read_y_plane(player, n)
        assert y is not None, (
            f"grid frame {n} is not readable any more (head {head}, hold 0-{hold - 1}, decode "
            f"already passed {probe_frame}): the decode head overwrote the held picture's slot")
        measured = mean_of(y)
        assert abs(measured - LONG_HOLD_HEAD) <= 1.0, (n, measured, head)
        hold_means.append(measured)

    # 后段每格都是它自己那张图（既没被首图盖掉，彼此也没串）。
    tail_means = {}
    for n in (conflict, probe_frame, probe_frame + 1, probe_frame + 2):
        y = read_y_plane(player, n)
        assert y is not None, f"grid frame {n} (already decoded) is not readable"
        measured, expected = mean_of(y), timeline.colour_at(n)
        assert abs(measured - expected) <= 1.0, (n, measured, expected)
        tail_means[n] = measured
    consecutive = [round(tail_means[probe_frame + k]) for k in range(3)]
    assert len(set(consecutive)) == len(consecutive), consecutive

    print(f"PASS: long hold -- seek(180) landed at {landed} inside the {hold}-frame hold; with "
          f"the decode head past {conflict} (frame {probe_frame} ready) the held picture still "
          f"reads back at head {head} and at grid 270 (luma {hold_means[0]:.0f}/"
          f"{hold_means[1]:.0f} == {LONG_HOLD_HEAD}), and frames "
          f"{conflict}/{probe_frame}+ still hold their own colours {consecutive}", flush=True)
    player.close_current_session()


def case_fps_div(player, timeline, media):
    """fps_div=2：dense 网格减半，时长不变，播完仍停在自己的末尾。"""
    player.set_fps_div(2)
    try:
        player.open(str(media.vfr))
        fps = player.fps()
        declared = player.frame_count()
        expected = math.ceil(timeline.grid / 2)
        assert abs(fps - timeline.fps / 2) <= 0.01 * timeline.fps, (fps, timeline.fps)
        assert abs(declared - expected) <= 2, (
            f"fps_div=2 frame_count()={declared} but the halved grid is {expected}: the retimed "
            f"timeline must still be duration*fps/2, not nb_frames/2 "
            f"({timeline.physical / 2:g})")
        play = Play(player, "vfr fps_div=2")
        started = time.monotonic()
        player.play()
        frame = play.wait_end(timeline.duration + 15)
        elapsed = time.monotonic() - started
        assert frame >= declared - 2, (frame, declared)
        assert abs(elapsed - timeline.duration) < 1.5, (elapsed, timeline.duration)
        target = declared // 2
        landed = player.seek(target)
        assert abs(landed - target) <= timeline.max_hold // 2 + 3, (landed, target)
        print(f"PASS: vfr at fps_div=2 is {declared} frames @ {fps:g}fps, played "
              f"{elapsed:.2f}s and paused at {frame}, seek({target}) -> {landed}", flush=True)
        player.close_current_session()
    finally:
        player.set_fps_div(1)


def case_cfr(player, trace, media):
    """CFR 回归：每格一张图，持帧逻辑不能把正常帧卡住或凭空 hold。"""
    player.open(str(media.cfr))
    timeline = Timeline(media.cfr, player.source_fps())
    play = Play(player, "cfr")
    player.play()
    assert play.until(lambda: play.frame >= 5, 15.0), f"cfr: clock stuck at {play.frame}"
    trace.delta()
    assert play.until(lambda: play.frame >= 5 + CFR_FPS, 5.0), \
        f"cfr: clock stalled at {play.frame}"
    rows = trace.delta()
    player.pause()
    play.run(0.05, allow_paused=True)  # 主动暂停后再取样：这里暂停是预期状态

    fresh = fresh_frames(rows)
    assert len(fresh) >= 0.5 * CFR_FPS, (len(fresh), CFR_FPS)

    planes = probe_run(player, play.frame, max(10, min(24, player.ring_capacity() - 16)))
    assert len(planes) >= 10, (len(planes), play.frame)
    runs = runs_of(planes)
    assert all(hi - lo <= 1 for lo, hi, _ in runs), runs  # 没有凭空 hold
    for n in sorted(planes):
        assert abs(mean_of(planes[n]) - timeline.colour_at(n)) <= 1.0, n
    for (_, _, a), (_, _, b) in zip(runs, runs[1:]):
        assert abs(a - b) >= 20.0, (a, b)
    hold_max = player.stats().get("pt_hold_max")
    if hold_max is not None:
        assert hold_max == 1, hold_max  # CFR：一格一张图，不该有持帧区间
    assert player.seek(0) <= 2
    print(f"PASS: cfr regression -- {play.declared} frames, {len(fresh)} fresh frames in the "
          f"last second, {len(runs)} distinct pictures over frames {min(planes)}-{max(planes)} "
          f"with no phantom hold", flush=True)
    player.close_current_session()


def case_short(player, media):
    """短视频：极短时间轴也要能播到末尾、能 seek 到最后一帧。"""
    player.open(str(media.short))
    timeline = Timeline(media.short, player.source_fps())
    play = Play(player, "short vfr")
    assert play.declared == timeline.grid, (play.declared, timeline.grid)
    player.play()
    frame = play.wait_end(timeline.duration + 10)
    assert frame >= play.declared - 2, (frame, play.declared)
    last = player.frame_count() - 1
    landed = player.seek(last)
    assert abs(landed - last) <= timeline.max_hold, (landed, last)
    assert read_y_plane(player, last) is not None, "short vfr: last frame unreadable"
    assert player.seek(0) <= 2
    print(f"PASS: short vfr ({timeline.duration:.2f}s, {play.declared} grid frames) paused at "
          f"{frame}, seek({last}) -> {landed}, seek(0) ok", flush=True)
    player.close_current_session()


def case_joined(player, media):
    """拼接流：回退 PTS 接到前段之后，真实分段扩展总长，前后两段都完整播放。"""
    player.open(str(media.joined))
    fps = player.fps()
    hdr = Media.headers(media.joined)
    initial_declared = player.frame_count()
    assert abs(initial_declared - hdr["duration"] * fps) <= 3, (
        initial_declared, hdr["duration"], fps)
    play = Play(player, "joined ts")
    player.play()
    frame = play.wait_end(initial_declared / fps + 15)
    player.pause()
    play.run(0.05)
    final_declared = player.frame_count()
    assert final_declared >= initial_declared * 1.8, (initial_declared, final_declared)
    assert frame >= final_declared - 2, (frame, final_declared)

    # 第一段逐帧变化，第二段是静止彩条。两段都能按各自扩展后的帧号读回，且回退 PTS 没有把
    # 第二段写到第一段的网格上。跨段精确 seek 仍由容器索引能力决定，这里只验证连续播放。
    planes = probe_run(player, 0, min(final_declared, player.ring_capacity() - 16))
    assert len(planes) >= final_declared - 2, (len(planes), final_declared)
    runs = runs_of(planes)
    first_runs = [(lo, hi, m) for lo, hi, m in runs if hi < initial_declared]
    second_runs = [(lo, hi, m) for lo, hi, m in runs if lo >= initial_declared]
    assert first_runs and second_runs, [(lo, hi, m) for lo, hi, m in runs]
    assert max(hi - lo + 1 for lo, hi, _ in first_runs) <= 2, first_runs
    second_means = [mean_of(planes[n]) for n in sorted(planes) if n >= initial_declared]
    assert len(second_means) >= 50 and max(second_means) - min(second_means) < 0.25, (
        len(second_means), min(second_means), max(second_means))
    assert player.seek(initial_declared // 2) <= initial_declared // 2 + 2
    print(f"PASS: joined ts expanded {initial_declared} -> {final_declared} grid frames and "
          f"paused at {frame}; first segment stays distinct, second segment is present as its own "
          f"held content instead of overwriting the first", flush=True)
    player.close_current_session()


def main():
    faulthandler.enable()
    dry_only = "--dry-run" in sys.argv[1:]
    (ROOT / "scripts" / "trace").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sumu-vfr-", dir=ROOT / "scripts" / "trace") as tmp:
        tmp = Path(tmp)
        media = Media(tmp)
        check_media(media)
        if dry_only:
            print("dry run: media, PTS table and grid criteria checks passed "
                  "(no player opened)")
            return
        sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu")]
        # 先把 torch 起起来（DLPack 包装要用），再建 Player：与 app.py 的既有顺序一致。
        import torch  # noqa: F401
        import sumu_core

        player = sumu_core.Player(640, 400)
        try:
            trace = Trace(player, tmp / "present.csv")
            timeline = case_timeline(player, media)
            case_full(player, trace, timeline, media)
            case_holds(player, timeline, media)
            case_seek(player, timeline, media)
            case_long_hold(player, media)
            case_cfr(player, trace, media)
            case_short(player, media)
            case_joined(player, media)
            case_fps_div(player, timeline, media)
            player.open(str(media.vfr))
            player.play()
            for _ in range(50):
                player.pump_messages()
                player.ui_tick()
                time.sleep(0.01)
            assert player.current_frame() > 0, player.stats()
            print("PASS: normal playback after the VFR cases", flush=True)
        finally:
            player.close()
    print("All playback-timeline (VFR) checks passed.")


if __name__ == "__main__":
    main()
