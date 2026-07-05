# Phase 4e 鲁棒性验证（压测 + 边界）

本阶段覆盖此前诚实标注的未测项：4K best-effort 降级、seek 风暴 + AI 并发（此前最高风险项）、
EOF/循环回绕、降级旋钮、诊断可用性。所有数字均为目标机（RTX 4080 · Win11 · torch
2.8.0+cu128）**真实测得**，无编造/无投影。**本次验证发现一个架构级 native 缺陷（EOF 循环回绕
导致 present 永久冻结），已按纪律停下并在下文详细 escalate，未擅自改动 `native/`。**

**更新（2026-07-06）：③发现的 native 缺陷及其次生的 scheduler 缺陷均已按建议修法实现、
重建、实测验证通过（不再冻屏、循环后 AI 继续工作），详见下文"③ 修复验证"小节；下方
③正文与 escalate 段落保留作为根因分析的原始记录，不再代表当前状态。**

## 交付脚本

- `scripts/stress_seek_ai.py`（新增）——item②驱动脚本，见下文。复用
  `scripts/run_player.py::build_models`/`print_status`/`TRACE_DIR`，不重复接线。
- `scripts/run_player.py`（未改动，直接复用）——item①、item③驱动。
- `scripts/analyze_present.py`（未改动，直接复用）——所有 present 轨迹的节奏分析。

---

## ① 4K best-effort 降级（`test_video_4k.mp4`，40s）

命令：`run_player.py test_video_4k.mp4 --seconds 40`

- 视频：3840x2160, 59.940fps, 10763 帧；`load_models` 4.72s，`trt=True`。
- **present 全程 4K60 单峰稳定，零卡顿**：
  ```
  COLD-START  [0s-6s]   n=359  mean=16.68 median=16.68 stddev=0.17 max=17.7  gaps>50ms=0
  STEADY-STATE[10s-end] n=1798 mean=16.68 median=16.68 stddev=0.11 max=18.1  gaps>50ms=0
  ```
  present_stats_cumulative：n=2397, median=16.6816ms, max=18.0783ms —— 与
  `docs/native_core.md` 记录的纯 passthrough 4K60 基线（median=16.68ms stddev=0.48ms
  max=31.2ms）同量级，**AI 满载运行下 present 没有任何可观测回归**。
- **`ai_hit_rate` 实测约等于 0**（`0.000417`，`n_ai_fresh=1`/2398 次 present，其余
  `n_pt_fresh=2397`，`n_pt_stale=0`）——比 brief 预期的"低但非零"更极端。根因（由
  `scheduler_stats` 反推，非猜测）：`clips_restored=72`、`frames_pushed=2109`
  （≈52.7fps 推流速率，接近但低于 60fps），但 clip 化流水线天然有整 clip
  （30 帧）攒批 + restore 的批延迟，`backlog_resyncs` 高达 **61 次/40s**（约每 640ms
  一次）——每次 restore 耗时超过其对应 30 帧的实时时长后，下一轮循环立即检测到
  `ai_frontier < head`，直接丢弃在制 scene、frontier 跳到 head 重新开始。这个"批延迟 >
  frontier lead 窗口"的组合使得几乎每个刚推送完成的帧在被推时 present 早已越过该帧号，
  永远赶不上——即使 AI 吞吐（52.7fps）已经很接近 present 帧率（60fps）。
- **无堆栈/无 stale**：`n_pt_stale=0` 全程，即使 61 次 backlog resync，present 也从未需要
  重复上一帧兜底，降级路径干净。
- **结论：I9（降级不停顿）在 4K 上成立**，present 完全不受 AI 跟不上影响；`ai_hit_rate≈0`
  是真实数字，如实报告，判定为"预期内的极端低命中"而非 bug（present 韧性才是本项要验证的
  核心不变量，已达成）。

## ② seek 风暴 + AI 并发（`test_video_long.mp4`，本阶段最高风险项）

