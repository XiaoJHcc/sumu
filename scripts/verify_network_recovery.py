# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# 网络恢复：HTTP Range 服务托管 320x180 H.264 faststart 高码率素材，在播放真的开始之后主动
# 中断一小段时间（进行中的连接短读+断连、新请求 503），然后恢复，验证
#     1. Player 层真的重连恢复了（network_recovery_attempts/successes 与 recovering）；
#     2. 服务端看得到中断期间的 503 和恢复之后的新请求（不只靠脚本成功/导入）；
#     3. 解码计数与 fresh 帧持续恢复，时钟与 present 自始至终不中止；
#     4. 真实 EOF 不触发无限重试；close() 能打断阻塞住的网络读。
#     .venv\Scripts\python.exe scripts/verify_network_recovery.py
#     .venv\Scripts\python.exe scripts/verify_network_recovery.py --dry-run  (只校验素材/服务)
#
# FFmpeg 自带 http 重连（decoder.cpp 的 reconnect* 选项）会吞掉一部分断开，所以判据里既有
# 服务端证据（请求记录/503 计数/恢复后的新请求），也有播放器证据（stats 恢复计数、解码计数、
# present trace 的 fresh/stale），只看其中一边都不算数。
#
# 服务只绑 127.0.0.1 随机端口，不动外部网络；素材与 trace 都在 scripts/trace 下的临时目录。
# 不覆盖：真实丢包/抖动、HLS、AI 去码与重连并发。
import faulthandler
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]

W, H = 320, 180
LONG_S, LONG_FPS = 25.0, 30
LONG_BPS = 6_000_000                # 高码率：让 HTTP 连接在整个播放期间都在搬字节
SHORT_S, SHORT_FPS = 2.0, 30        # 真实 EOF 用例
RATE_BPS = 4 * 1024 * 1024          # 服务端限速；读前量受解码环深度约束，限速只是别让它瞬发
CHUNK = 16 * 1024
OUTAGE_S = 4.0                      # > 网络会话的 PT 环深度（48 帧 ≈ 1.6s）
RECOVER_S = 4.0
START_FRAMES = 20                   # 先让时钟真的动起来再中断
STALL_S = 30.0                      # 挂死模式：接受连接、永不回包
FRESH = 0                           # Source::PassthroughFresh
STALE = 2                           # Source::PassthroughStale
STAT_KEYS = ("network_recovery_attempts", "network_recovery_successes", "network_recovering")


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)],
                   check=True, timeout=180)


class Media:
    """临时目录里的两份素材：长片（中断/恢复、close 打断）与短片（真实 EOF）。"""

    def __init__(self, tmp):
        self.long = tmp / "net-long.mp4"
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={LONG_FPS}",
               "-t", LONG_S, "-c:v", "libx264", "-preset", "ultrafast",
               "-b:v", LONG_BPS, "-minrate", LONG_BPS, "-maxrate", LONG_BPS,
               "-bufsize", LONG_BPS * 2, "-x264-params", "nal-hrd=cbr",
               "-pix_fmt", "yuv420p", "-g", LONG_FPS, "-movflags", "+faststart",
               "-video_track_timescale", 90000, self.long)
        self.short = tmp / "net-short.mp4"
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={W}x{H}:rate={SHORT_FPS}",
               "-t", SHORT_S, "-c:v", "libx264", "-preset", "ultrafast", "-b:v", "1M",
               "-pix_fmt", "yuv420p", "-g", SHORT_FPS, "-movflags", "+faststart", self.short)

    @staticmethod
    def info(path):
        """首条视频流的 ffprobe JSON；读不到时为 None。"""
        run = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=nb_frames,duration,bit_rate:format=duration,size",
             "-of", "json", str(path)],
            capture_output=True, text=True, timeout=180)
        if run.returncode != 0:
            return None
        info = json.loads(run.stdout or "{}")
        return info if info.get("streams") else None

    @classmethod
    def headers(cls, path):
        info = cls.info(path)
        assert info, f"{path.name}: ffprobe found no video stream"
        stream = info["streams"][0]
        duration = float(stream.get("duration") or info["format"]["duration"])
        size = path.stat().st_size
        return {
            "duration": duration,
            "nb_frames": int(stream.get("nb_frames") or 0),
            "byte_rate": size / duration,
            "size": size,
        }

    @staticmethod
    def faststart(path):
        """moov 在 mdat 之前，否则 FFmpeg 得先拉完整个文件才能开播。"""
        head = path.open("rb").read(2 * 1024 * 1024)
        moov, mdat = head.find(b"moov"), head.find(b"mdat")
        return 0 <= moov < mdat


