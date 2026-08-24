# AGENTS.md

本文件为 AI 助手提供 sumu 仓库的工作指引。代码标识符、路径、技术术语保留英文；说明性文字用中文。许可证 **AGPL-3.0**（继承自 lada），新源文件加 SPDX 头：
`// SPDX-FileCopyrightText: sumu Authors` / `// SPDX-License-Identifier: AGPL-3.0`。

## 项目是什么

sumu（澄む）是一个**时钟驱动、全程 GPU** 的实时去马赛克预览播放器。它由 lada 的实时化 fork `lada-realtime` 立项而来——**换掉「传输 + 呈现 + seek」这一层**，**照搬其 AI 计算核心**，从结构上根治旧架构的卡顿与 seek 冻屏。

**心智模型：播放器是主，AI 是仆。** present loop 每个 vblank 从 ready-map（帧号 → GPU 纹理）挑当前播放时间的最佳帧——AI 帧就绪就上去码，否则回退原片环缓冲。**present 永不阻塞在 AI 上，永不因 AI 慢而卡。**

项目现在**完全自包含**：AI 计算核心已落地在仓库内（`python/sumu/ai/`），不再依赖任何兄弟仓库路径（历史上一度依赖 `../lada-realtime`，已脱钩）。lada 仅作为**模型与推理核心的出处 / AGPL 授权来源**存在于文档中。

设计北极星见 **[DESIGN.md](DESIGN.md)**（不变量 I1–I10、照搬/重写清单）——一切选型服从它。面向用户的定位、功能列表与硬件需求见 **[README.md](README.md)** / **[README.en.md](README.en.md)**。

## 已锁定的技术选型

- **呈现**：原生 **D3D11 flip-model swapchain**（DWM 原生、免撕裂）。present loop 跑原生线程，不吃 GIL。
- **宿主**：极简 **Win32 窗口**，UI 覆盖层走 **Dear ImGui**（进度条 / scrub 缩略图 / 窗口 chrome / 设置旋钮 / 导出页 / 流媒体模态框）。
- **语言**：**C++（VS2022 BuildTools）+ pybind11**，原生内核编译为模块 `sumu_core`（类 `Player`）暴露给 Python。
- **解码**：基线走 **D3D11 硬解**（FFmpeg d3d11va）→ NV12 纹理 → shader → present，**基线不碰 CUDA**；AI 路径 NVDEC → torch，靠 **D3D11↔CUDA 零拷贝互操作**接起来。
- **音频**：WASAPI，以 QPC 主时钟为准的**纯附加从属时钟**，不扰动 present 节奏。
- **转码 / 流媒体**：无窗口 headless D3D11 硬解 → AI 去码 → **NVENC**（HLS / MP4），实现 Web 局域网串流与离线导出。带 NVENC 的 `ffmpeg.exe` 是这部分 + 缩略图的软依赖（`ffprobe.exe` 已是既有软依赖，用于读取视频元数据）。
- **分工**：原生内核（decode + present + interop + ready-map + 音频 + UI 渲染）＋ Python 编排 AI（检测 / 修复 / 调度）与转码（`python/sumu/webstream`）。

**目标机器（所有实测的唯一基准）**：RTX 4080 · 16GB · Win11 · 4K@150Hz · 驱动 610.47 · Python 3.13 · torch 2.8.0+cu128（运行期 CUDA 12.8）· MSVC = VS2022 BuildTools · CUDA Toolkit **v13.3**（仅构建期取 `cuda.h` / `cuda.lib` 驱动 API，无 nvcc）。

## 仓库结构

