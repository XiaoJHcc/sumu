<p align="center">
  <img src="assets/generated/sumu-logo-256.png" alt="Sumu" width="128" height="128">
</p>

<h1 align="center">Sumu</h1>

<p align="center">
  <strong>中文 | <a href="README.en.md">English</a></strong>
</p>

<p align="center">
  <em>一个真正时钟驱动、全程 GPU 处理的<b>实时</b>去马赛克播放器</em>
  <br>
  <sub><sup>A <b>real-time</b> mosaic-removal video player that is truly clock-driven and fully GPU-resident</sup></sub>
</p>

<p align="center">
  从内核起为「边播边去码」而设计——视频硬解、原生 D3D11 呈现、AI 全链路留 GPU、管线始终在线
  <br>
  <sub><sup>Built for play-while-restore from the core up — hardware decode, native D3D11 present, AI kept on the GPU end to end, pipeline always warm</sup></sub>
</p>

---

## 理念 · The Idea

去除马赛克的 AI 处理通常很重，难以做到实时。常见的做法要么离线处理导出视频，要么算不过来就卡在那儿等。
<br><sub><sup>Mosaic-removal AI is heavy — real-time restoration is genuinely hard. Most tools settle for one of two workarounds: processing everything offline into an exported file, or simply sitting and waiting whenever the GPU can't keep up.</sup></sub>

Sumu 换了个顺序想这件事：
<br><sub><sup>Sumu flips the script:</sup></sub>

**先有一个正经的播放器，再给它外挂一条 AI 链路。**
<br><sub><sup>**Ship a real player first. Bolt AI onto it second.**</sup></sub>

**播放永远第一位，AI 在后台尽力而为，视频绝不为 AI 停下来。**
<br><sub><sup>**Playback always comes first. AI does what it can in the background. The video never stops for AI.**</sup></sub>

首先得是个播放器——不管有没有 AI，视频都必须流畅播放。其次 AI 作为一条后台链路挂上去，尽量提前把内容处理出来，换上去码后的画面，来不及就回退原片，**从不打断播放**。
<br><sub><sup>Above all, it has to be a player — with or without AI, the video must simply play smoothly. AI is attached second, as a background path that works ahead to restore the content: the decensored frame swaps in the moment it's ready, and the video falls back to the original when it isn't. **Playback is never interrupted.**</sup></sub>

> 所以你依然需要一个强大的显卡，要不然还是会频繁回退到原片。
> <br><sub><sup>You still need a powerful GPU, though — otherwise you'll keep falling back to the original.</sup></sub>


## 功能特性 · Features

- **实时播放**——打开本地视频即开即播：AI 去码在后台持续工作，画面处理完成则显示去码图像。
  <br><sub><sup>**Real-time playback** — open a local video and it plays instantly. AI mosaic removal keeps working in the background: the moment a frame is processed, the decensored image is what you see.</sup></sub>

- **网络播放**——直接输入 HTTP 视频链接在线播放，无需先下载文件。
  <br><sub><sup>**Network playback** — paste an HTTP video link and play it online directly, with no need to download the file first.</sup></sub>

- **Web 流媒体服务器**——把去码后的视频串流到同一局域网内的手机、平板，浏览器打开即看。
  <br><sub><sup>**Web streaming server** — stream decensored videos to phones and tablets on the same LAN and watch them right in a browser.</sup></sub>

- **离线导出**——把去码结果导出为视频文件，支持自定义质量预设、视频批量处理队列。
  <br><sub><sup>**Offline export** — export the decensored result as a video file, with custom quality presets and a batch queue for processing many videos.</sup></sub>

---

<br>
<p align="center">
  <strong>中文 | <a href="README.en.md">English</a></strong>
</p>
<br>

---

## 硬件需求

- **Windows 系统**
- **nVidia 显卡**（其他显卡仍可运行，但无法使用 TRT 加速，效率较差）

| 流畅配置 | RTX 4080 | RTX 5070 Ti |
| --- | --- | --- |
| 最低配置 | RTX 4070 | RTX 3080 |

> 即便你有一个强大的显卡，流畅播放也建立在以下前提上：
> - 每批次处理长度默认为 30 帧，以实现快速响应，但画面会每秒规律性的抖动。
> - 同屏最多处理一个区块，多马赛克区块同屏会随机闪烁。调高区块数意味着消耗倍增。
> - 最高处理 30 FPS 的视频，当播放 60 FPS 视频时，AI 会频繁掉队。或者设置选择降帧播放。
>
> 这些设置项都是可调的，如果你拥有 RTX 5090，可以挑战更高的设置。


## 设计原则
围绕设计主线，sumu 认定以下原则（完整设计要点见 [DESIGN.md](DESIGN.md)）：

- **播放器本身先得够好**——4K 流畅、拖动跟手，是地基。绝不为了等 AI 而暂停或拖慢。
- **画面全程 GPU 处理**——从解码、AI 处理到最终显示，画面始终留在 GPU 显存中。这是为了最高效率。
- **AI 管线永远在线**——跳转只是把播放位置挪到新地方，不会推倒重建 AI 管线。
- **每一帧都有唯一编号**——进度、跳转、AI 处理全认这个编号，永远不会把画面对错位置。
- **扛不住就降级，绝不停**——显卡忙不过来时宁可退回原片，也绝不让视频停下来。


