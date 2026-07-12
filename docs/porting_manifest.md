# AI 计算核心移植清单（lada-realtime → sumu）

> 本文档为架构 agent 直接分派移植任务用的测绘清单。所有条目均基于对 `d:/Git/lada-realtime`（只读，未做任何修改）的实地代码阅读，行号对应当前 lada-realtime 工作树状态（2026-07-06）。

## 概述：AI 核心数据流

一条视频先被 `MosaicDetector` 解码一路帧喂给 YOLO11 分割模型逐帧检测马赛克区域，检测结果按帧号聚合进 `Scene`（连续同一区域的检测序列），场景结束时物化成裁剪+缩放+pad 好的 `Clip`；`Clip` 队列被 `FrameRestorer` 用 BasicVSR++（可选 TensorRT 6 子引擎加速）做时序修复，修复结果与 `FrameRestorer` 自己独立解码的第二路原始帧做逐帧 blend-back（按检测 mask 把修复区域贴回原图），最终输出去马赛克后的帧；这条链路里张量约定统一为 `(H,W,C)` BGR `uint8`，可在 CPU 或 CUDA 设备上（device-generic），是 sumu ready-map 里"帧号 → 已去码 GPU 张量"这一格子要填的东西。

---

## 一、照搬（Port Verbatim）

### 1. YOLO 检测调用

- **文件**：`lada/models/yolo/yolo11_segmentation_model.py`（全文件 104 行）
- **类/函数**：`Yolo11SegmentationModel`（整个类）—— `__init__`、`_preprocess_cpu`、`_preprocess_gpu`（配 `lada/utils/torch_letterbox.py` 的 `PyTorchLetterBox`，37 行，小巧独立）、`preprocess`、`inference`、`inference_and_postprocess`、`postprocess`、`construct_result`
- **依赖**：`ultralytics`（重依赖，`YOLO`/`AutoBackend`/`Results`）、`torch`、`torchvision.transforms.v2`（letterbox）
- **device 通用性**：**已验证真实存在**（非文档臆断）—— `preprocess()` 内部按 `imgs[0].device.type` 分派到 `_preprocess_cpu`（numpy+ultralytics 原生 `LetterBox`）或 `_preprocess_gpu`（torch 原生 `PyTorchLetterBox`，2026-07 修过 BHWC→BCHW permute 缺陷，见 memory）。
- **纯函数 vs 线程纠缠**：纯函数，零队列/线程依赖，可直接单元测试式调用。
- **输入/输出张量契约**：`preprocess(imgs: list[ImageTensor(H,W,C) uint8 BGR]) -> list[torch.Tensor]`（letterbox 后，指定 device）；`inference_and_postprocess(imgs, orig_imgs) -> list[UltralyticsResults]`（内部把 `imgs` `.to(device).to(dtype).div_(255.0)` 归一化后跑推理+NMS+`process_mask`）。sumu 侧：每次要检测的帧从 ready-map 上游解码环缓冲里取，输出的 `Results`（含 mask/box）直接喂给下一步 Scene/Clip 聚合。
- **移植风险/坑（引用 lada 实测）**：
  - GPU 预处理路径历史上有两个从没跑过就藏着的 bug（帧真正变 GPU 张量才触发）：`_preprocess_gpu` 缺 BHWC→BCHW permute；`PyTorchLetterBox` 用 `isinstance` 判重建导致首帧走 ultralytics 原生 `LetterBox`（无 `original_shape` 属性）+ pad 值应为 uint8 空间的 114（不是 /255 空间的 114/255）。移植时如果第一次真正跑 GPU 输入路径，应重新验证这两点没有回归。
  - `realtime-perf-measured-deadends.md`：YOLO `imgsz` 调小收益非单调（瓶颈是 GPU 时间片占用不是算力）；YOLO 与 restorer 同卡抢占在 lada 时代拖累 restorer ~27%。**sumu 已放弃 YOLO 跳帧**（瓶颈不在 YOLO，见 CLAUDE.md「已放弃方向」）；勿再当作待办。

