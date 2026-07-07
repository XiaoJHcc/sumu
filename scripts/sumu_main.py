# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Future PyInstaller entry point for the daily-use player. No sys.path hacks: the frozen build's
# spec is responsible for making `sumu` and `sumu_core` importable.
from sumu.app import main

if __name__ == "__main__":
    main()