新脚本 `scripts/stress_seek_ai.py`：AI 调度器持续运行的同时，穿插 **25 次** seek
（5 轮 × [10%, 50%, 90%, 99%, 30%]，与 `docs/native_core.md` seek 压测同一组深度），每次
`notify_seek(n)` + `player.seek(n)` 后有 4s "AI 并发恢复窗口"（present+scheduler 都继续跑，
这正是要压的场景）。每次 `seek()` 调用前后用
`faulthandler.dump_traceback_later(20s, exit=True)` 武装死锁看门狗（成功后立即
`cancel_dump_traceback_later()`），一旦真死锁会转储全部线程栈并硬退出——**全程从未触发**。

**结果：25/25 次 seek 全部成功，零崩溃，零冻屏，零死锁。**

```
== SUMMARY == n_seeks=25 crashed=False frozen=False quit_early=False
             n_exact_frame=25/25 final_seek_resets=25 (expected>=25)
```

- **帧级精确**：全部 25 次 `actual == target`（无一次落在关键帧间隙需要额外前向解码）。
- **`seek_resets` 计数精确对账**：每次 seek 后 `scheduler.get_stats()['seek_resets']`
  精确 +1，25 次 seek 后终值恰好 25，无漏记/无重复触发——scenes/frame_cache 每次都被正确清空
  （`frame_cache_misses` 全程 **0**）。
- **seek latency 分布**（按深度分组，ms）：10% ≈36-40，50% ≈46-56，90% ≈52-55，
  99%（近文件末尾）≈129-134，30%（重复中浅）≈118-134；min=35.8 max=133.8 mean=79.3
  median=53.0。与 `docs/native_core.md` 纯 native seek 压测基线（10%≈36ms/90%≈50ms/
  99%≈114ms）同一量级，深度越深/越靠尾部 latency 越高的规律一致，AI 并发引入的额外开销在
  个位数至十几 ms（99%档 114ms->~130ms），**远低于卡顿阈值**。
- **present 心跳全程未断**：`ticks_immediate`（seek 后 150ms 内推进的 present tick 数）
  每次都 ≥5，从未为 0 或负数。完整 present 轨迹分析：
  ```
  COLD-START  [0s-6s]    n=179  median=33.37 stddev=0.09 gaps>50ms=0
  STEADY-STATE[10s-end]  n=3060 median=33.37 stddev=0.10 max=35.2 gaps>50ms=0
  ```
  present_stats_cumulative：n=3359, median=33.3662ms, max=35.188ms —— 与单次 seek 场景
  （`docs/scheduler.md`：median=33.3667ms max=34.4875ms）几乎完全一致，**25 次穿插 seek +
  持续 AI 推流对 present 节奏零可观测影响**。
- **hit_rate**：持续 seek churn 下稳定在 0.55~0.65 区间（终值 0.6146），`n_pt_stale=0`
  全程——每次 seek 后 AI 有意义地重新起步（而不是每次都掉到 0 附近再慢慢爬），符合
  `notify_seek()` 主路径的设计预期。
- **锁序结论**：`decoder_mutex_ -> d3d_mutex_ -> ready_mutex_` 在
  `seek()`/`push_ai_frame()`/`get_cuda_nv12_by_frame()` 三者高频交错调用下**没有出现死锁
  迹象**（20s 看门狗全程未触发一次）——`docs/native_core.md` 遗留的"未测试 seek 风暴 +
  AI 推流同时进行"的空白，本次已补上，**结论正面**。

## ③ EOF / 循环回绕（`test_video.mp4`，150s，跨越 3576 帧总长）—— **发现架构级 native 缺陷**

命令：`run_player.py test_video.mp4 --seconds 150`（视频 119.3s 播完一遍，本次跑 150s，
应至少触达一次自然回绕）。

**实测：present 在自然到达视频末尾、尝试循环回绕时永久冻结，而非平滑续播。**

