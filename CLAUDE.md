# CLAUDE.md

本文件为 AI 助手提供 sumu 仓库的工作指引。代码标识符、路径、技术术语保留英文；说明性文字用中文。许可证 **AGPL-3.0**（继承自 lada），新源文件加 SPDX 头：
`// SPDX-FileCopyrightText: sumu Authors` / `// SPDX-License-Identifier: AGPL-3.0`。

## 项目是什么

sumu（澄む）是一个**时钟驱动、全程 GPU** 的实时去马赛克预览播放器。它为修复 [lada-realtime](../lada-realtime) 的**架构级 BUG** 而立项——不再打补丁，而是**换掉传输 + 呈现 + seek 这一层**，**照搬其 AI 计算核心**。

**设计北极星见 [DESIGN.md](DESIGN.md)**（不变量 I1–I10、照搬/重写清单、spike 验证方案）——一切选型服从它。

### lada-realtime 实测封死的三处结构病根（sumu 要从结构上消除）

| 病根 | 症状 | 出处 |
|---|---|---|
| GStreamer 软件 sink（Win+Nvidia 上 GL 双重封死：`wglShareLists` ERROR_BUSY + #62 颜色） | 最大化软件渲染墙、4K 上屏抖动 | lada `realtime-judder-presentation-rootcause` |
| AI 管线自持解码（PyAV 每帧 NVDEC→主存 6ms + 只能 CUDA 核色转，抢 AI 的 GPU） | 4K60「满帧+每帧去码」不可达 | lada `nvdec-hardware-decode` 阶段三 |
| seek = teardown/rebuild（pynvc 无稳定快速 seek；rebuild 持锁跑冷启动 1.8s） | seek 卡 8~38s / 冻屏 / 切换闪退 | 同上 |

## 已锁定选型（2026-07-05）

- **present 面**：原生 **D3D11 flip-model swapchain**（DWM 原生、免撕裂；GL 在本机被双重封死）。present loop 跑原生线程，不吃 GIL。
- **宿主**：极简 **Win32 窗口**（不用 Qt）。
- **语言**：**C++（VS2022 BuildTools）+ pybind11**，暴露给 Python。
- **解码**：player 基线走 **D3D11 硬解**（MF 或 FFmpeg-d3d11va）→ NV12 纹理 → shader → present，**基线不碰 CUDA**；AI 路径复用 lada NVDEC→torch，靠 **D3D11↔CUDA 互操作**接起来。
- **分工**：原生内核（decode + present + interop + ready-map API）＋ Python 编排 AI（torch，照搬 lada 核心）。

**心智模型**：播放器是主，AI 是仆。present loop 每 vblank 从 ready-map（帧号→GPU 纹理）挑当前播放时间的最佳帧，AI 就绪上 AI 帧、否则回退原片环缓冲。present 永不阻塞在 AI 上。

## 仓库结构

```
DESIGN.md            ★ 设计北极星（不变量 + spike 方案）——先读
native/              ★ C++ 原生内核（present + decode + interop + ready-map + 音频从属时钟）
  CMakeLists.txt
  src/               present loop、D3D11 swapchain、decode、CUDA interop、audio_loop（WASAPI）、
                     ImGui overlay（进度条/scrub 缩略图/窗口 chrome）、pybind11 绑定
  shaders/           NV12→RGB 等 HLSL
python/sumu/         Python 侧编排（Phase 4 起）
  app.py             应用入口 + UI 事件循环 + 降级旋钮/最近文件/位置持久化的 UI 消费
  pipeline.py        解码→AI→present 的编排管线
  scheduler.py       frontier gate、clip 化调度、frame_cache、seek 重置（docs/scheduler.md）
  settings.py        配置与播放位置持久化
  ai/                照搬自 lada-realtime 的 AI 核心（YOLO 检测、BasicVSR++、TRT 子引擎、blend-back）
    models/yolo/、models/basicvsrpp/（vendored mmagic）、restorationpipeline/、trt/
spikes/              ★ 逐个 spike 的独立验证代码（spike0~3）
scripts/             analyze_present.py（present 上屏间隔分析，PresentMon/ns 双格式）等
docs/                spike 结果记录、移植记录、鲁棒性/打包等状态文档
test_video.mp4       1080p30（全程马赛克，压去码模型）—— .gitignore
test_video_4k.mp4    3840×2160 HEVC 60fps（流畅解码硬指标）—— .gitignore
test_video_long.mp4  2.1GB（长视频跳转测试）—— .gitignore
```

## 目标机器（所有实测唯一基准）

RTX 4080 · 16GB · Win11 · 4K@150Hz（scale=1）· 驱动 610.47 · Python 3.13.6 · torch 2.8.0+cu128（cu128 镜像用南大，PyPI 用清华，见 lada `china-mirror-setup`）· MSVC = VS2022 BuildTools · CUDA Toolkit 12.8（Spike 1 起构建期依赖）。

## 三目标（按用户重申的优先级）——均已达成

1. **4K60 HEVC 硬解达正经播放器水平**（Spike 0，硬门）——已通过，见 `docs/spike_results.md`。
2. **架构预留 AI 画面插入能力**（Spike 1，零拷贝 CUDA↔D3D11）——已通过。
3. **AI 模型能在此架构运行对接**（Phase 4，照搬 lada 核心）——已完整移植并端到端验证，见 `docs/ai_core_port.md`、`docs/scene_clip_blend_port.md`、`docs/scheduler.md`。

三目标达成后项目继续推进到 Phase 5（ImGui UI：进度条/scrub 缩略图/窗口 chrome/降级旋钮）与 Phase 6（音频从属时钟、设置持久化、打包），当前处于功能收尾/打磨阶段而非 spike 阶段。已知缺口（YOLO 跳帧未实现、诊断卡片未接 UI、4K 下 AI 命中率接近零等）不在本节重复，如需现状核对以 `docs/robustness_4e.md`、`docs/packaging.md`、`python/sumu/scheduler.py` 为准，不要假设本文件同步更新。

## 约定

- **先埋点，再优化；实测推翻直觉**（I10）。任何 present/interop 优化先量后改，spike 结果落 `docs/`。
- 帧号是唯一事实来源，锚定真实解码 PTS（I5）。
- seek = reposition，不 teardown（I6）。
- 全程 GPU，帧不下主存（I3）——禁止 lada 那种每帧 GPU→host→GPU 兜圈。
- lada-realtime 仓库保留不动，作 AI 核心来源与行为对照；其 CLAUDE.md + CC memory 记录大量实测封死的坑，开工前查。
