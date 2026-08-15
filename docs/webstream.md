# Web 流媒体服务器 / 离线导出

sumu 除了本地实时预览，还能作为**去码转码后端**：把「处理好的视频」（BasicVSR 去码后的画面）
实时流式传输给局域网内任意设备（iPad 浏览器等），或离线导出成单个 MP4 文件。入口在首屏
「打开文件 / 打开 URL」之后的两个按钮——**Web 服务器** 与 **离线导出**。

Web 服务器有**两种转码模式**，由 `settings.stream_passthrough` 切换（默认 `False`，即 AI 去码）：

- **AI 去码（默认）**：headless 解码 → YOLO + BasicVSR 去码 → NVENC，即本文档原有链路，
  现已对齐直出的三项能力（色彩、seek、停转），见「AI 去码」一节。
- **原片直出（passthrough）**：不经过 AI，纯 ffmpeg `-ss` + NVENC 直出，作兜底/对照。

## 心智模型与边界

- **直播 + 允许缓冲**：点开一个视频，sumu 从第 0 帧起按 GPU 实际速度去码编码成 HLS，客户端
  从头播放、跟随生产边沿，追不上就正常缓冲。**不做帧级回退**（编码器只吃完整去码帧；播放器
  里为守 vblank 才需要的「回退原片」I9 在这里不存在——转码链路没有 vblank 死线）。
- **仅 Nvidia（NVENC）**：编码固定走 `ffmpeg -c:v h264_nvenc`，与项目既有的「仅 Nvidia 目标机」
  假设一致。
- **I3 的有意放宽**：AI 计算（解码→YOLO→BasicVSR→blend）仍全程 GPU；仅在喂编码器前做**一次**
  D2H（`final_bgr.cpu()`）。这条独立转码消费链路与 present 热路径无关，文档化放宽，不回改 I3。

## 架构

```
python/sumu/webstream/
  transcode.py   TranscodeEngine：headless 解码 → DecensorProcessor → NVENC → HLS/MP4
  decensor.py    DecensorProcessor：顺序版 AI 去码（照搬 scheduler 的 detect/clip/restore/blend）
  encoder.py     NvencEncoder：BGR rawvideo 管道 → ffmpeg h264_nvenc → HLS(event)/MP4(faststart)
  server.py      StreamingServer：stdlib ThreadingHTTPServer + StreamManager（目录索引/播放页/分片/token）
  export.py      ExportJob：MP4 导出 + 进度/取消（薄封装）
  index_page.py  目录/播放页 HTML（移植自 simple-http-video-server，视觉同步）

native/src/headless_decode.{h,cpp}  HeadlessDecode：无窗口顺序 d3d11va 硬解 → CUDA NV12
                                    （复用 decoder.cpp + player.cpp 的 AI-input-bridge blit）
```

- **headless 解码**是唯一实质原生新增：`Decoder`（d3d11va 硬解）+ AI 输入桥（NV12 纹理→CUDA
  零拷贝 blit），无 swapchain/present/音频。音频不经过它——编码器（ffmpeg）直接读源文件的音轨。
- **AI 计算零改动**：`scene_clip` / `blend` / `video_utils` / `cuda_dlpack` 全部复用；`DecensorProcessor`
  只是把 scheduler 里与 present head 耦合的处理逻辑改成顺序版。

## 原片直出（passthrough，默认）

`python/sumu/webstream/passthrough.py` 的 `PassthroughSession`：纯 ffmpeg
`-ss <t> -i <src> -c:v h264_nvenc -f hls`，按需启动、可重定位、可停。三个 blocker 的修法：

- **色彩正确**：ffmpeg 解码得到 YUV 后 NVENC 直接再编码 YUV，**没有 NV12→BGR（torch BT.709）
  →YUV（swscale BT.601）的往返**，颜色与源逐比特一致（实测 1080p 全像素 MAE ≈ 0.63，属有损
  h264 正常量级，非矩阵错误）。AI 路径的色偏正来自那次双重转换，与直出无关。
- **可 seek**：seek = 杀掉当前 ffmpeg、用新 `-ss` 重启（keyframe 粒度 ~2s）。NVENC ~4-6x 实时，
  重定位后 ~1-2s 起播。复用 DESIGN.md I6「reposition，不 teardown」的思路。
- **停转**：暂停/关闭页面由前端显式 `POST /stop`；另有服务器空闲清扫线程（20s 无拉流即停），
  兜底客户端崩溃/断网。

### 路由（passthrough）