```
DESIGN.md               设计北极星（不变量 + spike 方案）——先读
README.md / README.en.md  面向用户：功能、硬件需求、构建运行
native/                  C++ 原生内核，构建产物 sumu_core.cp313-win_amd64.pyd（pybind11）
  build.bat              唯一构建入口（source vcvars64 + CMake + Ninja）
  CMakeLists.txt         目标/依赖/产物落位
  src/                   player_engine / player_session / player_window / player_pybind /
                         player_ui_overlays / player_ui_bars / player_ui_export / decoder /
                         headless_decode（Web 转码用无窗口解码）
  src/ui/                ImGui 设计系统（theme / widgets / icons）
  shaders/present.hlsl   NV12→RGB 着色器
  cmake/                 HLSL/logo/图标图集的嵌入脚本
  third_party/imgui/     vendored Dear ImGui（唯一未 gitignore 的 third_party）
  smoke_player.py        native 冒烟驱动（smoothness/seek/pause/all）
python/sumu/             Python 侧编排
  app.py                 日常入口 main()：窗口 + 事件循环 + 后台模型预热 + 单实例/关窗驻留 + 导出/流媒体 UI 消费
  pipeline.py            build_models（dev 与 frozen 共用）
  scheduler.py           AI 调度：frontier gate / clip 化 / frame_cache / seek 重置
  settings.py            持久化（音量/最近文件/进度续播/旋钮/语言）——纯 stdlib
  i18n.py + locales/     UI 多语言（zh-CN / en / ja）
  single_instance.py     Windows 命名管道单实例转发
  ai/                    照搬自 lada 的 AI 核心（YOLO 检测、BasicVSR++、TRT 子引擎、blend-back）
  webstream/             Web 流媒体服务器 + 离线导出（headless→去码→NVENC）
spikes/                  spike0~3 历史验证代码（已收口，结果见 docs/）
scripts/                 构建/运行/验证/截图工具（详见下文）
packaging/               PyInstaller 打包（sumu.spec + 运行时 hook）
patches/                 第三方运行时补丁（ultralytics / mmengine）
assets/                  Logo 单源（sumu-logo-1024.png）+ lucide 图标 + generated/（生成产物）
model_weights/           模型权重（gitignored，需自行下载，见「构建与运行」）
docs/                    状态文档（索引见下文「docs 查询索引」）
test_video*.mp4          本地测试素材 1080p30 / 4K60 HEVC / 2.1GB 长片（gitignored）
```

**代码落点速查**：改播放内核/解码/窗口/UI → `native/src/`（C++）；改 AI 调度策略 → `python/sumu/scheduler.py`；改模型与推理 → `python/sumu/ai/`；改 Web 串流/导出 → `python/sumu/webstream/` + `native/src/headless_decode.cpp`；改用户持久化 → `python/sumu/settings.py`；改界面文案 → `python/sumu/locales/*.json` + `i18n.py`；改 UI 视觉规范 → `docs/ui_design.md` + `native/src/ui/`。

## 构建、运行与验证工具

### 构建与运行

| 工具 | 用途 |
|---|---|
| `cmd /c "native\build.bat"` | **唯一 native 构建入口**：source vcvars64 + CMake + Ninja，产物 `sumu_core.*.pyd` + FFmpeg DLL 落 `python/sumu/`。先杀残留 python 进程（关窗驻留会锁 pyd）、hardcode 了 `.venv` 路径；**勿手拼 ninja/cmake**（sandbox 无 vcvars，会锁死 build 目录） |
| `uv sync` | 安装 Python 依赖（镜像已配在 `pyproject.toml`） |
| `bash scripts/apply_patches.sh` | 给 .venv 打 ultralytics/mmengine 运行时补丁（重装这两包后须重跑） |
| lada HuggingFace 下载两权重 → `model_weights/` | 权重目录 gitignored，不随包分发 |
| `.venv\Scripts\python.exe scripts\play.py [video]` | dev 日常运行入口（VSCode task `sumu: run (dev)`）；不带 video 弹「打开文件 / 打开 URL」 |
| `scripts/sumu_main.py` | PyInstaller 冻结入口（窗口化，stdout/stderr 重定向 `sumu.log`） |
| `powershell -ExecutionPolicy Bypass -File scripts\build_dist.ps1` | 打包 onedir → `dist/sumu/`（≈6.9GB）；`-SkipNative` / `-FastFreeze` / `-SkipSmoke` 见 `docs/packaging.md` |

