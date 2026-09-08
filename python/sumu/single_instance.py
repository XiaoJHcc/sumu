# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Single-instance forwarding over a Windows named pipe (close-parking support, see app.py).
#
# sumu is a single-instance app: the first process to get past try_forward_to_running()
# becomes the primary and runs a PipeListener; every later launch connects to the pipe,
# sends its video path (or an empty payload = "just resurface the window"), waits for a
# 1-byte ack, and exits. The primary drains PipeListener.incoming on its main loop and
# reopens in place -- reusing its already-warm AI models, which is exactly what makes
# reopening within the close-park window free of warmup.
#
# stdlib-only (ctypes -> kernel32), so importing this costs nothing on the startup path.

import ctypes
import getpass
import queue
import re
import sys
import threading
import time
from ctypes import wintypes

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_GENERIC_READ = 0x80000000
_GENERIC_WRITE = 0x40000000
_OPEN_EXISTING = 3
_PIPE_ACCESS_DUPLEX = 0x00000003
_PIPE_TYPE_BYTE = 0x00000000
_PIPE_READMODE_BYTE = 0x00000000
_PIPE_WAIT = 0x00000000
_PIPE_REJECT_REMOTE_CLIENTS = 0x00000008
_ERROR_PIPE_CONNECTED = 535
_ERROR_FILE_NOT_FOUND = 2

# L4: 单条转发消息（换行终止的 UTF-8 路径）的 payload 上限，超限断开连接。
_MAX_PAYLOAD_BYTES = 64 * 1024
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

_kernel32.CreateNamedPipeW.restype = wintypes.HANDLE
_kernel32.CreateNamedPipeW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
]
_kernel32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID]
_kernel32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
_kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_kernel32.CreateFileW.restype = wintypes.HANDLE
_kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
_kernel32.ReadFile.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
]
_kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
]
_kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def _pipe_name() -> str:
    # Per-user pipe: named pipes are machine-global, so two users on one box must not share
    # an instance (their settings/model caches differ anyway).
    user = re.sub(r"[^A-Za-z0-9_.-]", "_", getpass.getuser() or "default")
    return rf"\\.\pipe\sumu-player-{user}"


def try_forward_to_running(video_path, attempts=3, ack_timeout_s=2.0) -> bool:
    """Client side: hand `video_path` (None/"" = bare foreground nudge) to the running or
    close-parked primary instance. Returns True once the primary acked the handoff -- the
    caller should then exit immediately. Returns False when no primary is listening (caller
    becomes the primary) or the handoff fails (wedged/mid-teardown primary: safer to just
    run standalone than to lose the user's launch)."""
    payload = ((video_path or "") + "\n").encode("utf-8")
    name = _pipe_name()
    for _ in range(attempts):
        if not _kernel32.WaitNamedPipeW(name, 250):
            if ctypes.get_last_error() == _ERROR_FILE_NOT_FOUND:
                return False  # no pipe at all -> no primary
            time.sleep(0.3)   # instance busy or primary still starting up: retry
            continue
        h = _kernel32.CreateFileW(name, _GENERIC_READ | _GENERIC_WRITE, 0, None,
                                  _OPEN_EXISTING, 0, None)
        if h in (None, 0, _INVALID_HANDLE_VALUE):
            time.sleep(0.3)
            continue
        try:
            n = wintypes.DWORD(0)
            if not _kernel32.WriteFile(h, payload, len(payload), ctypes.byref(n), None):
                return False
            # Wait for the primary's ack without a blocking ReadFile: if the primary's
            # process is mid-teardown between ConnectNamedPipe and its ack write, a blocking
            # read would hang this launcher forever. Peek-poll with a deadline instead.
            deadline = time.monotonic() + ack_timeout_s
            while time.monotonic() < deadline:
                avail = wintypes.DWORD(0)
                if not _kernel32.PeekNamedPipe(h, None, 0, None, ctypes.byref(avail), None):
                    return False  # pipe broke (primary gone)
                if avail.value > 0:
                    ack = ctypes.create_string_buffer(1)
                    if (_kernel32.ReadFile(h, ack, 1, ctypes.byref(n), None)
                            and n.value == 1):
                        return True
                    return False
                time.sleep(0.02)
            return False  # ack never came -- run standalone rather than lose the launch
        finally:
            _kernel32.CloseHandle(h)
    return False


class PipeListener(threading.Thread):
    """Primary side: one pipe instance at a time, newline-terminated UTF-8 path in, decoded
    path onto `incoming` (drained by app.py's main loop), 1-byte ack out. Daemon thread --
    process exit is its stop signal, same as the warmup/open workers."""

    def __init__(self):
        super().__init__(name="sumu-single-instance", daemon=True)
        self.incoming = queue.Queue()

    def run(self):
        while True:
            try:
                self._serve_one()
            except Exception as e:  # noqa: BLE001 -- the listener must never die silently
                print(f"== single-instance == listener error: {e!r}", file=sys.stderr)
                time.sleep(0.2)

    def _serve_one(self):
        h = _kernel32.CreateNamedPipeW(
            _pipe_name(), _PIPE_ACCESS_DUPLEX,
            _PIPE_TYPE_BYTE | _PIPE_READMODE_BYTE | _PIPE_WAIT | _PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, None)
        if h in (None, 0, _INVALID_HANDLE_VALUE):
            raise OSError(f"CreateNamedPipeW failed: {ctypes.get_last_error()}")
        try:
            if not _kernel32.ConnectNamedPipe(h, None):
                if ctypes.get_last_error() != _ERROR_PIPE_CONNECTED:
                    return
            payload = bytearray()
            chunk = ctypes.create_string_buffer(4096)
            scanned = 0  # L4: find 起点 —— 已扫过的前缀不可能新出现 '\n'，避免每次全量重扫（O(n²)）
            while True:
                if payload.find(b"\n", scanned) >= 0:
                    break
                scanned = len(payload)
                n = wintypes.DWORD(0)
                if (not _kernel32.ReadFile(h, chunk, len(chunk), ctypes.byref(n), None)
                        or n.value == 0):
                    return  # client vanished before finishing its line -- ignore it
                payload += chunk.raw[: n.value]
                # L4: payload 上限 —— 此前无上限，恶意/异常客户端永不发 '\n' 就无限累积。
                # 超限直接断开（finally 里 Disconnect/Close），不投递、不 ack，listener
                # 本身不受影响（路径转发正常 payload 远小于此 —— MAX_PATH 级）。
                if len(payload) > _MAX_PAYLOAD_BYTES:
                    print(f"== single-instance == payload over {_MAX_PAYLOAD_BYTES} bytes "
                          "without newline, dropping connection", file=sys.stderr)
                    return
            self.incoming.put(payload.rstrip(b"\r\n").decode("utf-8", "replace"))
            n = wintypes.DWORD(0)
            _kernel32.WriteFile(h, b"1", 1, ctypes.byref(n), None)
            _kernel32.FlushFileBuffers(h)
        finally:
            _kernel32.DisconnectNamedPipe(h)
            _kernel32.CloseHandle(h)