### 2. Scene / Clip 聚合逻辑

- **文件**：`lada/restorationpipeline/mosaic_detector.py`
- **类/函数**：
  - `Scene` 类，第 34-86 行（`__init__` 35, `add_frame` 48, `merge_mask_box` 60, `belongs` 71, `__iter__`/`__next__` 77/80）
  - `Clip` 类，第 89-171 行（`__init__` 90 内部做 `crop_to_box_v3` + resize 到 `(size,size)` + pad, `get_max_width_height` 136, `pop` 148, `__getitem__` 170）
  - 两个 builder 方法：`_create_clips_for_completed_scenes`（第 355-375 行）、`_create_or_append_scenes_based_on_prediction_result`（第 376-396 行）
  - `_NoDetectionResult` 占位类（第 26-31 行，历史 YOLO 跳帧 stub；sumu 已放弃跳帧，保留类无接线义务）
- **依赖**：`image_utils`（`crop_to_box_v3`、`pad_image`）、内部 `Box`/`MaskTensor`/`ImageTensor` 类型别名，无 ultralytics/torch_tensorrt 依赖。
- **device 通用性**：Scene/Clip 内部只做张量裁剪/resize/pad，未见强制 `.cpu()`/`.numpy()` 硬编码，随输入张量 device 走。
- **纯函数 vs 线程纠缠**：**关键坑**——`Scene`/`Clip` 两个类本身纯逻辑无线程依赖，但两个 builder 方法是 `MosaicDetector` 类的**成员方法**，被同一个类里的 `_frame_detector_worker`（第 483-525 行）worker 循环同步调用（第 495/509/515 行）。移植时必须把这两个方法的方法体**从 worker 循环的调用点里抽出来**，改造成独立可调用的纯函数（接收 `scenes: list[Scene]`、当前 `frame_num`、YOLO 的 `Results`，返回更新后的 `scenes` 列表 + 可能产出的 `Clip` 列表），不能把整个 `MosaicDetector` 类照搬。
- **输入/输出契约**：`_create_or_append_scenes_based_on_prediction_result(results, scenes, frame_num)` 按 box 重叠（`belongs`）决定延续现有 `Scene` 还是开新的；`_create_clips_for_completed_scenes(scenes, frame_num, eof)` 把已结束（不再有新检测延续）的 `Scene` 物化成 `Clip`（裁剪+缩放到 `clip_size`×`clip_size`+pad），返回值含 `StopMarker`（EOF 信号，移植时应替换为 sumu 自己的结束语义，不要把 `StopMarker` 类型本身带过去）。
- **移植风险/坑**：`Clip.pop()` 是有状态的消费式弹出（逐帧消费并收缩内部列表）——sumu 若不再用一次性消费的队列语义，需要确认新调度器怎么消费 `Clip`（一次性 vs 可重复读）,避免语义误用。`max_regions_per_frame=1` 是 `MosaicDetector.__init__` 的构造参数，决定每帧最多聚合几个马赛克区域，移植时这个约束要保留（不是免费可去掉的简化）。

### 3. BasicVSR++ 修复封装