> **沙箱受阻不重试**：在受限沙箱里 `build.bat` 可能因 WMI 进程枚举被拒（`Get-CimInstance 拒绝访问`，杀不掉驻留 python → 锁住 `sumu_core.*.pyd`）或 ninja 子进程/管道被拦而卡死。一旦出现访问拒绝 / `[sandbox: ...]` / ninja 长时间 0 CPU 且 `sumu_core.*.pyd` 未刷新，**不要换命令重试、不要手拼 ninja/cmake**——`job_kill` 清理残留进程后如实报告，交由用户在本机跑 `cmd /c "native\build.bat"` 验证编译。

> TRT 引擎不随包分发（绑定 GPU 架构+TRT 版本+精度+OS，首启自编译、编前 eager 回退约 3× 慢）。Web 串流 / 离线导出还需带 NVENC 的 `ffmpeg.exe` 在 PATH 上。

### 验证与压测

| 脚本 | 用途 |
|---|---|
| `native\smoke_player.py {smoothness\|seek\|pause\|all}` | native Player 冒烟：4K60 流畅度 / 长片 seek 风暴 / pause 帧号冻结 |
| `scripts\run_player.py [video] --seconds N [--seek-test] [--correctness]` | 端到端冒烟：接 Player+Scheduler，打印 ai_hit_rate / present；`--correctness` 用 PyAV CPU 解码交叉校验 RGBA 通道序与「马赛克确实被改」 |
| `verify_i18n.py` / `verify_settings.py` / `verify_weights_resolve.py` | 无 GPU 快速检查：文案键对齐 / 设置往返与原子写 / 权重目录解析 |
| `verify_ai_core.py` / `verify_scene_clip_blend.py` | AI 核心：load_models/YOLO/BasicVSR 实测 fps；scene/clip + blend 端到端 |
| `verify_transcode*.py` / `verify_stream*.py` | 转码与 Web 流媒体/导出（路由/token/passthrough 直出/e2e） |
| `stress_reopen.py` / `stress_seek_ai.py` | 压测：反复 reopen 不同尺寸视频、seek 风暴与 AI 并发 |
| `scripts\analyze_present.py` | 解析 present trace（PresentMon/ns），分冷启动/稳态窗口比 median/stddev/p99（I10 先量后改） |
| `scripts\measure_trt_build.py` | 一次性：测 6 个 TRT 子引擎编译耗时 |

### 截图 UI 验证（`scripts/shot_*.py` + `calib_button_center.py`）

打开裸 `sumu_core.Player` 窗口（或附着已运行窗口），注入合成 UI 快照后用 PIL ImageGrab 截 PNG 到 `native/trace/`，供肉眼检查布局——人工目检，非自动化断言：

| 脚本 | 用途 |
|---|---|
| `shot_running_app.py out.png [cx cy]` | 按标题 "sumu" 找已运行窗口，置顶/截图，可选按比例坐标发真实鼠标点击 |
| `shot_bottom_bar.py` / `shot_buttons.py` | 空播放器底栏布局 / 导出页按钮、关闭键 hover 红、Web 服务器模态框 |
| `shot_export_ui.py` / `shot_export_drag.py` | 导出页整体布局（合成快照）/ 队列真实鼠标拖拽排序 |
| `shot_modals.py` / `calib_button_center.py` | 模态框 padding 像素级取证 / 按钮文字居中标定 |

改 UI 后跑对应脚本目检 PNG；规范来源见 `docs/ui_design.md`「布局验证」。

### docs 查询索引

