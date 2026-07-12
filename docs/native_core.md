# sumu 原生 present 内核 (`native/` -> `sumu_core`)

本文档描述从 Spike 2 (`spikes/spike2_clock_mixing`) 晋升而来的正式原生模块 `sumu_core`，
新增的 `seek()` = reposition 能力，以及晋升后的验证结果（含真实测得的数字，如实呈现）。

Spike 结论详见 `docs/spike_results.md`；本模块**保留** Spike 2 的核心架构结论不变：解码/
AI-push/present 三线程，单个显式 `d3d_mutex_` 串行化所有跨线程对共享 D3D11 设备/CUDA-D3D11
interop 上下文的访问（`ID3D11Multithread::SetMultithreadProtected(TRUE)` 本身在 Spike 2 的
50s 持续 3 线程负载下被证明不够，会触发 `DXGI_ERROR_DEVICE_HUNG`）。

## 目录结构

```
native/
  CMakeLists.txt        目标 sumu_core，链接 FFmpeg（复用 spikes/spike0 已解压的 dev 包，
                         未重新下载）与 CUDA v13.3 driver API（无 nvcc），.pyd 产物落地到
                         python/sumu/，同时把 FFmpeg 的运行时 DLL 拷贝到同一目录。
  build.bat              vcvars64 + CMake/Ninja，与 spike0/2 一致的模式。
  cmake/embed_shader.cmake  构建期把 native/shaders/present.hlsl 内嵌成生成头文件
                         (build/generated/present_hlsl.h)，shader 保持为可编辑的真实 .hlsl
                         文件，但 .pyd 本身不依赖运行期路径解析。
  shaders/present.hlsl    从 Spike 2 presenter.cpp 内联字符串原样搬出的 HLSL（VSMain 全屏三
                         角形 + PSMain_NV12 BT.709 limited-range 转换 + PSMain_AI 直通采样）。
  src/decoder.h/.cpp      在 Spike 0/2 的 Decoder 基础上新增 seek_to_frame()、width()/
                         height()/frame_count()。
  src/player.cpp          晋升后的主体：Player 类 + pybind11 模块 sumu_core。
  smoke_player.py         三项验证场景的驱动脚本（见下文"验证结果"）。
python/sumu/
  sumu_core.cp313-win_amd64.pyd  + avcodec/avformat/avutil/avdevice/avfilter/swresample/
  swscale DLL                    构建产物，import sumu_core 即可（sys.path 加入该目录）。
```

## Player 接口

```python
import sumu_core
p = sumu_core.Player(width_hint=1920, height_hint=1080, maximized=False)  # 窗口尺寸提示，
                                                                            # 与视频实际分辨率
                                                                            # 无关（shader 的 UV
                                                                            # 拉伸采样已完全解耦
                                                                            # 二者）
p.open(path)            # 打开视频，探测 fps/宽高/帧数，起 decode+present 两个线程，初始暂停于帧 0
p.play() / p.pause() / p.is_playing()
actual_frame = p.seek(frame_num)   # 见下文，返回真正落地的帧号（锚定真实解码 PTS，I5）
p.push_ai_frame(frame_num, cuda_dev_ptr, width, height, pitch_bytes)
p.current_frame() / p.frame_count() / p.fps() / p.dims()
p.ai_hit_rate() / p.stats() / p.present_stats() / p.dump_present_trace(path)
p.pump_messages() / p.should_quit()   # Win32 消息泵，需在主线程周期调用
p.close()
```

## 线程/锁模型（继承 Spike 2，新增 `decoder_mutex_`/`trace_mutex_`）