- **文件**：`lada/restorationpipeline/basicvsrpp_mosaic_restorer.py`（全文件 59 行，本次移植中最干净的候选）
- **类/函数**：`BasicvsrppMosaicRestorer` 整个类 —— `__init__(model, device, fp16, split_forward=None)`、`warmup(num_frames=8, size=256)`、`restore(video, max_frames=-1)`
- **依赖**：仅 `torch`；`model` 参数既可以是 PyTorch `BasicVSRPlusPlusGan`，也可以是 TRT `BasicVSRPlusPlusNetSplit`（见照搬项 5）——本类不关心具体是哪个，只要求实现同一个 forward 接口，这是最干净的抽象边界。
- **device 通用性**：完全通过 `.to(device=self.device)` 泛化，零线程/队列。
- **纯函数 vs 线程纠缠**：零线程，`restore()` 是同步阻塞调用，可直接嵌入 sumu 的 AI 生产者协程/任务。
- **输入/输出张量契约**：`restore(video: list[ImageTensor(H,W,C) uint8 BGR]) -> list[ImageTensor(H,W,C) uint8 BGR]`（内部 stack 成 `(1,T,C,H,W)` float `[0,1]`，跑模型或 TRT split_forward 或按 `max_frames` 分块跑，再拆回逐帧 uint8 列表，帧数与输入相同）。这正是 sumu ready-map 里"一个 clip 长度的输入帧 → 等长的已修复帧"这一映射的核心实现，可直接对应 ready-map 生产者的一次"产出一个 clip"操作。
- **移植风险/坑**：`warmup()` 用随机 uint8 dummy tensor 跑一次 forward，用于把 CUDA/cuDNN 初始化代价移到加载期而非首个真实 clip（见照搬项 5 `load_models` 里的调用），sumu 若沿用 clip-based 调度，加载期 warmup 这个动作本身也该照搬（否则首个 clip 会有几倍于稳态的延迟，被 sumu 的时钟驱动 present loop 误判为"AI 跟不上"而触发降级）。`max_frames` 分块的具体切分逻辑需要跟 sumu 的 clip_length 上限对齐，避免和 TRT 引擎的 `max_clip_size` 编译上界打架（见照搬项 5）。

### 4. Blend-back

- **文件**：`lada/restorationpipeline/frame_restorer.py`
- **类/函数**：`_restore_frame`（第 249-288 行），内部两个闭包 `_blend_gpu`（第 256-266 行）、`_blend_cpu`（第 267-277 行），第 278 行按 `is_cpu_input` 分派；配套的 `_restore_clip_frames`（第 240-247 行）、`_restore_clip`（第 290-303 行，调用照搬项 3 的 `BasicvsrppMosaicRestorer.restore`）
- **依赖**：`torch`、`numpy`（仅 CPU 分支），无队列/线程依赖本身（但物理上是 `FrameRestorer` 类的成员方法，需要摘取）。
- **device 通用性**：**已验证真实存在**——`_restore_frame` 第 278 行显式按 `frame.device.type == 'cpu'` 在 `_blend_gpu`/`_blend_cpu` 间二选一，两分支各自实现（GPU 走原地 ROI 张量操作，CPU 走 numpy fallback）。
- **纯函数 vs 线程纠缠**：`_restore_frame`/`_restore_clip_frames`/`_restore_clip` 三个方法本身是纯张量操作（给定 `frame`、`frame_num`、`restored_clips` 列表，返回混合后的单帧），但同样是 `FrameRestorer` 类的成员方法，被 `_frame_restoration_worker`（第 385-432 行，第 416 行调用点）同步调用——与照搬项 2 的情况相同：**需要从 worker 循环里摘出方法体**，不能整类照搬。
- **输入/输出张量契约**：`_restore_frame(frame: ImageTensor(H,W,C) uint8, frame_num: int, restored_clips: list[Clip]) -> ImageTensor(H,W,C) uint8`——按 `frame_num` 匹配 `restored_clips` 里覆盖该帧号的 clip，用其 mask 做 ROI blend 贴回原始帧对应 box 位置，无覆盖则原样返回原始帧。这是 sumu ready-map 填格子的最后一步：ready-map[frame_num] = blend_back(原始解码帧, 该帧对应的已修复 clip)。
- **移植风险/坑**：CPU 分支存在只是为了兼容非 CUDA 设备，sumu 目标机器（RTX4080）应始终走 `_blend_gpu` 分支，但抽取代码时建议保留双分支以防未来降级到软解/CPU 兜底路径（DESIGN.md I9 降级旋钮列出的方向之一）。`restored_clips` 是一个列表，`_restore_frame` 内部要按 frame_num 命中匹配的 clip——这个匹配逻辑本身与 sumu 新调度器（ready-map 直接按帧号做 O(1) 查找）语义不同，需要重新设计匹配方式而非照搬"遍历列表找命中"的实现细节，但 blend 的数学/张量操作本身应逐字照搬。

