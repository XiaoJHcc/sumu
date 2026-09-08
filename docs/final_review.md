# 收尾全量审查与修复计划（final review）

> 审查时间：0.3.4 之后、项目收尾前。审查方式：五路只读并行审查（原生资源/内存、原生线程与时序、Python 侧缓存与生命周期、格式支持与已知缺口、性能热点）。本文档是发现的唯一台账，后续按「修复阶段」逐节执行，每阶段一个 git commit，完成后在对应条目打勾并补记验证结果。

## 总体结论

正常生命周期（open/播放/seek/reopen/close/park）的资源管理严谨：COM 全走 `ComPtr`、CUDA 注册成对、线程 join 顺序正确、锁序全仓库一致（decoder → d3d → ready，无循环）、主要缓存有界。**问题集中在失败/重建路径**：稳态质量好，异常路径清理链有确凿的崩溃级缺陷。

---

## 发现清单

### 高严重度（崩溃/泄漏链）

- **H1 `open_session()` 启动缓冲超时后：孤儿线程 + 下次 open 必崩**
  `native/src/player_session.cpp:183,195,215` —— decode/scrub 线程在起始缓冲等待之前启动；超时 throw 时不置 `session_stop_`、不 join、不清 CUDA 注册。后果链：线程泄漏；旧 decode 线程持 `fmt_ctx_` 时 `decoder_.close()` 释放它 → use-after-free；`std::thread` 对 joinable 赋值 → `std::terminate`。**任何少于 5 帧的视频永远无法打开**（缓冲高水位到不了阈值 4），先卡 10s 抛异常，下一次打开任何文件即崩——确定可复现。
- **H2 reopen 拆除缩略图 SRV 后，present 用悬空指针渲染 splash**
  `native/src/player_session.cpp:353-368` + `player_engine.cpp:365-387` + `player_ui_bars.cpp:377` —— 缩略图 SRV 被烘进 UI 快照 draw list；`close_session` 释放 SRV 后主线程阻塞在 `open_session()`（网络源最长 ~30s），期间无新快照，present 每 tick 渲染含已释放 SRV 的旧快照 → UB/AV。触发：悬停进度条显示缩略图时拖入打开慢的新文件。
- **H3 CUDA Map/Unmap 异常路径漏 Unmap（三处同型）**
  `player_engine.cpp:190-205, 351-382`、`headless_decode.cpp:196-224` —— Map 后 cuMemcpy2D throw → catch 只 rethrow 不 Unmap，资源留 mapped 状态，后续 Register/Unregister 行为未定义。
- **H4 `Scheduler._run` 无异常防护 → AI 线程静默死亡**
  `python/sumu/scheduler.py:375-442` —— producer 主循环无 try/except，CUDA OOM 或会话半关时抛异常 → daemon 线程死掉，AI 永久停摆无看门狗，UI 只表现为命中率归零。
- **H5 `Scheduler.stop()` 的 2s join 超时 → 旧线程向新会话推过期帧**
  `python/sumu/scheduler.py:266-270` —— 4K/长 clip restore 超 2s 时旧线程幸存并跑完 `push_ai_frame`；此时已 reopen 新文件（帧号重排），旧时间线小帧号落进新视频 ready-map → 呈现错误画面。

### 中严重度（极端情况健壮性）

- **M1 `close()` 不先翻 `session_active_`**：`player_session.cpp:470-492` —— 与 H5 叠加时 AI 线程可索引空 vector（UB）。修复是一行。
- **M2 `seek` 绑定不释放 GIL**：`player_pybind.cpp:63` —— 网络源下等 `decoder_mutex_`（decode 线程持锁跨 15s 网络读）→ 主线程冻结、窗口"未响应"。open/reopen 都有 `gil_scoped_release`，seek 漏了。
- **M3 D3D11 device removed / TDR 无恢复路径**：`Present(1,0)` 返回值不检查（`player_engine.cpp:1309,1331`）；resize 时 `check_hr` 异常穿越 WndProc 的 C 栈帧 → 进程终止（`player_session.cpp:598-609`）。
- **M4 时长未知的裸流（`*.hevc`/`*.h265`）`frame_count_=0`**：EOF clamp 失效，present 时钟无限越界、进度条跑飞（`player_engine.cpp:1156`）。文件选择器自己提供这类扩展名（`player_session.cpp:531-533`）。
- **M5 VRAM 满时无降级**：`pick_ring_capacities` 只按分辨率静态分档（`player.h:301-324`），分配失败即 open 失败——I8「按显存上限动态压 lookahead」在 native 侧不存在，文档宣称与实现不符。
- **M6 解码中途分辨率/像素格式变化**：`CopySubresourceRegion` 校验失败被静默跳过 → 画面永久定格旧帧无上报（直播流场景）。
- **M7 退出路径竞争**：async 网络 open 在途时关窗，`player.close()` 与 worker 内 `player.open()` 并发（`app.py:832-836, 1238`）；导出进行中退出时 `cancel_all` 不 join worker。