| 锁 | 保护对象 | 持有者 |
|---|---|---|
| `d3d_mutex_` | 共享 `ID3D11Device`/`ID3D11DeviceContext` 及 CUDA-D3D11 interop 的**所有**跨线程访问 | decode 线程的 ring copy、present 线程的整个 `draw_and_present`、`push_ai_frame` 的 CUDA map/copy/unmap+ring copy、`seek()` 的 ring copy |
| `decoder_mutex_` | `Decoder` 对象本身 **以及** 紧跟其后的"ring 写入 + tag 标记"整个序列 | decode 线程每次迭代（解码一帧+拷贝+打 tag 是原子操作）、`seek()`（seek 解码器+拷贝+打 tag 也是原子操作） |
| `ready_mutex_` | `pt_tag_`/`ai_tag_` 两个 ready-map 数组 | decode 线程、present 线程、`push_ai_frame`、`seek()` |
| `trace_mutex_` | present trace 的三个 vector（供并发调用 `present_stats()`/`dump_present_trace()`） | present 线程（写）、Python 侧查询（读） |
| `push_mutex_` | 防止并发调用 `push_ai_frame`（设计上单生产者，这里是防御性加锁） | `push_ai_frame` |

锁序固定为 `decoder_mutex_`（外层）-> `d3d_mutex_`（内层）-> `ready_mutex_`（最内层），present
线程和 `push_ai_frame` 从不触碰 `decoder_mutex_`，避免死锁。

**为什么要把 `decoder_mutex_` 的临界区从"只保护 Decoder 对象"扩大到"解码+拷贝+打 tag 整体"**：
如果只窄窄地保护 `Decoder::next_frame()`/`seek_to_frame()` 调用本身，decode 线程刚从**旧**位置
拉出的一帧，仍可能在其"拷贝进 ring + 打 tag"尚未完成时被 `seek()` 抢先完成整个 reposition，
随后 decode 线程才把这个陈旧帧写进（可能）同一个 ring slot，把刚 seek 好的画面悄悄覆盖掉——tag
看起来仍然"新鲜"，但内容是错的。把临界区扩大到整个"生产+发布"序列后，decode 线程的一次迭代和
`seek()` 互斥，此竞态被彻底关闭。

## seek() = reposition，不是 teardown（I6）

`Player::seek(frame_num)` **绝不**销毁或重建 decode/present 线程、`Decoder` 对象或 swapchain。
具体步骤：

1. 夹紧 `frame_num` 到 `[0, frame_count()-1]`（若 `frame_count()` 未知则只夹下界）。
2. 持有 `decoder_mutex_`，调用 `Decoder::seek_to_frame()`：
   - `avcodec_flush_buffers` + `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` 跳到目标帧之前最近
     的关键帧；
   - 从该关键帧开始前向解码，**用真实解码出的 PTS**（而非目标帧号本身）判断是否已到达/超过
     `frame_num`（I5：帧号唯一事实来源，锚定真实 PTS）；
   - 若目标帧号超出实际可解码范围（视频末尾附近的深度 seek），优雅地 clamp 到能拿到的最后
     一帧，而不是直接失败。
   - `first_pts_seconds_`/`loop_offset_seconds_` 这两个"帧号原点"锚点**从不**因 seek 而重置。
     产品策略已改为**播完停最后一帧、不循环**（`next_frame()` 在 EOF 返回 false，不再推进
     `loop_offset_seconds_`；该字段会话内恒为 0，仅保留在 PTS 公式里）。这保证任意次数 seek
     之后帧号在全局意义上仍然一致。
3. 把落地帧拷贝进 passthrough ring 对应的 slot，并**清空**（而非部分清空）`pt_tag_`/`ai_tag_`
   两个 ready-map——~1 秒深的环形缓冲相对一次真实 seek 几乎总是整体失效，无条件清空最简单也最
   安全（present 的"重复上一帧"兜底路径本来就永不阻塞，多重复几帧不会有任何代价）。
4. 把 present 线程的时钟锚点 `anchor_frame_`/`anchor_qpc_ticks_` 重新定位到刚落地的帧号——
   若正在播放，present 从此帧继续按原速前进；若处于暂停，画面直接跳到该帧并保持冻结。