```
avcodec_receive_frame failed: -541478725
[main] t=120.2s cur_frame=3523 present_count=3523 ai_push_count=3540 ...
[main] t=125.2s cur_frame=3523 present_count=3523 ai_push_count=3540 ...   (完全相同)
[main] t=130.2s cur_frame=3523 present_count=3523 ai_push_count=3540 ...   (完全相同)
[main] t=135.3s cur_frame=3523 present_count=3523 ai_push_count=3540 ...   (完全相同)
[main] t=140.3s cur_frame=3523 present_count=3523 ai_push_count=3540 ...   (完全相同)
[main] t=145.3s cur_frame=3523 present_count=3523 ai_push_count=3540 ...   (完全相同)
[main-end] t=150.0s cur_frame=3523 present_count=3523 ...                 (完全相同)
```

`current_frame()`/`present_count` 从 t=120.2s 到 t=150.0s（近 30 秒，7 次采样点）**逐字节
完全相同**——不是"降级变慢"，是彻底停止：present_loop 不再产生任何新 tick。
`decode_frame_count` 终值恰为 **3576**（= `frame_count()`，即完整解码完第一遍后，在尝试
回绕解码第二遍时死亡）。

**根因（读代码定位，非猜测）**：`-541478725` 即 `AVERROR_EOF`
（`FFERRTAG('E','O','F',' ')` 取负）。`native/src/decoder.cpp::pump_one_raw_frame()`
（178-222 行）顶层只处理了 `recv==0`（帧就绪）和 `recv==AVERROR(EAGAIN)`（需要更多输入）
两种情况；真正到达文件尾时，代码在 `EAGAIN` 分支内部（207-218 行）调用
`avcodec_send_packet(codec_ctx_, nullptr)` 让解码器进入"draining"状态，第一次
`avcodec_receive_frame` 若还能吐出缓冲中的帧就 `return 1`（注释明确写着
"next pump call will see EOF again"）——但**下一次调用** `pump_one_raw_frame()` 时，
顶层的 `avcodec_receive_frame()` 会直接返回 `AVERROR_EOF`（不是 `EAGAIN`！），这个值没有
被顶层任何分支处理，直接落入兜底的
`fprintf("avcodec_receive_frame failed: %d"); return -1;`（220-221 行）。
`Decoder::next_frame()`（225-241 行）把 `r<0` 当作"不可恢复错误"直接 `return false`，
`Player::decode_loop()`（1019-1025 行）据此 `stop_.store(true); return;`——**解码线程被
永久杀死**，present 线程随即也不再有新数据可呈现，最终表现为整屏冻结。

这与 `CLAUDE.md`/`DESIGN.md` 点名的 lada-realtime 原始架构病根（"冻屏"类症状）**同一症状
类别**，只是触发路径不同（这次是自然播放到底后的循环回绕，不是显式 seek）——直接违反 I9
（降级不停顿）。

**处置：按纪律停下，未修改 `native/`。** 本任务的改动授权明确写着"非必要不改 native/
（若 seek 风暴暴露 native 锁 bug，优先 escalate 由架构定夺，除非是显然的小修）"——这条
豁免专指"seek 风暴暴露的锁 bug"，而这是 EOF/draining 状态机 bug（不是锁序问题，也不是
seek 风暴发现的），不在豁免范围内，故完整证据+根因+建议修法附在此处，**escalate 给架构决策**，
不擅自动手。

**建议修法**（供架构决策，未应用）：在 `pump_one_raw_frame()` 顶层的
`avcodec_receive_frame` 判断里加一支 `if (recv == AVERROR_EOF) return 0;`（与
`recv==0`/`recv==AVERROR(EAGAIN)` 并列），使其和内层 EOF 分支已经处理过的语义一致——
"真正耗尽"直接返回 0，交由 `next_frame()` 已有的 `seek_to_start()` 回绕逻辑处理，不落入
不可恢复错误分支。这是一处局部、低风险的状态机补全，但因不在本次改动授权范围内，未直接应用。

