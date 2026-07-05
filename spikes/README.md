# sumu spikes —— 验证协议

每个 spike 是一段**独立可跑的验证代码**，只回答 DESIGN.md 第二部分里的一个问题。产出**实测数据**，不是"看起来能行"。结果写进 `docs/spike_results.md`。

## 通用测量契约（所有 spike 必须遵守）

**目标机器**：RTX 4080 · 4K@150Hz scale=1 · Win11。素材：`../test_video_4k.mp4`（3840×2160 HEVC 60fps）为主，`../test_video.mp4`（1080p30）对照，`../test_video_long.mp4` 用于 seek。

**present 埋点格式**：每次 present 后立刻记一条，写 `trace/present_<spike>_<run>.csv`，两列：
```
qpc_ns
<每次 present 完成时刻的纳秒时间戳>
```
时间戳用 `QueryPerformanceCounter` 换算 ns（`ticks * 1e9 / QueryPerformanceFrequency`）。**不要**在 present 关键路径做 IO——先攒进内存数组，退出时一次性落盘。

**分析**：统一用
```
python ../scripts/analyze_present.py trace/present_<spike>_<run>.csv --format ns --fps 60
```
输出冷启动 [0-6s] vs 稳态 [10s-end] 的 median/stddev/p50/p90/p95/p99/max、%>1.5x 预算、%>2x 预算、gaps>50ms、直方图。

**每个 present 变体测两轮**：**窗口化**一轮 + **最大化**一轮（lada 的残留墙正是最大化退化，必须分别量）。跑够 ≥30s 稳态。

## 通过判据（对齐 mpv 基线，不追机器本就达不到的目标）

先有 mpv/PresentMon 基线（`docs/spike_results.md` 顶部），再判：
- 稳态 median 贴 16.67ms（60fps 预算）、**单峰**（无双峰）；
- %>2x 预算与 mpv 基线同量级（个位数百分比内）；
- **最大化不显著退化**（这是 sumu 相对 lada 要证明的关键改善）。

## spike 清单

| spike | 问题 | 通过判据 | 依赖 |
|---|---|---|---|
| **spike0** | 纯 D3D11 硬解 4K60 HEVC + 自持 present loop 能否上屏稳定（不接 AI） | 见上；窗口化+最大化都对齐 mpv | 仅 D3D11 + 硬解（MF 或 FFmpeg-d3d11va） |
| **spike1** | torch CUDA 张量能否**不经主存**直接被 D3D11 present 面显示 | 互操作可用、无每帧主存拷贝、单帧开销 ≪16.6ms | spike0 通过 + CUDA Toolkit 12.8 + torch |
| **spike2** | present loop 每 vblank 在 ready-map 去码帧与原片环缓冲间**按帧号**挑帧，混流无顿挫 | AI 帧随机就绪/缺失时上屏分布与纯原片播放无可辨差异 | spike0/1 通过 |

任一门不过 → 记录原因、就地调整或换路，**在写任何 AI 集成代码之前**。
