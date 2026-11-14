# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Decoder 架构改造的真实端到端验证：软解回退 / 通用像素格式 / 流中分辨率变化 /
# HLS VOD 与滑动 live 的「暂时没有新数据」不能当 EOF。素材全部由本机 ffmpeg 现场生成，
# 走真实的 sumu_core.HeadlessDecode 与 sumu_core.Player，不只看 import 或函数签名。
#
#   .venv\Scripts\python.exe scripts/verify_decoder_compatibility.py --dry-run
#       只生成素材 + ffprobe/ffmpeg 自证 + 本地 HLS 服务自检，不 import sumu_core。
#   .venv\Scripts\python.exe scripts/verify_decoder_compatibility.py
#       全量；--only mpeg4_part2,resize_headless 可只跑指定 case；--list 列出 case。
#
# 覆盖的 case（每个都能单独跑，每个异常 case 结束后都重开普通 H.264 证明恢复）：
#   mpeg4_part2      MPEG-4 Part 2 / yuv420p：HeadlessDecode 解码 + Player 上屏。所有格式
#                    case 都要求两件事：解出的 Y 平面与 FFmpeg 独立解码逐帧一致（画面真的是
#                    源画面，不是「有点变化」），且相邻帧内容随时间推进。
#   h264_422         H.264 High 4:2:2 / yuv422p：旧版 open 直接拒绝，改造后必须软解成功。
#   h264_444         H.264 High 4:4:4 / yuv444p：同上。
#   h264_10bit       H.264 High 10 / yuv420p10le：本机 x264 是 10-bit 构建就生成并验证；
#                    不是则明确标「未覆盖」，最终汇总不会打印 ALL CHECKS PASSED。
#   resize_headless  同一条流里 320x180 后切 480x270（两条 TS 用 concat demuxer 合成单流，
#                    ffprobe 逐帧证明尺寸确实在流内变化）：HeadlessDecode 必须继续返回
#                    新尺寸，而不是一直报旧尺寸。
#   resize_player    Player 在切换点之后：dims() 变新尺寸、stats()['config_generation']
#                    递增（基线取在打开之后、播放之前，中途不 seek，排除 seek 的自增）、
#                    播放与 present 继续。
#   hls_vod          本地 HLS VOD（TS 分片 + ENDLIST）真实 Player 打开并推进到结尾。
#   hls_live         本地滑动窗口 HLS live：服务端逐步发布分片，中途冻结 playlist 并对分片
#                    短暂回 503，验证 Player 不提前 Ended/停止（时钟照走、present 心跳不断）、
#                    恢复发布后 fresh 帧重新出现并越过冻结前的位置。live 没有完整总长，
#                    因此不按 VOD 的 frame_count 断言。
#
# 边界：不覆盖真实公网丢包/时间抖动、DRM、AI 去码与心跳的并发、以及多轨道/多节目 TS。
# 服务只绑 127.0.0.1 随机端口；素材与 present trace 落在 scripts/trace 下的临时目录，
# 除非 --keep，退出即清理。
#
# 原生崩溃（访问冲突/堆损坏）会直接终结进程 —— 那样整个 run 立即结束、后面的 case 不会跑，
# 也不会有汇总行（stdout 行缓冲，崩溃前已经打印的 case 就是最后跑到的那一步）。要单独复跑
# 某个 case 用 --only，例如 --only resize_player。
#
# 退出码：0 = 全部已覆盖的 case 通过；1 = 有 FAIL；2 = 无 FAIL 但有未覆盖的 case
# （本机编码器造不出来），此时汇总行明确写 PASSED BUT NOT FULLY COVERED。
import argparse
import faulthandler
import json
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
TRACE_DIR = ROOT / "scripts" / "trace"

W, H = 320, 180
W2, H2 = 480, 270
FPS = 30
PART_S = 2.0                    # 分辨率切换素材每一半的长度
NORMAL_S = 3.0                  # 恢复用普通 H.264 的长度
HLS_VOD_S = 4.0
LIVE_SEG_N = 14                 # live 素材总段数（1s 一段）
LIVE_SEG_S = 1
LIVE_WINDOW = 5                 # playlist 里保留的分片数（真实 live 的滑动窗口）
LIVE_PUBLISH_S = 0.5            # 发布节拍：快于 1x，播放器才有余量
FREEZE_S = 4.0                  # playlist 不再出现新分片的时长
REJECT_S = 2.5                  # 分片回 503 的时长（落在冻结窗口内）
FRESH = 0                       # Source::PassthroughFresh
STALE = 2                       # Source::PassthroughStale

_failures = []
_uncovered = {}


class Uncovered(Exception):
    """素材在本机造不出来（例如 x264 不是 10-bit 构建）：不是失败，但也不许假装覆盖了。"""


def check(name, cond, detail=""):
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
    if not cond:
        _failures.append(name)
    return cond


def note(text):
    print(f"[note] {text}")


def mark_uncovered(name, reason):
    _uncovered[name] = reason
    print(f"[UNCOVERED] {name} -- {reason}")


# ============================================================================================
# ffmpeg / ffprobe
# ============================================================================================

def ffmpeg_try(*args, timeout=300):
    return subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                           *map(str, args)], capture_output=True, text=True, timeout=timeout)


def ffmpeg(*args, timeout=300):
    run = ffmpeg_try(*args, timeout=timeout)
    if run.returncode != 0:
        raise RuntimeError(f"ffmpeg failed ({run.returncode}): {run.stderr.strip()[-400:]}")
    return run


def ffprobe_json(*args, timeout=180):
    run = subprocess.run(["ffprobe", "-v", "error", "-of", "json", *map(str, args)],
                         capture_output=True, text=True, timeout=timeout)
    if run.returncode != 0:
        raise RuntimeError(f"ffprobe failed: {run.stderr.strip()[-400:]}")
    return json.loads(run.stdout or "{}")


def probe_stream(path):
    info = ffprobe_json("-select_streams", "v:0", "-show_entries",
                        "stream=codec_name,profile,pix_fmt,width,height,nb_frames,"
                        "avg_frame_rate,duration:format=duration", path)
    streams = info.get("streams") or []
    if not streams:
        raise RuntimeError(f"{path}: ffprobe found no video stream")
    return streams[0]


def video_stream_count(path):
    info = ffprobe_json("-show_entries", "stream=index,codec_type", path)
    return sum(1 for s in info.get("streams", []) if s.get("codec_type") == "video")


def count_frames(path):
    info = ffprobe_json("-select_streams", "v:0", "-count_frames", "-show_entries",
                        "stream=nb_read_frames", path)
    return int((info.get("streams") or [{}])[0].get("nb_read_frames") or 0)


def probe_frames(path):
    """逐帧 (pts_seconds, width, height)。分辨率变化素材的判据就靠它。"""
    info = ffprobe_json("-select_streams", "v:0", "-show_entries",
                        "frame=width,height,pts_time", path)
    frames = []
    for frame in info.get("frames", []):
        pts = frame.get("pts_time")
        frames.append((float(pts) if pts not in (None, "N/A") else None,
                       int(frame.get("width") or 0), int(frame.get("height") or 0)))
    return frames