### 低严重度（泄漏/堆积/杂项）

- L1 HeadlessDecode 每实例泄漏一次 CUDA primary ctx retain（无配对 Release）：`headless_decode.cpp:116,276-288`，每次导出 +1。
- L2 present trace 三 vector 全生命周期只增不清：`player_engine.cpp:1210-1214`，~5-10MB/h。
- L3 `settings.positions` 永不淘汰：`settings.py:114,169-170`。
- L4 命名管道 payload 无上限且 `in` 查找 O(n²)：`single_instance.py:147-155`。
- L5 `ExportQueue.remove/cancel` 未判 `engine is None`（`cancel_all` 判了）：`webstream/export.py:104-106,115-117`。
- L6 音频设备拔出即永久静音不恢复：`player_engine.cpp:1569`。
- L7 EOF 后按播放键是 no-op（不会回 0 重播，UX）：`player_engine.cpp:1156-1160`。
- L8 `activate_trt` 覆盖 `_split_forward` 不 close 旧 split；`model_cache.py` 的 `ModelCache` 是死代码。
- L9 VFR 视频帧号 round 映射产生尾部重复帧（已知设计假设，不崩）。
- L10 音频队列 512 包上界静默丢最旧包，恢复时可能一次性跳音。
- （已确认无问题：ImGui 快照配对、FFmpeg av_*_free 齐全、torch 全程 inference_mode、DLPack holder 生命周期、ffmpeg 子进程回收、settings 原子写、环缓冲容量固定、reopen 旧尺寸纹理释放。）

### 性能热点（按收益排序，I10：先埋点再优化）

- **P1 scheduler 每 clip 两次 `torch.cuda.synchronize()`**：`scheduler.py:481-487` —— 纯为计时埋点的全设备排空，打断 GPU 流水；webstream 侧同款已删（decensor.py 注释写明危害），播放器侧残留。改 CUDA event 计时。
- **P2 AI 输入/输出桥持 `d3d_mutex_` 跨过同步 cuMemcpy2D**：`player_engine.cpp:180-214, 301-382` —— AI 线程持锁做 1-3ms host 阻塞拷贝期间 present 堵在锁外，违背 I1 精神。改 `cuMemcpy2DAsync` + event。需先用 analyze_present.py 量测。
- **P3 frame_cache 全分辨率 BGR 驻留、容量与分辨率脱钩**：`scheduler.py:452-453,580-603` —— 4K 下 ~226 帧 ≈ 5.6GB VRAM，Python 侧最大消费者。按字节预算反推帧数上限（符合 I8）。
- P4 转码喂编码器 `yuv.cpu().numpy().tobytes()` 多一次整帧 CPU 拷贝（`encoder.py:318`）；decode→AI→write 全串行，pipe 阻塞时 GPU 闲置。
- P5 `_nv12_to_bgr_hwc_gpu` 每帧 6+ 个全分辨率中间张量（`video_utils.py:80-95`）。
- P6 present 持 `d3d_mutex_` 跨过 `Present(1,0)`（`player_engine.cpp:1198-1209`）——先埋点确认阻塞占比再动。
- P7 杂项：decode 线程 Sleep 轮询可换 condition_variable；UI `elide_text_to_width` O(n²)；blit shader 在 `headless_decode.cpp:68` 与 `player_engine.cpp:14` 内联复制两份（维护风险）。

### 格式支持矩阵与缺口