| 路由 | 作用 |
|---|---|
| `/stream/<rel>/index.m3u8?start=<秒>` | 拉（增长的）HLS 播放列表；首次按需启动 ffmpeg；`?start=` 触发重定位（seek 的实际载体，前端拖进度条即改这个 URL，同值重取不重启）|
| `/stream/<rel>/<sN.NNNNN.ts>` | 拉分片（`no-store`，避免 seek 后旧分片被浏览器缓存复用）|
| `/stream/<rel>/meta` | `{duration, position, fps, running}`，前端 seekbar 总长/起点 |
| `/stream/<rel>/seek?t=<秒>` | 重定位转码到 t，返回 `{position, duration}`（兼容 JSON API，前端不再使用）|
| `/stream/<rel>/stop`（GET/POST）| 停 ffmpeg（POST 供 `navigator.sendBeacon`）|
| `/static/hls.min.js` | 内置 hls.js（Apache-2.0），供桌面浏览器播放 HLS |

前端 `index_page.render_player` 是 **hls.js（内置，`/static/hls.min.js`）+ 自定义 seekbar** 的播放器：
桌面 Chrome/Edge/Firefox 走 hls.js，iOS Safari 走原生 HLS。进度条在页面加载即显示**全长**（时长来自
`/meta`）；拖进度条 = 跳转：目标在**已完成 VOD**（hls.js `level.details.live === false`，即
`#EXT-X-ENDLIST` 已出现）的已缓冲范围内则本地 seek（瞬时）；对**增长中的 live 流**一律用
`?start=<秒>` 触发服务端重定位（杀掉当前 ffmpeg、从新位置重启，NVENC ~4-6x 实时，~1-2s 起播）——
live 流的 hls.js 本地 seek 不可靠，故不采用。绝对播放位置 =
`basePos`（服务端 start/seek 位置）+ `video.currentTime`（本流内 0 基偏移）；服务端在每个 playlist
注入 `#EXT-X-START:TIME-OFFSET=0`，让 hls.js 从流的 0 处起播而非 live edge，保证该映射正确。`?start=`
还带 `_=<时间戳>` 作为 seek 代数，服务端据此**拒绝乱序的陈旧请求**（旧播放器销毁重载时仍在途的请求
不会把转码倒退回旧位置）。暂停 → `/stop`，`pagehide` → `sendBeacon('/stop')`。每个 (re)start 写入独立
位置目录 `p<ms>/`，分片用唯一 `s<nonce>.%05d.ts` 命名，seek 不会与上一位置的浏览器缓存 URL 冲突；
完成的 HLS 落盘缓存复用（同 AI 路径），转码到 EOF 后（`ENDLIST`）再取 playlist 不再重启进程。

### passthrough 已知边界

- 每视频一个 ffmpeg 进程，按需启动；不同视频可并发（NVENC 便宜，无共享 AI 模型，不做 503）。
- `-ss` 在 `-i` 前 = keyframe 快 seek（O(1)），精度 ~2s；深 seek 长片也秒回。
- 桌面浏览器 HLS 由**内置 hls.js**（`python/sumu/webstream/hls.min.js`，Apache-2.0 vendored，经
  `/static/hls.min.js` 提供）播放，无需 CDN/联网；Safari 走原生 HLS。
- 空闲清扫停转后再次进入，会从该 session 最近一次 `start/seek` 位置重启（非断点续传）。
- 启动不依赖 AI 预热：passthrough 模式 `StreamingServer(engine=None)`，`stream_start` 直接起服，
  点击「启动服务器」即生效。

## AI 去码（默认）

`python/sumu/webstream/ai_session.py` 的 `AiStreamSession` 是直出的 AI 对应物：同一套公开接口
（`start/seek/apply_seek/stop/touch/idle_seconds/running/finished/status/out_dir/m3u8_path`），
所以服务器路由对两种模式走同一条代码路径（`server.py` 不再按 `passthrough` 分叉）。三处 v1
blocker 的对齐方式（与直出同思路，映射到转码进程上）：

- **色彩**：`encoder.py` 不再喂 `bgr24`（ffmpeg 会用 BT.601 转回 YUV）。现在每帧 BGR 在喂给
  ffmpeg 前按**与解码侧相同的 BT.709 矩阵/range** 转回 YUV420p 再入管（`-pix_fmt yuv420p`），
  并给输出打 `-color_primaries/trc/colorspace/range` 标签。转换在 **GPU** 上做
  （`_bgr_to_yuv420_gpu`，与 `_bgr_to_yuv420` numpy 版逐位同矩阵、仅 ±1 取整），ffmpeg/NVENC
  不再做 RGB↔YUV 矩阵转换（yuv420p→nv12 只是色度重排），消除了 NV12→BGR(BT.709)→YUV(BT.601)
  的往返色偏，也顺带把每帧 ~20-60ms 的 numpy 浮点转换压到 ~1ms 的 CUDA kernel。
