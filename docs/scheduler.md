# AI 调度器 (`python/sumu/scheduler.py`)

本文档描述连接已验证的原生内核（`sumu_core.Player`，`native/src/player.cpp`，契约见
`docs/native_core.md` / `docs/native_ai_input.md`）与已照搬的 AI 计算核心
（`python/sumu/ai/`，`YOLO` 检测 + `BasicVSR++` 修复）之间的**集成层**：一个时钟驱动、独立
Python 线程运行的 AI 生产者，向 present 面的 ready-map 提前喂入去码后的帧。

`Scheduler` 是**重写**（lada-realtime `worker`/`PipelineQueue` 的单线程无队列版本，见 DESIGN.md
D「重写」清单），但它调用的每一个计算函数（`scene_clip.py` / `blend.py` / `video_utils.py` /
`cuda_dlpack.py`）都是**照搬**，且已由 `scripts/verify_scene_clip_blend.py` 单独验证过其调用方式。

## 设计

### 心智模型

播放器是主，AI 是仆（DESIGN.md）。`Scheduler` 跑在自己的 daemon 线程上，每一轮：

1. 检查是否有 `notify_seek()` 挂起的seek，有则立即整体重置状态（I6）。
2. 读 `player.current_frame()`（present 头），用启发式兜底检测未经 `notify_seek()` 的
   discontinuity（见下）。
3. **frontier 闸门**：若 `ai_frontier < head`（AI 落后于播放头），立即把 frontier 拉到
   `head`，丢弃所有在制品（I9：降级不停顿，绝不逐帧追赶挖坑）。
4. 若 `ai_frontier > head + lead`（AI 领先太多），`sleep(sleep_step_s)` 后重试。
5. 否则处理 `n = ai_frontier` 这一帧：`get_cuda_nv12_by_frame(n)`（非阻塞，`ready=False` 就
   sleep 重试）→ NV12→BGR → 缓存 → YOLO → `append_or_create_scenes`/
   `materialize_completed_clips` → 对每个刚完成的 clip：`restore_clip` + 逐帧
   `blend_back_frame` → BGR→RGBA → `push_ai_frame`。`ai_frontier` 前进一帧。

整个循环里没有任何调用会阻塞 present 线程：`get_cuda_nv12_by_frame`/`push_ai_frame`
本身在原生层就是为非阻塞设计的（`docs/native_ai_input.md`），调度器自己也从不等待 present。

### seek/不连续性处理

- **主路径**：调用方（`scripts/run_player.py`）在调用 `player.seek(n)` 的同时调用
  `scheduler.notify_seek(n)`（顺序不敏感——生产者线程在下一轮循环开始时先检查这个挂起值）。
- **兜底启发式**（仅当调用方绕过 `notify_seek()` 直接驱动 `player.seek()`/发生循环播放时才会
  触发）：`head` 相对上一轮**倒退**，或**前跳**超过 `seek_jump_threshold`（默认 500 帧）。
  之所以 500 帧是安全阈值：生产者循环每轮最多 sleep 1-2ms，即使某一轮处理耗时到了几十毫秒
  （60fps 预算内），一轮真实播放时间推进也只有个位数帧，几百帧的跳变只能用真实 seek 解释。
- 两条路径最终都调用同一个 `_reset_state(frame_num)`：清空 `scenes`、清空 `frame_cache`、
  `ai_frontier`/`_last_head` 重新锚定到 `frame_num`。`clip_counter` **不重置**（只是不透明 id，
  单调递增更安全，避免与仍在 restore/blend 中的旧 clip id 冲突）。

### frame_cache

`get_cuda_nv12_by_frame` 返回的是原生层**单缓冲区复用**的指针（`docs/native_ai_input.md`），
下一次调用就会覆写。调度器在拿到帧后立即 `.clone()` 存入一个以帧号为 key 的
`OrderedDict`（FIFO，超容量从最旧的开始淘汰），供该帧所属 clip 完成后 `blend_back_frame`
使用。

