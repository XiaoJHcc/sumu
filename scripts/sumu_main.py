# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# PyInstaller entry point for the daily-use player. No sys.path hacks: the frozen build's
# spec is responsible for making `sumu` and `sumu_core` importable.
#
# The frozen bundle is built windowed (console=False in packaging/sumu.spec) so it has no
# cmd black window. Windowed mode leaves sys.stdout/sys.stderr as None, which would turn any
# stray print() into a crash and, worse, make a startup/warmup failure a silent exit. So
# before importing anything that might log, redirect both streams to a log file next to the
# executable -- keeping the app diagnosable without a console window.
import sys


def _redirect_frozen_output_to_logfile():
    if not getattr(sys, "frozen", False):
        return  # dev runs keep the real console
    try:
        import os

        log_dir = os.path.dirname(sys.executable)
        log_path = os.path.join(log_dir, "sumu.log")
        # line-buffered so a crash mid-warmup still flushes the last lines to disk
        f = open(log_path, "w", buffering=1, encoding="utf-8", errors="replace")
        sys.stdout = f
        sys.stderr = f
    except Exception:
        # never let logging setup itself take down startup -- fall back to whatever
        # (possibly None) streams windowed mode gave us.
        pass


if __name__ == "__main__":
    _redirect_frozen_output_to_logfile()
    from sumu.app import main

    main()
