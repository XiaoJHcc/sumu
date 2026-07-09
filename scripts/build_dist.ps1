# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# One-command build pipeline for the daily-use frozen player (PyInstaller onedir bundle):
#   native build -> patch third-party deps -> pyinstaller freeze -> stage weights -> smoke test.
# Usage:
#   powershell -File scripts/build_dist.ps1 [-WeightsSrc <dir>] [-SkipNative] [-SkipSmoke] [-FastFreeze]
# -FastFreeze: skip PyInstaller's COLLECT stage (which unconditionally wipes and
#   re-copies the whole ~9-10GB dist\sumu\_internal tree every run -- see the
#   comment in packaging/sumu.spec). Only rebuilds dist\sumu\sumu.exe (sumu's own
#   Python source) plus the native pyd/ffmpeg DLLs, and hand-copies just those
#   over an EXISTING dist\sumu tree. Requires a prior full build (dist\sumu\_internal
#   must already exist) and is only correct when the dependency set itself hasn't
#   changed (no new/updated torch/cv2/tensorrt/mmengine binaries or data files) --
#   i.e. when only sumu's own source or native extension changed.
# Weights source resolution (anyone building this, not just the original dev machine):
#   1. -WeightsSrc if passed explicitly
#   2. $env:SUMU_WEIGHTS_SRC if set (setx SUMU_WEIGHTS_SRC "C:\path\to\model_weights" once, persists
#      across sessions) -- lets each machine/teammate configure this without editing the script or
#      remembering a flag every run.
#   3. the original dev machine's path, as a last-resort fallback for this repo's own history.
param(
    [string]$WeightsSrc = $(if ($env:SUMU_WEIGHTS_SRC) { $env:SUMU_WEIGHTS_SRC } else { "D:/Git/lada-realtime/model_weights" }),
    [switch]$SkipNative,
    [switch]$SkipSmoke,
    [switch]$FastFreeze
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

function Fail($msg) {
    Write-Host "BUILD FAILED: $msg" -ForegroundColor Red
    exit 1
}

# Run a native (non-PowerShell) command line and return its combined output.
# IMPORTANT: this always goes through `cmd /c "<cmdline> 2>&1"` -- i.e. the
# stderr->stdout merge happens INSIDE cmd.exe, not via a PowerShell-level
# "2>&1" on the call site. PowerShell 5.1 wraps each stderr line from a native
# command in a NativeCommandError when PowerShell itself does the merging,
# which becomes a terminating exception under $ErrorActionPreference = "Stop"
# (set below) even when the process exits 0 -- and every tool invoked here
# (bash/uv, PyInstaller/torch) writes routine progress/warnings to stderr, so
# without this the script would abort on the first such line. Funneling
# through cmd's own redirection means PowerShell only ever sees clean merged
# stdout, so no ErrorRecord is ever created.
function Invoke-Native([string]$CommandLine, [string]$FailMessage) {
    $out = & cmd /c "$CommandLine 2>&1"
    $out | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
        Fail $FailMessage
    }
    return $out
}

# --- 1. native build --------------------------------------------------------
if (-not $SkipNative) {
    Write-Host "== [1/5] building native extension (native/build.bat) ==" -ForegroundColor Cyan
    $nativeOut = Invoke-Native "native\build.bat" "native/build.bat exited with a non-zero code"
    if (-not (($nativeOut | Out-String) -match "BUILD_OK")) {
        Fail "native/build.bat did not report BUILD_OK"
    }
    Write-Host "native build OK" -ForegroundColor Green
} else {
    Write-Host "== [1/5] -SkipNative set, skipping native build ==" -ForegroundColor Yellow
}