### 5. TRT 子引擎装配 + 模型加载/缓存

- **文件与函数**（分 4 部分）：
  - **缓存键逻辑**：`lada/restorationpipeline/trt_engine_paths.py`（全文件 119 行）——`engine_system_suffix()`、`engine_precision_name(*, fp16)`、`engine_arch_suffix(device)`（如 `sm89`）、`engine_trt_suffix()`（如 `trt1012`）、`get_basicvsrpp_sub_engine_paths(model_weights_path, fp16, max_clip_size=60, device=None)`、`all_basicvsrpp_sub_engines_exist(...)`，常量 `BASICVSRPP_DIRECTIONS = ("backward_1","forward_1","backward_2","forward_2")`。纯函数，无 GPU 计算，仅查询 `torch.cuda.get_device_capability` 和拼路径字符串。
  - **子引擎包装 + 运行时**：`lada/restorationpipeline/basicvsrpp_sub_engines.py`（全文件 676 行，源自 jasna fork）——`_PropagateBodyWrapper`、`_UpsampleWrapper`、`_SPyNetWrapper`（6 层金字塔手工展开，仅因 `torch_tensorrt` 编译不了 Python for-loop/list，**逻辑必须逐层照搬，不能"优化"回循环写法**）、`_PreprocessWrapper`；`compile_basicvsrpp_sub_engines(model, device, fp16, model_weights_path, max_clip_size=60, optimization_level=5)`；`load_sub_engines(model_weights_path, device, fp16, max_clip_size=60)`（反序列化失败时自动删旧引擎文件回退 PyTorch，自愈）；`BasicVSRPlusPlusNetSplit(nn.Module)`（运行时替身，`forward` 编排 preprocess 引擎→per-direction `propagate()`→`upsample()`）；`create_split_forward(...)` 顶层工厂。常量 `FEATURE_SIZE=64`、`INPUT_SIZE=256`、`MAX_DYNAMIC_BATCH=180`、`OPT_DYNAMIC_BATCH=60`。
  - **编译策略**：`lada/restorationpipeline/basicvsrpp_trt_compilation.py`（全文件 119 行）——`get_gpu_vram_gb(device)`、`compile_mosaic_restoration_model(...)`（<4GB VRAM 或非 fp16 直接跳过编译）、`basicvsrpp_startup_policy(*, restoration_model_path, device, fp16, compile_basicvsrpp, max_clip_size=60, optimization_level=5)`。
  - **底层编译/加载 helper**：`lada/trt/torch_tensorrt_export.py`（177 行）——`get_workspace_size_bytes()`（取 95% 空闲显存）、`load_torchtrt_export(*, checkpoint_path, device)`（`torch.export.load` 优先，失败 fallback `torch.load`）、`compile_and_save_torchtrt_dynamo(...)`（核心 `torch_tensorrt.compile(ir="dynamo", ...)` 调用，`use_python_runtime=False`、`cache_built_engines=False`）、`_save_with_dynamic_shapes(...)`（用 `torch.export.Dim` 构造动态维度约束）。这是 BasicVSR++ TRT 走的路径（走 `torch.export`/dynamo，模块化保存/加载）。`lada/trt/trt_runner.py`（87 行，`TrtRunner` 类）是**另一条路径**——原生 `tensorrt.Runtime`/`deserialize_cuda_engine`，标注注释明确说明"用于可选的 YOLO-TRT 路径（phase 2），BasicVSR++ split forward 不需要它"——**不在本次 BasicVSR++ 照搬范围内**，若 sumu 未来照搬 YOLO 的 TRT 加速才需要它。
  - **模型加载入口**：`lada/restorationpipeline/__init__.py`（149 行）——`load_models(device, mosaic_restoration_model_name, mosaic_restoration_model_path, mosaic_restoration_config_path, mosaic_detection_model_path, fp16, detect_face_mosaics, allow_trt_compile=True)`（第 31-85 行，加载 PyTorch BasicVSR++ 模型→尝试构建 TRT split_forward→构造 `BasicvsrppMosaicRestorer`→加载 YOLO 检测模型→对两个模型分别做 warmup，见第 63-83 行的详细预热理由：把 CUDA/cuDNN 首次初始化代价移到加载期，否则首个真实 clip 会被 realtime 误判成"AI 掉速"触发降级/reposition 抖动）；`_maybe_build_trt_split_forward(...)`（第 88-148 行，gate: 非 cuda 或非 fp16 直接返回 None 走 PyTorch；`allow_compile=False` 时只加载已存在引擎绝不新编译，用于 warmup 阶段尊重用户"稍后编译"的选择）。
  - **模型缓存/生命周期**：`lada/gui/frame_restorer_provider.py` —— `FrameRestorerProvider` 类（第 95-223 行）：`init(options)`（第 101-106 行，device 变化时清缓存）、`warmup(...)`（第 108-122 行，无需打开视频即可预热）、`get(frame_restoration_queue_max_bytes=None, decoder_slot=None)`（第 124-153 行，被 GUI/CLI 调用的主入口，惰性触发 `_ensure_loaded` 后构造 `FrameRestorer`）、`_ensure_loaded(...)`（第 155-197 行，缓存命中判定逻辑：模型名/fp16/检测人脸开关任一变化则 `_clear_cache()` 后重新调 `load_models`）、`_clear_cache()`（第 207-223 行，显式 `del` + `gc.collect()` + `torch.cuda.empty_cache()`）。**注意**：`get()` 里第 142-153 行构造的 `FrameRestorer(...)` 本身属于"重写"边界（见下），照搬时只取 `_ensure_loaded`→`load_models` 这条"缓存好的模型对象"产出路径，不取 `FrameRestorerProvider.get()` 里构造 `FrameRestorer`/`PassthroughFrameRestorer` 那一段。
