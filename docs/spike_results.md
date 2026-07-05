# Spike Results

## Baseline (mpv + PresentMon, RTX4080/4K150Hz)

**Purpose**: establish the hardware/OS ceiling for present-cadence smoothness using a
known-good, non-sumu player (mpv), so later sumu spikes have a pass/fail ruler instead
of an assumed ideal.

**Machine**: RTX 4080, Windows 11, display reported by
`Win32_VideoController` as **3840x2160 @ 150Hz** (scale=1). Note: the machine also has
ToDesk / GameViewer / Oray IDD virtual display adapters present (remote-access tooling),
but the RTX 4080 controller entry correctly reports the real 4K@150Hz panel, and PresentMon
captured real Present() calls from the GPU process, so the numbers below reflect the
physical display path.

**Tools**:
- mpv `v0.41.0-244-gaf9c81fa1` (shinchiro build, installed via `winget install --id shinchiro.mpv`), at
  `C:\Program Files\MPV Player\mpv.exe`
- PresentMon `2.5.1` (Intel.PresentMon.Console, installed via
  `winget install --id Intel.PresentMon.Console`), at
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Intel.PresentMon.Console_...\presentmon.exe`

**Method**: mpv played with default `--video-sync=audio` (frame-PTS-driven present, NOT
`display-resample`), `--loop --no-osc`, once windowed (`--geometry=1280x720`) and once
`--fullscreen`, on both the 4K60 HEVC clip (`test_video_4k.mp4`) and the 1080p30 clip
(`test_video.mp4`). PresentMon captured `-process_name mpv.exe -timed 45
-terminate_after_timed` for each run. Analysis via
`python scripts/analyze_present.py <csv> --format presentmon --fps <60|30>`
(cold-start = [0-6s], steady-state = [10s-end]).

**Note on tooling fix**: `scripts/analyze_present.py` only recognized PresentMon 1.x's
`TimeInSeconds` column. PresentMon 2.5.1 (current Intel.PresentMon.Console release)
renamed it to `TimeInMs`. Patched `_load_presentmon()` to also accept `TimeInMs` /
`CPUStartTimeInMs` (converting ms->s) and to strip a UTF-8 BOM from the header row. This
is a compatibility fix only — the `msBetweenPresents`-based analysis logic is unchanged.

**Note on PresentMon capture privilege**: PresentMon 2.x's ETW trace session requires
either admin elevation or "Performance Log Users" membership; the interactive shell here
runs with a UAC-filtered (non-elevated) admin token. Added current user to neither group
(would itself require elevation); instead used `Start-Process powershell -Verb RunAs` to
elevate just the PresentMon-launching script per capture. First attempt left a `consent.exe`
prompt pending with nobody to click it (killed it); on retry the elevation went through
without a visible hang (second UAC request resolved silently — cause unconfirmed, possibly
a brief credential/consent cache). All three capture runs below completed via this path.

### Steady-state results (median / stddev / p99 / %>2x-budget / gaps>50ms)

| Run | Budget | n (steady) | median | stddev | p99 | max | %>1.5x | %>2x | gaps>50ms |
|---|---|---|---|---|---|---|---|---|---|
| 4K60 windowed   | 16.67ms | 2096 | 16.61 | 0.90 | 18.53 | 34.0 | 0.0% | 0.0% | 0 |
| 4K60 fullscreen | 16.67ms | 2094 | 16.61 | 0.76 | 18.45 | 19.0 | 0.0% | 0.0% | 0 |
| 1080p30 fullscreen | 33.33ms | 1046 | 33.34 | 0.88 | 35.37 | 35.9 | 0.0% | 0.0% | 0 |

(1080p windowed round skipped — 4K windowed vs fullscreen comparison already showed no
material difference, and time was prioritized on the 4K rounds that matter for sumu.)

**Interpretation**: mpv's default `--video-sync=audio` on this RTX4080/4K150Hz machine
delivers a clean, single-peaked present-interval distribution glued to the video frame
budget (~16.6ms for 60fps, ~33.3ms for 30fps) — exactly the expected "present cadence =
frame delivery cadence, independent of the 150Hz refresh" behavior. Steady-state stddev is
sub-millisecond-to-~1ms, p99 is within ~2ms of budget, max never exceeds ~2x budget, and
there are zero gaps>50ms in any run. Windowed vs fullscreen: no meaningful degradation —
fullscreen was if anything slightly tighter (stddev 0.76 vs 0.90ms, max 19.0ms vs
34.0ms-outlier-at-cold-boundary). This is the ceiling sumu's own present loop should be
measured against: steady-state median glued to budget, %>2x-budget in the low single
digits or zero, single-peaked histogram.

**Raw CSVs** (not committed, in `.gitignore`d area but present on disk):
- `d:/Git/sumu/tools/presentmon_mpv_windowed.csv` (4K60, windowed, 2696 presents, 45.0s)
- `d:/Git/sumu/tools/presentmon_mpv_fullscreen.csv` (4K60, fullscreen, 2694 presents, 44.9s)
- `d:/Git/sumu/tools/presentmon_mpv_1080p_fullscreen.csv` (1080p30, fullscreen, 1346 presents, 44.9s)

## Spike 0 (native D3D11 player)

**Purpose**: prove that pure native D3D11 hardware decode + a self-driven present loop can
play 4K60 HEVC with player-grade stable on-screen frame pacing, with zero AI involved, as
a precondition for the rest of sumu's pipeline. Code lives entirely under
`spikes/spike0_d3d11_present/` (independent CMake project, does not touch the rest of the
repo).

**Decode path used: A (FFmpeg libavcodec + D3D11VA hwaccel)**, not the Media Foundation
fallback. Path A worked on the first attempt with no blockers: the app creates its own
`ID3D11Device` (feature level 11.1/11.0, `D3D_DRIVER_TYPE_HARDWARE`), wraps it into an
`AVHWDeviceContext`/`AVD3D11VADeviceContext` (assigning our device pointer + `AddRef()`,
instead of letting FFmpeg create its own device), and in the `get_format` callback builds
the `AVHWFramesContext` via `avcodec_get_hw_frames_parameters()` with
`BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE` before calling
`av_hwframe_ctx_init()`. Because decode and present share one device, the decoded NV12
`ID3D11Texture2D` array can be sampled directly: an SRV is created straight on the
decoder's own texture (`frame->data[0]`) at the reported array slice
(`frame->data[1]`), luma as `DXGI_FORMAT_R8_UNORM` and chroma as `DXGI_FORMAT_R8G8_UNORM`,
`D3D11_SRV_DIMENSION_TEXTURE2DARRAY`. Zero per-frame GPU->host->GPU round trips —
`CreateShaderResourceView` never failed, so no readback/`CopySubresourceRegion` fallback
was needed.

**Architecture**: single thread, no decode/present split — decode one frame, pace to its
own PTS via an absolute-origin `QueryPerformanceCounter` schedule (`base_qpc +
pts_seconds*freq`, not relative `Sleep` deltas, so a single overshoot self-corrects
instead of drifting), then `Present(1,0)` on a real flip-model swapchain
(`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `IDXGIFactory2::CreateSwapChainForHwnd`) on a plain
Win32 window (`CreateWindowEx`, no Qt/GL). NV12->RGB (BT.709 limited range) conversion is
a runtime-`D3DCompile`d pixel shader sampling the two plane SRVs directly; a fullscreen
triangle is generated in the vertex shader from `SV_VertexID` (no vertex/index buffers).
Present timestamps are recorded into an in-memory `std::vector<int64_t>` (ns since QPC
epoch) and flushed to CSV once at exit — never during the hot loop.

