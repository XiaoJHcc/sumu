# AI 计算核心移植记录（Phase 4c）

> 本文记录 lada-realtime → sumu 的 AI 计算核心「照搬」（port verbatim）第一批三项的落地：
> 模型加载/缓存层（项5）、YOLO 检测（项1）、BasicVSR++ 修复封装（项3）。
> 源仓库 `d:/Git/lada-realtime` 只读未改。测绘清单见 `docs/porting_manifest.md`。
>
> 目标机：RTX 4080 · Win11 · Python 3.13.12 · torch 2.8.0+cu128 · CUDA cc8.9。

## 1. 结构（`python/sumu/ai/`）

镜像 lada 的包结构，import 前缀机械替换 `lada.` → `sumu.ai.`，其余逐字照搬。

```
python/sumu/ai/
  __init__.py                     # 移植支撑：gettext 安装(_)、LOG_LEVEL、MODEL_WEIGHTS_DIR、ModelFile/ModelFiles
  model_cache.py                  # 项5：ModelCache = lada FrameRestorerProvider 的 _ensure_loaded→load_models 缓存路径（不含 FrameRestorer 构造）
  utils/
    __init__.py                   # 类型别名 Box/Mask/Image/ImageTensor/Pad 等（照搬，无 lada 依赖）
    image_utils.py                # 照搬（inference.py 模块级依赖）
    ultralytics_utils.py          # 照搬（UltralyticsResults + 转换函数）
    torch_letterbox.py            # 项1：PyTorchLetterBox（照搬，含 pad=114 uint8 修复）
  models/
    yolo/yolo11_segmentation_model.py   # 项1：Yolo11SegmentationModel 整类（照搬）
    basicvsrpp/                    # 项3：vendored 模型定义（照搬）
      __init__.py                 # register_all_modules（去掉训练用 MosaicVideoDataset，见 §4）
      basicvsrpp_gan.py inference.py deformconv.py
      mmagic/*                    # vendored mmagic 子包（36 文件，照搬；SCOPE/registry 字符串改指 sumu.ai）
  restorationpipeline/
    __init__.py                   # 项5：load_models + _maybe_build_trt_split_forward（照搬）
    progress.py                   # 照搬
    basicvsrpp_mosaic_restorer.py # 项3：BasicvsrppMosaicRestorer 整类（照搬）
    basicvsrpp_sub_engines.py     # 项5：6 子引擎装配 + _SPyNetWrapper 6 层硬展开（逐层照搬，未改回循环）
    basicvsrpp_trt_compilation.py # 项5：编译策略（照搬）
    trt_engine_paths.py           # 项5：TRT 缓存键（照搬，无 lada 依赖）
  trt/
    __init__.py torch_tensorrt_export.py   # 项5：底层 dynamo 编译/加载 helper（照搬）
```

**权重不入库**：`sumu.ai.MODEL_WEIGHTS_DIR` 默认解析到同级 `../lada-realtime/model_weights`
（可用 `SUMU_MODEL_WEIGHTS_DIR` / `LADA_MODEL_WEIGHTS_DIR` 覆盖）。本次用
`lada_mosaic_restoration_model_generic_v1.2.pth` + `lada_mosaic_detection_model_v4_fast.pt`。

## 2. 依赖装配（步骤0，最大风险 —— 已解决）

### 补丁机制
lada 对 pin 死的第三方库打补丁：**改 install 后的 site-packages 文件**（`python -m patch`），
不 vendor 覆盖。dev 安装（`docs/windows_install.md §5`、`CLAUDE.md`）只打 3 个运行期补丁。
sumu 复刻同一机制：`patches/` 存补丁，`scripts/apply_patches.sh` 打入 `.venv`。

| 补丁 | 目标 | 作用 | 对本次是否必需 |
|---|---|---|---|
| `fix_loading_mmengine_weights_on_torch26_and_higher.diff` | `mmengine/runner/checkpoint.py` | `torch.load(..., weights_only=False)` | **必需**：torch≥2.6 默认 `weights_only=True`，否则 BasicVSR++ 权重加载失败 |
| `remove_ultralytics_telemetry.patch` | `ultralytics/utils/{events,__init__}.py` | 去 sentry/GA 遥测、`sync=False` | 隐私/离线（照搬保留） |
| `increase_mms_time_limit.patch` | `ultralytics/utils/nms.py` | NMS `max_time_img` 0.05→0.3 | 大帧检测质量（照搬保留） |

第 4 个 `remove_use_of_torch_dist_in_mmengine.patch`（Windows `torch.distributed` shim）
**未打**：本机（torch 2.8.0+cu128、py3.13）mmengine 0.10.7 + vendored mmagic 全部 import
正常，无 `ReduceOp/fsdp` 报错——与 lada dev 安装列表一致（也只打 3 个）。已 stage 备用。

**已知脆弱点**：任何重装 ultralytics/mmengine 的 `uv sync` 会覆盖补丁，需重跑
`scripts/apply_patches.sh`（lada 同样如此）。