## 技术选型

- **呈现**：原生 **D3D11 flip-model swapchain**（DWM 原生、免撕裂）。present loop 跑原生线程，不吃 GIL。
- **宿主**：极简 **Win32 窗口**。UI 叠加层走 **ImGui**（进度条 / scrub 缩略图 / 窗口 chrome / 降级旋钮 / 导出页）。
- **语言**：**C++（VS2022 BuildTools）+ pybind11**，原生内核暴露给 Python 编排。
- **解码**：基线走 **D3D11 硬解**（FFmpeg-d3d11va）→ NV12 纹理 → shader → present，基线不碰 CUDA；AI 路径 NVDEC → torch，靠 **D3D11↔CUDA 零拷贝互操作**接起来。
- **音频**：WASAPI，以 QPC 主时钟为准的**纯附加从属时钟**，不扰动 present 节奏。
- **转码 / 流媒体**：无窗口 headless D3D11 硬解 → AI 去码 → **NVENC**（HLS / MP4）。带 NVENC 的 `ffmpeg.exe` 需在 PATH 上，作为 Web 串流 / 离线导出 / 缩略图的编码软依赖（`ffprobe.exe` 早已是既有软依赖）。
- **分工**：原生内核（decode + present + interop + ready-map + 音频）＋ Python 编排 AI（检测 / 修复 / 调度）与转码（`python/sumu/webstream`：headless 解码 + DecensorProcessor + 编码器 + 服务器）。


## 构建与运行

**目标机器（所有实测唯一基准）**：RTX 4080 · 16GB · Win11 · 4K@150Hz · 驱动 610.47 · Python 3.13.6 · torch 2.8.0+cu128 · VS2022 BuildTools · CUDA Toolkit 12.8。

### 构建与运行

1. **native 内核**：`native/build.bat`（需 VS2022 BuildTools），产出 pyd + FFmpeg DLL。
2. **Python 依赖**：准备 `.venv`，torch 走 cu128（cu128 镜像用南大，PyPI 用清华）。
3. **第三方补丁**：`bash scripts/apply_patches.sh`（ultralytics / mmengine 运行时补丁）。
4. **模型权重**：从 [lada HuggingFace](https://huggingface.co/ladaapp/lada) 下载 `lada_mosaic_restoration_model_generic_v1.2.pth` 与 `lada_mosaic_detection_model_v4_fast.pt` 放入 `model_weights/`。
5. **运行**：VSCode task `sumu: run (dev)`，或 `.venv\Scripts\python.exe scripts/play.py`。首次运行无 TRT 引擎时走 eager 回退，首屏提示编译加速引擎。
6. **打包分发**：`powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1`，产物 `dist/sumu/`（≈6.9GB，不含 TRT 引擎）。`-SkipNative` / `-FastFreeze` 等选项见 [docs/packaging.md](docs/packaging.md)。

> 纯本地实时播放只需上面 1–5；**Web 串流 / 离线导出**还需把带 NVENC 的 `ffmpeg.exe` 放进 PATH（`ffprobe.exe` 已是既有软依赖）。

> **TensorRT 引擎不进分发包**：引擎绑定 GPU 架构 + TRT 版本 + 精度 + OS，不能跨机分发。分发包不含预编译引擎，每台机器首次运行自行编译——编译前走 eager 回退（约 3× 慢），首屏提示「编译加速引擎」，编完热切换并落盘缓存，非 Nvidia / 非 fp16 机器恒走 eager。

## License

sumu 使用的去码模型与部分推理代码源自 [lada](https://codeberg.org/ladaapp/lada)（AGPL-3.0），故 sumu 整体基于 **AGPL-3.0** 授权。完整条款见 [LICENSE.md](LICENSE.md)。新增源文件带 SPDX 头（`SPDX-FileCopyrightText: sumu Authors` / `SPDX-License-Identifier: AGPL-3.0`）。

## Acknowledgement

sumu 的播放器内核——present / decode / CUDA 互操作 / 调度 / 音频 / UI——是全新实现。它的**去码能力**则建立在以下项目的成果与思路之上，谨致谢意：

- **[lada](https://codeberg.org/ladaapp/lada)** —— 去马赛克模型、方法与推理核心的来源（sumu 据此以 AGPL-3.0 授权）。
- **[jasna](https://github.com/Kruk2/jasna)** —— TensorRT 拆子引擎加速修复模型的思路来源。
- **[BasicVSR++](https://ckkelvinchan.github.io/projects/BasicVSR++) / [MMagic](https://github.com/open-mmlab/mmagic)** —— 马赛克修复模型骨架。
- **[YOLO / Ultralytics](https://github.com/ultralytics/ultralytics)** —— 马赛克检测模型。
- **[DeepMosaics](https://github.com/HypoX64/DeepMosaics)** —— 马赛克数据集构建与早期启发。