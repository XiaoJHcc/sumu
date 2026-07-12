# 原生 AI 输入桥 (`Player::get_cuda_nv12_by_frame`)

本文档描述在已验证的 `sumu_core.Player`（`native/src/player.cpp`，见 `docs/native_core.md`）
之上**新增**（非修改）的 AI 输入桥：present 面之外的第二个方向——把已解码、已在
`pt_ring_tex_` 中的 NV12 帧，以**非阻塞**方式喂给 Python/torch 侧的 AI 编排代码。设计直接照搬
Spike 3（`spikes/spike3_nv12_interop/`，见 `docs/spike3_nv12_interop.md`）已验证过的
R8/R8G8-SRV-override + identity-blit + CUDA driver API 技术，但源纹理与生命周期管理做了一处
真实简化（见下文"相对 Spike 3 的简化"）。**不改动**任何既有 present loop / decode loop / seek /
push_ai_frame 代码路径——纯增量。

## API 契约

```python
result = p.get_cuda_nv12_by_frame(frame_num)  # -> dict
# result["ready"]:       bool，False 时其余字段无意义，调用方必须自行重试/跳过
# result["dev_ptr"]:     uint64，CUDA 设备指针（CUdeviceptr），Player 持久拥有的单缓冲区
# result["width"]:       int，= 视频显示宽度（src_width_）
# result["height"]:      int，= 视频显示高度（src_height_），Y 平面高度
# result["pitch_bytes"]: int，= width（**紧密排列**，无行距 padding）
# result["frame_num"]:   回显传入的 frame_num
```

- **绝不阻塞 present**（I1/I2 硬约束）：cache miss（该 `frame_num` 当前不在 ring 里）立即返回
  `{ready: False, frame_num}`，不等待、不重试、不触碰 D3D11/CUDA。cache hit 才做 blit + CUDA
  拷贝。
- 返回的 NV12 缓冲区布局与 lada 的 `to_ndarray('nv12')`/spike3 一致：形状
  `(height*3//2, width)`，rows `[0,height)` = 全分辨率 luma，rows `[height, height*3/2)` =
  半分辨率交错 chroma。**原生只出 NV12 原始数据，颜色转换留在 Python 侧**
  （`python/sumu/ai/utils/video_utils.py::_nv12_to_bgr_hwc_gpu`）。
- **单缓冲区复用**：`dev_ptr` 指向的内存是 `Player` 持有的一块持久 CUDA 缓冲区，每次调用
  `get_cuda_nv12_by_frame()` 都会被覆写。调用方必须在下一次调用前消费完（跑完颜色转换）或自行
  `.clone()`；这与 `push_ai_frame` 期望调用方每帧提供新缓冲区的方向相反，但都是"单生产者/单消费者，
  不做内部双缓冲"的同一设计哲学。
- `pitch_bytes == width` 是原生桥的**契约**（不是运行时特例），Python 侧
  `wrap_nv12_cuda_buffer_as_tensor` 会在违反时直接 `raise ValueError`，视为
  native/Python 契约不一致的严重错误，而非静默兜底。

## 实现机制

1. **`create_ai_input_bridge()`**（`open()` 时**立即**调用，非惰性）：
   - 用 `D3DCompile` 编译一份内嵌 HLSL（`kAiInputBlitShaderSrc`，从 spike3 的
     `kShaderSrc` 原样搬入）：`VSMain`（全屏三角形）+ `PSMain_Y`（采样 `.r`）+
     `PSMain_UV`（采样 `.rg`）。
   - 创建两张**持久、非数组、大小=真实显示尺寸**的 plain render target：
     `ai_in_y_tex_`（`R8_UNORM`，`src_width_`×`src_height_`）、
     `ai_in_uv_tex_`（`R8G8_UNORM`，半分辨率）。
   - 用 `cuGraphicsD3D11RegisterResource` 注册两张纹理（`DXGI_FORMAT_NV12` 本身不可注册，
     这两个 R8/R8G8 格式是 CUDA-interop 允许的 override，与 spike3 完全一致）。
   - `cuMemAlloc` 一块 `ai_in_cu_buf_`，大小 = `src_width_ * src_height_ * 3 / 2` 字节
     （紧密排列，`ai_in_cu_buf_pitch_ = src_width_`）。