### pyproject（对齐 lada nvidia extra）
新增：`ultralytics==8.4.4`、`torchvision==0.23.0`、`mmengine==0.10.7`、`av>=16.1.0`、
`opencv-python==4.12.0.88`、`tqdm`、`wcwidth`、`torch-tensorrt==2.8.0`、
`tensorrt-cu12-libs==10.12.0.36`、`tensorrt-cu12-bindings==10.12.0.36`。
索引：torch/torchvision/torch-tensorrt→南大 cu128；tensorrt-cu12-libs/bindings→`pypi.nvidia.com`（清华镜像 tensorrt 只到 10.2）。
`environments` pin 到 win/AMD64（避开 tegra 变体的 numpy<2 冲突，同 lada）。
**`PyNvVideoCodec` 未加**：它只服务 lada 的 VideoReader 生产解码路径（项6，出范围）。

`uv sync` 一次成功，与 torch==2.8.0+cu128 无冲突。

## 3. 验证（真实数字，未美化）

命令：`PYTHONPATH=python .venv/Scripts/python.exe scripts/verify_ai_core.py test_video.mp4`
（真实帧用 PyAV CPU 一次性解码若干帧作输入，非 sumu 生产路径）。

### 项5 load_models
- 加载 **成功**，warmup **成功**（restoration + detection batch=4 均预热，无 skip 警告）。
- **TRT 状态：走了已存在的 sub_engines（缓存命中，未重新编译）**。
  `model_weights/lada_mosaic_restoration_model_generic_v1.2_sub_engines/` 内 6 个引擎缓存键
  `sm89.trt1012.fp16.win` + `b180`，与本机（cc8.9 / tensorrt 10.12 / fp16 / win /
  `BASICVSRPP_TRT_MAX_CLIP_SIZE=180`）完全匹配 → `create_split_forward` 反序列化成功，
  `restore()` 走 `BasicVSRPlusPlusNetSplit`。
- 加载耗时：首次冷 **21.1s**，OS 缓存暖后 **9.8s**。

### 项1 YOLO（真实 1080p 帧，1920×1080）
- CPU-input 路径：boxes=**1** masks=**1**。
- GPU-input 路径：boxes=**1** masks=**1**，letterbox 正确惰性重建为 `PyTorchLetterBox`
  （两个历史 GPU 预处理修复均生效：BHWC→BCHW permute；PyTorchLetterBox 重建判定 + pad=114
  uint8——GPU 路径检出与 CPU 路径一致，无回归）。
- 吞吐：GPU 常驻输入、batch=4、64 帧含 preprocess+inference+NMS+postprocess：
  **63.9 fps（15.64 ms/帧）**。

### 项3 BasicVSR++（60 帧 256×256 clip）
输出契约两条路径均正确：`len=60`，每帧 `(256,256,3)` `uint8`，数值域 `[0,255]`。

| 路径 | 每 clip | 每帧 | 加速比 |
|---|---|---|---|
| TRT (`BasicVSRPlusPlusNetSplit`) | 284.6 ms | 4.74 ms | — |
| PyTorch (`BasicVSRPlusPlusGan`) | 1063.5 ms | 17.72 ms | 基准 |

TRT/PyTorch ≈ **3.74×**（含每次 clip 的 CPU→GPU 上传 + stack/permute/div 非加速开销；
lada jasna 文档 T=60 报 4.47×，同量级）。

## 4. 与源的差异（有意偏离，均已注释）

1. **`models/basicvsrpp/__init__.py` 去掉 `MosaicVideoDataset` 注册**：它是训练用 DATASETS，
   import 链拖入 `lada.utils.video_utils`（VideoReader/PyAV/pynvc 生产解码，项6，出范围）+
   `lada.datasetcreation`。推理路径（build 模型 / restore）不需要它。已在文件内注释说明。
2. **`sumu/ai/__init__.py` 取代 `lada/__init__.py`**：只保留 ported 代码依赖的
   `LOG_LEVEL` / `ModelFiles` / gettext `_` / albumentations·YOLO 环境变量；丢弃 version-git /
   flatpak / OS-语言检测等 GUI/打包 plumbing。
3. **`model_cache.py` 只取缓存产出路径**：照搬 `_ensure_loaded`（缓存键判定）+ `_clear_cache` +
   `load_models` 调用；**不含** `FrameRestorerProvider.get()` 里构造 `FrameRestorer` /
   `PassthroughFrameRestorer` 的部分（那是「重写」边界，归 sumu 调度器/ready-map）。
4. mmagic `SCOPE` 与 registry `locations/scope` 字符串随包路径改为 `sumu.ai.models.basicvsrpp.mmagic`
   （仅内部一致性，属机械改名）。

## 5. 已知局限 / 未覆盖

- **本次不碰**：Scene/Clip 聚合（项2）、blend-back（项4）、VideoReader 生产解码（项6 类）、
  所有 `_*_worker` / `PipelineQueue` / `FrameRestorer.start/stop`。留给 4d。
- **未接 native/ready-map**（4d）。张量契约保持 device-generic `(H,W,C)` BGR uint8，CPU/CUDA 通吃。
- **TRT 未实测「从零编译」**：本机已有匹配缓存，走命中路径。若删缓存或换 GPU 架构/TRT 版本，
  首次会触发一次多分钟阻塞编译（`load_models(allow_trt_compile=True)` 已设计此路径 + 失败回退
  PyTorch）；本次未强制走编译分支以免长时间阻塞。
- 补丁随 `uv sync` 覆盖需重打（见 §2）。
- torch_tensorrt 启动打印若干 `modelopt/TensorRT-LLM not installed` WARNING，属可选算子提示，
  不影响预编译引擎反序列化与运行。