- **依赖**：`torch`、`torch_tensorrt`（仅编译/加载引擎时需要）、`tensorrt`；不需要升级 torch 版本（memory 确认 torch 2.8.0+cu128 + torch-tensorrt 2.8.0 在 RTX4080 cc8.9 上工作正常）。
- **device 通用性**：非 cuda 设备或非 fp16 直接判定跳过 TRT、回退 PyTorch 路径（`_maybe_build_trt_split_forward` 第 108-113 行），这是显式设计好的降级路径，不是缺陷。
- **纯函数 vs 线程纠缠**：全部无线程/队列纠缠，是模型加载期的一次性同步调用链（`load_models` → `_maybe_build_trt_split_forward` → `basicvsrpp_startup_policy`/`create_split_forward`）。
- **输入/输出契约**：`load_models(...) -> (mosaic_detection_model, mosaic_restoration_model, pad_mode)`——`mosaic_restoration_model` 是已经包好 TRT-or-PyTorch 差异的 `BasicvsrppMosaicRestorer`（对上层调用者完全透明，见照搬项 3），`mosaic_detection_model` 是 `Yolo11SegmentationModel`（照搬项 1）。sumu 的模型加载/缓存层应直接复用这条"输出两个已就绪、已预热模型对象"的产出契约，不需要关心内部是否用了 TRT。
- **移植风险/坑（引用 lada 实测）**：
  - TRT 引擎缓存键 = GPU 架构（`sm89` 等）+ TRT 版本（`trtXXXX`）+ 精度（fp16/fp32）+ OS（win/linux）+ clip 上界（`max_clip_size`，默认 180）——**编码进引擎文件名本身**，不匹配时自动判定"引擎不存在/失效"并回退重新编译（自愈），**不做跨机器分发/共享缓存**；sumu 若打包分发要注意首次运行必有一次"多分钟阻塞编译"（`allow_trt_compile=True` 路径，GUI 有专门的"编译中"进度提示，见 `load_models` 第 43/59/71 行 `report_load_progress` 调用）。
  - `_SPyNetWrapper` 的 6 层金字塔硬展开只在空间尺寸是 32 的倍数时有效——若 sumu 的 clip_size/分辨率选型偏离 256×256 这个已验证配置，需要重新检查这个约束。
  - `BASICVSRPP_TRT_MAX_CLIP_SIZE`（默认 180，环境变量 `LADA_BASICVSRPP_TRT_MAX_CLIP` 可覆盖）是动态 batch 编译上界，必须 ≥ 实际喂给引擎的最大 clip 长度，否则运行时形状越界；sumu 的 clip-based 调度定的 `max_clip_length` 需要与此对齐或至少不超过它。
  - jasna 参考文档确认此 TRT 移植在 lada-realtime 内已完成验证：MAE=0.0012（几乎无精度损失），T=16 时 3.16x 加速、T=60 时 4.47x 加速。