class NetState:
    """服务端状态：ok / break（短读+断连，新请求 503）/ stall（永不回包）。"""

    def __init__(self, root):
        self.root = Path(root)
        self.lock = threading.Lock()
        self.mode = "ok"
        self.requests = []      # (t, name, range_start, mode_at_request_time)
        self.conns = set()
        self.truncated = 0
        self.rejected = 0

    def set_mode(self, mode):
        with self.lock:
            self.mode = mode
            conns = list(self.conns)
        if mode != "ok":
            # 老连接直接断掉：否则客户端可能靠已缓冲的数据把中断期撑过去。
            for conn in conns:
                try:
                    conn.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def register(self, conn):
        with self.lock:
            self.conns.add(conn)

    def unregister(self, conn):
        with self.lock:
            self.conns.discard(conn)

    def resolve(self, raw_path):
        rel = unquote(urlparse(raw_path).path).lstrip("/").replace("\\", "/")
        candidate = (self.root / rel).resolve()
        if not rel or not str(candidate).startswith(str(self.root.resolve())):
            return None
        return candidate if candidate.is_file() else None

    def requests_since(self, t):
        with self.lock:
            return [(when, name, start, mode) for when, name, start, mode in self.requests
                    if when >= t]


class Handler(BaseHTTPRequestHandler):
    """单 Range 静态文件服务 + 可控中断（足够 FFmpeg HTTP 用）。"""

    state = None  # NetState，main() 里注入

    def log_message(self, *args):
        pass

    def do_HEAD(self):  # noqa: N802
        self._serve(head=True)

    def do_GET(self):  # noqa: N802
        self._serve(head=False)

    def _serve(self, head):
        st = self.state
        path = st.resolve(self.path)
        if path is None:
            self.send_error(404, "File not found")
            return
        size = path.stat().st_size
        start, end = self._parse_range(size)
        with st.lock:
            st.requests.append((time.monotonic(), path.name, start, st.mode))
        if st.mode == "stall":
            time.sleep(STALL_S)  # 接受连接、不回包：客户端卡在读上
            self.close_connection = True
            return
        if st.mode != "ok":
            with st.lock:
                st.rejected += 1
            self.send_response(503)
            self.send_header("Retry-After", "1")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        length = end - start + 1
        self.send_response(206 if self.headers.get("Range") else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if self.headers.get("Range"):
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        if head:
            return

        st.register(self.connection)
        try:
            with path.open("rb") as f:
                f.seek(start)
                left = length
                next_t = time.monotonic()
                while left > 0:
                    if st.mode != "ok":
                        with st.lock:
                            st.truncated += 1
                        self._abort()  # 短读：Content-Length 没收满就断
                        return
                    chunk = f.read(min(CHUNK, left))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    left -= len(chunk)
                    if RATE_BPS > 0:
                        next_t += len(chunk) / RATE_BPS
                        delay = next_t - time.monotonic()
                        if delay > 0:
                            time.sleep(min(delay, 0.25))
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            st.unregister(self.connection)

    def _abort(self):
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.close_connection = True

    def _parse_range(self, size):
        rng = (self.headers.get("Range") or "").strip()
        if not rng.lower().startswith("bytes="):
            return 0, size - 1
        start_s, _, end_s = rng[len("bytes="):].partition("-")
        try:
            start = int(start_s) if start_s else 0
            end = int(end_s) if end_s else size - 1
        except ValueError:
            return 0, size - 1
        start = max(0, min(start, size - 1))
        end = max(start, min(end, size - 1))
        return start, end


class Server:
    """127.0.0.1 上的临时端口 Range 服务。"""

    def __init__(self, root):
        self.state = NetState(root)
        handler = type("BoundHandler", (Handler,), {"state": self.state})
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.httpd.daemon_threads = True
        self.thread = threading.Thread(target=self.httpd.serve_forever,
                                       name="verify-net-http", daemon=True)
        self.thread.start()

    def url(self, name):
        return f"http://127.0.0.1:{self.httpd.server_address[1]}/{name}"

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


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
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if not self.step()["is_playing"]:
                self.run(0.15)
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
        return (int(self.samples[-1][1]["current_frame"])
                - int(self.samples[0][1]["current_frame"]))

    @property
    def decode_span(self):
        return (int(self.samples[-1][1]["decode_frame_count"])
                - int(self.samples[0][1]["decode_frame_count"]))

    def fresh(self):
        return {frame for source, frame in self.rows if source == FRESH}

    def stale(self):
        return sum(1 for source, _ in self.rows if source == STALE)

    def assert_alive(self):
        """present 与时钟一刻都不能停（宏观上）。"""
        for (t0, a), (t1, b) in zip(self.samples, self.samples[1:]):
            assert b["present_count"] > a["present_count"], \
                f"{self.label}: present stopped between {t0:.2f}s and {t1:.2f}s"
            assert b["is_playing"], f"{self.label}: playback paused at frame {b['current_frame']}"


def watch(play, trace, label, seconds, step=0.1):
    """采一段时间的 stats（present/时钟/解码都在里面），窗口的 trace 行归属到这段。"""
    started = time.monotonic()
    samples = []
    while time.monotonic() - started < seconds:
        stats = dict(play.step())
        samples.append((time.monotonic() - started, stats))
        time.sleep(step)
    return Window(label, samples, trace.delta())


def recovery_stats(player):
    """Player 层重连计数是「真的恢复了」的唯一直接证据。"""
    stats = player.stats()
    missing = [key for key in STAT_KEYS if key not in stats]
    assert not missing, (
        f"Player::stats() is missing {missing}: the Player-layer recovery counters are the only "
        f"direct proof of recovery; got keys {sorted(stats)}")
    return (int(stats["network_recovery_attempts"]),
            int(stats["network_recovery_successes"]),
            bool(stats["network_recovering"]))


def check_media(media):
    """校验素材与判据里的数字（dry-run 也跑这一段）。"""
    long_hdr = Media.headers(media.long)
    assert abs(long_hdr["duration"] - LONG_S) < 0.2, long_hdr
    assert long_hdr["nb_frames"] == int(LONG_S * LONG_FPS), long_hdr
    assert long_hdr["byte_rate"] >= 400 * 1024, long_hdr["byte_rate"]
    assert Media.faststart(media.long), "long clip is not faststart (moov after mdat)"
    short_hdr = Media.headers(media.short)
    assert abs(short_hdr["duration"] - SHORT_S) < 0.1, short_hdr
    assert Media.faststart(media.short), "short clip is not faststart"
    print(f"PASS: long clip {long_hdr['duration']:.2f}s/{long_hdr['nb_frames']} frames at "
          f"{long_hdr['byte_rate'] / 1024:.0f} KiB/s (faststart), short clip "
          f"{short_hdr['duration']:.2f}s/{short_hdr['nb_frames']} frames (faststart)",
          flush=True)


def check_server(server, media):
    """本地 Range 服务自检：206、Range 头、以及中断模式真的会短读/503。"""
    import urllib.error
    import urllib.request

    url = server.url(media.short.name)
    req = urllib.request.Request(url, headers={"Range": "bytes=100-199"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        body = resp.read()
        assert resp.status == 206, resp.status
        assert body == media.short.read_bytes()[100:200], "range body mismatch"
    server.state.set_mode("break")
    try:
        with urllib.request.urlopen(urllib.request.Request(url), timeout=10):
            raise AssertionError("503 outage mode did not reject the request")
    except urllib.error.HTTPError as exc:
        assert exc.code == 503, exc.code
    finally:
        server.state.set_mode("ok")
        server.state.requests.clear()
        server.state.truncated = 0
        server.state.rejected = 0
    print("PASS: local Range server answers 206 with the right bytes and 503 while breaking",
          flush=True)


def case_recovery(player, trace, server, media):
    """播放真的开始后中断 4s，再恢复：重连计数、解码与 fresh 帧都要回来，时钟/present 不停。"""
    url = server.url(media.long.name)
    player.open(url)
    assert player.is_network(), "session is not flagged as a network source"
    fps = player.fps()
    play = Play(player, "net long")
    player.play()
    trace.delta()
    assert play.until(lambda: play.frame >= START_FRAMES, START_FRAMES / fps + 20), \
        f"net long: clock never started ({play.frame}/{play.declared})"

    base = watch(play, trace, "baseline", 1.0)
    base.assert_alive()
    assert len(base.fresh()) >= 0.5 * fps * base.seconds, (len(base.fresh()), base.seconds)
    assert len(base.fresh()) >= 5, len(base.fresh())

    attempts_before = recovery_stats(player)[0]
    server.state.set_mode("break")
    t_break = time.monotonic()
    during = watch(play, trace, "outage", OUTAGE_S)
    during.assert_alive()
    server.state.set_mode("ok")
    t_restore = time.monotonic()
    restored = recovery_stats(player)
    after = watch(play, trace, "recovery", RECOVER_S)
    after.assert_alive()
    final = recovery_stats(player)
    player.pause()

    # 中断真的生效了（否则这一轮什么也没证明）。
    assert during.stale() >= 10, (
        f"only {during.stale()} stale present ticks during the outage: the break was masked "
        f"(decode-ahead covered it) -- lengthen OUTAGE_S or raise the clip's bitrate")
    assert during.clock_span >= 0.6 * fps * during.seconds, (during.clock_span, during.seconds)
    assert len(during.fresh()) <= 0.85 * fps * during.seconds, (
        f"{len(during.fresh())} fresh frames during the outage -- the data cannot have been "
        f"missing, so this window proves nothing")

    # 服务端证据：中断期拒过请求，恢复之后来了新请求且重新开始供字节。
    rejected = server.state.rejected
    truncated = server.state.truncated
    after_outage = server.state.requests_since(t_restore)
    assert rejected >= 1 or truncated >= 1, (rejected, truncated)
    assert after_outage, (
        "no HTTP request after the outage ended: the client never reconnected, so the resumed "
        "frames could only have come from stale buffered data")
    served = [row for row in after_outage if row[3] == "ok"]
    assert served, after_outage

    # 播放器证据：Player 层重连计数 + 解码计数 + fresh 帧。
    attempts, successes, recovering = final
    assert attempts > attempts_before, (attempts_before, attempts)
    assert successes >= 1, final
    assert not recovering, final
    assert after.decode_span >= 0.4 * fps * after.seconds, (after.decode_span, after.seconds)
    fresh_after = after.fresh()
    # 门槛按「恢复窗口里至少重新播满 1.2s 内容」取；重连后追赶多快取决于实现（顺序重读还是
    # 重定位到时钟位置），留出余量。
    assert len(fresh_after) >= 0.3 * fps * after.seconds, (len(fresh_after), after.seconds)
    assert max(fresh_after) > min(fresh_after), (min(fresh_after), max(fresh_after))
    assert max(fresh_after) > play.declared // 4, (max(fresh_after), play.declared)

    print(f"PASS: net long survived a {t_restore - t_break:.1f}s outage -- during it "
          f"{during.stale()} stale ticks / {len(during.fresh())} fresh / clock +"
          f"{during.clock_span} frames / present never stopped; server saw {rejected} 503 + "
          f"{truncated} truncated streams and {len(served)} new request(s) after recovery; "
          f"Player recovery attempts {attempts_before}->{attempts} (ok {successes}); after "
          f"recovery {len(fresh_after)} fresh frames and +{after.decode_span} decoded in "
          f"{after.seconds:.1f}s (restored attempts={restored[0]}, ok={restored[1]})", flush=True)
    player.close_current_session()


def case_eof(player, trace, server, media):
    """真实 EOF 是正常结尾，不该被当成网络故障无限重试。"""
    url = server.url(media.short.name)
    player.open(url)
    play = Play(player, "net short")
    player.play()
    frame = play.wait_end(SHORT_S + 15)
    assert frame >= play.declared - 3, (frame, play.declared)
    attempts, successes, recovering = recovery_stats(player)
    assert not recovering, (attempts, successes, recovering)
    present = player.stats()["present_count"]
    play.run(2.0)
    later_attempts, later_successes, _ = recovery_stats(player)
    assert later_attempts == attempts and later_successes == successes, \
        (attempts, later_attempts, successes, later_successes)
    assert player.stats()["present_count"] > present, "present stopped at the end of the stream"
    print(f"PASS: net short hit its real EOF at {frame}/{play.declared} and stayed there -- "
          f"recovery counters frozen at attempts={attempts} ok={successes} for 2s "
          f"(EOF is not a network error)", flush=True)
    player.close_current_session()


def close_with_watchdog(player, limit_s=20.0):
    """主线程 close（与 app.py 一致）；超过 limit_s 还没回来就打印栈并退出，别让脚本挂死。"""
    done = threading.Event()

    def watchdog():
        if not done.wait(limit_s):
            faulthandler.dump_traceback()
            print(f"FAIL: Player.close() still blocked after {limit_s:.0f}s against a stalled "
                  f"network read (rw_timeout is 15s)", flush=True)
            os._exit(1)

    threading.Thread(target=watchdog, name="verify-net-close-watchdog", daemon=True).start()
    t0 = time.monotonic()
    try:
        player.close()
    finally:
        done.set()
    return time.monotonic() - t0


def case_close_interrupts(player, server, media):
    """挂死服务器 + close()：阻塞的网络读必须被打断，不能干等 rw_timeout(15s)。"""
    player.open(server.url(media.long.name))
    play = Play(player, "net close")
    player.play()
    assert play.until(lambda: play.frame >= 10, 20.0), f"net close: clock stuck at {play.frame}"
    server.state.set_mode("stall")  # 新请求不回包，老连接全断掉
    time.sleep(1.0)  # 让解码线程真的卡在 stalled 读上

    elapsed = close_with_watchdog(player)
    assert elapsed < 10.0, (
        f"Player.close() took {elapsed:.1f}s against a stalled read -- it waited out rw_timeout "
        f"(15s) instead of interrupting the blocked read")
    print(f"PASS: close() interrupted a stalled network read in {elapsed:.2f}s "
          f"(rw_timeout is 15s)", flush=True)


def main():
    faulthandler.enable()
    dry_only = "--dry-run" in sys.argv[1:]
    (ROOT / "scripts" / "trace").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sumu-netrec-", dir=ROOT / "scripts" / "trace") as tmp:
        tmp = Path(tmp)
        media = Media(tmp)
        check_media(media)
        server = Server(tmp)
        try:
            check_server(server, media)
            if dry_only:
                print("dry run: media and local Range server checks passed (no player opened)")
                return
            sys.path[:0] = [str(ROOT / "python"), str(ROOT / "python" / "sumu")]
            import sumu_core

            player = sumu_core.Player(640, 400)
            try:
                trace = Trace(player, tmp / "present.csv")
                case_recovery(player, trace, server, media)
                case_eof(player, trace, server, media)
                case_close_interrupts(player, server, media)  # 破坏性，放最后
            finally:
                try:
                    player.close()  # 幂等：close 用例已经关过时只是一次收尾
                except Exception:  # noqa: BLE001 - 收尾失败不该盖住真正的判据失败
                    pass
        finally:
            server.close()
    print("All network-recovery checks passed.")


if __name__ == "__main__":
    main()
