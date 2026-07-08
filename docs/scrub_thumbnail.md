<!-- SPDX-FileCopyrightText: sumu Authors -->
<!-- SPDX-License-Identifier: AGPL-3.0 -->

# M4 — 进度条 hover 缩略图预览（scrub thumbnail）

进度条悬停/拖动时，在竖直游标线上方浮出对应时间点的画面缩略图。M3 已把呈现骨架
（hover 检测、竖线、`get_thumbnail()` 调用点、`AddImage` 绘制）全部铺好，本里程碑补上
真正的缩略图产出。全部实现在 `native/src/player.cpp`（`decoder.{h,cpp}` 未改）。

## 架构

**独立 scrub-decode 路径，在本 Player 内**（不是第二个 Player 实例）：

- `scrub_decoder_` —— 第二个 `Decoder`（= 独立 NVDEC 会话），open_session 里 open 同一路径，
  只被 `scrub_thread_` 触碰。
- `scrub_thread_`（session 作用域，open_session 启 / close_session join）跑 `scrub_loop()`：
  1. 睡在 `scrub_cv_` 上，等主线程 push 新的 hover bucket；
  2. 在 `scrub_decoder_` 上做慢 seek（35–134ms），**不持任何 Player 锁**——解码器自管其
     d3d11va 池，靠设备级 `ID3D11Multithread` 序列化，与 `decode_loop()` 的解码同理；
  3. 极短 `d3d_mutex_` 临界区：`CopySubresourceRegion` 把 NV12 拷进单 slice `scrub_nv12_tex_`，
     再用 present 主绘制**完全相同**的管线（`vs_`+`ps_nv12_`+`sampler_`+`slice_cb_`）渲染到一张
     256×144 RGBA 缩略图 RTV（`render_thumbnail()`）；
  4. 在 `scrub_cache_mutex_` 下把结果发布进环形缓存。
- `get_thumbnail(frame)` 运行在**主线程** `ui_tick()`/`build_bottom_bar()`（该路径从不碰
  `d3d_mutex_`）：只量化 bucket、查缓存命中即返 SRV、未命中则 `scrub_request_frame_.store` +
  `notify_one` 后返回 `nullptr`。全程非阻塞、无 GPU 活，故 hover 永不扰动 present 节奏。

**量化**：bucket = 帧号按 `round(fps)` ≈ 1 秒粒度对齐，避免逐像素 hover 重复解码。

**Coalescing**：`scrub_request_frame_` 单值「最新胜」；scrub 线程 seek 完再检查一次，若期间
hover 已移走则丢弃本次结果、下轮服务最新 bucket——快速拖动时竖线即时跟随、缩略图追上不卡。

**缓存**：`kScrubCacheCap=12` 张预建复用的 RGBA 纹理（tex+RTV+SRV），环形回收；仅 `bucket`
随复用变化，ComPtr 常驻，故烘进 ImGui draw snapshot 的 SRV 直到 present 渲染都有效。≈2MB VRAM。

**锁序**：主线程 `get_thumbnail` 只取 `scrub_cache_mutex_`；scrub 线程先在 `d3d_mutex_` 下转换、
释放后再在 `scrub_cache_mutex_` 下发布——两把锁从不同时持有，与 present 线程（只 `d3d_mutex_`）
之间无环。

## 生命周期

- open_session：`create_ring_resources()` → `create_scrub_resources()` 建 scrub GPU 资源；随后
  best-effort open `scrub_decoder_`（失败仅禁用缩略图、不失败整个 session）并启 `scrub_thread_`。
- close_session：置 `session_stop_` + `notify_one` 唤醒 scrub 线程，**在 `d3d_mutex_` 拆建块之前**
  join 它（它会在 blit 时取 `d3d_mutex_`，块内 join 会自死锁）；块内释放 scrub 资源与缓存；
  块后 `scrub_decoder_.close()`。reopen 走 close→open，尺寸变了 scrub 资源随之重建。

## 取舍