### 6. GPU 解码后端（NVDEC 路径）

- **文件**：`lada/utils/video_utils.py`（全文件 1131 行）
- **类/函数**：`VideoReader` 类（第 242-751 行）——`__init__(file, device=None, decoder_slot=None, prefer_fast_seek=False)`（243）、`_try_pynvc()`（279）、`_make_hwaccel()`（374，仅 win32+cuda+`LADA_NVDEC` 开启时生效）、`__enter__`（393，先试 pynvc 再 fallback PyAV hwaccel）、`__exit__`（426，pynvc 池感知的清理）、`_pyav_frame_to_tensor(frame)`（452，hwaccel+nv12 格式时走 GPU 色转，否则退回 CPU `to_ndarray('bgr24')`）、`frames()`（467，主迭代器）、`_frames_pynvc()`（510，CLI 导出专用零拷贝路径）、`seek(offset_ns)`（541）；配套模块级函数 `_nv12_to_bgr_hwc_gpu(nv12, h, w, torch_device, bt709, full_range)`（202 行起，GPU 上做 nv12→bgr 色转，nearest chroma + fp16）、`get_video_meta_data`、`offset_ns_to_frame_num`、`pts_to_frame_num`、`first_decoded_frame_num_after_seek`（均为独立纯函数，不在 `VideoReader` 类内）。
- **依赖**：`av`（PyAV，16.1.0 已验证支持 cuda/dxva2/qsv/d3d11va/d3d12va hwaccel）、可选 `PyNvVideoCodec`（仅 CLI 导出路径需要，通过 `_ensure_pynvc_dll_dirs()` 手动挂 DLL 目录）、`torch`。
- **device 通用性**：`frames()` 按解码后端不同，可产出 CPU 或 CUDA 张量（PyAV hwaccel+nv12 走 `_nv12_to_bgr_hwc_gpu` 直接产出 CUDA 常驻 `BGR HWC uint8`；否则走 CPU `to_ndarray('bgr24')`），下游（YOLO/blend）已确认按 device 分派，全链路兼容。
- **纯函数 vs 线程纠缠**：`VideoReader` 本身不含 worker 线程/队列，是一个可迭代的生成器式解码器，可直接在 sumu 新的单一解码线程/任务里驱动;但需注意 lada-realtime 里它被两个独立地方各开一个实例分别解码（`MosaicDetector` 的 feeder 和 `FrameRestorer` 的 restoration worker 各自开一路 `VideoReader`，即**同一视频解码两次**）——DESIGN.md I7 明确要求 sumu 改为"单一解码头"，这是移植时必须打破的重复，不能照搬"各开一路"的用法模式，只照搬 `VideoReader` 类本身的实现。
- **输入/输出张量契约**：`frames() -> Iterator[(torch.Tensor(H,W,C) uint8 BGR [CPU或CUDA], pts:int)]`。这正是 sumu ready-map 上游"原始解码帧"来源的直接对应实现，sumu 的单一解码头可以直接复用这个类，然后自己把同一份解码结果同时供给 present 环缓冲和 AI 前沿（而不是像 lada 那样各自独立开一路）。
- **移植风险/坑（引用 lada 实测，均来自 `nvdec-hardware-decode.md`，2026-07-05 定案）**：
  - **务必走 realtime 分支（`prefer_fast_seek=True` → PyAV NVDEC-hwaccel），不要照搬 CLI 的 pynvc 零拷贝路径**：`PyNvVideoCodec` 经 6 个探针实测确认**没有任何稳定的快速 seek**（`SimpleDecoder.seek_to_index`/`ThreadedDecoder`/`reconfigure_decoder` 均是 O(目标帧号) 顺序扫描，600s 深度→7.7s、3000s→38.8s；唯一 O(1) 的 `PyNvDemuxer.Seek` 配 `PyNvDecoder` 在反复 seek 时会非确定性 segfault，属 native 内存损坏，NVIDIA 官方示例也无重复 seek 用法）。sumu 的 seek=reposition（I6）要求频繁重定位 AI 前沿，必须用 PyAV `container.seek()` 的真关键帧 seek（任意深度恒 ~2-130ms）。
  - **色彩转换必须走 GPU（`to_ndarray('nv12')`+`_nv12_to_bgr_hwc_gpu`），绝不能用 `to_ndarray('bgr24')`**：后者是 libswscale 在 CPU 上做色转，4K 单流仅 47fps（色转约 11ms/帧），是当年"4K 完全不可用"的真正瓶颈（seek 慢是伴随症状，不是主因）。GPU nv12→bgr 转换（nearest chroma+fp16）实测 2.1ms/帧、MAE=0.69（对 AI 输入可接受），把 4K 单流吞吐从 47 拉到 141fps。
  - **硬约束需在设计里认账**：即便走 PyAV NVDEC-hwaccel + GPU 色转的最优组合，"4K60 每帧必须去码"仍不可达——PyAV 16.1 内部把 NVDEC 显存帧强制下回主存再交给调用方（无真零拷贝，`decode()` 含 hw-transfer 每帧约 3.9ms），且没有硬件色转（NVDEC 芯片级 YUV→RGB 转换只有 pynvc 能拿到），这两个"免费午餐"PyAV 都拿不到。这不是实现细节问题而是 PyAV 库本身的能力边界，2026-07-05 项目已拍板此路径下专注 1080p（1080p 色转/往返成本低，轻松 60fps 全命中），4K 接受降级到约 38fps best-effort。sumu 若要 4K60 全速，需要重新评估"统一解码源省一路主机往返"或未来切换解码库的方向，而不是在当前 `VideoReader` 实现上继续挖潜。
  - 已知修过的历史坑（照搬实现时这些修复必须带上，不能回退到更早版本）：pynvc 池化/生命周期管理（CLI 路径用，`_PYNVC_DECODER_POOL`、`decoder_slot` 隔离键，防止不同 tab 撞用同一解码器）；`first_decoded_frame_num_after_seek` 必须锚定真实解码 PTS 而非标称 offset（PyAV BACKWARD seek 落在最近关键帧而非请求点）；负 pts 归一化（pynvc 路径需要，PyAV 路径 pts 本身从 0 起不需要）。

