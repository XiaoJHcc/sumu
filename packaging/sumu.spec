# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# PyInstaller spec for the daily-use frozen player (onedir bundle). Build with:
#   .venv/Scripts/python.exe -m PyInstaller packaging/sumu.spec --noconfirm
# NEVER run pyinstaller against scripts/sumu_main.py directly -- that would
# generate/clobber a fresh (unconfigured) spec instead of using this one.
#
# This is a torch+CUDA+TensorRT app: expect a large (~4-6GB) onedir bundle and
# a slow first analysis pass. Weights are NOT bundled here -- they're staged
# next to the exe by scripts/build_dist.ps1 (sumu.ai._default_model_weights_dir()
# resolves <dir of sys.executable>/model_weights at runtime when sys.frozen).
import os
import sys

sys.setrecursionlimit(sys.getrecursionlimit() * 5)

from PyInstaller.utils.hooks import (
    collect_all,
    collect_dynamic_libs,
    collect_data_files,
    collect_submodules,
    copy_metadata,
)

# SPECPATH is injected by PyInstaller into this file's globals -- it's the
# directory containing THIS .spec file (packaging/), not the invocation cwd.
# All repo-relative paths below are resolved off it so `pyinstaller
# packaging/sumu.spec` works regardless of the caller's cwd.
ROOT = os.path.abspath(os.path.join(SPECPATH, ".."))  # noqa: F821

block_cipher = None

datas = []
binaries = []
hiddenimports = []

# --- collect_all for the heavy packages with nontrivial data/binary/hidden-import needs ---
for pkg in ("torch", "torchvision", "ultralytics", "cv2"):
    d, b, h = collect_all(pkg)
    datas += d
    binaries += b
    hiddenimports += h

# --- TensorRT trio: no PyInstaller hook ships for these, collect explicitly ---
binaries += collect_dynamic_libs("torch_tensorrt")
binaries += collect_dynamic_libs("tensorrt")
binaries += collect_dynamic_libs("tensorrt_libs")
hiddenimports += collect_submodules("torch_tensorrt")
hiddenimports += collect_submodules("tensorrt")
datas += collect_data_files("torch_tensorrt")

# --- belt-and-suspenders: torch\lib CUDA DLLs (collect_all above may already grab these,
# but collect_dynamic_libs is cheap and idempotent-ish here -- duplicates are harmless) ---
binaries += collect_dynamic_libs("torch")

# --- mmengine: submodules + data files (configs etc. read at runtime) ---
hiddenimports += collect_submodules("mmengine")
datas += collect_data_files("mmengine")

# --- metadata read via importlib.metadata at runtime by these packages ---
for pkg in ("torch", "torchvision", "numpy", "ultralytics", "mmengine"):
    datas += copy_metadata(pkg)

# --- torch dynamic/native submodules not picked up by static analysis ---
hiddenimports += ["torch._C", "torch._C._jit", "torch._C._nvrtc", "torch._C._dynamo"]

# --- sumu's own package + native extension ---
hiddenimports += [
    "sumu_core",
    "sumu.app",
    "sumu.pipeline",
    "sumu.scheduler",
    "sumu.settings",
] + collect_submodules("sumu")

# native extension + its co-located ffmpeg DLLs (loaded by sumu_core via
# load-time import / LOAD_WITH_ALTERED_SEARCH_PATH -- must stay next to it)
binaries += [(os.path.join(ROOT, "python", "sumu", "sumu_core.cp313-win_amd64.pyd"), ".")]
for dll in (
    "avcodec-63.dll",
    "avformat-63.dll",
    "avutil-61.dll",
    "swresample-7.dll",
    "avdevice-63.dll",
    "avfilter-12.dll",
    "swscale-10.dll",
):
    binaries += [(os.path.join(ROOT, "python", "sumu", dll), ".")]

a = Analysis(
    [os.path.join(ROOT, "scripts", "sumu_main.py")],
    pathex=[os.path.join(ROOT, "python"), os.path.join(ROOT, "python", "sumu")],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[os.path.join(ROOT, "packaging", "rthook_dll_path.py")],
    excludes=["av"],
    noarchive=False,
    cipher=block_cipher,
)

# SUMU_FAST_FREEZE (set by scripts/build_dist.ps1 -FastFreeze): skip COLLECT.
# COLLECT.assemble() unconditionally _make_clean_directory()s the whole onedir
# tree and re-copies every binary/data file every run (PyInstaller has no
# incremental copy there -- see PyInstaller/building/api.py COLLECT._check_guts,
# which always returns True "in order to clean the output directory"). That's
# a full re-copy of ~9-10GB of torch/cv2/tensorrt payload that never changes
# between ordinary dev iterations. EXE(exclude_binaries=True) itself only
# writes the thin bootloader+PYZ exe (our own compiled Python) to
# build/sumu/sumu.exe and does NOT touch dist/ -- so when only sumu's own
# source or the native extension changed, build_dist.ps1's fast path runs the
# spec with COLLECT skipped, then hand-copies just that exe (and the native
# pyd/ffmpeg DLLs) over the existing dist/sumu tree. This is UNSAFE if the
# dependency set itself changed (new/updated torch/cv2/tensorrt/mmengine
# binaries or data files) -- those require a real COLLECT to land in dist/.
_fast_freeze = bool(os.environ.get("SUMU_FAST_FREEZE"))

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="sumu",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,  # windowed: no cmd black window for the daily-use bundle. stdout/stderr
                    # are redirected to <exe dir>/sumu.log by scripts/sumu_main.py so a
                    # startup/warmup failure is still diagnosable instead of a silent exit.
    disable_windowed_traceback=False,
)

if not _fast_freeze:
    coll = COLLECT(
        exe,
        a.binaries,
        a.zipfiles,
        a.datas,
        strip=False,
        upx=False,
        upx_exclude=[],
        name="sumu",
    )