class Materials:
    """临时目录里的全部素材；生成时就按判据需要自证（见 check_materials）。"""

    def __init__(self, tmp):
        self.tmp = Path(tmp)
        normal, normal_error = self._encode_mp4(
            self.tmp / "normal_h264_yuv420p.mp4", W, H, NORMAL_S, "yuv420p", "high")
        if normal is None:
            raise RuntimeError(
                f"cannot encode the recovery material (H.264 yuv420p, always expected to work): "
                f"{normal_error}")
        self.normal = normal

        self.mpeg4 = self.tmp / "mpeg4_part2_yuv420p.mp4"
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={FPS}", "-t", PART_S,
               "-c:v", "mpeg4", "-q:v", "3", "-pix_fmt", "yuv420p", "-g", FPS // 2,
               "-movflags", "+faststart", self.mpeg4)

        # 422 / 444 / 10bit 都按「真的编码出来 + ffprobe 认这个 pix_fmt」为准，不猜能力表。
        self.h264_422, self.err_422 = self._encode_mp4(
            self.tmp / "h264_high422_yuv422p.mp4", W, H, PART_S, "yuv422p", "high422")
        self.h264_444, self.err_444 = self._encode_mp4(
            self.tmp / "h264_high444_yuv444p.mp4", W, H, PART_S, "yuv444p", "high444")
        self.h264_10bit, self.err_10bit = self._encode_mp4(
            self.tmp / "h264_high10_yuv420p10le.mp4", W, H, PART_S, "yuv420p10le", "high10")

        self.resize = self.tmp / "resize_midstream.ts"
        self._build_resize()

        self.hls_vod_dir = self.tmp / "hls_vod"
        self.hls_vod_dir.mkdir()
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={FPS}", "-t", HLS_VOD_S,
               "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-g", FPS,
               "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
               "-hls_segment_filename", self.hls_vod_dir / "seg%03d.ts",
               self.hls_vod_dir / "index.m3u8")

        self.hls_live_dir = self.tmp / "hls_live"
        self.hls_live_dir.mkdir()
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={FPS}",
               "-t", LIVE_SEG_N * LIVE_SEG_S, "-c:v", "libx264", "-preset", "veryfast",
               "-pix_fmt", "yuv420p", "-g", FPS, "-f", "hls", "-hls_time", str(LIVE_SEG_S),
               "-hls_list_size", "0", "-hls_segment_filename", self.hls_live_dir / "seg%03d.ts",
               self.hls_live_dir / "live_vod.m3u8")
        self.live_segments = sorted(p.name for p in self.hls_live_dir.glob("seg*.ts"))

    @staticmethod
    def _encode_mp4(path, w, h, seconds, pix_fmt, profile):
        """编一份 H.264；成功返回 (path, None)，本机编码器做不出来返回 (None, 原因)。"""
        run = ffmpeg_try("-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={FPS}",
                         "-t", seconds, "-c:v", "libx264", "-preset", "veryfast",
                         "-pix_fmt", pix_fmt, "-profile:v", profile, "-g", FPS // 2,
                         "-movflags", "+faststart", path)
        if run.returncode != 0:
            tail = (run.stderr or "").strip().splitlines()
            return None, (tail[-1] if tail else "libx264 encode failed")
        stream = probe_stream(path)
        got = (stream.get("pix_fmt"), stream.get("width"), stream.get("height"))
        if got != (pix_fmt, w, h):
            return None, f"encoder accepted the options but wrote {got}, want {(pix_fmt, w, h)}"
        return path, None

    def _ts_part(self, path, w, h, offset):
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={FPS}", "-t", PART_S,
               "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
               "-g", FPS // 2,
               # 每个关键帧都带 SPS/PPS：单流里第二段的新尺寸必须能在流内被解出来。
               "-bsf:v", "dump_extra",
               # mpegts 默认有 1.4s 的 muxdelay，关掉才能让 PTS 由 -output_ts_offset 精确控制。
               "-muxdelay", "0", "-muxpreload", "0",
               "-output_ts_offset", f"{offset:.6f}", "-f", "mpegts", path)

    def _build_resize(self):
        """两段不同尺寸的 TS 用 concat demuxer 合成一条连续流：唯一的视频流里尺寸真的变了。"""
        part_a = self.tmp / "resize_a.ts"
        part_b = self.tmp / "resize_b.ts"
        self._ts_part(part_a, W, H, 0.0)
        a_last = probe_frames(part_a)[-1][0]
        want = a_last + 1.0 / FPS              # 第二段第一帧紧接第一段最后一帧
        self._ts_part(part_b, W2, H2, want)
        got = probe_frames(part_b)[0][0]
        if abs(got - want) > 0.5 / FPS:
            # -output_ts_offset 的语义在 ffmpeg 版本间有过变化：按实测差值纠正一次，
            # 结论文档由 check_materials 的逐帧 PTS 断言给出，而不是靠这里的推算。
            self._ts_part(part_b, W2, H2, want + (want - got))
        concat_list = self.tmp / "resize_concat.txt"
        concat_list.write_text(f"file '{part_a.name}'\nfile '{part_b.name}'\n", encoding="utf-8")
        ffmpeg("-f", "concat", "-safe", "0", "-i", concat_list, "-c", "copy",
               "-f", "mpegts", self.resize)