# --- 2. re-apply third-party runtime patches (must happen before freeze) ---
Write-Host "== [2/5] applying third-party patches (scripts/apply_patches.sh) ==" -ForegroundColor Cyan
# bash.exe isn't always on PATH in a plain PowerShell session even when Git for
# Windows is installed (Git\cmd is commonly on PATH, but bash.exe lives under
# Git\bin / Git\usr\bin) -- resolve it defensively instead of assuming PATH.
$bashExe = (Get-Command bash -ErrorAction SilentlyContinue).Source
if (-not $bashExe) {
    foreach ($candidate in @(
        "$env:ProgramFiles\Git\bin\bash.exe",
        "$env:ProgramFiles\Git\usr\bin\bash.exe"
    )) {
        if (Test-Path $candidate) { $bashExe = $candidate; break }
    }
}
if (-not $bashExe) {
    Fail "bash.exe not found on PATH or in the default Git for Windows install location"
}
Invoke-Native "`"$bashExe`" scripts/apply_patches.sh" "scripts/apply_patches.sh exited with a non-zero code" | Out-Null
Write-Host "patches applied OK" -ForegroundColor Green

# --- 3. pyinstaller freeze ---------------------------------------------------
$distDir = Join-Path $RepoRoot "dist\sumu"
$internalDir = Join-Path $distDir "_internal"

if ($FastFreeze -and -not (Test-Path (Join-Path $internalDir "sumu_core.cp313-win_amd64.pyd"))) {
    Fail "-FastFreeze requires an existing full build (dist\sumu\_internal not found) -- run once without -FastFreeze first"
}

if ($FastFreeze) {
    Write-Host "== [3/5] fast-freezing (skip COLLECT, only relink sumu.exe) ==" -ForegroundColor Cyan
    $env:SUMU_FAST_FREEZE = "1"
    try {
        Invoke-Native "`"$RepoRoot\.venv\Scripts\python.exe`" -m PyInstaller packaging/sumu.spec --noconfirm" "PyInstaller fast freeze failed" | Out-Null
    } finally {
        Remove-Item Env:\SUMU_FAST_FREEZE -ErrorAction SilentlyContinue
    }
    $builtExe = Join-Path $RepoRoot "build\sumu\sumu.exe"
    if (-not (Test-Path $builtExe)) {
        Fail "expected build\sumu\sumu.exe not found after fast freeze"
    }
    Copy-Item -Path $builtExe -Destination (Join-Path $distDir "sumu.exe") -Force
    # native ext + ffmpeg DLLs land in _internal\ (see COLLECT dest logic) -- refresh
    # them directly too, since -FastFreeze never re-runs COLLECT to pick them up.
    $nativeSumuDir = Join-Path $RepoRoot "python\sumu"
    foreach ($f in @("sumu_core.cp313-win_amd64.pyd", "avcodec-63.dll", "avformat-63.dll", "avutil-61.dll",
                     "swresample-7.dll", "avdevice-63.dll", "avfilter-12.dll", "swscale-10.dll")) {
        Copy-Item -Path (Join-Path $nativeSumuDir $f) -Destination (Join-Path $internalDir $f) -Force
    }
    Write-Host "fast freeze OK: $distDir (sumu.exe relinked, _internal left untouched)" -ForegroundColor Green
} else {
    Write-Host "== [3/5] freezing with PyInstaller (packaging/sumu.spec) ==" -ForegroundColor Cyan
    Invoke-Native "`"$RepoRoot\.venv\Scripts\python.exe`" -m PyInstaller packaging/sumu.spec --noconfirm" "PyInstaller freeze failed" | Out-Null
    if (-not (Test-Path (Join-Path $distDir "sumu.exe"))) {
        Fail "expected dist\sumu\sumu.exe not found after freeze"
    }
    Write-Host "freeze OK: $distDir" -ForegroundColor Green
}

# --- 4. stage model weights next to the exe ---------------------------------
Write-Host "== [4/5] staging model weights from $WeightsSrc ==" -ForegroundColor Cyan
$weightsDst = Join-Path $distDir "model_weights"
if (-not (Test-Path $weightsDst)) {
    New-Item -ItemType Directory -Path $weightsDst -Force | Out-Null
}

$weightFiles = @(
    "lada_mosaic_restoration_model_generic_v1.2.pth",
    "lada_mosaic_detection_model_v4_fast.pt"
)
foreach ($f in $weightFiles) {
    $src = Join-Path $WeightsSrc $f
    if (-not (Test-Path $src)) {
        Fail "missing weight file: $src"
    }
    Copy-Item -Path $src -Destination (Join-Path $weightsDst $f) -Force
}

# TRT sub-engines are deliberately NOT bundled. A hardware_compatible=False engine only
# deserializes on the exact GPU arch / TensorRT version / precision / OS it was built for
# (filename tag e.g. sm89.trt1012.fp16.win), so a prebuilt engine only helps identical hardware.
# Instead every install compiles its own on first run, driven by the in-app first-screen "编译加速
# 引擎" prompt (see python/sumu/app.py's compile state machine). This keeps the package ~520MB
# smaller and means the first-run compile path is what everyone actually exercises.
Write-Host "weights staged OK: $weightsDst (TRT engines compiled on first run, not bundled)" -ForegroundColor Green

# AGPL-3.0 requires that anyone you convey the binary to also gets a copy of the license
# (section 4). Stage it next to the exe so it travels with whatever archive is made from
# dist\sumu.
Copy-Item -Path (Join-Path $RepoRoot "LICENSE.md") -Destination (Join-Path $distDir "LICENSE.md") -Force
Write-Host "LICENSE.md staged OK" -ForegroundColor Green

# --- 5. bounded, non-interactive smoke test ---------------------------------
if ($SkipSmoke) {
    Write-Host "== [5/5] -SkipSmoke set, skipping smoke test ==" -ForegroundColor Yellow
    Write-Host "dist\sumu built but NOT verified to actually start -- run it manually before shipping" -ForegroundColor Yellow
    return
}

Write-Host "== [5/5] smoke testing dist\sumu\sumu.exe ==" -ForegroundColor Cyan
# test_video.mp4 is .gitignore'd (it's a local dev fixture, not distributed with the repo), so
# on a fresh clone -- another machine, a teammate, CI -- it won't exist. Passing a nonexistent
# path as the launch arg makes player.open() throw (do_open() in app.py has no try/except around
# it), which used to masquerade as a hard smoke FAIL even though the dist itself built fine.
# Degrade instead: without a video we can't validate the open->playback path, but we can still
# validate that the frozen exe starts, torch/CUDA imports, and models load -- so drop the
# "== player.open ==" requirement and launch with no argument (native side shows the open-prompt
# overlay, same as a normal no-file start).
$videoPath = Join-Path $RepoRoot "test_video.mp4"
$hasTestVideo = Test-Path $videoPath
if (-not $hasTestVideo) {
    Write-Host "no test_video.mp4 found at $videoPath -- smoke will only validate startup/model load, not the open/playback path" -ForegroundColor Yellow
}
$stderrLog = Join-Path $RepoRoot "build_smoke_stderr.log"
$stdoutLog = Join-Path $RepoRoot "build_smoke_stdout.log"
# The frozen bundle is windowed (console=False) and scripts/sumu_main.py reassigns
# sys.stdout/sys.stderr to <exe dir>/sumu.log, so the app's own markers (== env ==,
# == load_models ==, == player.open ==) land in sumu.log -- NOT in the OS-level
# $stderrLog handle we redirect below (that only catches pre-redirect native/C-level
# output, e.g. a torch DLL crash). We therefore key pass/fail off sumu.log, keeping
# $stderrLog only as a supplementary native-crash sink. Remove a stale sumu.log first
# (the app opens it "w"/truncate, but a failed launch might leave last run's markers).
$sumuLog = Join-Path $distDir "sumu.log"
Remove-Item -Path $stderrLog, $stdoutLog, $sumuLog -ErrorAction SilentlyContinue

# Combined marker source: the app's redirected sumu.log plus the OS-level native sink.
function Get-SmokeLog {
    $a = if (Test-Path $sumuLog)   { Get-Content $sumuLog   -Raw -ErrorAction SilentlyContinue } else { "" }
    $b = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw -ErrorAction SilentlyContinue } else { "" }
    return "$a`n$b"
}

