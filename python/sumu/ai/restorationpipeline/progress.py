# SPDX-FileCopyrightText: Lada Authors
# SPDX-License-Identifier: AGPL-3.0
#
# A single process-wide channel for reporting model-load / TRT-compile progress
# to whatever UI is driving the load (GUI spinner label, CLI stdout, or nobody).
#
# The producers (load_models stages, the TRT compiler's per-engine messages)
# call report_load_progress(msg). A consumer registers exactly one callback via
# set_load_progress_callback. With no callback registered, report is a no-op, so
# CLI export and library use are unaffected.
#
# Lives in its own module (not restorationpipeline/__init__.py) so the low-level
# compiler in lada/trt/ can import it without pulling in the package __init__
# (which imports torch and the model zoo).
from __future__ import annotations

import logging
from typing import Callable

logger = logging.getLogger(__name__)

_load_progress_callback: Callable[[str], None] | None = None


def set_load_progress_callback(callback: Callable[[str], None] | None) -> None:
    """Register the single consumer for load-progress messages (or None to clear).

    The callback is invoked on whatever (background) thread does the loading, so
    a GUI consumer must marshal to the main loop itself (e.g. GLib.idle_add).
    """
    global _load_progress_callback
    _load_progress_callback = callback


def clear_load_progress_callback() -> None:
    set_load_progress_callback(None)


def get_load_progress_callback() -> Callable[[str], None] | None:
    """The currently registered consumer, if any. Lets a temporary consumer (e.g. the
    startup eager-warmup landing UI) clear itself only if it hasn't since been replaced
    by another consumer (e.g. the realtime view's spinner once a video is opened)."""
    return _load_progress_callback


def report_load_progress(message: str) -> None:
    """Send a progress message to the registered callback, if any. Never raises."""
    cb = _load_progress_callback
    if cb is None:
        return
    try:
        cb(message)
    except Exception as e:  # noqa: BLE001 - a broken consumer must not break loading
        logger.debug("load progress callback raised (%s)", e)
