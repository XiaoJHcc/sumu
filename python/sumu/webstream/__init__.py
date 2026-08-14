# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Web streaming + offline export pipeline: headless d3d11va decode -> BasicVSR decensor ->
# NVENC -> HLS (streaming) / MP4 (export). See docs/webstream.md for the architecture.
#
# This subpackage is the transcode engine; the HTTP server / folder-browser front-end and the
# native UI entry points live alongside it (server.py, and app.py / native player.cpp intents).
from .transcode import TranscodeEngine, TranscodeError  # noqa: F401
from .server import StreamingServer  # noqa: F401