- **第二个 NVDEC 会话**：目标机 RTX 4080·16GB·NVDEC 余量足；I7「单解码头」意在不让**播放路径**
  重复解码/撑爆 lookahead 预算，按需 scrub 的轻量第二解码器不违其精神。若后续要省，可改惰性
  open（首次 hover 才建、空闲超时 close）。

## 验证（2026-07-08，RTX 4080 · 目标机）

构建 `cmd //c native\build.bat` → `BUILD_OK`。

1. **present 零回归（硬门）** —— `run_player.py test_video_4k.mp4 --seconds 12 --no-trt`，scrub
   解码器+线程存在但空闲。`analyze_present.py --format ns`：
   - 稳态 median=16.68ms stddev=0.09 p99=16.92 max=17.3，%>2×=0，gaps>50ms=0，单峰（16–17ms 占 99.2%）。
   - 与 present-pacing 基线（median≈16.68ms）完全一致 —— 加第二 NVDEC 会话 + scrub 线程对上屏节奏零影响。
   - 同一轮 AI 正常推理（ai_push_count 616），第二会话不饿死 AI。trace：`scripts/trace/present_m4_scrub_idle.csv`。
2. **scrub 生命周期（reopen 压测）** —— `stress_reopen.py --rounds 8`（1080p↔4K 交替）：8/8 轮
   `dims_ok=True err=None`，latency 28–64ms，`present_ticks_advanced>0`（present 跨 reopen 不冻），
   无 hang（20s hang-timeout 未触发）、无崩、无 DXGI device-removed。证明 scrub 线程 join / 资源随
   尺寸重建 / 解码器重开在会话切换下干净。

**尚待人工确认（需鼠标交互，自动化无法触发）**：hover 各处缩略图内容正确、快速拖动的
coalesce 手感、鼠标离开自动隐藏。用 `.venv\Scripts\python.exe scripts\play.py` 打开任一 test
视频、悬停/拖动底部进度条即可观察。

---

## 修订（2026-07-08）—— 上线后三个实测问题的连续修复

上文的「验证」只量了 present 非回归 + reopen 生命周期，**没真拿鼠标 hover 看过**，漏掉了产出
与呈现两层 bug。用户实测反馈后逐一修复（均在 `player.cpp`）：

### 1. 缩略图从不显示 —— 两层根因

- **产出侧（主因）**：`Decoder::seek_to_frame()` 开头有 `if(!have_first_pts_) return false`，而
  `have_first_pts_` 只在 `next_frame()` 里设。scrub 解码器只 seek 不播放 → 每次 seek 都在首行返
  false → cache 永不填 → `get_thumbnail()` 恒 null。**修**：`scrub_loop()` 启动先 `next_frame()`
  解一帧 frame 0 打底 PTS 原点。教训：**任何 seek-only 的 Decoder 实例必须先 next_frame() 一次**。
- **呈现侧（次因）**：缩略图画在底部条 ImGui 窗口（高仅 56px）的 draw list 上，位置在窗口顶上方
  ~100px，被窗口 clip rect 整个裁掉。**修**：改用 `ImGui::GetForegroundDrawList()`（只裁到视口、
  盖最上层）+ 背景/描边。缩略图源与显示均降到 **160×90**（1:1，免缩放）。

### 2. 移动 hover 闪烁 —— nearest-on-miss

每到新 bucket 先 miss→返回 null→那一帧不画图→空白闪。**修**：miss 时返回 cache 里**最近**一张
已解码缩略图（照旧异步请求精确 bucket，落地后无缝换上），移动中永不空白。on-demand 环形缓存
`kScrubCacheCap` 12→32。

### 3. 快速跳远处显示「错得离谱」的邻近帧 —— 两级缓存 + 后台粗网格

on-demand 环只覆盖最近 hover 过的点，跳到远处「最近」可能差几十秒/几分钟。**修**：加一层
**持久粗网格**（`scrub_grid_`），均匀跨满 `[0, frame_count-1]`，把 nearest 误差钳到 ~半个网格间距：

