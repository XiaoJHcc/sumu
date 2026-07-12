<p align="center">
  <img src="assets/generated/sumu-logo-256.png" alt="Sumu" width="128" height="128">
</p>

<h1 align="center">Sumu</h1>

<p align="center">
  <strong><a href="README.md">中文</a> | English</strong>
</p>

<p align="center">
  <em>A clock-driven, fully GPU-resident player for <b>real-time</b> mosaic removal</em>
</p>

<p align="center">
  Designed from the ground up for play-while-restoring — hardware decode, native D3D11 present, AI kept on the GPU end to end, pipeline always warm
</p>

---

## The Idea

**Ship a real player first. Bolt AI onto it second.**

Mosaic-removal AI is expensive. Trying to restore while the video plays usually means either falling behind or freezing the frame. Most tools either bake everything offline into an export, or simply wait whenever the GPU can't keep up.

Sumu inverts that priority stack:

**Playback always wins. AI does what it can in the background. Video never stalls for AI.**

Get the player right first — with or without AI, video has to stay smooth and seeking has to stay snappy. Then hang AI off as a separate background path that tries to restore whatever is currently on screen. When a restored frame is ready, it swaps in; when it isn't, the original keeps playing. **Playback is never interrupted.**

> You still need a powerful GPU — otherwise you'll fall back to the original a lot.


## Hardware Requirements

- **Windows**
- **NVIDIA GPU** (other GPUs can run, but without TensorRT the throughput is much worse)

| Comfortable | RTX 4080 | RTX 5070 Ti |
| --- | --- | --- |
| Minimum | RTX 4070 | RTX 3080 |

> Even on a powerful GPU, smooth real-time restore still assumes:
> - Default clip length is 30 frames for quick response, which produces a regular once-per-second hitch.
> - At most one mosaic region is processed at a time; multiple regions on screen will flicker. Raising the region limit multiplies cost.
> - AI is tuned for up to 30 FPS; at 60 FPS it falls behind often. You can also drop the playback frame rate in settings.
>
> All of these settings are adjustable. On an RTX 5090 you can push them higher.


## Design Principles

These are non-negotiable for sumu (full design notes in [DESIGN.md](DESIGN.md)):

- **Player quality comes first** — smooth 4K and responsive seeking are the foundation. Never pause or slow down to wait for AI.
- **Frames stay on the GPU end to end** — from decode through AI to display, pixels never leave GPU memory. That's what keeps the path efficient.
- **The AI pipeline stays warm** — seek only repositions playback; it does not tear down and rebuild the AI path.
- **Every frame has a unique ID** — progress, seek, and AI all key off it, so a frame can never land in the wrong place.
- **Degrade rather than stall** — when the GPU can't keep up, fall back to the original. Never freeze the video.


## Tech Stack

- **Present**: native **D3D11 flip-model swapchain** (DWM-native, tear-free). The present loop runs on a native thread and never takes the GIL.
- **Host**: a minimal **Win32 window**. Overlay UI via **ImGui** (timeline / scrub thumbnails / window chrome / quality settings).
- **Language**: **C++ (VS2022 BuildTools) + pybind11** — native core exposed to Python for orchestration.
- **Decode**: baseline is **D3D11 hardware decode** (FFmpeg-d3d11va) → NV12 texture → shader → present, no CUDA on the baseline path; the AI path is NVDEC → torch, joined by **zero-copy D3D11↔CUDA interop**.
- **Audio**: WASAPI, a **pure subordinate clock** driven by the QPC master — never disturbs present pacing.
- **Split of labor**: native core (decode + present + interop + ready-map + audio) + Python-side AI orchestration (detect / restore / schedule).


## Build & Run

**Reference machine (the sole benchmark for all measurements)**: RTX 4080 · 16GB · Win11 · 4K@150Hz · driver 610.47 · Python 3.13.6 · torch 2.8.0+cu128 · VS2022 BuildTools · CUDA Toolkit 12.8.

### Run from source (dev)

1. **Native core**: `native/build.bat` (needs VS2022 BuildTools) → produces the pyd + FFmpeg DLLs.
2. **Python deps**: set up `.venv`, with torch on cu128.
   > The developer uses Chinese mirrors (NJU for cu128, Tsinghua for PyPI) out of habit. Non-Chinese developers should switch back to the official PyPI / PyTorch indexes.
3. **Third-party patches**: `bash scripts/apply_patches.sh` (runtime patches for ultralytics / mmengine).
4. **Model weights**: place the restore model (≈75MB) and detect model (≈6MB) under `model_weights/`.
5. **Run**: VSCode task `sumu: run (dev)`, or `.venv\Scripts\python.exe scripts/play.py`.

### Package for distribution (Windows onedir)

```powershell
# One shot: native build -> third-party patches -> PyInstaller freeze -> assemble weights -> smoke test
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1
```

Output lands in `dist/sumu/` (`sumu.exe` + `_internal/` + `model_weights/`, measured ≈6.9GB without TRT engines). `-SkipNative` / `-FastFreeze` incremental options and known pitfalls are in [docs/packaging.md](docs/packaging.md).

### TensorRT engines are not shipped

TRT engines are bound to GPU architecture + TensorRT version + precision + OS, so **they cannot be redistributed across machines**. The package therefore ships no prebuilt engines; each machine compiles its own on first use:

- Until compilation finishes, mosaic removal falls back to eager PyTorch (works, ~3× slower);
- On the first screen, under "Open file", a "Compile acceleration engine" prompt appears — click it to compile in the background (several minutes). When done, the engine hot-swaps in and is cached to disk for the next run;
- Non-NVIDIA / non-fp16 machines never trigger compilation and always stay on eager.

## License

sumu's mosaic-removal models and parts of the inference code come from [lada](https://codeberg.org/ladaapp/lada) (AGPL-3.0), so sumu as a whole is licensed under **AGPL-3.0**. Full terms in [LICENSE.md](LICENSE.md). New source files carry SPDX headers (`SPDX-FileCopyrightText: sumu Authors` / `SPDX-License-Identifier: AGPL-3.0`).

## Acknowledgements

sumu's player kernel — present / decode / CUDA interop / scheduler / audio / UI — is a fresh implementation. Its **mosaic-removal capability** builds on the work and ideas of the projects below; with thanks:

- **[lada](https://codeberg.org/ladaapp/lada)** — source of the mosaic-removal models, method, and inference core (sumu is AGPL-3.0 on this basis).
- **[jasna](https://github.com/Kruk2/jasna)** — origin of the idea to split the restore model into TensorRT sub-engines.
- **[BasicVSR++](https://ckkelvinchan.github.io/projects/BasicVSR++) / [MMagic](https://github.com/open-mmlab/mmagic)** — backbone of the mosaic restore model.
- **[YOLO / Ultralytics](https://github.com/ultralytics/ultralytics)** — mosaic detection model.
- **[DeepMosaics](https://github.com/HypoX64/DeepMosaics)** — mosaic dataset construction and early inspiration.