- **Codec**：无白名单，理论可达 = FFmpeg d3d11va 全集（H.264/HEVC/VP9/AV1/MPEG-2/VC-1），实测只有 H.264/HEVC 两种素材。
- **像素格式（最大静默风险面）**：全链路硬编码 NV12 + BT.709 limited（`present.hlsl:40-57`、`scheduler.py:123-127`），不读流色彩 tag：
  - 10-bit HEVC（P010）：解出 P010 后 `CopySubresourceRegion` 拷进 NV12 纹理，不做转换、返回 void、无检查 → **静默黑屏/定格**。
  - BT.601 标清 / full-range：静默色偏。spike3 文档建议从 `AVCOL_SPC_*` 派生（`docs/spike3_nv12_interop.md:195-199`），未实现。
  - HDR：色偏 + P010 叠加，无 tone mapping。
- **无软解 fallback**：d3d11va 不支持的 codec 直接 open 失败（`docs/porting_manifest.md:57` 列为未来方向）。
- **协议**：仅 http/https 有网络 profile；rtsp 等未测无超时保护；直播流无支持声明。
- **旋转元数据未处理**：无 `rotation`/`displaymatrix` 处理，手机竖拍视频横着渲染。
- 播放端无字幕、无音轨选择（恒 best stream）。音频软解 + swr 重采样到 mix format，这块是好的。
- **i18n 漏网**：`player_ui_export.cpp:763`（`u8"高质量"/u8"小体积"`）、`:786`（`"p7（最高）"`）硬编码中文不走 i18n。

---

## 修复阶段

> 每阶段 = 一个 commit（`fix(...)`/`perf(...)` 风格）。native 改动须 `cmd /c "native\build.bat"` 编译通过 + `native\smoke_player.py` 相关项验证；Python 改动跑对应 verify/run_player 脚本。若 sandbox 阻止构建，如实记录由用户本机验证。

### S1 — H1：open_session 异常清理 + 短视频合法打开 ✅
- 线程启动后的所有 throw 点包 try/catch：置 `session_stop_` + join + 清理 CUDA 注册再 rethrow。
- 起始缓冲等待区分 EOF：流自然结束（帧数 < 阈值）应合法打开而不是超时。
- 验证（2026-09-08，RTX 4080）：3 帧视频 0.05s 打开（原 10s 超时 throw）、播至末帧自动暂停；
  截断 mp4（线程启动后 EOF 零帧）throw 后再 open 正常视频不崩（原 use-after-free/terminate 路径）；
  假文件失败后 open 正常；`smoke_player.py all` 与 `stress_reopen.py --rounds 12` 全绿。

> **插入修复（无新阶段编号）**：多 GPU 机器（核显+独显）`D3D11CreateDevice(默认适配器)` 可能选中
> 核显 → `cuD3D11GetDevice` 报 NO_DEVICE、Player 无法构造（驱动更新后实测复现，dist 旧包同样中招）。
> 已修：新增 `native/src/d3d_util.h`，两处 device 创建（Player 窗口路径 + HeadlessDecode）优先
> `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`，失败回退默认行为。commit `a5c16eb`。
> 验证：无注册表改动裸跑 open+present 正常（日志确认选中 RTX 4080）、HeadlessDecode 首帧正常、
> `smoke_player.py pause` 通过。

### S2 — H2：缩略图 SRV 悬空 ✅
- `close_session` 拆 scrub 资源前清空/替换 `ui_pending_`/`ui_active_`（或 SRV 生命周期延长到下一次快照发布）。
- 验证（2026-09-08，RTX 4080）：hover 进度条（缩略图烘入快照）→ reopen 1080p↔4K ×20 轮 +
  hover 中 `close_current_session` + 再 open，present 不 stall、不崩；`smoke_player.py seek` 回归全绿。

### S3 — H4+H5：scheduler 健壮性 ✅
- `_run` 拆为外壳 + `_run_iteration`：每轮包 try/except。stop/会话失效导致的异常安静退出；
  意外异常（如 CUDA OOM）记 `logger.exception` → `_resync_after_error`（清 scenes/frame_cache/
  pending_regions、frontier 按 backlog-resync 思路拉到 head、仅在恢复路径调一次
  `torch.cuda.empty_cache()`）→ 线性退避（0.1s×n，封顶 1s）后继续循环，线程不死。
