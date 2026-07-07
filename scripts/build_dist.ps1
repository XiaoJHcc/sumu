# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# One-command build pipeline for the daily-use frozen player (PyInstaller onedir bundle):
#   native build -> patch third-party deps -> pyinstaller freeze -> stage weights -> smoke test.
# Usage:
#   powershell -File scripts/build_dist.ps1 [-WeightsSrc <dir>] [-SkipNative]
param(
    [string]$WeightsSrc = "D:/Git/lada-realtime/model_weights",
    [switch]$SkipNative
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
Write-Host "== [3/5] freezing with PyInstaller (packaging/sumu.spec) ==" -ForegroundColor Cyan
Invoke-Native "`"$RepoRoot\.venv\Scripts\python.exe`" -m PyInstaller packaging/sumu.spec --noconfirm" "PyInstaller freeze failed" | Out-Null
$distDir = Join-Path $RepoRoot "dist\sumu"
if (-not (Test-Path (Join-Path $distDir "sumu.exe"))) {
    Fail "expected dist\sumu\sumu.exe not found after freeze"
}
Write-Host "freeze OK: $distDir" -ForegroundColor Green

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

$subEnginesName = "lada_mosaic_restoration_model_generic_v1.2_sub_engines"
$subEnginesSrc = Join-Path $WeightsSrc $subEnginesName
if (-not (Test-Path $subEnginesSrc)) {
    Fail "missing TRT engine dir: $subEnginesSrc"
}
Copy-Item -Path $subEnginesSrc -Destination (Join-Path $weightsDst $subEnginesName) -Recurse -Force
Write-Host "weights staged OK: $weightsDst" -ForegroundColor Green

# --- 5. bounded, non-interactive smoke test ---------------------------------
Write-Host "== [5/5] smoke testing dist\sumu\sumu.exe ==" -ForegroundColor Cyan
$videoPath = Join-Path $RepoRoot "test_video.mp4"
$stderrLog = Join-Path $RepoRoot "build_smoke_stderr.log"
$stdoutLog = Join-Path $RepoRoot "build_smoke_stdout.log"
Remove-Item -Path $stderrLog, $stdoutLog -ErrorAction SilentlyContinue

# The player window has no auto-timeout, so we poll its stderr for the
# "reached real playback" marker (== player.open ==) and kill it once seen.
# We poll (up to $SmokeTimeoutSec) rather than sleeping a fixed interval: the
# FIRST cold run of a ~10GB onedir bundle loads torch's CUDA DLLs off cold disk
# and can take well over 25s just to reach `import torch`, whereas a warm run
# gets there in <10s -- a fixed short sleep false-fails the cold case. We also
# bail early if the process dies (native DLL crash on `import torch` leaves no
# Python traceback, so an early exit is itself a failure signal).
$SmokeTimeoutSec = 120
$proc = Start-Process -FilePath (Join-Path $distDir "sumu.exe") `
    -ArgumentList $videoPath `
    -WorkingDirectory $distDir `
    -RedirectStandardError $stderrLog `
    -RedirectStandardOutput $stdoutLog `
    -PassThru -NoNewWindow

$exitedEarly = $false
for ($i = 0; $i -lt $SmokeTimeoutSec; $i++) {
    Start-Sleep -Seconds 1
    if ($proc.HasExited) { $exitedEarly = $true; break }
    $log = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw -ErrorAction SilentlyContinue } else { "" }
    if ($log -match "== player\.open ==") { break }
    if ($log -match "Traceback \(most recent call last\)") { break }
}

if (-not $proc.HasExited) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds 1
$log = if (Test-Path $stderrLog) { Get-Content $stderrLog -Raw } else { "" }

$hasEnv = $log -match "== env == torch"
$hasLoad = $log -match "== load_models =="
$hasOpen = $log -match "== player\.open =="
$hasTraceback = $log -match "Traceback \(most recent call last\)"

if ($hasEnv -and $hasLoad -and $hasOpen -and (-not $hasTraceback)) {
    Write-Host "SMOKE PASS" -ForegroundColor Green
} else {
    if ($exitedEarly) {
        Write-Host "SMOKE FAIL (process exited early -- likely a native DLL crash on import; check the log)" -ForegroundColor Red
    } else {
        Write-Host "SMOKE FAIL" -ForegroundColor Red
    }
}

Write-Host "---- last 30 lines of $stderrLog ----"
if (Test-Path $stderrLog) {
    Get-Content $stderrLog -Tail 30
} else {
    Write-Host "(no stderr log produced)"
}
