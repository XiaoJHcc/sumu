# Web 流媒体服务器 / 离线导出

sumu 除了本地实时预览，还能作为**去码转码后端**：把「处理好的视频」（BasicVSR 去码后的画面）
实时流式传输给局域网内任意设备（iPad 浏览器等），或离线导出成单个 MP4 文件。入口在首屏
「打开文件 / 打开 URL」之后的两个按钮——**Web 服务器** 与 **离线导出**。

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

## 运行时依赖

- **`ffmpeg.exe` 必须在 PATH 上**（编码/封装）。这是新增软依赖；`ffprobe.exe` 早已是既有软依赖
  （`get_video_meta_data`）。冻结分发时随包放一个带 `nvenc` 的 `ffmpeg.exe`（如 Gyan full build）。
- `sumu.webstream` 由 `packaging/sumu.spec` 的 `collect_submodules("sumu")` 一并收集，懒 import
  不影响冻结分析。

## 已知限制（v1）

- **单路转码**：AI 模型共享、BasicVSR 吃 GPU，同时只转一个视频；完成后的 HLS 落盘缓存复用，
  忙时请求其它视频返回 503。
- **无缩略图**（卡片用占位图）；目录仅列文件夹 + 视频。
- **桌面 Chrome/Firefox 不原生播 HLS**：目标设备是 iOS Safari（原生 HLS）；桌面需 Safari 或 hls.js。
- 4K 去码吞吐同播放器一样是 best-effort（BasicVSR 追不上 1x 时客户端在 live edge 缓冲）。
- 网络 URL 源：ffmpeg 会为音频单独拉一次源（v1 未做音轨经内存透传）。
- 服务器/导出与本地播放共享 GPU，并发时互相降速（torch 序列化 GPU 算子，不冲突）。

## 验证

- `scripts/verify_transcode.py`：编码 spike（native 硬解→NVENC→HLS/MP4，无 AI）。
- `scripts/verify_transcode_ai.py`：端到端（headless→去码→NVENC→MP4/HLS），实测 1080p 全马赛克
  片段 BasicVSR 净 ~125fps、总管线 ~23fps。
- `scripts/verify_stream_server.py`：假引擎路由/token/m3u8 注入测试，8 项全过。