2. **`get_cuda_nv12_by_frame(frame_num)`**：
   - `UINT slot = wrap_slot(frame_num);`
   - **Phase 1**（只加 `ready_mutex_`，不碰 `d3d_mutex_`）：若 `pt_tag_[slot] != frame_num`，
     立即返回 `ready=False`——这是让 miss 路径真正做到"零 D3D11/CUDA 接触"的关键，miss 是热路径
     （AI 编排大概率经常问到还没解码到的帧）。
   - **Phase 2**（仅 Phase 1 命中时才执行）：加 `d3d_mutex_`，在锁内**重新检查** tag（关闭竞态，
     见下节），确认仍命中后：用 `pt_srv_y_[slot]`/`pt_srv_uv_[slot]`（**既有**、`create_ring_resources()`
     里已经建好的、per-slot 的 ring SRV）作为源，向 `ai_in_y_rtv_`/`ai_in_uv_rtv_` 做两次全屏三角形
     draw（Y 全分辨率 viewport，UV 半分辨率 viewport）→ `context_->Flush()` → `cuCtxSetCurrent`
     → `cuGraphicsMapResources` → `cuGraphicsSubResourceGetMappedArray`（Y、UV 各一次）→
     两次 `cuMemcpy2D`（`CU_MEMORYTYPE_ARRAY` → `CU_MEMORYTYPE_DEVICE`，Y 写入
     `ai_in_cu_buf_` 起始处，UV 写入 `ai_in_cu_buf_ + src_height_*pitch` 处）→
     `cuGraphicsUnmapResources` → 返回 `ready=True` + 缓冲区描述。全程无 `cuMemcpyDtoH`/`HtoD`，
     两端都是设备内存（I3）。
   - 异常路径与 `push_ai_frame` 同款：`catch` 后附加 `GetDeviceRemovedReason()` 诊断信息再
     re-throw，不吞异常。

## 相对 Spike 3 的一处真实简化

Spike 3 的 plane target 必须**惰性**创建（首帧解码后才知道），因为它直接对接解码器自己的
FFmpeg d3d11va 帧池纹理——那张纹理是**按宏块对齐的 coded size**（16 的倍数），不是流的
display size（spike3 踩过的坑：1080 的内容实际纹理是 1088 高，未处理时导致约 0.74% 的垂直
拉伸，MAE ≈ 8.5）。

但 `player.cpp` 里 AI 桥的源纹理不是解码器的原始纹理，而是 `pt_ring_tex_`——这张纹理**已经**被
`decode_loop()`/`seek()`（既有、未改动的代码）用 `CopySubresourceRegion` 裁剪到了
`src_width_`×`src_height_`（显示尺寸）。这意味着 AI 桥完全不用关心宏块 padding，天生绕开了
spike3 那整类 bug，也因此可以在 `open()` 时**立即**（EAGER）创建 plane target，不需要等第一帧
解码、不需要探测"真实纹理尺寸"。这是一处比 spike3 更简单、且不牺牲正确性的实现——已通过下文的
MAE 结果验证（数字与 spike3 的 0.6994/0.6215 高度吻合，证明裁剪逻辑与 spike3 直接读原始纹理
在数值上等价）。

## 锁交互与竞态论证

现有锁序（`docs/native_core.md`）：`decoder_mutex_` → `d3d_mutex_` → `ready_mutex_`（外→内）。
`get_cuda_nv12_by_frame` **只**加 `d3d_mutex_` 和 `ready_mutex_`，从不加 `decoder_mutex_`——
与 present 线程、`push_ai_frame` 的既有加锁范围完全一致，不引入新的锁序、不可能死锁。

竞态论证：`pt_ring_tex_` 唯一的两个写者（`decode_loop()`、`seek()`）都遵循"先加
`decoder_mutex_`，随后（在同一临界区内）加 `d3d_mutex_` 写 ring + 打 tag"的既有模式。
`get_cuda_nv12_by_frame` 的 Phase 2 在**持有 `d3d_mutex_` 期间**重新检查 tag 并完成
blit+拷贝——由于写者对 ring 的任何写入也必须先拿到同一把 `d3d_mutex_`，只要 Phase 2 拿到了锁，
其间 ring 内容不可能被并发覆写，Phase 2 检查到命中之后到拷贝完成之前，数据是稳定的。
Phase 1 的无锁快速失败不读写共享 D3D11/CUDA 状态（只读 `pt_tag_`，这本身受 `ready_mutex_`
保护），因此不存在"脏读"风险——最坏情况只是拿到一个即将变旧的 tag，导致 Phase 2 重新检查时
判定为 miss（安全，只是保守），不会导致越界拷贝或脏数据返回给 Python。

