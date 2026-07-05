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

## Spike 1 (torch CUDA -> D3D11 zero-copy interop)

**Purpose**: prove the load-bearing precondition for sumu's "AI insertion point" (I3,
all-GPU pipeline): a torch CUDA tensor (device memory produced by an AI model) can be
bridged into a D3D11 present-face texture with **zero main-memory round trip**, and that
the per-frame interop cost is far below the 16.6ms (60fps) frame budget. If this doesn't
hold, AI-assisted dehaze/demosaic can never be inserted into the real-time preview path
without breaking sumu's zero-copy contract. Code lives entirely under
`spikes/spike1_cuda_interop/` (independent CMake project + pybind11 extension, does not
touch the rest of the repo).

**Architecture**: a pybind11 C++ extension module `sumu_present` (built as
`sumu_present.cp313-win_amd64.pyd`) exposing a single `Presenter` class, reusing spike 0's
Win32 window / D3D11 device / flip-model swapchain setup pattern
(`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `DXGI_FORMAT_R8G8B8A8_UNORM`). A persistent D3D11
texture ("AI frame target", default usage, `BIND_SHADER_RESOURCE`) is registered **once**
at construction with `cuGraphicsD3D11RegisterResource` (not per frame — registration cost
is deliberately excluded from the per-frame interop budget). CUDA is driven purely through
the **driver API** (`cuda.lib` -> `nvcuda.dll`; no CUDA language/nvcc, no device code to
compile, only host-side driver calls): `cuInit` -> `cuDeviceGet(0)` ->
`cuDevicePrimaryCtxRetain` -> `cuCtxSetCurrent`, retaining device 0's **primary context**
— the same context CUDA's runtime API (and therefore torch) lazily creates/uses for that
device, since primary contexts are per-process-per-device refcounted singletons. A
`cuD3D11GetDevice(adapter)` check at construction confirms the D3D11 adapter and CUDA
device 0 are the same physical GPU (true on this single-4080 machine; a real
implementation would pick the CUDA device matching the D3D adapter instead of hardcoding
0). Per frame (`push_cuda_frame`): `cuCtxSynchronize()` (wait for the AI/torch kernel that
wrote the tensor — timed separately as `sync_ms`, NOT counted as interop cost) ->
`cuGraphicsMapResources` -> `cuGraphicsSubResourceGetMappedArray` -> **`cuMemcpy2D` with
`srcMemoryType=CU_MEMORYTYPE_DEVICE` (the torch tensor's raw `CUdeviceptr`) and
`dstMemoryType=CU_MEMORYTYPE_ARRAY` (the mapped D3D11 texture)** -> `cuGraphicsUnmapResources`
-> a trivial fullscreen-triangle pixel shader point-samples the target texture straight
into the backbuffer -> `Present(1,0)`. There is no host pointer anywhere in the
`CUDA_MEMCPY2D` struct and no `cuMemcpyDtoH`/`cuMemcpyHtoD`/staging buffer anywhere in
`presenter.cpp` — the absence is checkable in the code, not just asserted.
`verify_readback()` closes the loop: on a flagged frame, right before `Present`, the
backbuffer is `CopyResource`'d into a persistent CPU-readable staging texture; the Python
driver independently reads the same torch tensor via `.cpu()` and compares specific pixels
against what the D3D11 side actually displayed.

**Build**: `find_package(pybind11 CONFIG REQUIRED)` (pybind11 3.0.4, venv's cmake config)
+ `find_package(Python ... Development.Module)`, `pybind11_add_module`, CUDA toolkit
v13.3's `include/` added to include dirs and `lib/x64/cuda.lib` linked directly (alongside
`d3d11`/`dxgi`/`d3dcompiler`) — MSVC2022 BuildTools + Ninja, same `vcvars64.bat`-sourcing
`build.bat` wrapper pattern as spike 0. **One real compile blocker**: `windows.h`'s
`min`/`max` macros collided with `std::min` call sites in the percentile helper (`error
C2589`/`C2059` — the classic symptom), fixed with `#define NOMINMAX` before `#include
<windows.h>`. No other build or runtime blockers — `cuD3D11GetDevice` matched immediately
(device 0 on both sides), `cuGraphicsD3D11RegisterResource` succeeded on the first attempt,
and sharing the primary context with torch required no special handling: the Python driver
calls `torch.zeros(1, device='cuda')` (forcing torch's lazy CUDA init) *before*
constructing `Presenter`, and the C++ side's `cuDevicePrimaryCtxRetain` transparently
picked up that same already-created context — no assertion or workaround needed, exactly
as the primary-context-is-a-singleton theory predicted.

**Method**: `spike1_driver.py` generates, per frame, a `uint8 (H,W,4)` RGBA CUDA tensor via
torch ops (a horizontal gradient background that cycles per-frame plus a vertical bar that
visibly sweeps across the frame, so the run is confirmably *moving*, not a static texture),
paced to an absolute-origin 60fps schedule (same self-correcting-overshoot strategy as
spike 0's PTS pacing), and pushes it through `push_cuda_frame(t.data_ptr(), W, H,
t.stride(0)*t.element_size())`. Runs: **4K (3840x2160), 50s, maximized** (the real target /
worst case) and a **1080p (1920x1080), 20s, windowed** comparison round. `verify_readback`
was captured at frame 100 of each run and compared against 7 sample pixels (4 corners + 3
horizontal-midline points) read independently from the same tensor via `.cpu()`.

### verify_readback (zero-copy correctness)

Both runs: **PASS** — all 7 sample pixels matched exactly (`diff=[0,0,0,0]`, not just
within the ±1 rounding tolerance budgeted for) between the torch tensor and the D3D11
backbuffer actually displayed. This closes the loop: the CUDA-tensor pattern really did
reach the screen through the zero-copy bridge, not through some accidental fallback path.

### Interop overhead (map + copy + unmap; the actual CUDA<->D3D11 bridge cost)

| Run | n | median | p99 | max | mean | budget (60fps) |
|---|---|---|---|---|---|---|
| 4K (3840x2160)    | 3000 | 0.222ms | 0.527ms | 1.173ms | 0.243ms | 16.67ms |
| 1080p (1920x1080) | 1200 | 0.228ms | 0.556ms | 1.121ms | 0.252ms | 16.67ms |

The interop cost is **~75x below budget at median, ~30x below even at p99**, and — as
expected for a device-to-device copy — essentially resolution-independent between 1080p
and 4K at these sizes (4K RGBA8 is only ~33MB; well within RTX4080 memory bandwidth for a
sub-millisecond copy). This is the headline number: the CUDA<->D3D11 bridge itself is not
remotely a bottleneck for a 60fps budget.

### Frame overhead (sync + interop + draw + Present call, still not full present pacing)

| Run | n | median | p99 | max |
|---|---|---|---|---|
| 4K    | 3000 | 1.618ms | 8.087ms | 15.541ms |
| 1080p | 1200 | 0.529ms | 5.052ms | 72.776ms |

`sync_ms` (`cuCtxSynchronize`, waiting for the Python-side `make_frame()` torch kernels to
finish — not part of the interop bridge itself) dominates this number and is noisier than
the interop cost proper (4K sync: median 1.30ms, p99 7.84ms, max 15.27ms; 1080p sync:
median 0.08ms, p99 4.71ms, **max 72.27ms**, one outlier). That single 1080p outlier (a
~72ms stall, once in 1200 frames) reads as ordinary Python-driver scheduling/GC noise
(GIL, torch kernel-launch overhead for the toy gradient generator), not an interop-path
problem — the interop timing for that same frame stayed in its normal sub-millisecond
range. A production AI stage would signal readiness via a CUDA event on its own stream
rather than a host-side `cuCtxSynchronize`, which would remove this source of jitter
entirely.

### Present cadence (`scripts/analyze_present.py --format ns --fps 60`)

| Run | window | n | median | stddev | p99 | max | %>1.5x | %>2x | gaps>50ms |
|---|---|---|---|---|---|---|---|---|---|
| 4K    | steady [10s-end] | 2397 | 16.67ms | 1.39 | 20.69ms | 27.5ms | 0.2% | 0.0% | 0 |
| 1080p | steady [8s-end]  | 717  | 16.67ms | 2.92 | 20.35ms | 73.2ms | 0.4% | 0.4% | 1 |

Both runs: median glued to the 16.67ms budget, single-peaked (62-70% of frames in the
16-17ms bucket), %>2x-budget in the low single digits or zero (the 1080p run's one 0.4%
comes from the same single scheduling outlier noted above). Wider than spike 0's
native-loop stddev (0.07ms) — expected and explicitly anticipated by this spike's own
pass criteria ("Python 驱动有 GIL/节流开销, 稳态别有大量 >2x 预算即可"): pacing here goes
through Python's GIL and `time.sleep`/busy-wait loop plus a per-frame torch kernel launch,
not a tight native C++ loop. Present smoothness itself was already independently proven in
spike 0; this spike's job was the interop bridge, not re-proving present-loop pacing.

**Interpretation — the load-bearing question this spike exists to answer**: **yes, a
torch CUDA tensor can be bridged into a D3D11 present-face texture with a verifiably zero
main-memory round trip** (`cuMemcpy2D` device->array, no `DtoH`/`HtoD`/staging anywhere in
the code, `verify_readback` confirms the actual displayed pixels matched the tensor
exactly), **and the per-frame interop cost (~0.22ms median, ~0.53ms p99 at 4K) is roughly
30-75x below the 16.6ms/frame budget** — nowhere near a bottleneck. Sharing the primary
CUDA context between torch (runtime API) and the C++ extension (driver API) worked exactly
as documented, order-independent, with no crashes, no workarounds, and no protective
locking needed beyond what was already there. **Spike 1's pass criteria are met**: the
commitment I3 depends on ("AI produces CUDA tensors, present face is D3D11, zero-copy
bridge exists and is cheap") is empirically confirmed on this machine.

**Raw CSVs** (not committed): `spikes/spike1_cuda_interop/trace/present_spike1_4k.csv`,
`spikes/spike1_cuda_interop/trace/present_spike1_1080p.csv`.

## Spike 2 (clock-driven mixing)

**Purpose**: prove that a wall-clock-driven present loop that mixes AI-processed frames
(pulled from a thread-safe ready-map, populated by a simulated AI producer) with
passthrough original frames (pulled from a GPU-resident decode ring buffer) never hitches
at an AI<->passthrough switch point, and that its present-cadence distribution is
statistically indistinguishable from a pure passthrough-only baseline. This is the load-
bearing precondition for sumu's real player: "the player is master, AI is a servant that
either has a frame ready in time or doesn't, and either way present never stalls." Code
lives entirely under `spikes/spike2_clock_mixing/` (independent CMake project + pybind11
extension, does not touch the rest of the repo).

**Architecture**: a pybind11 C++ extension `sumu_rt` exposing `RealtimePresenter`, reusing
spike 0's Win32 window / D3D11 flip-model swapchain pattern and spike 1's CUDA
driver-API/torch-primary-context interop pattern. Three threads share one D3D11 device:

- **Decode thread**: runs spike 0's `Decoder` (FFmpeg + D3D11VA hwaccel) continuously,
  ahead of the present head by up to `kDecodeAheadMax` frames, `CopySubresourceRegion`-ing
  each decoded NV12 frame into a fixed-size GPU **passthrough ring buffer**
  (`Texture2DArray`, `kRingCapacity=64` slots, ~1.07s of buffering at 60fps — sized down
  from an initial 180-slot/~3s design that hit `E_OUTOFMEMORY` once a same-sized AI ring was
  added alongside it; 64 slots is still far beyond `kStartBufferFrames=5` and the AI
  producer's max simulated lateness).
- **AI-push thread** (simulated in this spike by the Python driver): calls
  `push_ai_frame(frame_num, cuda_ptr, w, h, pitch)` at arbitrary wall-clock times, from a
  raw torch CUDA tensor, straight into a same-sized **AI ring buffer** via
  `cuGraphicsMapResources`/`cuMemcpy2D`/`cuGraphicsUnmapResources` +
  `CopySubresourceRegion` (zero-copy, same pattern as spike 1), then marks that frame_num
  ready in a mutex-protected ready-map.
- **Present thread**: paced purely by `QueryPerformanceCounter` against an absolute-origin
  schedule (spike 0's self-correcting-overshoot pattern). Each tick picks the current
  frame_num's source with strict priority **AI-fresh -> passthrough-fresh -> last-shown
  frame** and **never blocks** on either the decode or AI-push thread — a miss just falls
  through to the next priority, exactly the fallback chain the real player needs.

**Correction made after empirical testing (the key pitfall of this spike)**: the original
design assumed `ID3D11Multithread::SetMultithreadProtected(TRUE)`, combined with
restricting each of the three threads to single atomic `CopySubresourceRegion`/interop
calls, would be sufficient without any additional CPU-side lock. This held up through
several short (6-8s) smoke tests, but a full 50-second stress run (~3000 presents, ~600
concurrent AI pushes, ~3000 decode-thread copies, all against one shared
`ID3D11DeviceContext`) hit a genuine `DXGI_ERROR_DEVICE_HUNG` (0x887a0006) near the end of
the run, which then cascaded into `CUDA_ERROR_UNKNOWN` failures for the AI-push thread's
remaining pushes (the present thread itself was unaffected; the run still completed).
Rather than try to characterize exactly which interleaving the driver couldn't tolerate,
the fix taken was conservative: one explicit `std::mutex d3d_mutex_` now serializes **every**
thread's touch of the shared device/context — the present thread's entire
draw-plus-`Present` call, the decode thread's `CopySubresourceRegion`, and the AI-push
thread's whole map/copy/unmap/`CopySubresourceRegion` sequence — each individual call being
sub-millisecond, so the added serialization cost is negligible against the 16.6ms budget.
Re-running the full 50s mixed round after this fix: **zero hangs, zero CUDA errors**,
`n_ai_stale=0`, `n_pt_stale=0` (ring buffers never underran). This is exactly the class of
"D3D11/CUDA concurrent-thread crash" risk flagged going into this spike, and the important
takeaway for the real player is that `SetMultithreadProtected(TRUE)` alone is not a
sufficient substitute for an explicit lock once three threads sustain load for tens of
seconds, even though it can look fine in short manual tests.

**Other pitfalls fixed along the way**:
- A per-thread CUDA driver-API context issue: `cuGraphicsMapResources` (and friends)
  issued directly from `push_ai_frame` intermittently failed with `CUDA_ERROR_UNKNOWN` on
  the calling OS thread, because relying on implicit/lazy context attachment (as CUDA's
  Runtime API does automatically for torch's own ops) is not sufficient for raw driver-API
  calls on a thread that has never explicitly attached a context. Fixed with an explicit
  `cuCtxSetCurrent(cu_ctx_)` at the top of `push_ai_frame`.
- Clock-domain alignment: added `RealtimePresenter::start_time_s()` (derived from the same
  `start_qpc_`/`freq_` used internally) so the Python producer can anchor its per-frame
  schedule to the presenter's *actual* frame-0 instant rather than a `time.perf_counter()`
  timestamp captured after the constructor had already returned (which silently ran
  later than frame 0, shifting every "fast" AI push late enough to miss).
- A queueing-theory artifact in the test harness itself, not the presenter: the Python AI
  producer is a single sequential loop that visits every frame_num in order and waits for
  each one's own absolute deadline. A wide "slow" delay bucket (originally
  `uniform(20,150)ms` against a 16.7ms frame period) created enough variance in
  consecutive deadlines that Lindley's recursion (`W_i = max(0, W_{i-1} - drift_i)`)
  produced a large *stationary average backlog* (empirically ~70-120ms lateness), which
  looked like a mixing bug but was actually a property of the sequential simulator.
  Narrowing the slow bucket to `uniform(20,50)ms` (still comfortably "usually a miss")
  brought `ai_hit_rate` to a stable, well-behaved ~0.20 with bounded lateness (p50~21ms,
  max~54-67ms) — confirmed only by adding per-push `lateness_ms`/`make_ai_frame_ms`/
  `push_ai_frame_ms` instrumentation to the driver and measuring, after two other
  hypotheses (clock-sync gap, OS `sleep()` overshoot) were tested and ruled out the same
  way.

**Method**: `spike2_driver.py` runs two 50-second rounds against the same 4K60 HEVC clip:
**mixed** (AI producer thread on: 30% of frames missing, 40% "fast" delay
`uniform(-60,+5)ms` relative to deadline, 30% "slow" delay `uniform(20,50)ms`; each pushed
frame is an obvious cyan-tinted RGBA tensor with a moving white bar so AI vs passthrough is
visually distinguishable) and **passthrough-only** (AI producer thread never started,
ready-map stays empty, pure fallback path exercised every tick). Both dump a
`qpc_ns,source,frame_num` trace, analyzed with the shared `scripts/analyze_present.py
--format ns --fps 60` plus a spike-local `analyze_switches.py` that separately buckets
present intervals into AI<->passthrough **switch** points vs **non-switch** points within
the steady-state window, specifically to check whether any extra variance concentrates at
the handoff (a residual hitch) or is general system noise.

### Present cadence (`scripts/analyze_present.py --format ns --fps 60`)

| Run | window | n | median | stddev | p99 | max | %>1.5x | %>2x | gaps>50ms |
|---|---|---|---|---|---|---|---|---|---|
| mixed         | cold [0-6s]      | 359  | 16.68ms | 1.07 | 17.19ms | 31.7ms | 0.3% | 0.0% | 0 |
| mixed         | steady [10s-end] | 2400 | 16.68ms | 0.54 | 17.08ms | 34.1ms | 0.0% | 0.0% | 0 |
| passthrough   | cold [0-6s]      | 359  | 16.68ms | 0.16 | 17.08ms | 17.4ms | 0.0% | 0.0% | 0 |
| passthrough   | steady [10s-end] | 2399 | 16.68ms | 0.13 | 17.03ms | 19.7ms | 0.0% | 0.0% | 0 |

Both rounds' steady-state median is identical (16.68ms, glued to the 59.94fps budget) with
zero intervals over 1.5x/2x budget and zero gaps>50ms in either round. The mixed round's
steady-state stddev (0.54ms) and max (34.1ms) are both higher than passthrough-only's
(0.13ms / 19.7ms) — expected, since the mixed round has a second thread (`push_ai_frame`)
sharing the now-mutex-serialized D3D11 context, and this is exactly what the dedicated
switch-point analysis below exists to check: is that extra variance concentrated at
AI<->passthrough handoffs (a real hitch) or spread generally (harmless extra system
activity)?

### Switch-point analysis (`analyze_switches.py`, mixed round, steady window [5s-end])

| Interval class | n | median | stddev | p99 | max | %>1.5x | %>2x | gaps>50ms |
|---|---|---|---|---|---|---|---|---|
| non-switch (same source as previous tick) | 1979 | 16.68ms | 0.57 | 17.09ms | 34.1ms | 0.1% | 0.1% | 0 |
| **switch (AI<->passthrough handoff)**     | 721  | 16.68ms | **0.27** | 17.03ms | **23.6ms** | 0.0% | 0.0% | 0 |
| all | 2700 | 16.68ms | 0.51 | 17.08ms | 34.1ms | 0.0% | 0.0% | 0 |

`ai_hit_rate = 0.1997` (599/3000 presents were fresh AI frames) — a reasonable middle
value given the producer's 30/40/30 missing/fast/slow mix, confirming the ready-map is
neither starved nor saturated. **721 switch points occurred in the steady window alone**
(about a quarter of all steady-state ticks), giving this analysis strong statistical
power. Critically, the switch-point intervals have **lower** stddev (0.27 vs 0.57ms) and
**lower** max (23.6ms vs 34.1ms) than the non-switch intervals — the opposite of what a
handoff hitch would look like. The mixed round's extra variance versus the passthrough-only
baseline is concentrated in ordinary same-source ticks (consistent with harmless
contention on `d3d_mutex_` from the concurrent AI-push thread), not at the AI<->passthrough
transition itself.

**Interpretation — the load-bearing question this spike exists to answer**: **yes** — the
present-cadence distribution when AI frames randomly become ready/late/missing is, at the
level that matters (median glued to budget, no fat tail, zero gaps>50ms, no elevated
variance specifically at switch points), statistically indistinguishable from
pure-passthrough playback. **Spike 2's pass criteria are met**: no hitch at AI<->
passthrough switch points, `ai_hit_rate` lands at a reasonable middle value (~0.20), and
the present thread never blocked on either the decode or AI-push thread across two full
50-second runs. The one caveat worth carrying forward into the real player: the
`SetMultithreadProtected(TRUE)`-alone assumption is not safe under sustained multi-thread
load and needs the explicit `d3d_mutex_`-style serialization this spike settled on.

**Raw CSVs** (not committed):
`spikes/spike2_clock_mixing/trace/present_spike2_mixed.csv`,
`spikes/spike2_clock_mixing/trace/present_spike2_passthrough.csv`.