# The player window has no auto-timeout, so we poll sumu.log and kill it once
# BOTH markers are present: == load_models == (warmup thread finished) AND
# == player.open == (playback started). Warmup is now backgrounded (sumu.app),
# so these two happen concurrently and in EITHER order -- player.open() no longer
# waits on the models, so it usually prints first; breaking on player.open alone
# (the old behavior) would kill the process before load_models had a chance to
# write, false-failing the $hasLoad check below. We poll (up to $SmokeTimeoutSec)
# rather than sleeping a fixed interval: the FIRST cold run of a ~10GB onedir
# bundle loads torch's CUDA DLLs off cold disk and can take well over 25s just to
# reach `import torch`, whereas a warm run gets there in <10s -- a fixed short
# sleep false-fails the cold case. (Startup warmup is load-only now -- it never
# compiles TRT -- so load_models itself is fast; the generous timeout is purely
# for the cold CUDA-DLL load off a ~10GB bundle.) We also bail early if the
# process dies (native DLL crash on `import torch` leaves no Python traceback, so
# an early exit is itself a failure signal).
$SmokeTimeoutSec = 180
$procArgs = @{
    FilePath = Join-Path $distDir "sumu.exe"
    WorkingDirectory = $distDir
    RedirectStandardError = $stderrLog
    RedirectStandardOutput = $stdoutLog
    PassThru = $true
    NoNewWindow = $true
}
if ($hasTestVideo) { $procArgs.ArgumentList = $videoPath }
$proc = Start-Process @procArgs