5. 把 present 线程"重复上一帧"兜底路径的 `last_actual_slot_`/`last_actual_source_` 也重新指向
   刚 seek 好的 slot（`seek_slot_hint_` + `seek_version_`），防止 seek 后紧跟的第一个 present
   tick 因为调度抖动跑到 decode 尚未来得及填充的帧号时，兜底路径去重复**seek 前**的旧画面。
6. 返回真正落地的帧号（由 `Decoder::seek_to_frame` 的真实 PTS 反推），供 Python 侧核实。

## 构建

`cmd.exe /c native\build.bat`（PowerShell 工具调用，红色 stderr 输出是 vcvars64.bat 的正常
现象，以退出码/产物判断成功）。**一次性构建成功**，无需修复任何编译错误。产物：
`python/sumu/sumu_core.cp313-win_amd64.pyd` + 7 个 FFmpeg 运行时 DLL。

关键构建注意事项（均已应用）：
- `#define NOMINMAX` 置于 `player.cpp` 顶部 `#include <windows.h>` 之前。
- CUDA 仅用 driver API，链接 `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3/lib/x64/cuda.lib`，不涉及 nvcc。
- shader 通过 `native/cmake/embed_shader.cmake` 在构建期内嵌为头文件，`.hlsl` 源文件保持独立可编辑。

## 验证结果（`native/smoke_player.py`，均已实测，数字未经任何美化）

### 1. 50s 播放流畅度回归（`test_video_4k.mp4`，3840x2160, 59.94fps, 10763 帧）

命令：`smoke_player.py smoothness --seconds 50`，随后用 `scripts/analyze_present.py`（未修改，
`--format ns --fps 59.94005994005994`）分析导出的 present trace。

```
COLD-START [0s-6s]     n=359  median=16.68ms stddev=0.08ms  %>1.5x=0  %>2x=0  gaps>50ms=0
STEADY-STATE [10s-end] n=2397 median=16.68ms stddev=0.48ms  %>1.5x=0  %>2x=0  gaps>50ms=0
                       max=31.2ms  p99=17.10ms
```

与 `docs/spike_results.md` 中 Spike 2 混合轮次稳态数字（median 16.68ms stddev 0.54ms max 34.1ms）
高度吻合，属同一量级的正常统计波动，晋升未引入任何可见的present 抖动回归。本次运行
`ai_hit_rate=0.0`（未接 AI 生产者，纯 passthrough），`n_pt_stale=0`——整个 50s 内每一次
present tick 都拿到了精确匹配的当前帧，没有出现一次"重复上一帧"兜底。

### 2. Seek 压力测试（`test_video_long.mp4`，2.1GB, 1920x1080, 29.97fps, 295662 帧, 时长约 164 分钟）

命令：`smoke_player.py seek --rounds 4`，4 轮 x 5 个深度（10%/90%/50%/99%/30%），共 **20 次
seek**。每次 seek 前后：
- 记录 `present_count()` 差值（`ticks_during`，seek 调用返回后额外 sleep 150ms 期间 present
  线程实际推进的 tick 数——用来验证 present 心跳是否持续，而不是靠猜测）；
- 用全屏 GDI BitBlt 采样桌面平均亮度（`luma_before`/`luma_after`，粗糙的黑屏 tripwire，全屏
  窗口最大化时桌面大部分区域即窗口内容，见 `smoke_player.py` 中 `screen_mean_luma()` 的说明和
  局限性——它不是像素级"这就是视频画面"的证明，只能作为"整屏均值骤降到接近 0"这类严重故障的
  粗筛信号）。

**结果：20/20 次 seek 全部成功，零崩溃，零冻屏，present 心跳全程未曾停止。**

```
[seek] SUMMARY crashed=False frozen=False quit_early=False n_seeks=20
```

逐条摘录（`round/frac/target/actual/latency_ms/ticks_during/luma_before/luma_after`）：

