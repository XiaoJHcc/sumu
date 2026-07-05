# Scene/Clip 聚合 + blend-back 移植记录（porting_manifest.md 项 2、项 4）

来源：`d:/Git/lada-realtime`（只读）
- 项 2：`lada/restorationpipeline/mosaic_detector.py`
- 项 4：`lada/restorationpipeline/frame_restorer.py`

落地：
- `python/sumu/ai/restorationpipeline/scene_clip.py`（项 2：`Scene`、`Clip`、`_NoDetectionResult` + 两个 builder 摘出的纯函数）
- `python/sumu/ai/restorationpipeline/blend.py`（项 4：blend-back 摘出的纯函数）
- 依赖一并照搬进 `python/sumu/ai/utils/`：`box_utils.py`（`box_overlap`）、`scene_utils.py`（`crop_to_box_v3`）、`mask_utils.py`（`create_blend_mask`）
- 验证：`scripts/verify_scene_clip_blend.py`

## 结论：两项均照搬跑通，端到端验证通过

在目标机器（RTX 4080，`test_video.mp4` 1080p30 全程马赛克）上实测：

```
== item5 load_models == 4.87s  pad_mode=zero  trt=True
== item2 Scene/Clip aggregation ==
   ran YOLO + aggregation over 90 frames in 1.35s (66.5 fps)
   frames with >=1 detection: 90/90
   scenes still open at EOF: 0 (should be 0 - eof=True flushes all)
   clips produced: 3
     clip id=0 frames=[0,29]  len=30 tensor_shape=(256, 256, 3) dtype=torch.uint8 mask_shape=(256, 256, 1)
     clip id=1 frames=[30,59] len=30 tensor_shape=(256, 256, 3) dtype=torch.uint8 mask_shape=(256, 256, 1)
     clip id=2 frames=[60,89] len=30 tensor_shape=(256, 256, 3) dtype=torch.uint8 mask_shape=(256, 256, 1)

== item4 restore + blend-back ==
   using clip id=0 frames=[0,29] len=30
   restore_clip: 148.9ms (4.96ms/frame)
   blended 30 frames
   per-frame max |blended - original| (uint8 scale): min=110.0 max=148.0 mean=128.8
   per-frame fraction of pixels changed: min=0.0281 max=0.0329
   PASS: every blended frame's masked region differs from the original mosaiced frame

== item4 CPU branch sanity ==
   CPU-branch max |blended - original|: 141.0
   PASS: CPU blend branch (_blend_cpu) also produces a real, non-zero change
```

- 90 帧全部检出马赛克区域（`test_video.mp4` 是全程马赛克的压测视频，符合预期）。
- `max_clip_length=30` 约束生效：90 帧一路连续检测精确切成 3 个 30 帧 clip，`frame_start/frame_end` 边界正确（`[0,29] [30,59] [60,89]`），EOF 时 `scenes` 清空（0 个残留场景）——证明 `materialize_completed_clips` 的完成判定（`frame_end < frame_num`、`len >= max_clip_length`、`eof`）都按预期工作。
- `Clip` 尺寸：每帧张量 `(256,256,3) uint8`、mask `(256,256,1)`，与 `clip_size=256` 一致，crop→resize→pad 链路（`crop_to_box_v3` + `image_utils.resize` + `image_utils.pad_image`）产出形状正确。
- blend 前后差异证据：30 帧全部 `max|diff| > 0`（110~148，接近 uint8 满量程一半以上），且每帧约 2.8%~3.3% 像素发生变化——与 mask 覆盖的马赛克区域比例量级吻合，证明 blend 确实按 mask 把修复内容贴回、而不是空操作。
- `Clip.pop()` 完整耗尽验证：blend 完成后 `len(clip)==0` 且 `frame_start/frame_end` 被清空为 `None`，证明消费语义按原样保留。
- GPU 分支（`_blend_gpu`）：主流程全程在 CUDA 上跑通（load_models 用 fp16+TRT，YOLO/Clip/blend 张量全在 `cuda`）。
- CPU 分支（`_blend_cpu`）：额外用第二个真实 clip，把其张量 `.cpu()` 后走 `blend_back_frame`（靠 `frame.device.type=='cpu'` 触发分派），同样产出非零差异（max diff 141），验证 numpy 路径可用。