present 线程与 `get_cuda_nv12_by_frame` 会争抢同一把 `d3d_mutex_`，但这与 `push_ai_frame`
已经建立并接受的模式相同："AI/生产者线程偶尔为一小段（远小于一帧）时间等待 `d3d_mutex_`
是可接受的，present 线程本身永不因为 AI 桥而等待"——因为 present 线程只在自己的
`draw_and_present` 临界区内持锁，AI 桥的临界区（两次 draw + Flush + CUDA map/copy/unmap）
经验测得远小于一帧预算（见下文吞吐结果，全流程含颜色转换 4K 下均值仅 ~1.4ms），不会造成
present 线程的可观测停顿——回归结果（见下）也从实测角度印证了这一点：present 稳态数字与
基线相比没有劣化，反而 stddev/抖动更小。

## 回归结果（硬性要求，如实呈现）

**历史误报（已过时，勿再当待办）**：早期用 `smoke_player.py all`（单进程连跑多场景）时出现
`quit_early=True`，根因是当时文件级全局 `g_quit` 跨 `Player` 实例共享。**已修（M-C1）**：
`quit_` 现为 per-instance atomic，经 `GWLP_USERDATA` 路由。当时改用每场景独立进程得到干净
回归结果如下：

**smoothness**（`test_video_4k.mp4`，50s，`scripts/analyze_present.py --format ns --fps 59.9401`）：

| | median | stddev | max | p99 |
|---|---|---|---|---|
| COLD-START [0-6s] (n=359) | 16.68ms | 0.13ms | 18.0ms | 16.76ms |
| STEADY-STATE [10s-end] (n=2397) | 16.68ms | 0.06ms | 17.1ms | 16.94ms |

对照 4a 基线（`docs/native_core.md`）：COLD-START median=16.68ms stddev=0.08ms；
STEADY-STATE median=16.68ms stddev=0.48ms max=31.2ms p99=17.10ms。**本次实测持平或更好**
（steady-state stddev 0.06 < 0.48，max 17.1ms < 31.2ms，无 gaps>50ms）——单峰 ~16.68ms 节奏
完全保持，无劣化。

**seek stress**（`test_video_long.mp4`，4 轮 × 5 个 fraction = 20 次 seek，独立进程）：

```
[seek] SUMMARY crashed=False frozen=False quit_early=False n_seeks=20
```

20/20 全部 `actual == target`（帧号精确），latency 30.85ms ~ 111.98ms（4a 基线为
36~114ms，同一量级），present 心跳全程不断（`ticks_during` 每次 5~8，无 0 或负值即无冻屏）。
present 轨迹分析（`--format ns --fps 29.97`，177 个 present，覆盖整个 seek 过程）：
median=33.37ms stddev=0.11ms max=33.8ms（4a 基线 STEADY-STATE median=33.37ms
stddev=0.13ms）——同样持平。

**结论：无回归。** AI 输入桥的加入（含新增的持久 render target、CUDA 注册、
`get_cuda_nv12_by_frame` 本身）对 present/seek 路径的实测表现没有可观测的负面影响。

## 新 AI 输入路验证（正确性 + 吞吐，独立进程，真实测得）

方法：分别打开 `test_video.mp4`（1080p29.97）与 `test_video_4k.mp4`（4K59.94），用
`get_cuda_nv12_by_frame(current_frame())` 拉取正在播放的帧，经
`python/sumu/ai/utils/cuda_dlpack.py::wrap_nv12_cuda_buffer_as_tensor` 零拷贝包装为
CUDA-resident torch tensor，跑
`python/sumu/ai/utils/video_utils.py::_nv12_to_bgr_hwc_gpu`，与**独立**用 PyAV
`to_ndarray('bgr24')` CPU 解码的参考帧逐像素比较（MAE）。验证脚本是本次任务的一次性产物，
验证完成后已删除（不在交付范围内，`native/`、`python/sumu/ai/utils/`、
`docs/native_ai_input.md` 之外不新增文件）。

**零拷贝验证**：对所有采样帧，`tensor.data_ptr() == dev_ptr`（原生返回的原始指针）——真零拷贝，
非隐藏拷贝，两个分辨率均确认。

**MAE**（vs 独立 PyAV CPU bgr24 参考）：

| 分辨率 | n | mean MAE | max MAE |
|---|---|---|---|
| 1080p (`test_video.mp4`) | 30 | **0.6990** | 0.7012 |
| 4K (`test_video_4k.mp4`) | 13 | **0.6258** | 0.6326 |

与 spike3 的独立测得数字（1080p 0.6994、4K 0.6215）高度吻合，验证了"用已裁剪的
`pt_ring_tex_` 代替解码器原始纹理"这一简化在数值上等价、未引入额外误差。