**次生风险（静态审查发现，因上述 native 缺陷阻断而未能实测验证，先如实记录不擅自改）**：
即使上述 native 缺陷修好、循环回绕本身能正常工作，`python/sumu/scheduler.py::_run()`
里的
```python
if frame_count > 0 and n >= frame_count:
    time.sleep(cfg.sleep_step_s)
    continue
```
（约 274-279 行）看起来是按"播放一次性播完、不循环"的假设写的。但 `docs/native_core.md`
明确记录 `Player` 的帧号是全局单调递增、**循环回绕后不归零**（`loop_offset_seconds_`
持续累加，只有视频内容本身回绕，暴露的 frame_num 是 `frame_count, frame_count+1, ...`
而非 `0, 1, 2, ...`）。若未来 native 修好回绕，这条 guard 会在 `ai_frontier` 第一次到达
`frame_count` 后**永久**卡在这个分支（此后每轮都命中同一判断，即使
`player.current_frame()` 已经在回绕后持续爬升到 `frame_count` 之上），导致 AI 生产者对
"第二遍循环"永久失效、`ai_hit_rate` 回绕后永久归零——即使 present 本身（一旦 native 修好）
能正常继续跳动。**这一条我没有在本次改动，原因：native 侧的死锁在回绕点之前就已经把进程杀死，
我无法真实驱动到"回绕后 scheduler 继续跑"这个状态来验证/回归测试这个修复，为避免"禁编造"
而盲目改代码，这里只如实记录发现+建议修法，留给 native 缺陷解决后的下一轮验证**：
建议把该 guard 整段删除（`get_cuda_nv12_by_frame` 对"还没解码到的帧号"本身已经有
`ready=False` 的正确兜底，不需要这条基于绝对帧号的提前拦截），并把
`eof = bool(frame_count > 0 and n == frame_count - 1)` 泛化为
`eof = bool(frame_count > 0 and (n % frame_count) == frame_count - 1)`，让 flush 语义
按"每圈边界"生效而不是只在第一圈生效一次。

## ③ 修复验证（2026-07-06，两处修法均已落地并实测通过）

**修法1（native）**：`native/src/decoder.cpp::pump_one_raw_frame()` 在顶层
`avcodec_receive_frame` 判断里新增 `if (recv == AVERROR_EOF) return 0;`
分支（放在通用失败 `fprintf/return -1` 之前，不改动 `EAGAIN` 分支现有的
flush+drain 逻辑）。`cmd.exe /c native\build.bat` 重建成功
（`python/sumu/sumu_core.cp313-win_amd64.pyd`，2026-07-06 03:19 产物，
`BUILD_OK`；stderr 里的 `vswhere.exe`/936 代码页字符警告是既有的 vcvars/
非 ASCII 头文件噪声，不影响构建，退出码 0）。

**修法2（scheduler）**：`python/sumu/scheduler.py::_run()` 删除了
`n >= frame_count` 的永久停产 guard（AI frontier 现在跨循环持续跟随单调
递增的 present head，仅靠 `get_cuda_nv12_by_frame` 自身的 `ready=False`
节流），并把 `eof` 判定从 `n == frame_count - 1` 泛化为
`(n % frame_count) == frame_count - 1`，使每个内容循环边界都触发一次
`materialize_completed_clips(eof=True)` 场景 flush。

### 验证1：冻屏已消除（`run_player.py test_video.mp4 --seconds 150`）

视频 3576 帧 @29.970fps（≈119.3s/圈），150s 必然越过一次循环边界。
`current_frame`/`present_count` 全程持续递进，**无一次重复采样**（此前
bug 版本在 t=120.2s~150.0s 之间 7 次采样完全相同、彻底冻结）：

```
t=117.8s  cur_frame=3532  present_count=3532   (边界前)
t=118.9s  cur_frame=3562  present_count=3562   (临近边界, frame_count=3576)
t=119.9s  cur_frame=3593  present_count=3593   (已越过边界, backlog_resyncs 1->2)
t=130.0s  cur_frame=3897  present_count=3897
t=149.3s  cur_frame=4476  present_count=4476
t=150.0s(main-end) cur_frame=4495 present_count=4495
```

present 轨迹分析（`analyze_present.py --format ns --fps 29.97`，跨越边界
的整段 150s 轨迹）：

```
COLD-START  [0s-6s]   n=179  mean=33.36 median=33.37 stddev=0.28 max=35.4  gaps>50ms=0
STEADY-STATE[10s-end] n=4195 mean=33.37 median=33.37 stddev=0.15 max=35.6  gaps>50ms=0
```