$exitedEarly = $false
for ($i = 0; $i -lt $SmokeTimeoutSec; $i++) {
    Start-Sleep -Seconds 1
    if ($proc.HasExited) { $exitedEarly = $true; break }
    $log = Get-SmokeLog
    $loadReady = $log -match "== load_models =="
    $openReady = (-not $hasTestVideo) -or ($log -match "== player\.open ==")
    if ($loadReady -and $openReady) { break }
    if ($log -match "Traceback \(most recent call last\)") { break }
}

if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 1
$log = Get-SmokeLog

$hasEnv = $log -match "== env == torch"
$hasLoad = $log -match "== load_models =="
$hasOpen = (-not $hasTestVideo) -or ($log -match "== player\.open ==")
$hasTraceback = $log -match "Traceback \(most recent call last\)"

$smokePassed = $hasEnv -and $hasLoad -and $hasOpen -and (-not $hasTraceback)

if ($smokePassed) {
    Write-Host "SMOKE PASS" -ForegroundColor Green
} else {
    if ($exitedEarly) {
        Write-Host "SMOKE FAIL (process exited early -- likely a native DLL crash on import; check the log)" -ForegroundColor Red
    } else {
        Write-Host "SMOKE FAIL" -ForegroundColor Red
    }
}

Write-Host "---- last 30 lines of $sumuLog (app markers) ----"
if (Test-Path $sumuLog) {
    Get-Content $sumuLog -Tail 30
} else {
    Write-Host "(no sumu.log produced -- app may have crashed before redirect)"
}
Write-Host "---- last 30 lines of $stderrLog (native/C-level sink) ----"
if (Test-Path $stderrLog) {
    Get-Content $stderrLog -Tail 30
} else {
    Write-Host "(no stderr log produced)"
}

# A silent 0-exit on smoke failure previously let the VSCode task show green even though the
# frozen bundle didn't actually come up -- fail loudly so a bad dist\sumu never looks done.
if (-not $smokePassed) {
    Fail "smoke test did not pass (see markers above)"
}
