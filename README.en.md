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
  Designed from the ground up for play-while-restore — hardware decode, native D3D11 present, AI kept on the GPU end to end, pipeline always warm
</p>

---

## The Idea

Mosaic-removal AI is heavy — real-time restoration is genuinely hard. Most tools settle for one of two workarounds: processing everything offline into an exported file, or simply sitting and waiting whenever the GPU can't keep up.

Sumu flips the script:

**Ship a real player first. Bolt AI onto it second.**

**Playback always comes first. AI does what it can in the background. The video never stops for AI.**

Above all, it has to be a player — with or without AI, the video must simply play smoothly. AI is attached second, as a background path that works ahead to restore the content: the decensored frame swaps in the moment it's ready, and the video falls back to the original when it isn't. **Playback is never interrupted.**

> You still need a powerful GPU, though — otherwise you'll keep falling back to the original.


## Features

- **Real-time playback** — open a local video and it plays instantly. AI mosaic removal keeps working in the background: the moment a frame is processed, the decensored image is what you see.
- **Network playback** — paste an HTTP video link and play it online directly, with no need to download the file first.
- **Web streaming server** — stream decensored videos to phones and tablets on the same LAN and watch them right in a browser.
- **Offline export** — export the decensored result as a video file, with custom quality presets and a batch queue for processing many videos.


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
- **Transcode / streaming**: windowless headless D3D11 hardware decode → AI removal → **NVENC** (HLS / MP4). An NVENC-enabled `ffmpeg.exe` on PATH is a soft dependency for web streaming / offline export / thumbnails (`ffprobe.exe` already was).
- **Split of labor**: native core (decode + present + interop + ready-map + audio) + Python-side orchestration (detect / restore / schedule) and transcode (`python/sumu/webstream`: headless decode + DecensorProcessor + encoder + server).


## Build & Run

**Reference machine (the sole benchmark for all measurements)**: RTX 4080 · 16GB · Win11 · 4K@150Hz · driver 610.47 · Python 3.13.6 · torch 2.8.0+cu128 · VS2022 BuildTools · CUDA Toolkit 12.8.

### Build & Run

1. **Native core**: `native/build.bat` (needs VS2022 BuildTools) → produces the pyd + FFmpeg DLLs.
2. **Python deps**: set up `.venv`, with torch on cu128.
   > The developer uses Chinese mirrors (NJU for cu128, Tsinghua for PyPI) out of habit. Non-Chinese developers should switch back to the official PyPI / PyTorch indexes.
3. **Third-party patches**: `bash scripts/apply_patches.sh` (runtime patches for ultralytics / mmengine).
4. **Model weights**: download `lada_mosaic_restoration_model_generic_v1.2.pth` and `lada_mosaic_detection_model_v4_fast.pt` from [lada HuggingFace](https://huggingface.co/ladaapp/lada) and place them under `model_weights/`.
5. **Run**: VSCode task `sumu: run (dev)`, or `.venv\Scripts\python.exe scripts/play.py`. On the first run without TRT engines, falls back to eager; a compile prompt appears on the first screen.
6. **Package for distribution**: `powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1`. Outputs `dist/sumu/` (≈6.9GB, without TRT engines). `-SkipNative` / `-FastFreeze` options are documented in [docs/packaging.md](docs/packaging.md).

> Plain local real-time playback only needs steps 1–5; **Web streaming / offline export** additionally need an NVENC-enabled `ffmpeg.exe` on PATH (`ffprobe.exe` was already a soft dependency).

> **TensorRT engines are not shipped**: engines are bound to GPU architecture + TRT version + precision + OS and cannot be redistributed across machines. The package ships no prebuilt engines; each machine compiles its own on first use — falls back to eager (~3× slower) until then, a "Compile acceleration engine" prompt appears on the first screen, hot-swaps in on completion and is cached to disk. Non-NVIDIA / non-fp16 machines never trigger compilation and always stay on eager.

## License

sumu's mosaic-removal models and parts of the inference code come from [lada](https://codeberg.org/ladaapp/lada) (AGPL-3.0), so sumu as a whole is licensed under **AGPL-3.0**. Full terms in [LICENSE.md](LICENSE.md). New source files carry SPDX headers (`SPDX-FileCopyrightText: sumu Authors` / `SPDX-License-Identifier: AGPL-3.0`).

## Acknowledgements

sumu's player kernel — present / decode / CUDA interop / scheduler / audio / UI — is a fresh implementation. Its **mosaic-removal capability** builds on the work and ideas of the projects below; with thanks:

- **[lada](https://codeberg.org/ladaapp/lada)** — source of the mosaic-removal models, method, and inference core (sumu is AGPL-3.0 on this basis).
- **[jasna](https://github.com/Kruk2/jasna)** — origin of the idea to split the restore model into TensorRT sub-engines.
- **[BasicVSR++](https://ckkelvinchan.github.io/projects/BasicVSR++) / [MMagic](https://github.com/open-mmlab/mmagic)** — backbone of the mosaic restore model.
- **[YOLO / Ultralytics](https://github.com/ultralytics/ultralytics)** — mosaic detection model.
- **[DeepMosaics](https://github.com/HypoX64/DeepMosaics)** — mosaic dataset construction and early inspiration.