---

## 二、重写/剥离（不照搬，仅供设计参考）

以下内容**不移植**，sumu 需要围绕新的时钟驱动 ready-map 调度器完全重新设计，仅在理解"lada 原本怎么做 frontier gate / 冷启动 / teardown 语义"时作参考：

- **5 个 `_*_worker` 循环 + `PipelineQueue` + `EOF_MARKER`/`STOP_MARKER` 握手**：`mosaic_detector.py` 的 `_frame_feeder_worker`（397-448）、`_frame_inference_worker`（449-482）、`_frame_detector_worker`（483-525）；`frame_restorer.py` 的 `_clip_restoration_worker`（322-354）、`_frame_restoration_worker`（385-432）。以及贯穿两个文件的 `lada/utils/threading_utils.py`（132 行）——`StopMarker`/`EofMarker`/`ErrorMarker`、`PipelineThread`、`PipelineQueue`、`put_queue_stop_marker`、`empty_out_queue*` 系列。这整套哨兵驱动的线程编排是 lada 数据驱动模型（"模型跟不上就暂停缓冲"）的产物，与 sumu I1/I2（时钟驱动、present 与 AI 完全解耦、永不为 AI 阻塞）根本冲突，不能改良只能重建。
- **`FrameRestorer.start`/`stop` 的 5 线程 teardown 握手**：`frame_restorer.py` 第 120-165 行（`start`）、166-208 行（`stop`）。sumu 按 I6 把"seek"改造成"reposition"（重定位 present clock + decode-ahead 缓冲 + AI 前沿，不销毁重建线程/解码器），这部分的握手逻辑整体作废，仅其中的语义（`_detector_lead = max(max_clip_length, round(1.2*max_clip_length))` 第 117 行这类"前瞻量怎么算"的参数关系、`get_output_frame_pos`/`get_start_frame`/`set_processing_frontier` 这套 API 对外承诺）可作新调度器接口设计的参考。
- **GStreamer appsrc 胶水 + seek 拆建状态机**：CLAUDE.md 中描述的 appsrc push loop、`_start_appsource_worker`/`_stop_appsource_worker`、`close_video_file` 的 `GLib.idle_add` 主线程编排等，是 GStreamer/GTK4 承载层特有的，sumu 换成原生 D3D11 present loop 后这整层不存在对应物。
- **整个 GUI 外壳**：`lada/gui/*`（除 `frame_restorer_provider.py` 里模型缓存那一小段外）、`PassthroughFrameRestorer`（`frame_restorer_provider.py` 第 225-286 行，GStreamer/GTK 特有的原片直通队列适配器）——sumu 是新项目，无历史包袱，present/UI 从零设计。