| 文档 | 内容 |
|---|---|
| `DESIGN.md`（根） | 设计北极星：不变量 I1–I10、照搬/重写清单、spike 方案 |
| `docs/native_core.md` | 原生内核 `native/` → `sumu_core`：Player 接口、线程/锁模型、seek=reposition、构建与验证 |
| `docs/native_ai_input.md` | 原生 AI 输入桥 `Player::get_cuda_nv12_by_frame`（D3D11 NV12 → CUDA 零拷贝） |
| `docs/scheduler.md` | AI 调度器 `python/sumu/scheduler.py`：frontier gate / clip / frame_cache / seek 重置 + 实测数字 |
| `docs/ai_core_port.md` | AI 计算核心移植记录：`python/sumu/ai/` 结构、依赖装配、验证 |
| `docs/porting_manifest.md` | AI 核心移植清单（lada→sumu）：逐项照搬 vs 重写 |
| `docs/scene_clip_blend_port.md` | Scene/Clip 聚合 + blend-back 移植记录 |
| `docs/spike_results.md` | Spike 0~2 实测结果（mpv 基线、present、零拷贝互操作、时钟混流） |
| `docs/spike3_nv12_interop.md` | Spike 3：D3D11 解码 NV12 → CUDA → torch 反向互操作 |
| `docs/robustness_4e.md` | 鲁棒性压测与边界：4K 降级、seek 风暴、EOF、降级旋钮 |
| `docs/scrub_thumbnail.md` | 进度条 hover 缩略图（scrub thumbnail）架构与实测 |
| `docs/ui_design.md` | UI 设计规范：色板 / 尺寸刻度 / 控件 API / 页面布局 / Dos&Don'ts |
| `docs/webstream.md` | Web 流媒体服务器 / 离线导出：passthrough 与 AI 去码两种模式、路由、边界 |
| `docs/packaging.md` | PyInstaller onedir 打包管线、spec 要点、验证边界、已知坑 |

### 其它要点

- **模型权重解析顺序**（`python/sumu/ai/__init__.py`）：`SUMU_MODEL_WEIGHTS_DIR` → `LADA_MODEL_WEIGHTS_DIR`（legacy）→（frozen）exe 同级 `model_weights/` → CWD 相对 `model_weights/`。
- **单实例与关窗驻留**：关窗（X/ESC）后进程驻留 60s 保持模型热，期间重开通过命名管道转发给原进程、零预热重开；超时或 Ctrl-C 才真正退出。
- **设置持久化**：`settings.json` 落在仓库外（用户态），缺失/损坏/清空都必须回退默认值，绝不 crash；写入走同目录临时文件 + `os.replace()` 原子替换。
- **i18n**：文案唯一来源是 `python/sumu/locales/{zh-CN,en,ja}.json`，Python 拥有翻译，native ImGui 标签经 `Player.set_ui_strings()` 一次性下发。
- **国内镜像**：torch cu128 走南京大学、PyPI 走清华（`pyproject.toml` 已配好）；别用阿里云 flat 布局做 torch 源（uv 解析不出干净版本）。
- **vendor 边界**：`third_party/` 与 `tools/` 整体 gitignore（拉取型依赖/测量数据）；唯一例外是 `native/third_party/imgui/`（源码入树的 vendored 依赖，见其 `README.sumu.md`）。

## 约定与硬约束

- **帧号是唯一事实来源**，锚定真实解码 PTS（I5）；ready-map、frontier、进度、seek 全部以帧号为键。
- **seek = reposition，不 teardown**（I6）：只重定位时钟 + decode-ahead + AI 前沿，绝不销毁重建线程/解码器/模型。
- **全程 GPU，帧不下主存**（I3）：唯一文档化放宽是 Web 转码喂编码器前的**一次** D2H（`final_bgr.cpu()`），与 present 热路径无关（`docs/webstream.md`）。
- **先埋点，再优化；实测推翻直觉**（I10）：任何 present / interop 优化先量后改，spike / 压测结果落 `docs/`。
- **VRAM 是一等约束**（I8）：环缓冲存 NV12（减半），按显存上限动态压 lookahead。
- `clip_size` 锁死 256，与 TRT 引擎编译 shape 绑定，不是运行时旋钮。

**已定案、勿重开的方向**（历史决策，保持现状）：YOLO 跳帧 / 稀疏检测（瓶颈不在 YOLO）；完整诊断仪表盘（只保留 AI 修复速度）；把 4K「AI 命中率≈0」当架构缺陷（present 韧性已验证，4K 去码是 best-effort，靠旋钮调）；循环回绕播放（播完停在最后一帧并暂停，代码里残留的 loop 符号是防御代码不是待修功能）。