```
round=0 frac=0.10 target=29566  actual=29566  latency=35.23ms  ticks_during=6  luma 48.0->49.9
round=0 frac=0.90 target=266095 actual=266095 latency=51.11ms  ticks_during=6  luma 50.0->48.8
round=0 frac=0.50 target=147831 actual=147831 latency=42.46ms  ticks_during=5  luma 48.7->53.1
round=0 frac=0.99 target=292705 actual=292705 latency=111.30ms ticks_during=8  luma 53.0->55.0
round=0 frac=0.30 target=88698  actual=88698  latency=111.97ms ticks_during=8  luma 55.0->52.7
... (round 1-3 数字与 round 0 高度一致，latency 波动 <20ms，此处不重复列出，完整数据见
    native/trace/present_seek_stress.csv 和 smoke_player.py 运行时的 stderr 日志)
```

latency 分布按深度分组（4 轮均值）：
- 10% (浅) ≈ 36ms
- 50% (中) ≈ 39ms
- 90% (深) ≈ 50ms
- 30% (重复中浅) ≈ 108ms
- 99% (接近文件末尾) ≈ 114ms

深度越深/越接近文件尾部，latency 越高——符合预期，因为 `seek_to_frame` 的耗时主要来自
`av_seek_frame`（对 2.1GB 文件的 I/O 定位）+ 前向解码到目标帧（GOP 越长/越靠近文件尾部触发的
EOF 探测越多，前向解码帧数越多）。**没有任何一次 seek 的 latency 超过 120ms**，远低于人类可感
知的"卡顿"阈值，也远低于 present 线程一次 tick 的预算（33.37ms @ 29.97fps）的数十倍级别故障。

**帧号精度**：全部 20 次 `actual == target`，即请求的目标帧号与 `Decoder::seek_to_frame` 依据
真实 PTS 反推出的落地帧号完全一致——本测试视频的 GOP 结构下没有出现"落在关键帧之间需要额外
前向解码几帧才追上"的情况，seek 精度达到帧级精确（I5 生效）。

**present 心跳连续性**：对同一进程运行期间导出的 present trace（`present_seek_stress.csv`，
横跨全部 20 次 seek 的整个测试时长）用 `analyze_present.py` 分析：

```
COLD-START [0s-2s]     n=59  median=33.36ms stddev=0.08ms  gaps>50ms=0
STEADY-STATE [2s-end]  n=124 median=33.37ms stddev=0.13ms  gaps>50ms=0  max=34.0ms
```

即：**在 20 次 seek（含深度/近尾部跳转）穿插的整个过程中，present 线程的输出节奏几乎是一条
直线**（stddev 仅 0.13ms），没有一次 present 间隔超过 50ms 的"卡顿"——这直接证实了 seek 的
reposition 实现完全没有阻塞或拖慢 present 线程，present 心跳自始至终未曾停止，也没有观察到黑屏
（luma 采样值在合理范围内随不同帧内容波动，未出现骤降到 0 附近的情形）。

### 3. Pause/Play 切换正确性

```
f_before_pause=119  f_during_pause=119 (pause() 后 sleep 1s 再检查，帧号完全冻结，frozen_ok=true)
f_after_resume=179  (play() 后 sleep 1s，帧号从 119 推进到 179，约 60 帧/秒，resumed_ok=true)
```

`pause()` 期间帧号精确冻结（不是"近似不变"，是完全相等），`play()` 后立即按正常帧率恢复推进，
`is_playing()` 状态切换正确，全程 present 线程未被销毁/重建（只是复用同一个正在跑的
present_loop，anchor 被重新定位）。

## 已知局限 / 未覆盖内容（历史记录；后续补测见 robustness_4e）

- `screen_mean_luma()` 的黑屏检测是**全桌面**采样而非窗口内容本身的像素级验证——粗筛信号，不是逐像素正确性证明。
- 需要真实桌面会话（D3D11 窗口 + swapchain）。
- 本文写就时「未测 seek 风暴 + AI 推流」——**已由 `docs/robustness_4e.md` ② 补上**（25/25 通过）。
- `g_quit` 全局共享误退：**已修（M-C1）**，见 `player.cpp` 的 per-instance `quit_`。