## 从 worker 循环摘取的纯函数签名

### 项 2 — `python/sumu/ai/restorationpipeline/scene_clip.py`

```python
def append_or_create_scenes(
    results: UltralyticsResults,
    scenes: list[Scene],
    frame_num: int,
    video_meta_data: VideoMetadata,
    max_regions_per_frame: int = 1,
) -> list[Scene]:
    ...  # 原地 mutate 并返回 scenes

def materialize_completed_clips(
    scenes: list[Scene],
    frame_num: int,
    eof: bool,
    max_clip_length: int,
    clip_size: int,
    pad_mode: str,
    clip_counter: int,
) -> tuple[list[Scene], list[Clip], int]:
    ...  # 返回 (更新后的 scenes, 本轮产出的 completed_clips, 前进后的 clip_counter)
```

对应原方法：
- `MosaicDetector._create_or_append_scenes_based_on_prediction_result`（mosaic_detector.py:376-396）
- `MosaicDetector._create_clips_for_completed_scenes`（mosaic_detector.py:355-375）

与用户建议签名的差异（均为必要的显式化，不是行为变化）：
- `self.max_regions_per_frame` / `self.max_clip_length` / `self.clip_size` / `self.pad_mode` / `self.video_meta_data` 全部变成显式参数（原来是 `MosaicDetector.__init__` 的构造参数或成员）。
- `self.clip_counter`（原来靠成员变量在多次调用间累加）改为显式传入/返回，调用方自己持有并递增。
- `self.mosaic_clip_queue.put(clip)` → 直接 append 进返回的 `completed_clips` 列表；不再有 `STOP_MARKER` 中途短路返回值（那是 `PipelineQueue` 背压逼停生产者线程用的，纯函数没有线程可逼停）。

`Scene`、`Clip`、`_NoDetectionResult` 三个类是逐字照搬（只改了 import 路径 `lada.utils` → `sumu.ai.utils`），包括 `Clip.pop()` 的有状态消费式弹出语义、`max_regions_per_frame=1` 的默认值约束都原样保留。

### 项 4 — `python/sumu/ai/restorationpipeline/blend.py`

```python
def restore_clip_frames(restoration_model, mosaic_restoration_model_name: str, images: list[ImageTensor]) -> list[ImageTensor]: ...

def restore_clip(restoration_model, mosaic_restoration_model_name: str, clip: Clip) -> None:
    ...  # 原地改写 clip.frames

def blend_back_frame(frame: ImageTensor, frame_num: int, matched_clips: list[Clip], restoration_model) -> ImageTensor:
    ...  # 原地改写 frame 的 ROI，并返回 frame
```

对应原方法：
- `FrameRestorer._restore_clip_frames`（frame_restorer.py:240-247）
- `FrameRestorer._restore_clip`（frame_restorer.py:290-303）
- `FrameRestorer._restore_frame`（frame_restorer.py:249-288，含闭包 `_blend_gpu`/`_blend_cpu` 及第 278 行按 `frame.device.type` 的分派）

与原方法体的刻意差异：
- `self.mosaic_restoration_model`（提供 `.dtype`）与 `self.device`（提供 `.device`，构建 blend_mask 时用）合并成一个 `restoration_model` 参数——两者在 lada 的每个真实调用点其实是同一个对象（`FrameRestorer` 总是用 `device=self.device` 构造 `mosaic_restoration_model`），这里只是少传一个参数，不是行为变化。
- `_restore_clip` 去掉了 `mosaic_detection` 调试可视化分支（`visualization_utils.draw_mosaic_detections`）——`visualization_utils` 没有被移植进 sumu，不在本次照搬清单（porting_manifest.md 项 4）范围内，纯属越界功能，未覆盖。
- `blend_back_frame` **不**照搬原来"遍历 `restored_clips` 找 `frame_start==frame_num` 命中"的列表扫描；改为要求调用方直接传入 `matched_clips`（已经算好、确实命中该帧号的 clip 列表）。函数内部额外加了一条 `assert buffered_clip.frame_start == frame_num` 作为契约保险丝（原来没有，因为原来靠自己的过滤保证；现在契约转移给调用方，加断言防止调度器接错）。
- blend 的数学/张量操作（`_blend_gpu`/`_blend_cpu` 内部逐行）**逐字照搬**，未作任何"优化"。