def check_materials(media):
    """素材自证（--dry-run 也跑这一段）：所有判据里的「事实」都在这里钉死。"""
    normal = probe_stream(media.normal)
    assert (normal["codec_name"], normal["pix_fmt"], normal["width"], normal["height"]) == \
        ("h264", "yuv420p", W, H), normal
    assert count_frames(media.normal) == round(NORMAL_S * FPS), count_frames(media.normal)
    print(f"PASS: normal H.264 {W}x{H} {count_frames(media.normal)} frames is the recovery "
          f"material", flush=True)

    mpeg4 = probe_stream(media.mpeg4)
    assert (mpeg4["codec_name"], mpeg4["pix_fmt"]) == ("mpeg4", "yuv420p"), mpeg4
    assert count_frames(media.mpeg4) == round(PART_S * FPS), count_frames(media.mpeg4)
    print(f"PASS: mpeg4 Part 2 / yuv420p {count_frames(media.mpeg4)} frames", flush=True)

    for label, path, want, err in (
            ("h264 high422 / yuv422p", media.h264_422, "yuv422p", media.err_422),
            ("h264 high444 / yuv444p", media.h264_444, "yuv444p", media.err_444)):
        assert path is not None, f"{label}: 本机 ffmpeg 造不出这份素材：{err}"
        stream = probe_stream(path)
        assert (stream["pix_fmt"], count_frames(path)) == (want, round(PART_S * FPS)), stream
        print(f"PASS: {label} {count_frames(path)} frames (profile {stream.get('profile')})",
              flush=True)

    if media.h264_10bit is None:
        mark_uncovered("h264_10bit",
                       f"本机 libx264 不是 10-bit 构建，无法生成 yuv420p10le：{media.err_10bit}")
    else:
        stream = probe_stream(media.h264_10bit)
        assert (stream["pix_fmt"], count_frames(media.h264_10bit)) == \
            ("yuv420p10le", round(PART_S * FPS)), stream
        print(f"PASS: h264 high10 / yuv420p10le {count_frames(media.h264_10bit)} frames",
              flush=True)

    # 分辨率变化素材：一条流、120 帧、前 60 帧 320x180 后 60 帧 480x270、PTS 单调且连续。
    assert video_stream_count(media.resize) == 1, \
        f"resize: {video_stream_count(media.resize)} video streams, want exactly 1"
    frames = probe_frames(media.resize)
    assert len(frames) == 2 * round(PART_S * FPS), f"resize: {len(frames)} frames"
    half = round(PART_S * FPS)
    dims = [(w, h) for _, w, h in frames]
    assert set(dims[:half]) == {(W, H)}, f"resize: first half dims {set(dims[:half])}"
    assert set(dims[half:]) == {(W2, H2)}, f"resize: second half dims {set(dims[half:])}"
    pts = [p for p, _, _ in frames]
    assert all(p is not None for p in pts), "resize: missing PTS"
    steps = [b - a for a, b in zip(pts, pts[1:])]
    assert all(step > 0 for step in steps), f"resize: PTS not monotone, min step {min(steps)}"
    assert abs(max(steps) - 1.0 / FPS) < 1e-3, f"resize: max PTS step {max(steps)}"
    print(f"PASS: mid-stream resolution change is inside one stream: 60x{W}x{H} then "
          f"60x{W2}x{H2}, PTS {pts[0]:.3f}..{pts[-1]:.3f} step {max(steps):.4f}", flush=True)

    # 独立参照：FFmpeg 自己解这条流不出错（native 的 d3d11va 路径结果只作参考，见下）。
    sw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-i", media.resize,
                         "-f", "null", "-"], capture_output=True, text=True, timeout=180)
    assert sw.returncode == 0 and "corrupt" not in sw.stderr.lower(), \
        f"resize: plain FFmpeg decode complained: {sw.stderr.strip()[-300:]}"
    print("PASS: plain FFmpeg decodes the mid-stream switch without complaints", flush=True)
    hw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-hwaccel",
                         "d3d11va", "-i", media.resize, "-f", "null", "-"],
                        capture_output=True, text=True, timeout=180)
    note("FFmpeg's own d3d11va decode of the switch: "
         + ("clean" if hw.returncode == 0 and "corrupt" not in hw.stderr.lower()
            else f"complained ({hw.stderr.strip().splitlines()[-1:] or hw.returncode}); the "
                 "software backend is the documented fallback, so this only informs the report"))

    # HLS VOD：ENDLIST + 有完整总长。
    playlist = (media.hls_vod_dir / "index.m3u8").read_text(encoding="utf-8")
    assert "#EXT-X-ENDLIST" in playlist, "hls vod: playlist has no ENDLIST"
    vod_frames = count_frames(media.hls_vod_dir / "index.m3u8")
    assert vod_frames == round(HLS_VOD_S * FPS), vod_frames
    print(f"PASS: HLS VOD playlist has ENDLIST and {vod_frames} decodable frames", flush=True)

    # Live 素材：够多段、每段是一秒、单片是合法 TS。
    assert len(media.live_segments) == LIVE_SEG_N, len(media.live_segments)
    first_seg = media.hls_live_dir / media.live_segments[0]
    assert first_seg.read_bytes()[:1] == b"\x47", "live segment does not start with TS sync byte"
    seg_duration = float(ffprobe_json("-show_entries", "format=duration",
                                      first_seg)["format"]["duration"])
    assert abs(seg_duration - LIVE_SEG_S) < 0.15, seg_duration
    print(f"PASS: {len(media.live_segments)} live segments of ~{seg_duration:.2f}s each",
          flush=True)


# ============================================================================================
# 本地 HLS 服务（只绑 127.0.0.1）
# ============================================================================================

class FileServer:
    """HLS VOD 用的静态文件服务：Range 不需要，分片整段取即可。"""

    def __init__(self, root):
        self.root = Path(root).resolve()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *args):
                pass

            def do_GET(self):  # noqa: N802
                outer._serve(self)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       name="verify-decoder-hls-vod", daemon=True)
        self.thread.start()

    def url(self, name=""):
        return f"http://127.0.0.1:{self.httpd.server_address[1]}/{name}"

    def _serve(self, handler):
        name = unquote(urlparse(handler.path).path).lstrip("/")
        path = (self.root / name).resolve()
        if not name or not str(path).startswith(str(self.root)) or not path.is_file():
            handler.send_response(404)
            handler.send_header("Content-Length", "0")
            handler.end_headers()
            return
        data = path.read_bytes()
        handler.send_response(200)
        handler.send_header("Content-Type", "video/mp2t" if name.endswith(".ts")
                            else "application/vnd.apple.mpegurl")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class LiveServer:
    """滑动窗口 HLS live：playlist 由内存状态生成，分片是磁盘上的静态 TS。

    状态机刻意做成真实 live 的形状：窗口保持 LIVE_WINDOW 段，发布新段时丢掉最旧的一段；
    freeze() 期间 playlist 内容完全不再变化（服务端真相），reject() 期间分片请求回 503。
    """

    def __init__(self, root, names):
        self.root = Path(root).resolve()
        self.names = list(names)
        self.lock = threading.Lock()
        self.first = 0                       # EXT-X-MEDIA-SEQUENCE
        self.published = min(LIVE_WINDOW, len(self.names))
        self.frozen_until = 0.0
        self.reject_until = 0.0
        self.last_publish = 0.0
        self.requests = []                   # (monotonic, kind, name, status)
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *args):
                pass

            def do_GET(self):  # noqa: N802
                outer._serve(self)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       name="verify-decoder-hls-live", daemon=True)
        self.thread.start()

    def url(self):
        return f"http://127.0.0.1:{self.httpd.server_address[1]}/live.m3u8"

    # ---- 状态 ------------------------------------------------------------------------------
    def publish(self, count=1):
        """滑动发布 count 段：窗口跟着前进，媒体序列号递增（真实 live 的样子）。"""
        with self.lock:
            for _ in range(count):
                if self.published >= len(self.names):
                    return False
                self.published += 1
                if self.published - self.first > LIVE_WINDOW:
                    self.first = self.published - LIVE_WINDOW
            return True

    def publish_if_due(self, interval=LIVE_PUBLISH_S):
        now = time.monotonic()
        with self.lock:
            if now < self.frozen_until or now - self.last_publish < interval:
                return False
            self.last_publish = now
        return self.publish(1)

    def freeze(self, seconds):
        with self.lock:
            self.frozen_until = max(self.frozen_until, time.monotonic() + seconds)
            self.last_publish = self.frozen_until

    def reject(self, seconds):
        with self.lock:
            self.reject_until = max(self.reject_until, time.monotonic() + seconds)

    def snapshot(self):
        with self.lock:
            return self.first, self.published

    def requests_since(self, since):
        with self.lock:
            return [row for row in self.requests if row[0] >= since]

    def playlist(self):
        first, published = self.snapshot()
        lines = ["#EXTM3U", "#EXT-X-VERSION:3",
                 f"#EXT-X-TARGETDURATION:{LIVE_SEG_S}", f"#EXT-X-MEDIA-SEQUENCE:{first}"]
        for name in self.names[first:published]:
            lines += [f"#EXTINF:{LIVE_SEG_S}.000000,", name]
        return ("\n".join(lines) + "\n").encode()

    # ---- HTTP ------------------------------------------------------------------------------
    def _serve(self, handler):
        name = unquote(urlparse(handler.path).path).lstrip("/")
        now = time.monotonic()
        if name.endswith(".m3u8"):
            body = self.playlist()
            with self.lock:
                self.requests.append((now, "playlist", name, 200))
            handler.send_response(200)
            handler.send_header("Content-Type", "application/vnd.apple.mpegurl")
            handler.send_header("Content-Length", str(len(body)))
            handler.end_headers()
            handler.wfile.write(body)
            return
        path = (self.root / name).resolve()
        with self.lock:
            rejecting = now < self.reject_until
        if rejecting:
            with self.lock:
                self.requests.append((now, "segment", name, 503))
            handler.send_response(503)
            handler.send_header("Retry-After", "1")
            handler.send_header("Content-Length", "0")
            handler.end_headers()
            return
        if not str(path).startswith(str(self.root)) or not path.is_file():
            with self.lock:
                self.requests.append((now, "segment", name, 404))
            handler.send_response(404)
            handler.send_header("Content-Length", "0")
            handler.end_headers()
            return
        data = path.read_bytes()
        with self.lock:
            self.requests.append((now, "segment", name, 200))
        handler.send_response(200)
        handler.send_header("Content-Type", "video/mp2t")
        handler.send_header("Content-Length", str(len(data)))
        handler.end_headers()
        handler.wfile.write(data)

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def check_servers(media):
    """本地服务自检（--dry-run 也跑）：206/分片可取、滑动、冻结、503 都真的会发生。"""
    import urllib.error
    import urllib.request

    vod = FileServer(media.hls_vod_dir)
    try:
        with urllib.request.urlopen(vod.url("index.m3u8"), timeout=10) as resp:
            assert resp.status == 200 and b"#EXT-X-ENDLIST" in resp.read(), resp.status
        probe = ffprobe_json("-show_entries", "format=duration", vod.url("index.m3u8"))
        assert float(probe["format"]["duration"]) > 3.0, probe
        vod_segment = sorted(p.name for p in media.hls_vod_dir.glob("seg*.ts"))[0]
        with urllib.request.urlopen(vod.url(vod_segment), timeout=10) as resp:
            assert resp.read(1) == b"\x47", "vod server did not return TS bytes"
        print("PASS: local HLS VOD server serves the playlist and segments (ffprobe reads it)",
              flush=True)
    finally:
        vod.close()

    live = LiveServer(media.hls_live_dir, media.live_segments)
    try:
        def fetch_playlist():
            with urllib.request.urlopen(live.url(), timeout=10) as resp:
                assert resp.status == 200, resp.status
                return resp.read().decode()

        body = fetch_playlist()
        assert "#EXT-X-ENDLIST" not in body, "live playlist must not declare ENDLIST"
        segs = [line for line in body.splitlines() if line.endswith(".ts")]
        assert len(segs) == LIVE_WINDOW, segs
        seq_before = live.snapshot()
        assert live.publish(1), "live server ran out of segments"
        body2 = fetch_playlist()
        segs2 = [line for line in body2.splitlines() if line.endswith(".ts")]
        assert len(segs2) == LIVE_WINDOW and segs2[0] != segs[0], (segs, segs2)
        assert live.snapshot()[0] == seq_before[0] + 1, (seq_before, live.snapshot())
        live.freeze(1.0)
        frozen = live.snapshot()
        assert not live.publish_if_due(0.0), "publish_if_due ignored the freeze"
        assert live.snapshot() == frozen, (frozen, live.snapshot())
        live.reject(1.5)
        try:
            urllib.request.urlopen(live.url().replace("live.m3u8", media.live_segments[-1]),
                                  timeout=10)
            raise AssertionError("503 mode did not reject a segment")
        except urllib.error.HTTPError as exc:
            assert exc.code == 503, exc.code
        finally:
            with live.lock:
                live.frozen_until = 0.0
                live.reject_until = 0.0
        # 真实的 FFmpeg 客户端能从这条滑动 playlist 里读出帧：这是素材+服务可用性的硬证据。
        run = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", live.url(),
                              "-frames:v", "5", "-f", "null", "-"],
                             capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, f"ffmpeg could not read the live playlist: {run.stderr[-300:]}"
        print("PASS: local HLS live server slides its playlist, freezes on demand, 503s a "
              "segment, and FFmpeg reads frames from it", flush=True)
    finally:
        live.close()