容量 = `lead + clip_length + frame_cache_margin`（默认 `36 + 30 + 16 = 82`
帧）——推导依据：一个 clip 最坏情况在 frontier 的末尾才刚开始，其 `frame_start` 也不会早于
`head`（否则早被 frontier 闸门追上/重置），所以只要缓存跨度覆盖 `lead + clip_length`，任何
仍在 in-flight 状态的 clip 所需的原始帧就不会被淘汰。两次真实运行（10s、45s+seek）里
`frame_cache_misses` 均为 **0**，验证了这个容量公式在实测下是足够的（miss 分支仍然写了防御性
代码：跳过 push、记录一次 miss、调用 `clip.pop()` 保持 clip 内部 bookkeeping 一致，而不是
desync 或抛异常）。

### RGBA 通道序

`_nv12_to_bgr_hwc_gpu` 输出的是 `torch.stack([b,g,r], dim=2)`，也就是 index 0=B、1=G、2=R。
原生 `push_ai_frame` 端要求的是 `DXGI_FORMAT_R8G8B8A8_UNORM`（byte0=R）。`_to_rgba` 显式做了
这个映射（`rgba[...,0]=bgr[...,2]`），并在实测中用独立通道相等性检查验证过（见下「正确性验证」）
——写反就是任务里点名的"蓝脸"故障。

## Config（`SchedulerConfig`）

| 字段 | 默认值 | 说明 |
|---|---|---|
| `clip_length` | 30 | BasicVSR++ clip 长度（TRT 引擎上限 180） |
| `clip_size` | 256 | 送入 BasicVSR++ 的方形 crop/resize 尺寸。**锁死 256，非可调降级旋钮**——烧进 TRT 引擎编译 shape（`INPUT_SIZE`），改动需重新编译引擎 |
| `max_regions_per_frame` | 1 | 每帧转成 scene 的 YOLO 检测数上限 |
| `lead` | `max(clip_length, round(1.2*clip_length))` = 36 | frontier 闸门上界：`ai_frontier ∈ [head, head+lead]` |
| `frame_cache_capacity` | `lead+clip_length+frame_cache_margin` = 82 | 见上「frame_cache」 |
| `frame_cache_margin` | 16 | 容量公式的安全余量 |
| `sleep_step_s` | 0.0015 | 无事可做（decode 未到/frontier 太超前）时的节流 sleep |
| `seek_jump_threshold` | 500 帧 | 兜底 discontinuity 启发式的前跳阈值 |
| `bt709` / `full_range` | `True` / `False` | 两个测试视频都是 BT.709 limited-range（见 CLAUDE.md） |
| `model_name` | `"basicvsrpp-v1.2"` | 传给 `restore_clip` 的模型名 |

## 真实数字（如实呈现，均为目标机 RTX 4080 实测，非估算）

### 10 秒干跑（`--seconds 10`，无 seek/correctness）

- `load_models`：4.81s，`pad_mode=zero`，`trt=True`
- 视频：`test_video.mp4`，1920x1080，fps=29.970，frames=3576
- `ai_hit_rate` 爬升：t=2s→0.467，4s→0.733，6s→0.823，8.1s→0.863，10s→0.890
- **`cold_start_s = 1.0468s`**（从 `scheduler.start()` 到第一次 `push_ai_frame` 成功）
- `backlog_resyncs=1`（预期中的一次性启动重同步，无害——见下「坑」）
- present_stats（累计）：n=299, median=33.3662ms, p99=33.941ms, max=34.2975ms, min=32.4373ms, mean=33.3644ms
- scheduler_stats（末态）：frames_detected=322, clips_restored=10, frames_pushed=300, frame_cache_misses=0, seek_resets=0

### 45 秒 + 15 秒 seek 观察窗（`--seconds 45 --seek-test --seek-observe-seconds 15 --correctness --capture-samples 5`）

- `load_models`：4.75s，`trt=True`
- `ai_hit_rate` 爬升：5s→0.713, 10s→0.850, 15s→0.900, 20.1s→0.925, 25.1s→0.940, 30.1s→0.950, 35.1s→0.957, 40.1s→0.963, 45.0s(主循环末)→0.967
- **`cold_start_s = 1.1397s`**
- **seek 测试**：`target=1788, actual=1788`（帧号精确），`seek() latency_ms=13.17`
- **seek 后 15 秒恢复窗**：恢复窗 t=5s 时 `ai_hit_rate=0.953`（相对 seek 前 0.967 有一次预期中的
  下探——AI 在新位置重新冷启动），t=10s→0.957，窗口结束→0.960；`present_count` 在整个 15 秒
  窗口内前进了 450（present 全程没有冻结）；`seek_resets=1`（准确检测到一次，未误触发）
