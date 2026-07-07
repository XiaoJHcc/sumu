# 打包与分发（PyInstaller onedir）

sumu 的日常播放器可冻结为一个**自包含 onedir 包**（Windows）。本文记录管线、依赖、验证边界与已知坑。所有数字均为目标机（RTX 4080 · Win11 · torch 2.8.0+cu128）**实测**。

## TL;DR

```powershell
# 一键：native 构建 -> 打第三方补丁 -> PyInstaller 冻结 -> 装配权重/引擎 -> 冒烟
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1
# pyd 已构建且未改时可跳过 native 重建：
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1 -SkipNative
```
产物：`dist/sumu/`（含 `sumu.exe` + `_internal/` + `model_weights/`），实测 **≈9.8GB**。

## 组成与关键文件

- `scripts/sumu_main.py` — PyInstaller 入口（`from sumu.app import main; main()`，无 sys.path hack）。
- `python/sumu/app.py` — frozen-safe 的日常播放器 `main()`（由 `scripts/play.py` dev shim 复用）。
- `python/sumu/pipeline.py` — `build_models`（从 run_player 抽出，dev/frozen 共用）。
- `packaging/sumu.spec` — 冻结配方（见下）。
- `packaging/rthook_dll_path.py` — 运行时 hook，把 bundle 目录塞进 PATH + `os.add_dll_directory`。
- `scripts/build_dist.ps1` — 编排整条管线。

## 依赖（构建期）

- **PyInstaller 6.21.0 + pyinstaller-hooks-contrib 2026.6**（`.venv/Scripts/python.exe -m pip install pyinstaller pyinstaller-hooks-contrib`）。
- native pyd 需 VS2022 BuildTools（`native/build.bat`）。补丁步骤需 git-bash（`scripts/apply_patches.sh`）。

## spec 要点（`packaging/sumu.spec`）

- **onedir**（`COLLECT`），**全程 `upx=False`**（UPX 会毁 CUDA/TRT 的 CFG DLL）。
- `sys.setrecursionlimit(*5)`（torch 分析会爆默认递归深度）。
- `collect_all`：torch / torchvision / ultralytics / cv2。
- **TensorRT 三件套无自动 hook**，显式 `collect_dynamic_libs` + `collect_submodules`：torch_tensorrt / tensorrt / tensorrt_libs。
- 额外 `collect_dynamic_libs('torch')`（Windows 下 CUDA DLL 在 `torch/lib` 内）。
- mmengine：`collect_submodules` + `collect_data_files`；`copy_metadata`(torch,torchvision,numpy,ultralytics,mmengine)。
- **native ext + 7 个 ffmpeg DLL 作为 `binaries` 落到 bundle 根 `.`**（pyd 靠同目录加载 ffmpeg，见 `docs/native_core.md`）。
- **`excludes=['av']`** —— daily 入口不用 PyAV（只 run_player 的 `--correctness` 用），排除可避免与自带 ffmpeg DLL 冲突并瘦身。
- `console=True`（bringup 期便于读 traceback）。

## 权重与 TRT 引擎装配

- 冻结态 `sumu.ai._default_model_weights_dir()` 返回 **`<exe 同级>/model_weights`**（`sys.frozen` 分支），该目录须**可写**（TRT 引擎缓存写在 `<weights_dir>/<stem>_sub_engines/`）。
- `build_dist.ps1` 从 `-WeightsSrc`（默认 `D:/Git/lada-realtime/model_weights`）拷贝**默认路径所需的**：
  - `lada_mosaic_restoration_model_generic_v1.2.pth`（≈75MB）
  - `lada_mosaic_detection_model_v4_fast.pt`（≈6MB）
  - `lada_mosaic_restoration_model_generic_v1.2_sub_engines/`（6 个引擎，≈520MB）
- 冒烟实测：`load_models` **4.75s**（与 dev 4.76s 一致）——预编译引擎被**复用而非重编**（重编需数分钟）。

## 验证边界（重要）

- **预编译 TRT 引擎只对 sm89 = Ada（RTX 40 系）+ TensorRT 10.12 + fp16 + Windows 有效**（引擎文件名 tag `sm89.trt1012.fp16.win`）。**换 GPU 架构**：首启会删除该缓存并**重新编译**（数分钟，需 `torch_tensorrt` 运行时 + 可写 `model_weights/`）。故本包默认只保证**同类硬件（RTX 40 系）**开箱即用。
- 整栈只在目标机（RTX 4080 / 驱动 610.47 / py3.13 / torch 2.8.0+cu128）验证过。建议在无 Python/CUDA 的干净机再验一次（需 VC++ 2015+ 运行库）。
- CJK 字体运行时从 `C:\Windows\Fonts` 加载（msyh.ttc…），缺失回退 ASCII——不随包；stripped/N 版 Windows 可能丢中文 UI。

## 已知坑（实测踩过）

- **用 `python -m PyInstaller` 可能命中错误解释器**（uv 的 cpython 而非项目 `.venv`）——`build_dist.ps1` 已固定用 `.venv\Scripts\python.exe -m PyInstaller`。**切勿**并发跑两个构建写同一 `dist/`（会互相覆盖损坏）。
- **首次冷启动慢**：未命中 OS 缓存时，从 ≈10GB 包冷加载 torch CUDA DLL 到 `import torch` 可能 >25s；暖启 <10s。冒烟因此**轮询 `== player.open ==` 标记（上限 120s）**而非定时 sleep。
- 构建期这些 ERROR/WARNING 均**无害**：`torch._C._jit/_nvrtc/_dynamo not found`（是 `_C` 的属性非独立模块）、大量 `torch.distributed._shard.checkpoint.* not found`（`collect_submodules` 扫到不存在项）、`nvrtc64_120_0.dll required via ctypes not found`（仅影响 JIT，基础推理不受影响）。
- **不要在冻结路径用 `torch.compile`**（dynamo/inductor/triton 在冻结态很脆）；当前 AI 路径走预编译 TRT 引擎 + eager，不触发。
- PowerShell 5.1 对 native 命令做 `2>&1` 会把 stderr 每行包成 NativeCommandError；`build_dist.ps1` 用 `cmd /c "<cmd> 2>&1"` 在 cmd 内部合流规避。
- `.vscode/` 默认在 `.gitignore` 中——若要把 `launch.json`/`tasks.json` 作为共享工程配置纳入版本管理，需在 `.gitignore` 里为这两个文件加 `!` 例外。

## 调试/运行 task（VSCode）

- `.vscode/launch.json`：**Debug sumu (dev)** —— debugpy 起 `scripts/play.py`（提示输入视频，默认 test_video.mp4）。
- `.vscode/tasks.json`：`sumu: run (dev)` / `sumu: build native` / `sumu: apply patches` / `sumu: build dist` —— 均为薄转发到 scripts/ + native/。
