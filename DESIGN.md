# sumu — 设计要点与验证方案

> **sumu**（澄む，日语「由浊转清、变透明」）—— 一个真正**时钟驱动、全程 GPU** 的实时去马赛克预览播放器。
>
> 前身是 [lada](https://codeberg.org/ladaapp/lada) 的实时化 fork（`lada-realtime`）。lada 的实时预览是「数据驱动」——模型跟不上就暂停缓冲；lada-realtime 在其架构上做了大量时钟驱动改造，但受制于「AI 管线自持解码 + seek 拆建 + GStreamer 软件 sink」三处结构选择，反复出现 **seek 卡死 / 上屏帧时间不稳 / 最大化软件渲染墙**。sumu 不再打补丁，而是**换掉传输+呈现+seek 这一层**（AI 计算核心照搬），从结构上让这些症状不再产生。

本文件分两部分：

1. **设计要点（不变量）** —— 一切技术选型都服从它，不随框架/语言变动。
2. **验证方案（spike）** —— 用实测数据决定尚未拍板的选型（播放器宿主、present 语言、音频范围）。

代码标识符、技术术语保留英文；说明性文字用中文。许可证沿用 AGPL-3.0（继承自 lada）。

---

## 第一部分 · 设计要点（北极星，选型永远服从这些）

### A. 核心心智模型

> **播放器是主，AI 是仆。** 播放器有一条永不停顿、按显示刷新率驱动的 present loop；AI 是挂在后台的异步生产者，把去码帧按帧号塞进一张 ready-map。present loop 每个 vblank 只做一件事：为当前播放时间挑「已就绪的最佳帧」——AI 帧就绪就上去码，否则回退原片。**present 永不阻塞在 AI 上，永不因 AI 慢而卡。**

这条心智模型是 lada-realtime 已经验证有效的方向，sumu 完整继承，只是把它坐在一个真正的 GPU present loop 上，而不是 Python appsrc → GStreamer 软件 sink 上。

### B. 硬性设计要点（Invariants）

| # | 要点 | 含义 / 为什么 |
|---|---|---|
| I1 | **时钟驱动，永不缓冲停顿** | 以显示墙钟为准前进；绝不为等 AI 而 pause/rebuffer。lada 上游的 buffer-first 是我们要彻底摆脱的行为。 |
| I2 | **present loop 与 AI 完全解耦** | present 只读 ready-map + 原片环缓冲，非阻塞挑帧；AI 生产快慢完全不影响上屏节奏。 |
| I3 | **全程 GPU，帧不下主存** | decode(NVDEC) → AI(torch/CUDA) → present，全链路留显存。**禁止**当前 lada 那种每帧 GPU→host→GPU 兜圈（D2H `cuda.synchronize` + 主存拷贝 + videoconvert），那是窗口化抖动和 GPU 争用的根源。 |
| I4 | **player-grade 是地基，也是第一道门** | 必须先做到 4K60 HEVC 硬解、上屏帧时间稳定、seek 顺滑——**在接任何 AI 之前**。player 不达标，一切免谈。 |
| I5 | **帧号是唯一事实来源，锚定真实 PTS** | ready-map、frontier、进度、seek 全部以「真实解码 PTS 换算的帧号」为键。lada 曾因把关键帧硬标成标称帧号而整段丢弃——不再重犯。 |
| I6 | **seek = reposition，不是 teardown** | seek 只把 present clock + decode-ahead 缓冲 + AI 前沿**重定位**到新帧号；**不销毁不重建**线程/解码器。这从根上消掉 lada 的 seek 多秒黑屏和解码器 churn 堆损坏。 |
| I7 | **单一解码头 + decode-ahead 环缓冲** | 一个 NVDEC 会话跑在 AI 前沿，原片帧进 GPU 环形缓冲供 present 消费，同批帧喂 AI。省 NVDEC 名额、免重复解码。**VRAM 要显式预算**（见 I8）。 |
| I8 | **VRAM 是一等约束** | 4K 帧缓一个 lookahead 窗口（~180 帧）≈ 2–4GB 显存。环缓冲存 **NV12**（减半），present 时才转；按 VRAM 上限动态压 lookahead。设计里必须带预算，不能事后补。 |
| I9 | **降级而非停顿** | GPU 跟不上时，手段是：回退原片、缩小 max_clip_length、fp16、目标帧率降采样、收窄缓冲窗口——**永远不停时钟**。（`clip_size` 已锁死 256，与 TRT 引擎编译 shape 绑定，不是可调旋钮。YOLO 跳帧已放弃，见 CLAUDE.md「已放弃方向」） |
| I10 | **先埋点，再优化；实测推翻直觉** | lada 史上多个「听起来合理」的结论被实测推翻（如「AI GIL 争用饿死呈现」实为帧时钟空转）。sumu 从第一天带 present/AI trace，任何优化先量后改。 |

### C. 要照搬的 README 核心机制（全部保留，只换承载层）

这些是 lada-realtime 已落地、要**逻辑照搬**到 sumu 的机制（详见 lada-realtime 的 CLAUDE.md）：

- **处理前沿闸门（frontier gate）**：AI 只处理「播放头前方 N 帧」，不全速冲片尾抢 GPU。
- **clip-based AI 调度 + 冷启动超前**：seek 后 passthrough 从落点起播，AI 从「落点 + cold_start_clips × clip_length」起，播放头到达时去码帧就绪 → 无缝切换。
- **AI 超前窗口 / 缓冲窗口（lookahead_frames，帧）**：简单段囤去码帧给难段消费。用帧数（与 clip 长度、内存挂钩、fps 不定）。
- **模型预热（warmup）**：加载后跑一次 dummy forward，把 CUDA/cuDNN 初始化在加载期付清。
- **TRT 加速 BasicVSR++**：拆 6 子引擎，独占 3–4x；非 cuda/非 fp16/引擎缺失无缝回退 PyTorch。缓存键编 arch+TRT 版本+精度+OS+clip 上界（自愈、不跨机分发）。
- **GPU 解码后端**：NVDEC/PyNvVideoCodec 零拷贝（sumu 里升级为「解码后帧全程不下 GPU 直到 present」）。
- **降级旋钮**：max_clip_length / fp16 / 目标帧率 / 缓冲窗口 / 同帧区块数。（`clip_size` 锁死 256，烧进 TRT 引擎编译 shape，非运行时旋钮。YOLO 跳帧已放弃。）
- **设置面板诊断**：仅保留 AI 修复速度；**完整诊断卡片已放弃**。可选后续只加缓冲进度条，不做命中率/丢弃帧等仪表盘。
- **落后重定位（reposition / frontier 闸门）**：lada 中因帧号问题默认关闭；sumu 帧号锚定后已落地（`scheduler` 的 `backlog_resyncs`）。

### D. AI 管线：照搬什么，重写什么

来自对 lada-realtime 代码的实测耦合分析——**AI 计算核心干净可复用，缠住的只有编排层**：

**✅ 照搬（device-generic torch 张量、无队列纯函数、低风险移植）**
- YOLO 检测调用（`model.preprocess` + `inference_and_postprocess`）
- Scene / Clip 聚合逻辑（`mosaic_detector.py` 的 Scene/Clip 类 + 两个 builder，~140 行纯逻辑）
- BasicVSR++ 修复封装（`BasicvsrppMosaicRestorer.restore`，零线程）
- blend-back（`_restore_frame`，纯张量操作）
- TRT 子引擎装配、模型加载/缓存（`load_models`、`FrameRestorerProvider`）
- GPU 解码后端（`VideoReader` 的 NVDEC 路径）

**♻️ 重写（缠死在 lada 线程/队列机制里，围绕新调度器重建）**
- 5 个 `_*_worker` 循环 + `PipelineQueue` + `EOF/STOP_MARKER` 握手
- `FrameRestorer.start/stop` 的 5 线程 teardown 握手（→ 改为 reposition，见 I6）
- GStreamer appsrc 胶水 + seek 拆建状态机
- 整个 GUI 外壳（新项目，无历史包袱）

**已放弃的新增方向（勿再当作待办）**
- **YOLO 跳帧 / 稀疏检测**：lada 时代曾认定为吞吐活路；sumu 实测瓶颈不在 YOLO，BasicVSR 压力已由 `clip_length=30` 等旋钮消化——**放弃，不再实现**。`_NoDetectionResult` 等历史 stub 可保留但无接线义务。

### E. 已知硬约束（不是 sumu 能消除的，设计时认账）

- **BasicVSR++ 时间维度延迟**：模型必须吃一段连续帧，延迟下界 ≈ clip 帧数。这是模型属性，任何架构都绕不开——sumu 用小 clip 窗口 + 回退原片来**遮蔽**它，不是消除。
- **单卡检测/修复争用**：一张卡上 YOLO 与 restorer 抢时间片。sumu 不靠 YOLO 跳帧缓解；靠 clip 长度、目标帧率、区块数与 frontier 闸门做产品层降级。
- **模型本身不可重写**：YOLO、BasicVSR++ 是产品本体。
- **播完行为**：产品策略是**播完停在最后一帧并暂停**，不循环回绕。解码/调度里残留的 loop 相关符号（如 `loop_offset_seconds_` 恒为 0、scheduler 的取模 eof 判定）是历史兼容/防御代码，不是待修的「循环播放」功能。

---

## 第二部分 · 验证方案（spike，用实测决定选型）

### 待决选型（本文件不锁定，由下面的 spike 拿数据定）——均已由后续 spike/开发落定，见 CLAUDE.md「已锁定选型」

1. **播放器宿主 + present 面**：Qt(PySide6/QRhi) ／ GTK4+叠加原生 HWND ／ 内嵌 libmpv ／ Win32·GLFW+D3D11。→ 已定：Win32 + 原生 D3D11 flip-model swapchain（变体 B）。
2. **present + CUDA 互操作的语言**：小原生模块(C++/Rust) ／ 尽量纯 Python(cupy·pycuda + CUDA↔GL)。→ 已定：C++（VS2022 BuildTools）+ pybind11。
3. **音频 / A-V 同步是否进 v1**。→ 已进 v1：`native/src/player.cpp` 的 `audio_loop()`（WASAPI），以 QPC 主时钟为准的纯附加从属时钟，已实测验证不扰动 present。

**成败 100% 押在「4K60 全 GPU present 从可选宿主/语言里能否做到上屏稳定」。** 所以先只验证这个，AI 一概不接。

### 目标机器（所有实测的唯一基准）

RTX 4080 · 4K@150Hz（scale=1）· Windows。素材：一段 **4K60 HEVC** 片（以及 lada 的 `test_video.mp4` 1080p30 作对照）。

### Spike 0 —— present 能力基线（第一道门）

**目的**：在目标机上证明 `NVDEC 解码 → GPU present` 能以稳定上屏帧时间放 4K60 HEVC，**不接 AI**。
**先测天花板**：用 **mpv / 浏览器**放同一段 4K60 片，量它的上屏间隔分布——这是硬件/OS 能达到的上限，作为 pass/fail 的标尺（别追一个机器本就达不到的目标）。
**再分变体各测一遍**（同一素材、窗口化 + 最大化各一轮）：
- **变体 A — Qt(PySide6) + QRhi present**：验证从 Python 可达性 + 能否 4K60 稳定。
- **变体 B — 原生 D3D11 模块 + swapchain**（最小 C++/Rust）：验证最稳健路径的实际平滑度与集成成本。
- **变体 C — 纯 Python CUDA↔GL 互操作**（cupy/pycuda + GLFW/PyOpenGL present）：验证「不写原生」是否可达、脆不脆。

**量什么**（复用 lada `scripts/analyze_present.py` 的方法学：分冷启动 vs 稳态窗口）：
- present 间隔 median / stddev / p99
- %frames > 2×帧预算（60fps 下 >33ms）、卡顿帧数 > 50ms
- 窗口化 vs 最大化的差异（lada 的残留墙正是最大化退化）

**通过判据**：稳态 median 贴目标帧时间、单峰无双峰、%>2×预算 与 mpv 基线同量级（个位数百分比内）、最大化不显著退化。

### Spike 1 —— CUDA↔present 互操作（全 GPU 的命门）

**目的**：证明一个 **torch CUDA 张量**（模拟 AI 产出的去码帧）能**不经主存**直接被选中的 present 面显示。这是 I3（全程 GPU）能否成立的命门。
**做法**：在 Spike 0 通过的变体上，构造一个 GPU 上的测试帧（torch tensor），走 external-memory / 互操作注册成 present 面的纹理，present 出来；量每帧互操作开销。
**通过判据**：互操作可用、无每帧主存拷贝、单帧开销远小于帧预算（60fps 下 ≪16.6ms）。

### Spike 2 —— 时钟驱动混流（架构验证，Spike 0/1 通过后做）

**目的**：证明 I1/I2——present loop 每个 vblank 在「ready-map 去码帧」与「原片环缓冲帧」之间**按帧号**挑最佳帧，混流不产生任何顿挫。
**做法**：present loop + 两个 GPU 帧源（一个模拟 AI ready-map，随机延迟填充；一个原片解码环缓冲），按 I5 帧号挑帧、按 I9 缺帧回退原片。量切换处有无 hitch。
**通过判据**：AI 帧随机就绪/缺失时，上屏间隔分布与「纯原片播放」无可辨差异。

### 决策矩阵（spike 出数后据此定选型）

| 若… | 则… |
|---|---|
| 变体 A(Qt) Spike 0+1 通过 | 选 Qt + QRhi，present 走 Qt 原生互操作，尽量少原生代码。 |
| A 不达标、B(原生) 通过 | 引入小原生 present 模块；宿主可 Qt 或极简窗口 + 原生视频面。 |
| 仅 C(纯 Python) 勉强通过 | 接受纯 Python 但记录其脆弱点；否则回到 B。 |
| 全部达不到 mpv 基线 | 重新评估「内嵌 libmpv + 想办法注入 AI 帧」这条备选，或止损。 |
| Spike 1 在任何变体都做不到零主存互操作 | I3（全程 GPU）需降级为「AI 帧允许一次上传」，重评整体收益。 |

**顺序**：Spike 0（含 mpv 基线）→ 若通过 → Spike 1 → 若通过 → Spike 2 → 选型落定 → 才开始铺 AI 集成与正式骨架。任一门不过就止损/换路，**在写任何 AI 集成代码之前**。

---

## 附：与 lada-realtime 的关系

- lada-realtime 仓库**保留不动**，作为 AI 计算核心的来源和行为对照。
- sumu **照搬**其 AI 计算核心与 README 核心机制（第一部分 C/D），**重写**其传输/呈现/seek 层。
- lada-realtime 的 CLAUDE.md 记录了大量实测封死的无效方向和踩过的坑——sumu 开工前应通读，避免重走。