# ============================================================================================
# sumu 驱动
# ============================================================================================

class Trace:
    """present trace 的增量视图（qpc_ns,source,frame_num）。"""

    def __init__(self, player, path):
        self.player = player
        self.path = path
        self.seen = 0

    def delta(self):
        self.player.dump_present_trace(str(self.path))
        rows = []
        for line in self.path.read_text(encoding="utf-8").splitlines()[1:]:
            _, source, frame = line.rsplit(",", 2)
            rows.append((int(source), int(frame)))
        new = rows[self.seen:] if len(rows) >= self.seen else rows
        self.seen = len(rows)
        return new


class Play:
    """每 tick 守两条不变量：时钟只进不退、已知总长时不得提前暂停。"""

    def __init__(self, player, label):
        self.player = player
        self.label = label
        self.declared = player.frame_count()
        self.frame = int(player.current_frame())

    def step(self):
        self.player.pump_messages()
        self.player.ui_tick()
        stats = self.player.stats()
        frame = int(stats["current_frame"])
        if frame < self.frame - 3:  # 重配置/重连后允许一个 tick 的发布滞后
            raise AssertionError(f"{self.label}: clock went backwards {self.frame} -> {frame}")
        if self.declared > 0 and frame < self.declared - 3 and not stats["is_playing"]:
            raise AssertionError(
                f"{self.label}: playback stopped early at frame {frame}/{self.declared}")
        self.frame = frame
        return stats

    def run(self, seconds, tick=None):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if tick:
                tick()
            self.step()
            time.sleep(0.01)

    def until(self, done, timeout, tick=None):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if tick:
                tick()
            self.step()
            if done():
                return True
            time.sleep(0.01)
        return False

    def wait_end(self, timeout, tick=None):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if tick:
                tick()
            if not self.step()["is_playing"]:
                self.run(0.15, tick)
                return self.frame
            time.sleep(0.01)
        raise AssertionError(f"{self.label}: still playing at {self.frame}/{self.declared}")


class Window:
    """一段时间里的 stats 采样 + 这段 present trace 的归属。"""

    def __init__(self, label, samples, rows):
        self.label = label
        self.samples = samples
        self.rows = rows

    @property
    def seconds(self):
        return self.samples[-1][0] - self.samples[0][0] if len(self.samples) > 1 else 0.0

    @property
    def clock_span(self):
        return int(self.samples[-1][1]["current_frame"]) - int(self.samples[0][1]["current_frame"])

    def fresh(self):
        return {frame for source, frame in self.rows if source == FRESH}

    def stale(self):
        return sum(1 for source, _ in self.rows if source == STALE)

    def assert_heartbeat(self):
        for (t0, a), (t1, b) in zip(self.samples, self.samples[1:]):
            assert b["present_count"] > a["present_count"], (
                f"{self.label}: present stopped between {t0:.2f}s and {t1:.2f}s")

    def assert_playing(self):
        for when, stats in self.samples:
            assert stats["is_playing"], (
                f"{self.label}: playback stopped at {when:.2f}s "
                f"(frame {stats['current_frame']}) -- a temporarily silent source must not end "
                f"the session")

    def assert_clock_running(self, ratio=0.7):
        assert self.clock_span >= ratio * FPS * self.seconds, (
            f"{self.label}: clock advanced only {self.clock_span} frames in "
            f"{self.seconds:.2f}s -- playback was suspended, not just starved of pictures")


def sample(play, trace, label, seconds, tick=None, step=0.1):
    started = time.monotonic()
    samples = []
    while time.monotonic() - started < seconds:
        if tick:
            tick()
        samples.append((time.monotonic() - started, dict(play.step())))
        time.sleep(step)
    return Window(label, samples, trace.delta())