**吞吐**（拉取 + `wrap` + `_nv12_to_bgr_hwc_gpu` + `torch.cuda.synchronize()`，3 秒窗口，
包含仅当 `ready=True` 时的完整链路耗时）：

| 分辨率 | n_hits | pull+convert fps | mean 单帧耗时 | max 单帧耗时 |
|---|---|---|---|---|
| 1080p | 4978 | **~1659 fps** | 0.469ms | 31.29ms（一次性冷启动尖峰，非稳态代表值） |
| 4K | 1711 | **~570 fps** | 1.602ms | 25.78ms（同上） |

两个分辨率下的吞吐均远超 60fps（16.7ms/帧）预算，与 spike3 的结论（这一层不是瓶颈）一致；
偶发的个位数次高耗时尖峰（~25-31ms）出现在测试窗口前几次调用附近，与 CUDA/D3D11 上下文/JIT
warm-up 一致，非稳态代表值（这与 spike3 记录的"cold-start 尖峰不计入稳态预算"的既定方法论
一致）。

**非阻塞验证**：`open()` 后立即用 `frame_count()-1`（远未解码到的帧号）调用
`get_cuda_nv12_by_frame`，两个分辨率下均在 **0.01ms 量级**内返回 `ready=False`
（1080p 实测 0.0100ms，4K 实测 0.0060ms）——确认 cache miss 路径是真正的"零 D3D11/CUDA
接触"快速失败，不会以任何可观测方式阻塞。

## 已知坑与未覆盖项（如实列出，不回避）

- **单缓冲区复用是调用方的责任**：`dev_ptr` 指向的内存每次调用都被覆写，Python 侧没有做
  自动 double-buffer 或自动 clone。若未来的 AI 编排代码需要跨多次调用保留某一帧（例如批量
  攒帧再推理），必须显式 `.clone()`，否则会读到后续调用覆写后的数据——这是设计上的取舍
  （避免原生层内部再管理一份池化缓冲区的复杂度），但确实是一处容易踩的坑，值得在
  `python/sumu/ai/` 后续消费这个 API 的代码里再次强调。
- **`get_cuda_nv12_by_frame` 与 ring 覆写窗口的关系没有做专门的压力测试**：本次验证用的采样
  策略是"查询当前正在播放的 `current_frame()`"，这类帧因为解码只会领先当前播放帧最多
  `kDecodeAheadMax`（54）帧，ring 容量 64，天然不会被覆写。但没有专门构造"AI 消费者故意查询
  一个已经被 ring 覆写掉的旧帧号"这种边界场景的自动化用例（手工验证过：查询一个已经过去
  很久的低帧号确实会返回 `ready=False`，行为符合预期，但没有写成回归用例保留下来）。
- **`cuda_dlpack.py` 的 DLPack 包装是纯 Python/ctypes 手搓实现**，不是走某个官方库的
  `dlpack` 绑定——本次已用真实 rebuild 后的 `.pyd` 端到端验证过零拷贝（见上），但如果未来
  升级 torch 版本，`DLManagedTensor` 的 ABI（legacy 非 versioned 形式）理论上有被上游弃用的
  风险，需要在升级 torch 时重新验证这一路径，而不是想当然地假设仍然兼容。
- **`first_decoded_frame_num_after_seek`（`python/sumu/ai/utils/video_utils.py`）不是
  lada 原函数的逐字照搬**——已在该函数的 docstring 里明确标注：lada 原版会在 PyAV 探测回退
  路径上构造一个 `VideoReader`，而 sumu 的生产解码不走 PyAV/pynvc，且 `Player.seek()`
  自身已经锚定真实解码 PTS 返回权威落地帧号，所以这里只保留了纯算术快路径，不构成对任务
  "只搬运纯函数"要求的违反，但这是一处对 lada 原始行为的**有意偏离**，明确记录在此。
- **`get_cuda_nv12_by_frame` 目前没有专门的多消费者/多线程调用测试**——目前的验证都是
  单线程 Python 顺序调用；虽然锁交互的论证（见上）对任意调用者数量都成立，但没有真的起
  多个 Python 线程同时高频调用它做压力测试。
- 本文档记录的 MAE/吞吐数字来自单次验证脚本运行，未做多次重复取统计分布（present/seek
  回归数字则是每个场景各自实测得到的单次结果，与 4a 基线的比较方法论一致）——如果后续要
  把这些数字当作长期基准维护，建议像 `docs/native_core.md` 的回归数字一样，考虑跑几次取
  中位数增加可信度。