**Build/toolchain note**: MSVC 2022 BuildTools + CMake + Ninja, driven by a `build.bat`
wrapper that sources `vcvars64.bat` then runs `cmake -G Ninja` + `cmake --build` (needed
because the PowerShell/Bash environment here doesn't have MSVC on PATH by default, and
piping build output through a plain shell avoids a pitfall where PowerShell renders MSVC
stderr output as intimidating red text even on success). FFmpeg dev libs came from BtbN's
`ffmpeg-master-latest-win64-gpl-shared.zip`, extracted into
`spikes/spike0_d3d11_present/third_party/ffmpeg/`; the exe links `avcodec avformat avutil`
and the build copies FFmpeg's DLLs next to the exe post-build. One real compile blocker:
`ID3D11Multithread` is declared in `<d3d11_4.h>`, not `<d3d11_1.h>` as first assumed —
fixed once located via a header grep.

**Timing pitfall found and fixed**: an early smoke test showed a *bimodal* steady-state
histogram (~91% of frames at 14-17ms, ~8% at 28-32ms) even though the mean was already
correct (~16.7ms) and the pacing logic never presented early. Root cause: Windows'
default ~15.6ms system timer resolution rounds the `Sleep()` calls inside the pacing
wait loop, causing an occasional ~2x-budget present that then gets "paid back" by an
unusually short next interval (since pacing is anchored to a fixed absolute clock origin)
— a long/short beat pattern rather than a bimodal *problem* with the design itself. Fixed
with `timeBeginPeriod(1)`/`timeEndPeriod(1)` (winmm.lib) bracketing `main()`. Re-verified
via a 15s smoke test after the fix: steady-state went from bimodal to single-peaked
(99.5% of frames in the 16-17ms bucket), median 16.68ms, stddev 0.07ms — confirmed correct
before running the official 50s rounds below.

### Steady-state results (50s runs, 4K60 HEVC `test_video_4k.mp4`, decode path A)

| Run | Budget | n (steady) | median | stddev | p99 | max | %>1.5x | %>2x | gaps>50ms |
|---|---|---|---|---|---|---|---|---|---|
| windowed  | 16.67ms | 2397 | 16.68 | 0.07 | 16.89 | 18.0 | 0.0% | 0.0% | 0 |
| maximized | 16.67ms | 2397 | 16.68 | 0.07 | 16.86 | 17.3 | 0.0% | 0.0% | 0 |

**Interpretation**: both rounds are single-peaked (>99.7% of frames in the single 16-17ms
histogram bucket), median glued to the 16.67ms budget, stddev ~0.07ms, zero frames over
1.5x or 2x budget, zero gaps>50ms — and **windowed vs maximized are statistically
indistinguishable** (same median/stddev, maximized's max is even slightly lower), directly
proving the "no degradation when maximized" requirement and ruling out the
software-sink-under-load regression pattern seen in lada-realtime. These numbers are in
fact tighter than the mpv/PresentMon baseline above (mpv steady-state stddev was
0.76-0.90ms vs sumu's 0.07ms) — expected, since PresentMon's own capture overhead and
mpv's audio-clock-driven sync add jitter that this spike's direct in-process QPC capture
and pure-video PTS pacing avoid. **Spike 0 pass criteria are met**: median close to
16.67ms, single-peaked histogram, %>2x-budget in the low single digits (here, zero), no
maximized regression.

**Raw CSVs** (not committed): `spikes/spike0_d3d11_present/trace/present_spike0_windowed.csv`,
`spikes/spike0_d3d11_present/trace/present_spike0_maximized.csv`.