- **运行末态整体**：`present_count=1813`, `ai_push_count=1830`, `n_ai_fresh=1741`,
  **`n_pt_stale=0`**（零次"重复上一帧"的 stale present），`ai_hit_rate=0.9603`
- present_stats（累计，含整个运行+seek 事件）：n=1812, median=33.3667ms, p99=33.666ms,
  max=34.4875ms, min=32.2267ms, mean=33.3663ms
- scheduler_stats（末态）：frames_detected=1871, clips_restored=61, frames_pushed=1830,
  **frame_cache_misses=0**（全程含 seek 事件，零 miss）, seek_resets=1, backlog_resyncs=1

### `analyze_present.py` 冷启动/稳态窗口分析（60.5s 全轨迹，1812 次 present）

```
COLD-START [0s-6s]   n=179  mean=33.36 median=33.37 stddev=0.06  max=33.8   p99=33.44  gaps>50ms=0
STEADY-STATE [10s-end] n=1513 mean=33.37 median=33.37 stddev=0.10 max=34.5  p99=33.71  gaps>50ms=0
```

对照原生基线（`docs/native_core.md` seek-stress 稳态：median=33.37ms stddev=0.13ms）：**持平，
无回归**——即使全程 AI 命中率爬升到 ~96% 且中途发生一次 seek，present 单峰节奏（~33.37ms）
完全没有被扰动。额外对整条 1812 个间隔的原始轨迹做了逐间隔扫描（不局限于
`analyze_present.py` 默认的冷启动/稳态窗口）：`max=34.4875ms, min=32.2267ms`，**没有任何一个
间隔超过 40ms**，全程（含 seek 事件本身）零异常抖动。

### 正确性验证（5 个样本，帧 16-20，均来自第一个完成的 clip）

方法：调度器在每次 `push_ai_frame` 前捕获 `(original_bgr, final_bgr, rgba)` 三元组
（`capture_correctness_samples` hook，默认关闭、零开销），运行结束后用**独立**的 PyAV CPU
解码同一视频到相同帧号做交叉验证（`scripts/run_player.py::run_correctness_check`）。

| frame | MAE vs PyAV 参考 | 通道序 exact match | max\|final-original\| | 变化像素占比 |
|---|---|---|---|---|
| 16 | 0.6976 | True | 128 | 0.0307 |
| 17 | 0.6991 | True | 133 | 0.0302 |
| 18 | 0.7002 | True | 128 | 0.0299 |
| 19 | 0.7000 | True | 131 | 0.0314 |
| 20 | 0.6990 | True | 130 | 0.0308 |

结论：**5/5 全部通过**——通道序（R@byte0）精确相等（非近似），去码修复清晰可见
（`max_abs_diff` 128-133/255，每帧约 3% 像素发生变化，对应马赛克区域大小），且调度器自己的
NV12→BGR 采集引入的 MAE（0.698-0.700）与 `docs/native_ai_input.md` 已验证过的原生桥基线
（1080p mean 0.6990）几乎完全吻合——证明调度器这一层没有引入任何额外误差。

## 已知坑与未覆盖项（如实列出，不回避）

- **一次性 `backlog_resyncs=1` 属预期**：调度器线程在 `scheduler.start()` 之后、
  `player.play()` 之前就已启动（`run_player.py` 的顺序），`ai_frontier` 初始为 0；
  播放刚开始的极短窗口内，YOLO+aggregation 的耗时可能让 `ai_frontier` 短暂落后于飞速前进的
  `head`，触发一次 frontier 重同步。这是设计内的正常降级行为（I9），不是 bug，但值得在这里
  明确记录，避免被误读成故障。
- **兜底 discontinuity 启发式（前跳>500帧/倒退）从未被单独触发测试过**：本次 seek 测试全程
  走的是 `notify_seek()` 主路径，`seek_resets` 计数器里的那一次确实来自 `notify_seek()`
  分支，不是启发式分支。启发式代码本身逻辑简单（阈值比较），但没有构造"调用方绕过
  `notify_seek()` 直接 `player.seek()`"这种场景验证过。
