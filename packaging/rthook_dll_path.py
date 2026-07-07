# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# PyInstaller runtime hook -- runs at frozen-app startup, before any of our own
# modules import. Makes sure the onedir bundle dir (where the ffmpeg DLLs,
# sumu_core.pyd's DLL deps, torch/TensorRT/CUDA DLLs all land -- see
# packaging/sumu.spec) is on the DLL search path, so LoadLibrary calls made by
# sumu_core, torch, torch_tensorrt, tensorrt resolve without needing PATH set
# externally.
import os
import sys

base = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
os.environ["PATH"] = base + os.pathsep + os.environ.get("PATH", "")
try:
    os.add_dll_directory(base)
except Exception:
    pass