---

## 三、建议移植顺序与 ready-map 集成点

建议顺序：先落地**照搬项 5 的模型加载/缓存层**（`load_models` + TRT 缓存键/子引擎，因为它是后两项的前置依赖且本身零线程/纯加载期逻辑，风险最低、验证最快——直接跑一次加载+warmup+dummy forward 就能确认移植正确）→ 接着**照搬项 6 的 `VideoReader`**（单一解码头改造，先只验证能否产出 CUDA 常驻 BGR 帧流，不接 AI，可与 sumu DESIGN.md 的 Spike 0/1 阶段并行）→ 然后**照搬项 1（YOLO）+ 项 3（BasicVSR++ 封装）**（两者互相独立，可并行移植，都是零线程纯函数，单元测试式验证："喂一批固定输入张量，输出形状/dtype/大致数值是否符合预期"）→ 最后**照搬项 2（Scene/Clip 聚合）+ 项 4（blend-back）**，这两项需要从原 worker 循环里摘取方法体，风险和工作量都更高，且需要与 sumu 新调度器的 ready-map 填格子时机对齐（"检测/聚合出一个 Clip"对应"预约 ready-map 上一段帧号区间"，"blend-back 出一帧"对应"往 ready-map[frame_num] 填入最终纹理"），建议放在其余四项都验证通过、新调度器骨架（frontier gate + clip 调度 + 冷启动前瞻）已经确定接口之后再做，避免返工。

ready-map 集成的核心接口对应关系：`VideoReader.frames()` 产出的 `(tensor, pts)` 流 → sumu 原片解码环缓冲的写入源（同时也是 AI 前沿的读取源，二者共享同一路解码，不再各开一路）；YOLO 检测 + Scene/Clip 聚合的产出（一个个 `Clip`）→ 对应 ready-map 上"预告一段帧号区间即将由 AI 产出"的声明；`BasicvsrppMosaicRestorer.restore(clip 帧列表)` 的产出 → 逐帧走 blend-back → 写入 ready-map[frame_num] 的最终值；present loop 每个 vblank 按帧号查 ready-map，命中则用 AI 帧，未命中则回退原片环缓冲对应帧号——这正是 DESIGN.md I2/I9 的直接体现，也是本次照搬六项与 sumu 新调度器之间唯一需要"胶合"的边界。

