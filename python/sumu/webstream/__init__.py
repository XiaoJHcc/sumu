# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Web streaming + offline export pipeline: headless d3d11va decode -> BasicVSR decensor ->
# NVENC -> HLS (streaming) / MP4 (export). See docs/webstream.md for the architecture.
#
# Lazy submodule surface (PEP 562 __getattr__): importing `sumu.webstream` itself, or its
# stdlib-only submodules (server / passthrough / index_page / thumbnail), must NOT import the
# torch/cv2-heavy transcode chain. That chain (transcode.py -> decensor.py -> torch) is pulled in
# only when the caller actually references TranscodeEngine/TranscodeError -- i.e. the AI transcode
# path. This is what lets the passthrough (原片直出) web server start instantly, before the app's
# background AI model warmup finishes.

__all__ = [
    "TranscodeEngine",
    "TranscodeError",
    "StreamingServer",
    "ExportJob",
    "PassthroughSession",
    "AiStreamSession",
]


def __getattr__(name: str):
    if name in ("TranscodeEngine", "TranscodeError"):
        from .transcode import TranscodeEngine, TranscodeError

        return TranscodeEngine if name == "TranscodeEngine" else TranscodeError
    if name == "StreamingServer":
        from .server import StreamingServer

        return StreamingServer
    if name == "ExportJob":
        from .export import ExportJob

        return ExportJob
    if name == "PassthroughSession":
        from .passthrough import PassthroughSession

        return PassthroughSession
    if name == "AiStreamSession":
        from .ai_session import AiStreamSession

        return AiStreamSession
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