- 会话生成号（`_generation`，`_seek_lock` 保护）：`start()` 自增并 `stop_event.clear()`；
  producer 捕获启动时 gen，`_session_ok()` 在循环顶部、`_restore_and_push` restore 完成后、
  `_flush_pending_to_native` 入口及每次 `push_ai_frame` 前校验——stop() join 超时后孤儿线程
  跑完当前迭代也绝不会把旧时间线帧推进新会话（同实例重启与新实例同 player 两条路径都覆盖）。
- 验证（2026-09-08，RTX 4080）：临时脚本（已删）12 项断言全过——孤儿线程在 stop 超时 +
  reopen 后 push_ai_frame 零落入（同实例/新实例两路径）、注入 `RuntimeError("CUDA out of
  memory")` ×3 后线程存活、backlog_resyncs=3、恢复后继续推进、stop 时干净退出；
  `run_player.py --seconds 30 --seek-test`：seek 1788/1788 精确、恢复窗 ai_hit_rate=0.950、
  present median=33.36ms/p99=33.82ms、frame_cache_misses=0、seek_resets=1，与
  docs/scheduler.md 基线无回归。

### S4 — H3+M1+M2+L1：CUDA RAII 与杂项原生修复 ✅
- Map/Unmap 上 RAII guard（三处）；`close()` 开头先 `session_active_.store(false)`；seek 加 `gil_scoped_release`；HeadlessDecode 配对 `cuDevicePrimaryCtxRelease`。
- 验证（2026-09-08，RTX 4080）：`run_player.py --seconds 15` AI 会话正常（ai_push 567，覆盖
  guard 正常路径）；HeadlessDecode 50× new/open/decode/close 无泄漏报错，同实例 open→close→open
  （re-retain 分支）与 double close 幂等通过；`smoke_player.py pause` + `seek --rounds 2` 全绿；
  seek GIL 修复为静态确认（与 open/reopen 同款 call_guard）。

### S5 — P1+P3：scheduler 性能与 VRAM ✅/⬜
- `torch.cuda.synchronize()` ×2 改 CUDA event 计时；frame_cache 按字节预算（分辨率感知）反推帧数上限。
- 验证：`scripts\run_player.py --seconds 60` 对比 restore_fps / ai_hit_rate。

### S6 — P2：AI 桥异步拷贝（需先量测）✅/⬜
- 先 `scripts\analyze_present.py` 量 AI 并发时 present p99 毛刺；改 `cuMemcpy2DAsync` + event，锁内只留 enqueue，tag 翻转时机重新论证。
- 若量测显示毛刺不显著则记录结论、跳过实施。

### S7 — 像素格式防线：非 NV12 显式检测 ✅/⬜
- decoder 打开后检查 `sw_pix_fmt`/色彩 tag：非 NV12（P010、4:2:2、HDR 等）给出明确错误提示（i18n），不再静默黑屏/色偏。
- 完整 P010/HDR 支持、软解 fallback、旋转元数据列为后续方向，记入本文档「后续方向」。

### S8 — 低危杂项打包 ✅/⬜
- i18n 两处硬编码中文改走 `set_ui_strings`；`settings.positions` 加 LRU cap；named pipe payload 加 64KB 上限；`ExportQueue.remove/cancel` 判 None；present trace 换定长 ring。

### S9 — 收尾回归与文档 ✅/⬜
- `native\smoke_player.py all`、`scripts\run_player.py --seconds 60 --seek-test`、相关 verify 脚本全跑；更新本文档勾选与验证记录；AGENTS.md docs 索引补本文件。

## 后续方向（本次不做，记录在案）

- P010/10-bit、HDR tone mapping、BT.601/full-range 从流 tag 派生色彩参数（spike3 已给方案）。
- 软解 fallback（porting_manifest 已列为未来方向）；解码中途分辨率/格式切换处理。
- 旋转元数据（手机竖拍）；播放端字幕与音轨选择；VFR 正式支持。
- D3D11 device-removed 恢复路径（M3）；网络源 AVIOInterruptCB（M7 退出等待 15s）。
- P4/P5/P6 性能项（转码管线重叠、NV12→BGR 融合、Present 出锁）——按量测结果单独立项。
- L6 音频设备热插拔恢复；L7 EOF 后播放键回 0 重播（UX 决策）。