## 遇到的坑

1. **`crop_to_box_v3` 不在 `image_utils.py` 里，而在 `scene_utils.py`**——`porting_manifest.md` 项 2 的措辞把它归在"依赖 image_utils 的 crop_to_box_v3、pad_image 等"下，容易让人以为要塞进 `image_utils.py`；实地读 lada 源码后确认它是独立文件 `lada/utils/scene_utils.py`（仅此一个函数），已按原样落地为 `python/sumu/ai/utils/scene_utils.py`，与 lada 目录结构保持一致而非塞进 `image_utils.py`。
2. **`VideoMetadata` 构造**——`Scene.__init__` 需要一个 `VideoMetadata` 实例（虽然 Scene/Clip 内部只读它的 `.video_file`），但产出它的 `video_utils.get_video_meta_data`（porting_manifest.md 项 6）不在本次移植范围。验证脚本里用 PyAV 直接读容器元数据现拼一个 `VideoMetadata`（`scripts/verify_scene_clip_blend.py:build_video_meta_data`），不是照搬，只是测试脚手架；真正的项 6 移植时应该用那边的 `get_video_meta_data` 替换掉这段手拼代码。
3. **CPU blend 分支的测试真实性打了折扣**——CPU 分支验证是把同一次真实 GPU 流程里产出的第二个 clip，事后把张量 `.cpu()` 搬过去测的，不是从头一条独立的 CPU 全链路跑出来的（避免在同一进程里再实例化一套 CPU 版模型的开销/复杂度）。blend 数学本身在 CPU 上被真实执行且产出非零差异，但检测/聚合/修复这几步在 CPU 分支下没有被独立验证过。
4. **`max_regions_per_frame=1` 的影响未被压测触发**——`test_video.mp4` 看起来每帧只有一个马赛克区域被检出，所以这次跑没有实际验证到"同一帧出现多个检测框、只取第一个"这条约束路径是否被正确保留（代码逻辑上保留了 `min(len(results.boxes), max_regions_per_frame)`，但没有真实多区域画面去触发它）。
5. **TRT 引擎缓存命中,加载很快（4.87s）**——本机之前已经跑过项 5 的验证并生成了 `lada_mosaic_restoration_model_generic_v1.2_sub_engines/*.engine`，这次直接命中缓存,没有触发"首次多分钟编译"路径，因此这次验证没有覆盖到冷编译场景（该场景在项 5 的移植/验证里已覆盖，这里不重复）。

## 未覆盖项（诚实报告）

- `visualization_utils.draw_mosaic_detections`（`mosaic_detection` 调试可视化分支）未移植，不在项 4 范围内。
- `video_utils.get_video_meta_data`（项 6）未移植；本次用 PyAV 手拼 `VideoMetadata` 仅用于测试脚手架。
- CPU 全链路（检测+聚合+修复，不仅仅是 blend 数学）未独立跑通验证，只验证了 blend 数学本身的 CPU 分支。
- 多区域检测（`max_regions_per_frame>1` 时"只取前 N 个"的截断路径）未被真实数据触发验证，只是代码走查确认逻辑保留。
- 未来调度器怎么给 `blend_back_frame` 喂 `matched_clips`（ready-map O(1) 查找)、`Clip` 是一次性消费还是可重复读，都还没设计，仅在本次移植的函数签名/注释里留了扩展点。