`present_stats_cumulative`：n=4494, median=33.3662ms, max=35.5535ms —— 与
基线（33.37ms 单峰）完全一致，全程 `gaps>50ms=0`。**冻屏 bug 已消除，
根因修复确认有效。**

### 验证2：循环后 AI 仍工作（次生 bug 已修）

- **`seek_resets` 全程为 0**——循环边界（单调递增）未被误判为 seek/
  discontinuity，符合修法2"不应触发 seek_resets"的要求。
- **`backlog_resyncs` 从 1 变为 2**，且恰好只在跨越边界的那一次采样
  （t=119.9s）发生 +1，此后一直保持 2 直到运行结束——是边界 scene-flush
  瞬间的一次性重同步，不是失控增长。
- **`n_pt_stale` 从 0 变为 1**，同样只在边界那次采样 +1，之后不再增长
  ——边界切换瞬间一帧走了 stale 回退，之后立即恢复，不是持续劣化。
- **`frame_cache_misses` 全程为 0**。
- **hit_rate 边界前后对比**（跨圈窗口用增量而非累积值，避免累积平均掩盖
  骤降）：
  - 边界前累积 `ai_hit_rate`（t=117.8s）：0.989
  - 边界后窗口增量命中率（t=118.9s -> t=150.0s，第二圈内）：
    `Δn_ai_fresh=4420-3517=903`，`Δpresent_count=4495-3562=933`，
    窗口命中率 ≈ **0.968**——与第一圈稳态（0.98~0.99 同量级）基本持平，
    **没有塌到 0**，次生 bug 确认修复有效。
  - `clips_restored` 边界前 118（t=117.8s）、运行结束 149（t=150.0s main-end）
    ——边界后仍以与边界前相近的速率持续产出 clip，AI 生产管线未在边界
    处停摆。

### 验证3：回归（seek smoke + smoothness，`test_video.mp4`）

`run_player.py test_video.mp4 --seconds 40 --seek-test --seek-frac 0.5
--seek-observe-seconds 15`：

- **seek 帧精确**：`target=1788 actual=1788`（零偏差），`latency_ms=17.88`。
- **`seek_resets=1`**（与本次唯一一次 seek 调用精确对账，无漏记/无虚增）。
- **present 心跳未断**：`ticks_during_and_after=450`（seek 前后 present
  持续推进，未停顿）。
- **`ai_hit_rate_after_recovery_window=0.960`**——seek 后 15s 恢复窗口内
  AI 命中率维持高位。
- **present 节奏无回归**：`present_stats_cumulative` n=1648,
  median=33.3666ms, max=35.2001ms；`analyze_present.py` 输出：
  ```
  COLD-START  [0s-6s]   n=179  median=33.37 stddev=0.15 max=34.1  gaps>50ms=0
  STEADY-STATE[10s-end] n=1351 median=33.37 stddev=0.12 max=35.2  gaps>50ms=0
  ```
  与 `docs/scheduler.md` 记录的单次 seek 基线（median=33.3667ms
  max=34.4875ms）几乎完全一致（max 高出约 0.7ms，在系统噪声范围内），
  **native 改动对既有 seek/present 指标无可观测回归**。

### 结论

两处修法均已落地、重建、实测通过：冻屏 bug 已从根源消除（EOF 不再落入
不可恢复错误分支，触发既有 loop-to-start 路径）；次生的"循环后 AI 永久
失效"bug 已修复（frontier 跨圈持续跟随单调 head，eof 判定按内容位置取模
在每圈边界正确 flush 一次）；循环边界未误触发 seek_resets；既有 seek/
present 节奏基线无回归。本次测试为单次运行（与既有方法论一致的局限），
若需要长期回归基准建议多次运行取中位数。

## ④ 降级旋钮（`backlog_resyncs`）

`SchedulerStats.backlog_resyncs` 已经是现成的可观测降级信号（`python/sumu/scheduler.py`
的 `_run()` 在 `ai_frontier < head` 时递增并硬拉回 frontier）。用①的 4K 数据即可完整证明：