- **按时长动态定槽**：`slots = clamp(dur/4s, 128, 2048)`，间距 ~4s。实测 119s→128 槽/0.93s、
  2.7h(9865s)→2048 槽(上限)/4.80s/118MB。跨满全片（含尾部）—— 早期 `i*stride` 版本尾部欠覆盖
  ~1 个 stride，那里 nearest 差 4.7s，已修为 `i*(fc-1)/(slots-1)` 端点全覆盖。
- **nearest-keyframe-only 解码是让「边播放边填」免费的关键**（I10 硬门，实测多轮推翻直觉）：
  - 第一版让网格「跳关键帧再前解到精确」（`seek_to_frame` 默认路径，一格可能前解一整个 GOP =
    几十帧 4K）。**边播放边填**时后台 NVDEC 突发抢前台 4K 解码 → present 稳态 stddev 0.09→**3.17**，
    且**节流只降尖峰频率、降不了高度**：throttle 0→1000ms，stddev 3.17→1.37 但 p99 恒 ~22ms
    （每填一格必有一次 ~6ms 单-vblank 微顿）。%>2×=0、无 gap，但达不到基线。
  - 改成网格**只解到目标附近最近的关键帧（1 帧）**（`seek_to_frame(..., nearest_keyframe=true)`，
    grid 槽 bucket 记实际落点帧）：present 直接回**基线**——4K60 播放中填 6.6/s 时 median 16.68 /
    **stddev 0.07** / p99 16.85 / %>2×=0 / 无 gap，与不填时（0.07 / 16.90）无法区分。每格从解一个
    GOP 降到解 1 帧，尖峰主因（前解突发）随之消失。代价：网格图是关键帧、可能比目标早 ≤1 GOP，
    nearest 误差 = ~半间距 + ≤1 GOP（2min 测试实测 worst 1.67s），hover 停住后仍由 ring 精确刷新。
  - **hover 环（精确预览）仍走前解到精确**（`nearest_keyframe=false`），只有粗网格用关键帧快路径。
- **播放中就填**（不再暂停限定）：`kGridFillThrottleMs=150` 的 hover-可打断节流仅在**播放时**加
  （避免突发），**暂停时全速**（前台空闲）。测：2min 128 槽播放中 ~20s 填满；2.7h 2048 槽 ~5min。
- **开场即武装**（非惰性）：`open_session` 末尾置 `scrub_grid_wanted_`，网格立刻开填。**hover 永远
  优先**（每填一格回头看 hover）；**就近先填**（优先填离上次 hover 最近的空槽 → 正看的区域先覆盖）。
- 两级 cache 同一把 `scrub_cache_mutex_`；`get_thumbnail()` 先扫 ring（精确）再扫 grid（兜底最近）。
  reopen 多轮干净（网格随视频尺寸重建、present 跨切换不冻）。

**仍待人工确认**：真实鼠标下的手感与内容正确性。

---

## 修订（2026-07-09）—— 分级加密（coarse-to-fine）+ 实测把填充提速 ~20–80×

上一版网格是「就近 hover 顺序填、`clamp(dur/4s,128,2048)` 槽、播放中恒 150ms 节流(6.6/s)」。
用户追问：填充能否更快？改「分级取点逐渐变密 64→128→256→512」、槽上限 512 下限 64；1080p30
全片缓存能否进 10s？据此实测（I10，全部无 AI，目标机 RTX 4080，`analyze_present.py --format ns`
稳态窗口，`--sustain` 让短视频持续满负荷以在稳态窗口内真正加压）：

### 1. 单帧成本极低 → 时间从不是瓶颈

nearest-keyframe 单帧解码（`seek_to_frame(...,nearest_keyframe=true)`）在 1080p30 上**暂停满速
~600–830/s**：512 槽 <1s、连 2.7h 影片的 2048 点也只要 ~3.2s 填满。**「1080p30 全片缩略图进 10s」
远远达标**——瓶颈是 VRAM 不是时间。所以槽上限设 **1024**（~1024×57KB≈58MB，原 2048/118MB 太多、
初版 512/29MB 偏保守），长片靠 ring 悬停即精确刷新，粗网格间距大一点可接受。**1024 全满(播放态)
估算：1080p30 ~4s、4K60 ~14s**（4K 超 10s 但粗覆盖前 64 点仍 <1s、前 256 点 ~3.4s，体感不受影响）。