def luma_stats(a, b):
    """相邻两帧的差异证据：(平均绝对差, 变化像素占比, 最大差)。"""
    diff = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return float(diff.mean()), float((diff > 4).mean()), float(diff.max())


def ffmpeg_luma_frames(path, width, height, count):
    """独立参照：FFmpeg 自己把同一份素材解成 I420，取每帧的 Y 平面。

    sumu 的解码契约本来就是 display-oriented BT.709 limited 8-bit NV12，源素材也是
    limited-range，所以两边 Y 平面可以直接比（硬件直通路径实测逐像素完全一致）。
    """
    run = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-frames:v", str(count),
                          "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"],
                         capture_output=True, timeout=180)
    if run.returncode != 0:
        raise RuntimeError(f"ffmpeg reference decode failed: {run.stderr[-300:]!r}")
    frame_bytes = width * height * 3 // 2
    usable = len(run.stdout) // frame_bytes * frame_bytes
    if usable == 0:
        raise RuntimeError("ffmpeg reference decode produced no complete frames")
    planes = np.frombuffer(run.stdout[:usable], np.uint8).reshape(-1, frame_bytes)
    return planes[:, :width * height].reshape(-1, height, width)


class HeadlessScan:
    """HeadlessDecode 解完一份素材后留下的证据：尺寸/帧号/Y 平面。

    只保留 Y 平面（一份 480x270 的百来帧约十几 MB），因为「帧内容随时间推进」必须用
    真实像素差证明，掉采样或近似哈希都可能在低对比运动上给出假阴性。"""

    def __init__(self, path, max_frames=200):
        import sumu_core
        from sumu.ai.utils.cuda_dlpack import wrap_nv12_cuda_buffer_as_tensor

        self.path = path
        self.frames = []
        self.eof = False
        decoder = sumu_core.HeadlessDecode()
        try:
            decoder.open(str(path))
            self.fps = float(decoder.fps())
            self.frame_count = int(decoder.frame_count())
            self.open_dims = (int(decoder.width()), int(decoder.height()))
            for _ in range(max_frames):
                got = decoder.next_frame()
                if got.get("eof"):
                    self.eof = True
                    break
                width, height = int(got["width"]), int(got["height"])
                plane = wrap_nv12_cuda_buffer_as_tensor(
                    int(got["dev_ptr"]), width, height, int(got["pitch_bytes"]))
                luma = plane[:height, :width].cpu().numpy().copy()
                self.frames.append({
                    "w": width, "h": height,
                    "frame_num": int(got["frame_num"]),
                    "pts": float(got["pts_seconds"]),
                    "mean": float(luma.mean()),
                    "y": luma,
                })
        finally:
            decoder.close()

    # ---- 判据 ------------------------------------------------------------------------------
    def assert_frames(self, label, minimum):
        assert len(self.frames) >= minimum, (
            f"{label}: decoded only {len(self.frames)} frames, want >= {minimum} "
            f"(open reported {self.open_dims}, frame_count={self.frame_count})")

    def assert_dims(self, label, dims):
        got = {(frame["w"], frame["h"]) for frame in self.frames}
        assert got == {tuple(dims)}, f"{label}: decoded frames report {sorted(got)}, want {dims}"

    def source_mad(self, tolerance=3.0):
        """画面必须是源画面：和 FFmpeg 独立解出的 Y 平面逐帧比对，返回 (median, worst)。

        只看「帧在动」会漏掉「动的不是源画面」——例如软解上传把行距算错，画面会在垂直
        方向被压成一条，帧与帧之间照样有一点点变化。硬件直通路径实测是逐像素一致
        （MAD 0.00），所以 3 个码值的容差已经足够宽。
        """
        first = self.frames[0]
        reference = ffmpeg_luma_frames(self.path, first["w"], first["h"], len(self.frames))
        mads = []
        for index, frame in enumerate(self.frames):
            candidates = [float(np.abs(frame["y"].astype(np.int16)
                                      - reference[j].astype(np.int16)).mean())
                          for j in (index - 1, index, index + 1)
                          if 0 <= j < len(reference)]
            mads.append(min(candidates))
        mads.sort()
        return mads[len(mads) // 2], mads[-1]

    def assert_matches_source(self, label, tolerance=3.0):
        median, worst = self.source_mad(tolerance)
        assert median <= tolerance, (
            f"{label}: the decoded picture is not the source picture -- median MAD "
            f"{median:.2f} codes against an independent FFmpeg decode of the same frames "
            f"(worst {worst:.2f}, want <= {tolerance}). The material is BT.709 limited-range "
            f"8-bit 4:2:0, i.e. exactly the documented output contract, so it must come through "
            f"untouched; a non-zero MAD here means the normalize/upload path changed the pixels")
        return median

    def assert_clock(self, label):
        numbers = [frame["frame_num"] for frame in self.frames]
        assert all(b >= a for a, b in zip(numbers, numbers[1:])), (
            f"{label}: frame numbers are not monotone: {numbers[:8]}..")
        assert numbers[-1] - numbers[0] >= len(numbers) - 2, (
            f"{label}: frame numbers stalled ({numbers[0]}..{numbers[-1]} over {len(numbers)} "
            f"frames)")

    def pair_stats(self):
        """同一尺寸的相邻帧差异；尺寸切换处（resize 素材）自然跳过。"""
        stats = []
        for a, b in zip(self.frames, self.frames[1:]):
            if (a["w"], a["h"]) != (b["w"], b["h"]):
                continue
            stats.append(luma_stats(a["y"], b["y"]))
        return stats

    def movement(self):
        """(变化过的相邻帧对数, 总对数, 相邻 MAD 中位数, 首末帧 MAD)。"""
        stats = self.pair_stats()
        moving = sum(1 for mad, frac, _ in stats if mad >= 0.2 or frac >= 0.002)
        typical = sorted(mad for mad, _, _ in stats)[len(stats) // 2] if stats else 0.0
        same_dims = ((self.frames[0]["w"], self.frames[0]["h"]) ==
                     (self.frames[-1]["w"], self.frames[-1]["h"]))
        span = (luma_stats(self.frames[0]["y"], self.frames[-1]["y"])[0]
                if same_dims else 0.0)
        return moving, len(stats), typical, span

    def assert_content_advances(self, label, min_moving=0.5):
        moving, total, typical, span = self.movement()
        assert total >= 10 and moving >= min_moving * total and span >= 0.2, (
            f"{label}: picture content did not advance over time "
            f"({moving}/{total} adjacent frame pairs changed; first..last MAD {span:.2f}; "
            f"median pair MAD {typical:.2f}; luma means "
            f"{[round(f['mean'], 1) for f in self.frames[:3]]}.."
            f"{[round(f['mean'], 1) for f in self.frames[-2:]]})")

    def dims_sequence(self):
        return [(frame["w"], frame["h"]) for frame in self.frames]

    def summary(self):
        moving, total, typical, _ = self.movement()
        sizes = {}
        for frame in self.frames:
            dims = (frame["w"], frame["h"])
            sizes[dims] = sizes.get(dims, 0) + 1
        size_text = " + ".join(f"{count}x{w}x{h}" for (w, h), count in sizes.items())
        numbers = (f"frame_num {self.frames[0]['frame_num']}..{self.frames[-1]['frame_num']}"
                   if self.frames else "no frames")
        return (f"{len(self.frames)} frames [{size_text}], {numbers}, "
                f"{moving}/{max(1, total)} adjacent pairs changed (median MAD "
                f"{typical:.2f} codes)")


def config_generation(player):
    """stats()['config_generation']：解复用/解码配置（尺寸、色度、后端）变化时必须递增。"""
    stats = player.stats()
    value = stats.get("config_generation")
    return int(value) if value is not None else None


def source_state(stats):
    value = stats.get("source_state")
    return None if value is None else int(value)


# Decoder::SourceState 的序号（native/src/decoder.h）。只用于报告里给读的人做提示，
# 判据本身一律看 is_playing / 时钟 / dims 这些行为证据，不依赖枚举顺序。
SOURCE_STATE_NAMES = ("Opening", "Buffering", "Ready", "Starved", "Reconfiguring",
                      "Recovering", "Ended", "Failed")


def source_state_text(stats):
    value = source_state(stats)
    if value is None:
        return "n/a"
    name = SOURCE_STATE_NAMES[value] if 0 <= value < len(SOURCE_STATE_NAMES) else "?"
    return f"{value}({name})"


class Ctx:
    """一次运行共享的 Player / 本地服务；每个 case 自己负责关掉上一份 session。"""

    def __init__(self, media, tmp):
        self.media = media
        self.tmp = tmp
        self.player = None
        self.trace = None
        self.session_open = False
        self._vod = None
        self._live = None

    # ---- Player ---------------------------------------------------------------------------
    def get_player(self):
        if self.player is None:
            import sumu_core
            self.player = sumu_core.Player(640, 400)
            self.trace = Trace(self.player, self.tmp / "present.csv")
            for _ in range(5):
                self.player.pump_messages()
                self.player.ui_tick()
                time.sleep(0.01)
        return self.player

    def open(self, path):
        player = self.get_player()
        if self.session_open:
            self.close_session()
        player.open(str(path))
        self.session_open = True
        return player

    def close_session(self):
        player = self.player
        if player is None or not self.session_open:
            return
        try:
            player.close_current_session()
            for _ in range(5):
                player.pump_messages()
                player.ui_tick()
                time.sleep(0.01)
        finally:
            self.session_open = False

    def close_session_quiet(self):
        try:
            self.close_session()
        except Exception as exc:  # noqa: BLE001 - 收尾失败不该盖住真正的判据失败
            note(f"close_current_session failed while cleaning up: {exc}")

    # ---- 本地服务 -------------------------------------------------------------------------
    def vod_server(self):
        if self._vod is None:
            self._vod = FileServer(self.media.hls_vod_dir)
        return self._vod

    def live_server(self):
        if self._live is None:
            self._live = LiveServer(self.media.hls_live_dir, self.media.live_segments)
        return self._live

    def close(self):
        self.close_session_quiet()
        for server in (self._vod, self._live):
            if server is not None:
                try:
                    server.close()
                except Exception:  # noqa: BLE001
                    pass
        self._vod = self._live = None
        if self.player is not None:
            try:
                self.player.close()
            except Exception:  # noqa: BLE001
                pass
            self.player = None


# ---- 恢复证明：每个异常场景之后都必须重开普通 H.264 -----------------------------------------

def recovery_headless(ctx, label):
    name = f"recovery after {label}: plain H.264 decodes again"
    try:
        scan = HeadlessScan(ctx.media.normal, max_frames=12)
        scan.assert_frames(name, 10)
        scan.assert_dims(name, (W, H))
        scan.assert_matches_source(name)
        scan.assert_content_advances(name, min_moving=0.5)
    except Exception as exc:  # noqa: BLE001 - 恢复失败是一条判据，不能盖住原始失败
        check(name, False, f"{type(exc).__name__}: {exc}")
        return False
    check(name, True, f"{len(scan.frames)} frames {scan.open_dims}")
    return True


def recovery_player(ctx, label):
    name = f"recovery after {label}: plain H.264 plays again"
    try:
        player = ctx.open(ctx.media.normal)
        play = Play(player, name)
        player.play()
        play.run(1.2)
        stats = player.stats()
        assert play.frame >= 12, (
            f"playback did not advance ({play.frame} frames in 1.2s)")
        assert stats["present_count"] > 0, "nothing was presented"
        assert tuple(player.dims()) == (W, H), f"dims {player.dims()}, want {(W, H)}"
        player.pause()
        ctx.close_session()
    except Exception as exc:  # noqa: BLE001
        check(name, False, f"{type(exc).__name__}: {exc}")
        ctx.close_session_quiet()
        return False
    check(name, True, f"current_frame={play.frame} present={stats['present_count']}")
    return True


def player_playback(ctx, path, label, seconds=1.2, min_advance=12):
    """普通播放：打开 -> play -> 时钟真的走了、present 一直在。返回 stats 供报告。"""
    player = ctx.open(path)
    play = Play(player, label)
    player.play()
    play.run(seconds)
    stats = player.stats()
    assert play.frame >= min_advance, (
        f"{label}: clock advanced only {play.frame} frames in {seconds}s")
    assert stats["present_count"] > 0, f"{label}: present_count stayed 0"
    assert tuple(player.dims()) == (W, H), f"{label}: dims {player.dims()}, want {(W, H)}"
    player.pause()
    ctx.close_session()
    return stats


# ============================================================================================
# cases
# ============================================================================================

def _format_case(ctx, path, label, missing_reason):
    if path is None:
        raise AssertionError(f"{label}: 本机 ffmpeg 造不出这份素材：{missing_reason}")
    scan = HeadlessScan(path, max_frames=80)
    scan.assert_frames(label, 55)
    scan.assert_dims(label, (W, H))
    scan.assert_clock(label)
    median_mad, worst_mad = scan.source_mad()
    moving, total, typical, span = scan.movement()
    check(f"{label}: decoded picture is the source picture",
          median_mad <= 3.0,
          f"median MAD {median_mad:.2f} codes vs FFmpeg's own decode (worst {worst_mad:.2f}, "
          f"want <= 3.0)")
    check(f"{label}: picture content advances over time",
          total >= 10 and moving >= 0.5 * total and span >= 0.2,
          f"{moving}/{total} adjacent pairs changed (median MAD {typical:.2f}, first..last "
          f"{span:.2f} codes)")
    try:
        player_stats = player_playback(ctx, path, f"{label} (Player)")
    finally:
        # 恢复证明必须跑：即使上面的播放判据失败，也要看普通 H.264 还能不能放。
        recovery_headless(ctx, label)
        recovery_player(ctx, f"{label} (Player)")
    return f"HeadlessDecode {scan.summary()}; Player current_frame advanced, " \
           f"present={player_stats['present_count']}"


def case_mpeg4_part2(ctx):
    return _format_case(ctx, ctx.media.mpeg4, "mpeg4 Part 2 / yuv420p", None)


def case_h264_422(ctx):
    return _format_case(ctx, ctx.media.h264_422, "h264 High 4:2:2 / yuv422p", ctx.media.err_422)


def case_h264_444(ctx):
    return _format_case(ctx, ctx.media.h264_444, "h264 High 4:4:4 / yuv444p", ctx.media.err_444)


def case_h264_10bit(ctx):
    if ctx.media.h264_10bit is None:
        raise Uncovered(
            f"本机 libx264 无法生成 yuv420p10le（{ctx.media.err_10bit}）：10-bit 420 的软解"
            f"路径本次未覆盖，换一台 10-bit x264 的机器再跑这个 case")
    return _format_case(ctx, ctx.media.h264_10bit, "h264 High 10 / yuv420p10le",
                        ctx.media.err_10bit)


def case_resize_headless(ctx):
    try:
        scan = HeadlessScan(ctx.media.resize, max_frames=140)
        dims = scan.dims_sequence()
        half = round(PART_S * FPS)
        if len(scan.frames) < 2 * half - 5:
            raise AssertionError(
                f"resize: HeadlessDecode stopped after {len(scan.frames)}/{2 * half} frames "
                f"(eof={scan.eof}, last frame {dims[-1] if dims else None}) -- the decode "
                f"session ended at the mid-stream resolution change instead of adopting the "
                f"new size")
        old = set(dims[:half])
        new = set(dims[half:])
        assert old == {(W, H)}, (
            f"resize: the first half decoded as {sorted(old)}, want {(W, H)}")
        assert new == {(W2, H2)}, (
            f"resize: HeadlessDecode reported {sorted(new | old)} around the switch to "
            f"{W2}x{H2} at frame {half} -- the decoder did not adopt the new size; observed "
            f"{len(dims)} frames, first switch at "
            f"{next((i for i, d in enumerate(dims) if d != dims[0]), None)}")
        scan.assert_clock("resize")
    finally:
        recovery_headless(ctx, "resize")
    return f"{scan.summary()}; dims switch at frame " \
           f"{next(i for i, d in enumerate(dims) if d != dims[0])} from {(W, H)} to {(W2, H2)}"


def pump_until(player, done, timeout):
    """只推进 present/UI，不守「提前暂停」不变量：用于收集重配置卡住的现场证据。"""
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        player.pump_messages()
        player.ui_tick()
        if done():
            return True
        time.sleep(0.02)
    return False


def case_resize_player(ctx):
    try:
        return _resize_player_checks(ctx)
    finally:
        # 无论判据结果如何，都要看普通 H.264 还能不能放（异常场景后的恢复证明）。
        ctx.close_session_quiet()
        recovery_headless(ctx, "resize player")
        recovery_player(ctx, "resize player")


def _resize_player_checks(ctx):
    player = ctx.open(ctx.media.resize)
    assert tuple(player.dims()) == (W, H), (
        f"resize player: first half dims {player.dims()}, want {(W, H)}")
    # 基线取在刚打开、还没播放的时候：中途不 seek，config_generation 之后的自增就只能来自
    # 「解码配置变了」这一件事（seek 也会自增，不能让它污染这一判据）。
    gen_before = config_generation(player)
    assert gen_before is not None, (
        "resize player: Player::stats() has no 'config_generation' -- the reconfigure path this "
        "case verifies is not exposed yet; got keys "
        f"{sorted(player.stats())}")
    dims_before = tuple(player.dims())
    declared = player.frame_count()
    switch_frame = round(PART_S * FPS)

    player.play()
    # 跨过切换点（再留 ~20 帧余量）。这里不强求播放不中断，让下面的三条判据各自给证据：
    # 卡死在 Reconfiguring 时会表现为「尺寸/代数变了但时钟不走」，比一个笼统的超时有用。
    crossed = pump_until(
        player, lambda: int(player.stats()["current_frame"]) >= switch_frame + 20, 12.0)
    stats = player.stats()
    dims_after = tuple(player.dims())
    gen_after = config_generation(player)

    check("resize player: dims follow the mid-stream resolution change",
          dims_before == (W, H) and dims_after == (W2, H2),
          f"{dims_before} -> {dims_after}, want ({W, H}) -> ({W2, H2})")
    check("resize player: config_generation increases on the reconfiguration",
          gen_after is not None and gen_after > gen_before,
          f"config_generation {gen_before} -> {gen_after} (no seek in between; the switch must "
          f"add at least one)")
    check("resize player: playback continues on the new size",
          bool(stats["is_playing"]) and int(stats["current_frame"]) >= switch_frame + 15,
          f"crossed={crossed} current_frame={stats['current_frame']}/{declared} "
          f"is_playing={stats['is_playing']} decode_frame_count={stats['decode_frame_count']} "
          f"n_pt_fresh={stats['n_pt_fresh']} n_pt_stale={stats['n_pt_stale']} "
          f"source_state={source_state_text(stats)} dims={dims_after}")

    before_present = stats["present_count"]
    pump_until(player, lambda: False, 0.5)
    check("resize player: present keeps ticking after the switch",
          player.stats()["present_count"] > before_present,
          f"{before_present} -> {player.stats()['present_count']}")
    player.pause()
    ctx.close_session()
    direct = ctx.open(ctx.media.resize)
    direct_gen = config_generation(direct)
    direct_landed = direct.seek(switch_frame + 20)
    pump_until(direct, lambda: False, 0.4)
    check("resize player: seek directly into the new size reconfigures too",
          tuple(direct.dims()) == (W2, H2) and
          config_generation(direct) is not None and config_generation(direct) > direct_gen and
          abs(int(direct.current_frame()) - int(direct_landed)) <= 1,
          f"landed={direct_landed} current={direct.current_frame()} dims={direct.dims()} "
          f"generation={direct_gen}->{config_generation(direct)}")
    return (f"dims {dims_before}->{dims_after}, config_generation {gen_before}->{gen_after}, "
            f"current_frame={stats['current_frame']}")


def case_hls_vod(ctx):
    try:
        return _hls_vod_checks(ctx)
    finally:
        ctx.close_session_quiet()
        recovery_player(ctx, "hls vod")


def _hls_vod_checks(ctx):
    server = ctx.vod_server()
    url = server.url("index.m3u8")
    player = ctx.open(url)
    assert player.is_network(), "hls vod: session is not flagged as a network source"
    declared = player.frame_count()
    check("hls vod: the declared length is known", declared > 0,
          f"frame_count={declared} (a VOD playlist must declare its total length)")
    play = Play(player, "hls vod")
    player.play()
    frame = play.wait_end(declared / max(1.0, player.fps()) + 20)
    stats = player.stats()
    check("hls vod: playback reaches the end of the playlist", frame >= declared - 3,
          f"paused at {frame}/{declared}, present={stats['present_count']}")
    check("hls vod: present ran for the whole session", stats["present_count"] > 20,
          f"present_count={stats['present_count']}")
    return f"{declared} frames played to the end at {frame}, present={stats['present_count']}"


def wait_fresh_after(play, trace, min_frame, timeout, tick=None):
    """等到 frame_num >= min_frame 的 fresh 帧重新上屏；返回它，超时返回 None。"""
    until = time.monotonic() + timeout
    best = None
    while time.monotonic() < until:
        if tick:
            tick()
        play.step()
        frames = {frame for source, frame in trace.delta() if source == FRESH}
        if frames:
            best = max(frames) if best is None else max(best, max(frames))
            if best >= min_frame:
                return best
        time.sleep(0.05)
    return None


def case_hls_live(ctx):
    try:
        return _hls_live_checks(ctx)
    finally:
        ctx.close_session_quiet()
        recovery_player(ctx, "hls live")


def _hls_live_checks(ctx):
    server = ctx.live_server()
    assert len(server.names) >= 10, f"live: only {len(server.names)} segments available"
    player = ctx.open(server.url())
    assert player.is_network(), "hls live: session is not flagged as a network source"
    declared = player.frame_count()
    note(f"hls live: declared frame_count={declared} (0/unknown is normal for live; this case "
         f"never asserts a VOD-style total)")
    play = Play(player, "hls live")
    player.play()
    trace = ctx.trace
    trace.delta()

    assert play.until(lambda: play.frame >= 40, 20.0, tick=server.publish_if_due), (
        f"hls live: clock never reached frame 40 ({play.frame}), declared={declared}")
    baseline = sample(play, trace, "live baseline", 1.0, tick=server.publish_if_due)
    baseline.assert_heartbeat()
    baseline.assert_playing()
    assert len(baseline.fresh()) >= 5, (
        f"hls live: only {len(baseline.fresh())} fresh frames on the baseline window; the live "
        f"playlist is not feeding the decoder")
    fresh_before = max(baseline.fresh())

    # 冻结 playlist（不再有新分片）并在其中一段时间对分片回 503：真实 live 的「暂时没数据」。
    t_freeze = time.monotonic()
    frozen_state = server.snapshot()
    server.freeze(FREEZE_S)
    server.reject(REJECT_S)
    during = sample(play, trace, "live freeze", FREEZE_S)
    during.assert_heartbeat()
    during.assert_playing()
    during.assert_clock_running()
    assert server.snapshot() == frozen_state, (
        "hls live: the playlist grew during the freeze window -- the test setup is broken")
    frozen_requests = server.requests_since(t_freeze)
    playlist_polls = sum(1 for row in frozen_requests if row[1] == "playlist")
    rejected = sum(1 for row in frozen_requests if row[1] == "segment" and row[3] == 503)
    check("hls live: the client kept polling the playlist while nothing new was published",
          playlist_polls >= 2, f"{playlist_polls} playlist reloads during the {FREEZE_S}s freeze")
    check("hls live: a segment really was answered 503 during the outage", rejected >= 1,
          f"{rejected} 503 responses (a 503 that never happens would make this window moot)")

    # 恢复发布：先补几段保证有内容，再按正常节拍发布。
    t_resume = time.monotonic()
    server.publish(4)
    resumed = wait_fresh_after(play, trace, fresh_before + 5, timeout=12.0,
                               tick=server.publish_if_due)
    after = sample(play, trace, "live after resume", 1.0, tick=server.publish_if_due)
    after.assert_heartbeat()
    served_after = [row for row in server.requests_since(t_resume)
                    if row[1] == "segment" and row[3] == 200]
    check("hls live: the decoder picks up fresh frames after the source recovers",
          resumed is not None and len(after.fresh()) >= 5,
          f"fresh frames resumed up to {resumed} (pre-freeze high water {fresh_before}), "
          f"{len(after.fresh())} fresh in the 1s window after recovery")
    check("hls live: the recovered playlist really fed new segments",
          len(served_after) >= 1, f"{len(served_after)} segment(s) served after recovery")
    final = player.stats()
    check("hls live: the session never ended during the silence",
          bool(final["is_playing"])
          and (declared <= 0 or int(player.current_frame()) < declared - 3),
          f"is_playing={final['is_playing']} current_frame={player.current_frame()} "
          f"declared={declared} source_state={source_state_text(final)}")

    player.pause()
    return (f"{len(during.fresh())} fresh / {during.stale()} stale ticks in the {FREEZE_S}s "
            f"freeze, clock +{during.clock_span} frames, {rejected} 503s, resumed at frame "
            f"{resumed}, present never stopped")


CASES = [
    ("mpeg4_part2", case_mpeg4_part2),
    ("h264_422", case_h264_422),
    ("h264_444", case_h264_444),
    ("h264_10bit", case_h264_10bit),
    ("resize_headless", case_resize_headless),
    ("resize_player", case_resize_player),
    ("hls_vod", case_hls_vod),
    ("hls_live", case_hls_live),
]


def run_case(name, fn, ctx):
    print(f"\n-- {name} --")
    before = len(_failures)
    try:
        detail = fn(ctx)
    except Uncovered as exc:
        mark_uncovered(name, str(exc))
        ctx.close_session_quiet()
    except AssertionError as exc:
        check(name, False, str(exc))
        ctx.close_session_quiet()
    except Exception as exc:  # noqa: BLE001 - 任何异常都是 FAIL，绝不降级成 skip
        check(name, False, f"{type(exc).__name__}: {exc}")
        ctx.close_session_quiet()
    else:
        if len(_failures) > before:
            check(name, False, "see the failing checks above")
        else:
            check(name, True, detail or "")


def finish():
    print()
    if _failures:
        print(f"\nFAILED ({len(_failures)}): " + "; ".join(_failures))
    if _uncovered:
        print("UNCOVERED (not verified on this machine): "
              + "; ".join(f"{name} ({reason})" for name, reason in _uncovered.items()))
    if _failures:
        return 1
    if _uncovered:
        print("\nPASSED BUT NOT FULLY COVERED -- see the UNCOVERED list above")
        return 2
    print("\nALL CHECKS PASSED")
    return 0


def main():
    faulthandler.enable()
    # 原生崩溃（访问冲突/堆损坏）会直接终结进程：行缓冲保证崩溃前每个 case 的输出都已落盘，
    # 报告里能看出最后跑到哪一步，而不是一片空白。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:  # 极旧的 Python；其它解释器没有这个 API
        pass
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true",
                        help="只生成并自证素材、自检本地 HLS 服务，不 import sumu_core")
    parser.add_argument("--only", default="",
                        help="逗号分隔的 case 名（配合 --list 查看）")
    parser.add_argument("--list", action="store_true", help="列出 case 名后退出")
    parser.add_argument("--keep", action="store_true", help="保留临时素材与 trace")
    args = parser.parse_args()

    if args.list:
        for name, _ in CASES:
            print(name)
        return 0

    selected = [name.strip() for name in args.only.split(",") if name.strip()]
    unknown = [name for name in selected if name not in dict(CASES)]
    if unknown:
        print(f"unknown case(s): {', '.join(unknown)} (use --list)", file=sys.stderr)
        return 2

    TRACE_DIR.mkdir(parents=True, exist_ok=True)
    tmp_dir = tempfile.mkdtemp(prefix="sumu-decoder-compat-", dir=TRACE_DIR)
    tmp = Path(tmp_dir)
    try:
        print(f"[note] materials and traces: {tmp}")
        if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
            check("ffmpeg/ffprobe on PATH", False,
                  "material generation and the ffprobe criteria need both tools")
            return finish()
        try:
            media = Materials(tmp)
        except Exception as exc:  # noqa: BLE001 - 素材造不出来就是 FAIL，不是崩给用户看
            check("materials", False, f"generation failed: {type(exc).__name__}: {exc}")
            return finish()
        try:
            check_materials(media)
            check_servers(media)
        except AssertionError as exc:
            check("materials / local HLS servers", False, str(exc))
            return finish()
        if args.dry_run:
            check("dry run avoided importing sumu_core", "sumu_core" not in sys.modules)
            return finish()

        sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu")]
        ctx = Ctx(media, tmp)
        try:
            for name, fn in CASES:
                if selected and name not in selected:
                    continue
                run_case(name, fn, ctx)
        finally:
            ctx.close()
        return finish()
    finally:
        if args.keep:
            print(f"[note] kept {tmp}")
        else:
            remove_tree(tmp)


def remove_tree(path, attempts=3):
    """Windows 上刚释放的句柄偶尔还占着目录：重试几次，实在删不掉就明说，不留静默垃圾。"""
    for _ in range(attempts):
        try:
            shutil.rmtree(path)
            return True
        except FileNotFoundError:
            return True
        except OSError:
            time.sleep(0.2)
    shutil.rmtree(path, ignore_errors=True)
    if path.exists():
        note(f"could not fully remove the temporary directory: {path}")
        return False
    return True


if __name__ == "__main__":
    sys.exit(main())