- **真实触发**：40s 内触发 **61 次**（约每 640ms 一次），是 4K 上 AI 吞吐追不上 60fps
  present 时的持续降级，不是一次性冷启动噪声（对照：seek 风暴测试全程 112s 只有 1 次，
  1080p 下 AI 轻松跟上，backlog_resyncs 基本不触发——证明这个信号只在真过载时响应，不是
  误触发）。
- **present 全程零可观测影响**（见①的 present 轨迹分析，两个窗口 gaps>50ms 均为 0，
  median 与无 AI 基线持平）。
- **`n_pt_stale` 不失控**：4K 全程 `n_pt_stale=0`，降级路径始终走"回退新鲜原片"而不是
  "重复旧帧"。
- 未额外构造 `clip_length` 调大等人工过载场景——4K 已是 brief 自己点名的"人为制造过载"
  方案，且已完整证明该旋钮在真实过载下工作正常，额外做一次人工过载实验对结论没有增量信息，
  为节省本阶段的 GPU 时间/会话预算而跳过。

**结论：I9 描述的"降级旋钮"（frontier 闸门 -> backlog_resyncs -> present 不受影响）在真实
4K 过载场景下完整、正确地工作。**

## ⑤ 诊断可用性

三个场景（①②③）全程都在用同一套诊断 API，均确认一致可用：

| API | 用途 | 本次验证 |
|---|---|---|
| `player.stats()` | 12 个字段：`fps/frame_count/present_count/decode_frame_count/ai_push_count/n_ai_fresh/n_ai_stale/n_pt_fresh/n_pt_stale/ai_hit_rate/is_playing/current_frame` | 三场景全程轮询，字段含义与实测行为一致 |
| `player.ai_hit_rate()` | 独立访问器 | 与 `stats()['ai_hit_rate']` 交叉核实数值完全一致（0.0 == 0.0） |
| `player.present_stats()` | 累计 present 节奏摘要（`n/median_ms/p99_ms/max_ms/min_ms/mean_ms`） | 三场景各跑完一次均正确返回 |
| `player.dump_present_trace(path)` | 导出逐次 present 的 ns 时间戳 | 三份 trace（`robustness_4k.csv`/`present_stress_seek_ai.csv`/`robustness_eof.csv`）全部成功导出，`scripts/analyze_present.py --format ns` 全部正确解析 |
| `scheduler.get_stats()` | 10 个字段：`frames_detected/clips_restored/frames_pushed/frame_cache_misses/seek_resets/backlog_resyncs/cold_start_s/ai_frontier/scenes_open/frame_cache_size` | 三场景全程轮询，数字在 seek/backlog/miss 等场景下均如预期变化 |

**诊断流程（本次验证实际用到的方法，可复用）**：

1. **实时脉搏**：跑测试时按 `print_status()`（`scripts/run_player.py`/
   `scripts/stress_seek_ai.py` 都有）每 2-5s 打一行 `ai_hit_rate`/`ai_frontier`/
   `backlog_resyncs`/`seek_resets`/`n_pt_stale`，异常（冻结/hit_rate 骤降不回升/
   backlog 狂增）当场可见——本次③的冻结现象就是靠这个立刻发现的（present_count 连续
   7 次打印完全相同）。
2. **节奏取证**：`player.dump_present_trace(path)` + `analyze_present.py --format ns
   --fps F --cold a b --steady c d`，看 `gaps>50ms` 是否为 0、`stddev`/`max` 是否偏离
   基线——本次①②的"present 零回归"结论均由此定量给出，不是目测。
3. **精度取证**：seek 场景下 `actual == target` 逐条核对（`stress_seek_ai.py` 已内置），
   `seek_resets` 计数与 seek 次数逐条对账。