### 2. 播放中的真限制是 present 抖动，且只在 4K、只在 0ms 忙等时出现（拐点极陡）

| 视频 | 播放中 inter-fill yield | 填充率 | present 稳态 stddev（基线 0.10） | gaps |
|---|---|---|---|---|
| 1080p30 | 0ms（忙等） | 586/s | 0.16 | 0 |
| 1080p30 | 2ms | 263/s | 0.09 | 0 |
| 4K60 | 0ms（忙等） | 224/s | **2.23** | 0 |
| 4K60 | 5ms | 161/s | 0.09 | 0 |
| 4K60 | 12ms | 75/s | 0.11 | 0 |

**唯一坏点是「4K + 0ms 忙等」**：scrub 解码器背靠背喂解码队列、从不让步 → present stddev 0.10→2.23
（无丢帧、无 gap，但 trace 上可见抖）。**任何非零让步（哪怕 5ms）即回基线**——问题是「不让步」，
不是「解得多」。1080p 因present预算大（此机 30fps=33ms/帧），连 0ms 忙等都基本干净（0.16）。

### 3. 定案：暂停满速；播放中按分辨率让步

- 暂停：0ms，满速（前台空闲）。
- 播放 ≤1080p：`kGridPlayThrottleLoRes=2`ms → ~263/s，stddev 0.09。
- 播放 >1080p(4K)：`kGridPlayThrottleHiRes=12`ms → ~75/s，stddev 0.11（留 AI 竞争余量）。
  阈值 `kHiResPixels≈1920×1080`，在 `create_scrub_resources()` 按 `src_width_*src_height_` 定 `grid_play_throttle_ms_`。
- 端到端（实际接线、非 debug override）复测一致：4K 73/s·0.16、1080p 267/s·0.10，均零 gap；
  512 槽在 1080p 播放中 ~1.6s 填满。比原 6.6/s 提速 ~20×(4K)/~40×(1080p)。

### 4. 分级加密顺序（bit-reversal，coarse-to-fine）

槽按 `rank[i]=bit_reverse(i)` 升序填：先 0、中点、四分、八分……**整条时间轴先被 64 点粗覆盖，
再逐批对分加密到 128/256/512**，每多解一帧就把**全片任意处**的最坏 nearest 大致减半——而不是先把
一段填密、远端留空。同一 octave 内以「离 hover 最近」破平局，正在拖的区域先加密。实测（暂停满速）
全片最坏 nearest 随点数单调收缩：2.7h 片 64pt→64s、256pt→22s、512pt→12.5s（理想 9.65s，差值=
keyframe 吸附 ≤1 GOP）；短片 119s→64 槽/最坏 1.84s。若是旧的「就近向外填」，161 点时远端半条片
（~4900s）还全空——12.5→64s 这条曲线正证明是全局覆盖（该曲线在 512-上限 build 实测；下述上限提到
1024 后再加一层对分 octave，同片 1024 点最坏 nearest ~6s）。reopen 8 轮（1080p↔4K 交替）rank/octave
向量随尺寸重建、每轮 fail=0、present 跨切换不冻。

**上限 512→1024（2026-07-09 定案）**：`kGridSlotsMax=1024`（~58MB VRAM，16GB 上无压力）。槽钳位
`clamp(dur/4s, 64, 1024)`：短片 119s→64，>~68min 的片旗满 1024（2.7h→1024，间距 ~9.6s、最坏
~6s 粗预览）。粗网格只做「跳远处立刻有个近似帧」的兜底，精确永远由 ring 悬停刷新（≤~30ms）。1024
全满(播放态)估算：1080p30 ~4s、4K60 ~14s；粗覆盖前 64 点仍 <1s，体感不受 4K 的 14s 尾巴影响。

**仍待人工确认**：真实鼠标下正常播放（不暂停）拖到远处应立刻见近似帧且不卡、停住后细化到精确。