- **EOF flush 路径未被触达**：两次测试运行都只推进到 frame ~1200-2300（3576 帧视频的中段），
  从未真正到达视频末尾，因此 `_process_frame` 里 `eof=True` 触发的
  `materialize_completed_clips(..., eof=True, ...)` 那个"强制 flush 未完成 clip"的分支完全没有
  被执行过，只有静态代码审查，没有运行时验证。
- **视频循环播放（loop）/wraparound 未测试**：`_last_head` 倒退触发 reset 的逻辑理论上能覆盖
  "播放到末尾后从头循环"的场景，但没有实际构造循环播放场景验证过。
- **只测试了一次 seek**：45 秒运行里只做了一次 `notify_seek`+`seek()`（中点位置），没有像
  `docs/native_ai_input.md` 里原生层那样做多轮×多个 fraction 的 seek 压力测试（附带调度器）。
  多次连续 seek 对 `frame_cache`/`scenes` 状态机的重复清空-重建行为没有压力测试过。
  hit_rate 在单次 seek 后的恢复轨迹（0.967→0.953→0.957→0.960，15 秒内）是唯一一组样本，没有
  验证是否在更差情况（例如 seek 到视频最开始、或背靠背连续 seek）下恢复模式会不同。
  性质：这是"消费方一次真实调用"的验证，不是穷举/压力测试。
- **`get_cuda_nv12_by_frame` 的多消费者/多线程调用测试范围**：调度器设计上只有一个生产者线程
  调用它，`docs/native_ai_input.md` 已经指出原生层没有专门测过多线程并发调用；本调度器同样
  没有引入或测试这种场景（设计上就是单线程模型，不需要）。
- **frame_cache 容量公式（82 帧）在两次测试里都从未被逼近极限**：`frame_cache_size` 观测到的
  实际峰值在 20-22 帧左右，远低于容量上限 82；公式的"最坏情况"论证是分析性的，没有构造出真正
  逼近该上限的负载场景（例如极慢的 clip 完成速度、或反常大的 clip_length）来验证淘汰边界行为。
- **数字来自单次运行，非多次重复取统计分布**：与 `docs/native_ai_input.md` 已有先例一致的
  方法论局限——present/hit_rate/cold_start 数字都是各自场景下的单次真实测得结果，如果要作为
  长期回归基准维护，建议后续像 `docs/native_core.md` 一样多跑几次取中位数。
- **`ai_hit_rate` 未达到 1.0**：这不被认为是 bug——`test_video.mp4` 虽然全程马赛克
  （CLAUDE.md），但 clip 化处理天然有批延迟（一个 clip 要攒够/等到场景结束才能 restore+push），
  加上 frontier 闸门把提前量限制在 `lead=36` 帧以内，个别帧在 present 需要它的那一刻 AI 还没
  处理完，present 会退回 passthrough（`n_pt_fresh`，不是 `n_pt_stale`，即不是重复旧帧，是原始
  未去码帧）。0.96-0.97 且仍在稳定爬升是一个符合"降级不停顿"设计预期的健康结果，而非应该强行
  拉到 1.0 的缺陷。

## 交付文件

- `python/sumu/scheduler.py` —— 调度器实现（本文档描述的对象）
- `scripts/run_player.py` —— 端到端冒烟测试驱动（`load_models → Player.open → Scheduler.start
  → play() → pump_messages 主循环 → 可选 seek 测试 → 可选正确性校验 → dump_present_trace`）
- `scripts/trace/present_run_player_full.csv` / `scripts/trace/run_player_full_result.json` ——
  本文档「真实数字」一节引用的完整 45s+15s 运行的原始 present 轨迹与结构化结果
- `scripts/trace/present_run_player.csv` / `scripts/trace/run_player_result.json` —— 10 秒
  干跑的对应产物

未修改：`native/`、`python/sumu/ai/` 内部实现（只调用，未改动）、`spikes/`、`lada-realtime`。
`python/sumu/__init__.py` 未新增/未修改——`python/sumu` 作为 PEP 420 隐式命名空间包已可正常
`import`（`from sumu.ai import ModelFiles` 等均验证通过），无需接线改动。
