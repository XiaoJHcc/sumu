# Spike 3 (D3D11 decoded NV12 -> CUDA -> torch, reverse-direction interop)

**Purpose**: prove the other half of sumu's AI-insertion precondition (I3, all-GPU
pipeline) that spike 1 didn't cover. Spike 1 proved torch CUDA -> D3D11 (AI output onto
the present face). Spike 3 proves the **reverse** direction that the real AI path
actually needs first: a **decoder-produced D3D11 NV12 texture -> CUDA -> torch tensor**,
zero main-memory round trip, so the AI core (which expects an NV12-or-BGR CUDA tensor,
see lada's `_nv12_to_bgr_hwc_gpu`) can consume hardware-decoded frames without ever
touching host memory. Code lives entirely under `spikes/spike3_nv12_interop/`
(independent CMake project + pybind11 extension; reuses `native/src/decoder.{h,cpp}`
copied verbatim, does not touch the rest of the repo).

## Hard blocker (documented, not just anticipated) and the workaround

`cudaD3D11.h`'s own doc comment on `cuGraphicsD3D11RegisterResource()` restricts
registrable resources to single-plane 1/2/4-channel 8/16/32-bit formats (`A8_UNORM`,
`B8G8R8A8_UNORM`, `R8_UNORM`, `R8G8_UNORM`, `R8G8B8A8_UNORM`, the 16/32-bit float/int
variants, etc.) — **`DXGI_FORMAT_NV12` (bi-planar 4:2:0) is not in that list** and can
never be registered directly, confirmed by design (not empirically re-derived by trying
and failing, since the header is authoritative and the task's own suggested workaround
already assumed this).

**Workaround used** (an established pattern already present elsewhere in this repo, see
spike0/spike2's decoder/presenter code): D3D11 allows building a Shader Resource View
directly on an NV12 texture with a **format override** — `DXGI_FORMAT_R8_UNORM` addresses
the full-resolution luma plane, `DXGI_FORMAT_R8G8_UNORM` addresses the half-resolution
chroma plane. Both overrides ARE CUDA-interop-supported formats. So, once per decoded
frame:

1. Two fullscreen-triangle draws point-sample those plane SRVs and write straight (1:1
   texel "identity blit") into two **persistent, plain (non-array)** render targets:
   `y_tex_` (`R8_UNORM`) and `uv_tex_` (`R8G8_UNORM`, half resolution).
2. Those two plain textures are registered with CUDA **once** (lazily, on the first
   decoded frame — see the padding pitfall below for why lazily and not at `open()`).
   Registration cost is excluded from the per-frame budget, matching spike 1's policy.
3. Per frame: `cuGraphicsMapResources` -> `cuGraphicsSubResourceGetMappedArray` (one
   `CUarray` per plane) -> `cuMemcpy2D(ARRAY -> DEVICE)` straight into a caller-supplied
   CUDA device buffer, at the row offsets that reproduce PyAV's/lada's NV12 stacked layout
   (rows `[0,h)` = luma, rows `[h, h*3/2)` = interleaved chroma) -> `cuGraphicsUnmapResources`.
   Both memcpy sides are GPU memory (`ARRAY` src, `DEVICE` dst) — **no
   `cuMemcpyDtoH`/`HtoD` anywhere in `interop.cpp`**, matching spike 1's zero-copy proof
   style (checkable in the code, not just asserted).
4. The caller-supplied buffer is just a `torch.empty((h*3//2, w), dtype=uint8,
   device='cuda')` tensor's own `.data_ptr()` — this avoids `__cuda_array_interface__`/
   DLPack plumbing entirely by flipping the ownership: Python allocates the destination
   tensor, C++ writes into it (mirrors spike 1's `push_cuda_frame(dev_ptr, ...)`
   convention, direction reversed).

CUDA is driven purely through the **driver API** (`cuda.lib` -> `nvcuda.dll`; no nvcc, no
device code to compile), sharing torch's lazily-created **primary context**
(`cuDevicePrimaryCtxRetain`/`cuCtxSetCurrent`), exactly as spike 1 established.

## Real bug found and fixed during bring-up: macroblock-padded decode texture

The first correctness pass came back with mean MAE ≈ 8.5 (vs. lada's own stated "~1,
AI-transparent" expectation for this exact colour-convert formula) — not a small
numerical fuzz, a real bug. Isolating it by diffing the **raw NV12 planes** (before any
colour conversion) against PyAV's `to_ndarray(format='nv12')` showed the Y plane matching
exactly for the first ~66 rows, then diverging by up to 154/255 from row 67 down to the
bottom of the frame (1080p, `test_video.mp4`).

Root cause: FFmpeg's d3d11va hwaccel allocates its frame-pool texture at the **coded**
size (macroblock-aligned, a multiple of 16), not the stream's **display** size. For this
1080p content, `stream->codecpar->height == 1080` (used by `decoder.height()`) but the
actual `ID3D11Texture2D` backing the decode pool is **1920x1088** (confirmed by querying
`ID3D11Texture2D::GetDesc()` on the decoder's own texture — added as a one-off debug
probe during bring-up, since removed). My fullscreen-triangle identity blit's UV mapping
spans `[0,1]` over the **full source texture extent**; sizing the plane render targets
and viewport to the **display** height (1080) instead of the **real** texture height
(1088) silently **vertically stretched** the image by a factor of `1088/1080 ≈ 1.0074` —
a sub-pixel-per-row drift that only becomes visible against content with sharp edges,
which is exactly the "differences appear partway down the frame, get worse toward the
bottom" symptom observed. (2160p was unaffected since 2160 is already a multiple of 16 —
this bug is 1080p/1088-specific on this content, but would recur for any height not a
multiple of 16.)

**Fix**: plane-target creation and CUDA registration were moved from `open()` to a
lazy, one-time init on the **first** `next_frame_into()` call, once the decoder's real
texture size is known (`ID3D11Texture2D::GetDesc()` on the first decoded frame's
texture). `y_tex_`/`uv_tex_` and the render-pass viewports are now sized to that **real**
(possibly padded) size — a true 1:1 texel identity blit, no stretch — while the CUDA
`cuMemcpy2D` calls still crop to the stream's **visible** `width_`/`height_` (this assumes
top-left-anchored cropping, i.e. any padding is added at the bottom/right, which is how
d3d11va/DXVA frame pools pad; true for both test videos here). After the fix, a direct
raw-NV12-plane diff against PyAV came back **bit-exact (MAE = 0.0, max diff = 0.0)** on
both the Y and UV planes, across 5 frames checked.

## Second bug found: PyAV `color_range` enum value used incorrectly in the driver

After the padding fix, MAE was still ≈ 6-8 on the full BGR conversion (raw NV12 was
already bit-exact, so this second discrepancy was isolated to the colour-convert /
reference-comparison step, not the interop bridge). Cause: `libavutil/pixfmt.h`'s
`AVColorRange` enum is `AVCOL_RANGE_UNSPECIFIED=0`, **`AVCOL_RANGE_MPEG=1`** ("tv",
limited range), **`AVCOL_RANGE_JPEG=2`** ("pc", full range) — confirmed against the
header and cross-checked with `ffprobe -show_entries stream=color_range` on both test
files (`color_range=tv` while PyAV's `frame.color_range == 1`). My driver's first draft
mapped `full_range = (frame.color_range == 1)`, mislabelling value 1 as `AVCOL_RANGE_JPEG`
— this mirrors a comment in lada-realtime's `video_utils.py` that has the same
mislabelling, presumably harmless there only because lada's own test content is tagged
differently or the discrepancy was never checked against ground truth this precisely.
Fixed to `full_range = (frame.color_range == 2)`; both test videos are actually BT.709
**limited** range (`bt709=True, full_range=False`). After this fix MAE dropped to the
expected ballpark (see results below).

## Correctness (MAE vs. independent PyAV CPU `to_ndarray('bgr24')` decode)

For each video: `n_verify` frames decoded through the full bridge (`next_frame_into` +
a local, spike-only GPU colour-convert function adapted from lada-realtime's
`_nv12_to_bgr_hwc_gpu` — see caveat below) are diffed pixel-by-pixel (MAE) against an
**independently PyAV-decoded** reference of the same file/frames.

| Video | Resolution | n | mean MAE | max MAE |
|---|---|---|---|---|
| `test_video.mp4` | 1920x1080 | 60 | **0.6994** | 0.7033 |
| `test_video_4k.mp4` | 3840x2160 | 60 | **0.6215** | 0.6277 |

Both land right in the "~1, AI-transparent" ballpark lada's own docstring for this
formula claims relative to libswscale's `bgr24` output — i.e. **this is a real,
independently-verified pass**, not a skipped or faked check. (Colour tags used, read
from the PyAV reference frame and cross-checked with `ffprobe`: both files are
`bt709=True, full_range=False`.)

## Throughput (N=300 frames, full bridge + colour-convert, python-loop-synced end to end)

Per-frame loop: `next_frame_into()` (decode + SRV + render-blit + CUDA
map/copy/unmap) -> local GPU colour-convert -> `torch.cuda.synchronize()`. This measures
a **synchronous, worst-case-honest** throughput (a real pipelined scheduler that doesn't
sync every frame would do better); it is not cherry-picked to look good.

| Video | n | fps (synced e2e) | mean e2e/frame |
|---|---|---|---|
| 1080p (`test_video.mp4`) | 300 | **551.6 fps** | 1.81ms |
| 4K (`test_video_4k.mp4`) | 300 | **278.5 fps** | 3.59ms |

Both are far beyond the 60fps (16.7ms/frame) target budget for the bridge alone —
this component is not the pipeline's bottleneck at either resolution.

### Per-stage breakdown (C++ side, `interop.stats()`; steady-state median/p99/max, ms)

| Stage | 1080p median | 1080p p99 | 1080p max | 4K median | 4K p99 | 4K max |
|---|---|---|---|---|---|---|
| decode (FFmpeg d3d11va) | 0.109 | 0.421 | 22.7 (first-frame cold start) | 0.163 | 1.243 | 29.1 (cold start) |
| SRV cache lookup | 0.0004 | 0.011 | 0.024 | 0.0005 | 0.015 | 0.025 |
| render blit (2 draws + Flush) | 0.033 | 0.102 | 0.427 | 0.039 | 0.113 | 0.538 |
| `cuGraphicsMapResources` | 0.044 | 0.109 | 0.157 | 0.052 | 0.114 | 0.315 |
| `cuMemcpy2D` x2 (Y+UV) | 0.025 | 0.056 | 0.078 | 0.027 | 0.073 | 0.132 |
| `cuGraphicsUnmapResources` | 0.030 | 0.083 | 0.119 | 0.031 | 0.125 | 0.168 |
| **total (C++ bridge)** | **0.246** | **0.589** | 23.4 (cold) | **0.318** | **1.488** | 30.2 (cold) |
| python colour-convert (torch, fp16) | 1.426 | 2.161 | 2.263 | 3.030 | 3.797 | 4.046 |

The entire C++ bridge (decode + interop) is sub-millisecond at steady state for **both**
resolutions — the python-side colour-convert (a straightforward, unoptimized fp16 torch
op sequence, not the fused/optimized kernel a production path would use) is actually the
larger cost, ~1.4ms at 1080p and ~3.0ms at 4K, still comfortably within a 60fps budget on
its own. The interop-specific steps (map/copy/unmap) are ~0.10ms combined at both
resolutions — essentially resolution-independent at these frame sizes, consistent with
spike 1's finding for the reverse direction.

**Zero main-memory round trip — verified by code inspection, not just claimed**: outside
of the correctness-check path (which deliberately calls `.cpu()` to build the ground-truth
comparison, that's the whole point of that check), the throughput/bench loop and
`interop.cpp` never call `cuMemcpyDtoH`/`HtoD`, `.cpu()`, or any host-staging texture —
grep confirms the only `CU_MEMORYTYPE_ARRAY`/`CU_MEMORYTYPE_DEVICE` pair appears in both
`cuMemcpy2D` calls, matching spike 1's "absence checkable in the code" standard.

## Discrepancy found in the task's premise (flagged, not silently worked around)

The task specification asserted `_nv12_to_bgr_hwc_gpu` was already ported (照搬) into
`python/sumu/ai/utils/`. **This is not the case** — `python/sumu/ai/utils/` currently
contains only `image_utils.py`, `torch_letterbox.py`, `ultralytics_utils.py`, and
`__init__.py`; the function only exists in `lada-realtime/lada/utils/video_utils.py`
(CPU-numpy-input variant) and has not been ported. Per this spike's file-scope
restriction (`spikes/spike3_nv12_interop/` and `docs/spike3_nv12_interop.md` only), the
function was **not** added to `python/sumu/`; instead `spike3_driver.py` carries a
local, spike-only adaptation (same math, minus the `torch.from_numpy(...).to(device)`
upload step since the input here is already CUDA-resident) — clearly commented as
adapted-from-lada, credited, and not meant to be the production copy.

## sumu_core API recommendation

For the eventual scheduler's "get CUDA BGR tensor by frame number" interface, based on
what this spike + spike 1 + spike 2 collectively proved:

1. **Keep the two-stage split this spike validated**: a native decode-bridge call that
   fills a **caller-owned** CUDA buffer (`next_frame_into(dev_ptr, pitch_bytes)` style —
   avoids `__cuda_array_interface__`/DLPack entirely, proven cheap: sub-millisecond at
   both 1080p and 4K), followed by a **separate** Python/torch colour-convert step. Don't
   fuse them into one opaque native call — the colour-convert is the larger, more
   likely-to-change cost (fp16 math, BT.601/709, range flags), and keeping it in Python
   keeps it iterable without a native rebuild.
2. **Register plane textures lazily against the decoder's real (possibly padded)
   texture size**, never the stream's reported display size — this spike's own bug is a
   trap the real `sumu_core` decode-bridge must not repeat. Any height not a multiple of
   16 (very common: 1080, 720, etc.) will hit this.
3. **Derive `bt709`/`full_range` from the actual decoded stream's tags** (`AVCOL_SPC_*`,
   `AVCOL_RANGE_MPEG=1`/`AVCOL_RANGE_JPEG=2` — get the enum values right, this spike's
   second bug), not a hardcoded assumption — different source content will vary, and a
   wrong flag silently produces a plausible-looking but wrong-by-~6-8-MAE image with no
   crash to signal it.
4. **Expose a frame-number-indexed API on top of this bridge, not a bare
   "next decoded frame"** — this spike deliberately kept it simple
   (`next_frame_into` = "give me whatever's next"), but the real scheduler needs the
   ready-map contract from `DESIGN.md`/spike 2 (frame number is the source of truth,
   I5): `get_cuda_bgr_by_frame(frame_num) -> (tensor, ready: bool)`, non-blocking,
   falling back to "not ready" rather than blocking decode, so the present loop's
   AI-fresh -> passthrough-fresh -> last-shown priority chain (already proven in spike 2)
   composes cleanly on top.
5. **Budget-wise this bridge is a non-issue**: even the unoptimized colour-convert
   leaves ~13ms/frame of headroom at 1080p60 and ~13.5ms at 4K30 (or ~10ms at 4K60) before
   the actual AI model (detection + restoration) runs at all — the real bottleneck for
   the 4K60 target will be the AI model's own inference cost (Phase 4), not this bridge.

## Pitfalls summary (for future spike/porting work)

- `DXGI_FORMAT_NV12` cannot be registered with CUDA directly (documented in
  `cudaD3D11.h`); the R8/R8G8-plane-SRV-override + identity-blit workaround (already
  established elsewhere in this repo) works and costs essentially nothing per frame.
- **Decoder texture pools are padded to the coded (macroblock-aligned) size, not the
  display size** — query the real texture dimensions via `GetDesc()` rather than trusting
  `codecpar->height`/`width`, size any per-plane blit targets to the real size, and only
  crop to display size at the final copy-out step.
- `AVCOL_RANGE_MPEG` = 1 (limited/"tv"), `AVCOL_RANGE_JPEG` = 2 (full/"pc") — easy to get
  backwards (a comment in lada-realtime's own source does), and a wrong value doesn't
  crash, it just silently produces a plausible-but-wrong image with elevated MAE.
- MSYS2/Git-Bash mangles `cmd.exe /c <path>` arguments starting with `/c`, `/d`, etc.
  unless `MSYS_NO_PATHCONV=1` is set; even with that set, a bare relative `build.bat`
  filename was intermittently not found — using the full absolute Windows path in the
  `/c` argument was reliable.
- `NOMINMAX` before `<windows.h>` and `#include <d3d11_4.h>` (for `ID3D11Multithread`)
  are both required when mixing D3D11 + STL `std::min`/`std::max`, matching spike 1's
  precedent exactly.
- The task's premise that `_nv12_to_bgr_hwc_gpu` was already ported into
  `python/sumu/ai/utils/` was incorrect at the time of this spike — it still only exists
  in `lada-realtime`.

**Raw traces** (not committed): `spikes/spike3_nv12_interop/trace/spike3_throughput_1080p.csv`,
`spikes/spike3_nv12_interop/trace/spike3_throughput_4k.csv`.