4. **死锁防线**：新方法（`stress_seek_ai.py` 引入）——在任何"新的并发组合第一次被压测"
   时，用 `faulthandler.dump_traceback_later(timeout, exit=True)` 包住可疑调用（本次包
   `player.seek()`），成功后立即 `cancel_dump_traceback_later()`；一旦真死锁，会把全部
   线程的 Python 栈转储到 stderr 并硬退出进程，比"整个进程卡住只能靠外部超时杀"更有诊断
   价值，建议后续任何新并发压测都沿用这个模式。

---

## 汇总

| 项 | 结论 | 关键数字 |
|---|---|---|
| ① 4K 降级 | **通过** | present median=16.68ms全程零 gaps>50ms；`ai_hit_rate≈0`（比预期更极端但在架构容许范围内）；`backlog_resyncs=61`；`n_pt_stale=0` |
| ② seek 风暴+AI | **通过，本阶段最高风险项已排除** | 25/25 seek 精确+零崩溃/冻屏/死锁；`seek_resets`=25 精确对账；present median=33.37ms 无回归；latency 35.8-133.8ms |
| ③ EOF/回绕 | **架构级 native 缺陷已修复并实测通过**（2026-07-06） | 修法1（`decoder.cpp` 补 `AVERROR_EOF` 分支）+ 修法2（`scheduler.py` 去掉 `n>=frame_count` guard、eof 取模泛化）均已落地重建；150s 跨边界运行 present 零冻结、`gaps>50ms=0`、`seek_resets=0`；边界后窗口 `ai_hit_rate≈0.968` 未塌到 0；seek smoke 回归 median=33.3666ms 无回归 |
| ④ 降级旋钮 | **通过（复用①数据）** | `backlog_resyncs` 61 次真实触发，present 零影响，`n_pt_stale=0` |
| ⑤ 诊断可用性 | **全部确认可用** | 5 类 API 全部验证一致；新增 faulthandler 看门狗诊断模式 |

### 需要架构决策的问题（escalate）

**均已修复（详见"③ 修复验证"小节），无待决问题。** 原两条 escalate（native
`AVERROR_EOF` 分支缺失、scheduler `n>=frame_count` guard 与循环回绕不兼容）已按
③报告中的建议修法逐一实现、重建、实测验证通过。
2. **`python/sumu/scheduler.py` 的 `n >= frame_count` guard 与循环回绕的"次生"不兼容**
   （详见③次生风险）——待①修好后需要一轮新的回归验证，我建议的修法（删除该 guard +
   `eof` 判断取模泛化）已写在上面，但未实测验证，未擅自应用。

### 仍存局限（如实列出）

- 修复后 2026-07-06 重跑的③（150s）覆盖了"越过一次自然循环边界"并拿到了正面数据
  （present 不冻结、AI hit_rate 不塌到 0、`seek_resets` 不被误触发）——但仍只越过
  **一次**边界，未验证越过多次边界（例如跑 300s+ 越过 2~3 次）是否会有累积态问题
  （如 `frame_cache`/`scenes` 长期增长、`loop_offset_seconds_` 长期累加的浮点精度
  漂移）；也未覆盖②式的"回绕+seek 混合"压测（循环边界前后穿插 seek）——这两项建议
  留给下一轮验证。
- ④ 沿用①的数据而非独立构造一次新的过载场景（如调大 `clip_length`）——分析上认为
  这是同一个降级机制，实测上④和①在同一次运行里得到验证，为节省 GPU 时间未重复构造，
  如果后续需要一个与"4K 分辨率"正交的过载诱因（例如验证 CPU/GPU 争用型过载）需要另外
  设计实验。
- 数字均为单次运行结果（与 `docs/scheduler.md`/`docs/native_core.md` 已有先例一致的
  方法论局限）——如需长期回归基准，建议多跑几次取中位数，尤其是①的 `backlog_resyncs`
  次数和②的 seek latency 分布。
- ②的死锁看门狗超时设为 20s（`--hang-timeout`），本次全程未触发，说明 20s 内没有死锁，
  但不能排除锁竞争在更极端的调用节奏（例如 seek 间隔 <100ms 连打）下产生的、时间尺度
  更短或更长的问题——本次的"AI 并发恢复窗口"是 4s/次，不是背靠背零间隔连续 seek。
