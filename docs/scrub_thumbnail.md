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