- **seek**：seek = 取消当前转码、用 `HeadlessDecode.seek_to_frame`（原生 `Decoder::seek_to_frame`，
  关键帧级、~1-2s 重定位）在新位置重启，并新建一个 `DecensorProcessor`（scene/clip 状态重置，
  即 DESIGN.md I6「reposition，不 teardown」）。音频侧 ffmpeg 用 `-ss` 对齐到同一偏移。
  每个 (re)start 写入独立位置目录（`p<ms>_<run_id>_<nonce>/`），分片走 `no-store`，seek 不与
  上一位置的浏览器缓存冲突。
- **停转**：`stop()` 取消共享引擎并 join worker；服务器空闲清扫线程对**两种模式**都生效。
  AI 会话用更短的 `AI_IDLE_TIMEOUT=30s`（直出仍 `IDLE_TIMEOUT=120s`），因为 BasicVSR 烧 GPU，
  浏览器被硬关（`/stop` beacon 发不出）后必须在 ~30s 内释放 GPU，而不是等 2 分钟。暂停同理：
  `/stop` 后 m3u8 路由只在**新的 playFrom（更新的 `_` 缓存代数）**到来时才重启转码，客户端
  hls.js 的 live-sync 轮询复用同一 `_`，不会把暂停的转码又拉起来（对齐 PC 端的前向缓冲闸门：
  暂停即停算，不再无限往下转码）。

### AI 模式已知边界

- **单路转码**：AI 模型共享、BasicVSR 吃 GPU，同时只转一个视频；忙时请求其它视频返回 503。
  完成后的 HLS 落盘缓存复用。
- 启动依赖 AI 预热：`stream_start` 在模型未预热完成前会提示稍后（app.py 的 warmup 闸门）。
- seek 到新位置后，BasicVSR 需重新冷启动（首个 clip 需 `clip_length` 帧前瞻 + 去码 + 2s 分片，
  起播 ~3-5s，比直出的 ~1-2s 略慢）。
- 网络 URL 源：音频由 ffmpeg 单独拉一次源（未做音轨经内存透传），seek 对网络源为 best-effort。

## 运行时依赖

- **`ffmpeg.exe` 必须在 PATH 上**（编码/封装）。这是新增软依赖；`ffprobe.exe` 早已是既有软依赖
  （`get_video_meta_data`）。冻结分发时随包放一个带 `nvenc` 的 `ffmpeg.exe`（如 Gyan full build）。
- `sumu.webstream` 由 `packaging/sumu.spec` 的 `collect_submodules("sumu")` 一并收集，懒 import
  不影响冻结分析。

## 通用限制

- 目录仅列文件夹 + 视频（卡片缩略图按需 ffmpeg 生成）。
- **桌面 Chrome/Firefox 不原生播 HLS**：由内置 hls.js（`/static/hls.min.js`）补齐，无需 CDN；
  iOS Safari 原生 HLS。
- 4K 去码吞吐同播放器一样是 best-effort（BasicVSR 追不上 1x 时客户端在 live edge 缓冲）。
- 网络 URL 源：ffmpeg 会为音频单独拉一次源（未做音轨经内存透传）。
- 服务器/导出与本地播放共享 GPU，并发时互相降速（torch 序列化 GPU 算子，不冲突）。

## 验证

- `scripts/verify_transcode.py`：编码 spike（native 硬解→NVENC→HLS/MP4，无 AI）。
- `scripts/verify_transcode_ai.py`：端到端（headless→去码→NVENC→MP4/HLS，含 `start_seconds`
  seek 与 BT.709 色彩路径），实测 1080p 全马赛克片段 BasicVSR 净 ~125fps。编码侧 BGR→YUV 走 GPU
  （`_bgr_to_yuv420_gpu` 单帧 ~1-2ms，vs numpy 版 ~20-60ms），总管线不再被 CPU 色彩转换卡在
  ~23fps，恢复为 GPU 绑定（BasicVSR/YOLO）。
- `scripts/verify_stream_server.py`：假引擎路由/token/m3u8 注入/hls.js 静态路由（AI 路径）。
- `scripts/verify_stream_passthrough.py`：原片直出端到端（假引擎 + 真 ffmpeg）——索引/hls.js +
  自定义 seekbar 播放页/`/static/hls.min.js`/meta/HLS（唯一分片名 + token 注入 +
  `EXT-X-START`）/真实 h264 分片/重定位 seek/陈旧请求 `_` 代数守卫/m3u8 URL `&` 分隔符回归守卫
  /stop，14 项全过。
