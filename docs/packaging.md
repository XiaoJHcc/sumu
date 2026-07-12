# 打包与分发（PyInstaller onedir）

sumu 的日常播放器可冻结为一个**自包含 onedir 包**（Windows）。本文记录管线、依赖、验证边界与已知坑。所有数字均为目标机（RTX 4080 · Win11 · torch 2.8.0+cu128）**实测**。

## TL;DR

```powershell
# 一键：native 构建 -> 打第三方补丁 -> PyInstaller 冻结 -> 装配权重/引擎 -> 冒烟
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1
# pyd 已构建且未改时可跳过 native 重建：
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1 -SkipNative
# 只改了 sumu 自己的 Python 源码 / native 扩展（依赖集合没变）时，跳过第 3 步的全量重拷贝：
powershell -ExecutionPolicy Bypass -File scripts/build_dist.ps1 -FastFreeze
```
产物：`dist/sumu/`（含 `sumu.exe` + `_internal/` + `model_weights/`），实测 **≈6.9GB**（不含 TRT 引擎，首启自编；layer-1 剔除 torch `*.lib`/headers + polars/scipy/matplotlib 后）。

### `-FastFreeze`（第 3 步耗时优化）

`packaging/sumu.spec` 里的 `COLLECT` 阶段**每次都无条件清空重建整个 `dist\sumu`**（PyInstaller `COLLECT._check_guts` 恒返回 True，"in order to clean the output directory"——见 spec 文件里的注释），也就是把 `_internal/`（torch/cv2/tensorrt 等 DLL，≈7GB）不管改没改都整份重拷一遍；这是第 3 步"极长"的根因，跟 sumu 自己代码改动大小无关。而 `EXE(exclude_binaries=True)` 本身只把 sumu 自己的 Python 源码/字节码链接成一个 47MB 的瘦 exe 写到 `build\sumu\sumu.exe`，不碰 `dist/`,这一步很快。

`-FastFreeze` 让 `sumu.spec` 靠 `SUMU_FAST_FREEZE` 环境变量跳过 `COLLECT(...)` 调用，只产出新的 `build\sumu\sumu.exe`，再由 `build_dist.ps1` 手动把这个 exe + native 的 pyd/7 个 ffmpeg DLL（这些落在 `dist\sumu\_internal\` 下）直接覆盖拷进已有的 `dist\sumu`，完全不碰 `_internal` 里其余的数 GB 内容。

**边界（重要）**：
- 要求先跑过一次不带 `-FastFreeze` 的完整构建（`dist\sumu\_internal` 必须已存在，否则报错拒绝跑）。
- 只在**依赖集合本身没变**（torch/torchvision/ultralytics/cv2/tensorrt/mmengine 等的 collect_all/collect_dynamic_libs 结果不变）时正确——加了新依赖、升级了这些包、或它们的 DLL/数据文件变了，必须跑一次完整构建才能让 `_internal` 真正更新，`-FastFreeze` 不会捕捉到这类变化。

## 组成与关键文件

- `scripts/sumu_main.py` — PyInstaller 入口（`from sumu.app import main; main()`，无 sys.path hack）。
- `python/sumu/app.py` — frozen-safe 的日常播放器 `main()`（由 `scripts/play.py` dev shim 复用）。
- `python/sumu/pipeline.py` — `build_models`（从 run_player 抽出，dev/frozen 共用）。
- `packaging/sumu.spec` — 冻结配方（见下）。
- `packaging/rthook_dll_path.py` — 运行时 hook，把 bundle 目录塞进 PATH + `os.add_dll_directory`。
- `scripts/build_dist.ps1` — 编排整条管线。
- `scripts/gen_logo_assets.py` — Logo 单源流水线（见下）。
- `assets/sumu-logo-1024.png` — **唯一可改的 Logo 源**（1024×1024 RGBA）；其余尺寸/ico/嵌入用 RGBA 由脚本生成。

## Logo 单源（改一处，打包时自动更新）

| 产物 | 用途 |
|---|---|
| `assets/generated/sumu-logo-256.png` 等 | README 标题图 / 文档 |
| `assets/generated/sumu.ico` | Windows 可执行文件图标（PyInstaller `icon=` 嵌入 PE）+ 窗口/任务栏（`LoadIcon` 读资源 id 1） |
| `assets/generated/sumu-logo-256.rgba` | native 编译时嵌入首屏 ImGui Logo（`native/cmake/embed_binary.cmake`） |

流程：`build_dist.ps1` 第 0 步跑 `gen_logo_assets.py` → native 构建嵌入 `.rgba` → PyInstaller `icon=` 把 `.ico` 写入 `sumu.exe` 的 PE 资源表（Explorer + 运行时 `LoadIcon(hInstance, MAKEINTRESOURCE(1))`）。**分发包不再附带松散 `sumu.ico`**。开发机（pyd 无 PE 图标）仍把 `.ico` 拷到 `python/sumu/` 作 `LoadImage` 回退。



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
- **`excludes=['av', 'polars', 'scipy', 'matplotlib', 'mpl_toolkits']`** —— daily 入口不用 PyAV；后三者是 ultralytics/mmengine 的可选依赖，推理路径不 import。
- **layer-1 TOC 过滤**（`Analysis` 之后，见 spec 里 `_is_layer1_bloat`）：`collect_all` 会无视 `excludes=` 把 binary/data 硬塞进 TOC，所以再剥掉
  - `torch/lib/*.lib`、`torch/include/**`、`torch/testing/**`、`torch/share/**`（~2.7GB，链接/头文件/测试）
  - `polars` / `_polars_runtime_32` / `scipy` / `matplotlib` 残余
  - **保留** `PIL`（ultralytics 启动即 import）与 tcl/tk（`pyi_rth__tkinter` 硬查 `_tcl_data`，缺了直接 `FileNotFoundError`）
  - 目标机 quarantine 冒烟实测：9.78GB → 6.87GB，`== env/load_models/player.open ==` 全过、TRT active
- `console=False`（日常包无黑框；stdout/stderr 由 `scripts/sumu_main.py` 重定向到 `<exe 同级>/sumu.log`）。

## 权重与 TRT 引擎装配

- 冻结态 `sumu.ai._default_model_weights_dir()` 返回 **`<exe 同级>/model_weights`**（`sys.frozen` 分支），该目录须**可写**（TRT 引擎缓存写在 `<weights_dir>/<stem>_sub_engines/`）。
- `build_dist.ps1` 只拷贝**权重**（不含引擎）：
  - `lada_mosaic_restoration_model_generic_v1.2.pth`（≈75MB）
  - `lada_mosaic_detection_model_v4_fast.pt`（≈6MB）
- **TRT 引擎不随包分发**——`hardware_compatible=False` 的引擎只在编译它的那套 GPU 架构 / TRT 版本 / 精度 / OS 上能反序列化（文件名 tag 如 `sm89.trt1012.fp16.win`），预编译引擎只对同款硬件有用。故改为**每台机器首次运行自行编译**：启动 warmup 走 load-only（`build_models(..., allow_trt_compile=False)`，引擎在就用、不在就 eager），首屏「打开文件」按钮下方给出「编译加速引擎」提示，用户点击后后台编译（数分钟，进度条原位显示），编完**热切换立即生效**、且落盘缓存供下次 load-only 直接命中（届时提示不再出现）。编译流程见 `python/sumu/app.py` 的 compile 状态机 + `restorationpipeline.compile_and_activate_trt`。
- 冒烟只验证 `== env == / == load_models == / (== player.open ==)` 三标记 + 无 Traceback；load-only 不再编译，`load_models` 很快，不再断言「引擎复用」时长。
- **权重源目录解析顺序**（其他机器/团队成员构建时不用改脚本）：`-WeightsSrc` 显式参数 → `$env:SUMU_WEIGHTS_SRC` 环境变量（`setx SUMU_WEIGHTS_SRC "C:\path\to\model_weights"` 设一次，跨会话生效）→ 本仓库原开发机路径 `D:/Git/lada-realtime/model_weights` 兜底。缺文件时报错会指出具体缺哪个文件。

## AGPL-3.0 许可证随包

- `build_dist.ps1` 会把仓库根的 `LICENSE.md` 拷进 `dist/sumu/LICENSE.md`，满足"向他人转让程序副本时一并给出许可证"（AGPL-3.0 §4）。自行打包分发（如打 zip）时保留这个文件。
- 本管线**不**处理"提供对应源代码"义务（AGPL §6 的书面要约/网络访问等）——分发给仓库之外的人前，自行确认满足这部分。

## 验证边界（重要）

- **本包不含任何预编译 TRT 引擎**，每台机器首次运行经首屏提示自行编译（数分钟，需 `torch_tensorrt` 运行时 + 可写 `model_weights/`），产物 tag 如 `sm89.trt1012.fp16.win`，只对本机这套 arch/TRT/精度/OS 有效、落盘后下次直接命中。**编译前**去码走 eager PyTorch 回退（能用但约 3x 慢，实时可能追不上→回退原片）；**非 Nvidia / 非 fp16** 机器不触发编译，恒走 eager。
- 整栈只在目标机（RTX 4080 / 驱动 610.47 / py3.13 / torch 2.8.0+cu128）验证过。建议在无 Python/CUDA 的干净机再验一次（需 VC++ 2015+ 运行库）。
- CJK 字体运行时从 `C:\Windows\Fonts` 加载（msyh.ttc…），缺失回退 ASCII——不随包；stripped/N 版 Windows 可能丢中文 UI。

## 已知坑（实测踩过）

- **用 `python -m PyInstaller` 可能命中错误解释器**（uv 的 cpython 而非项目 `.venv`）——`build_dist.ps1` 已固定用 `.venv\Scripts\python.exe -m PyInstaller`。**切勿**并发跑两个构建写同一 `dist/`（会互相覆盖损坏）。
- **首次冷启动慢**：未命中 OS 缓存时，从 ≈10GB 包冷加载 torch CUDA DLL 到 `import torch` 可能 >25s；暖启 <10s。冒烟因此**轮询标记（上限 180s）**而非定时 sleep。
- **`test_video.mp4` 是 .gitignore 的本机测试素材**，别的机器/团队成员 clone 后不带这个文件。冒烟测试检测不到它时会**自动降级**：跳过 `== player.open ==`（播放路径）校验，只验证 exe 能起、torch/CUDA 能 import、模型能 load、无 Traceback；同时打印黄字提醒"未做完整播放路径验证"。不会因为缺这个文件就把整条构建管线判失败。
- smoke 判定失败（含降级模式下的失败）现在会让 `build_dist.ps1` **非零退出**——之前只打印红字但仍 0 退出，VSCode task 面板会一直显示绿色，看不出冒烟其实没过。
- 构建期这些 ERROR/WARNING 均**无害**：`torch._C._jit/_nvrtc/_dynamo not found`（是 `_C` 的属性非独立模块）、大量 `torch.distributed._shard.checkpoint.* not found`（`collect_submodules` 扫到不存在项）、`nvrtc64_120_0.dll required via ctypes not found`（仅影响 JIT，基础推理不受影响）。
- **不要在冻结路径用 `torch.compile`**（dynamo/inductor/triton 在冻结态很脆）；当前 AI 路径走预编译 TRT 引擎 + eager，不触发。
- PowerShell 5.1 对 native 命令做 `2>&1` 会把 stderr 每行包成 NativeCommandError；`build_dist.ps1` 用 `cmd /c "<cmd> 2>&1"` 在 cmd 内部合流规避。
- `.vscode/` 默认在 `.gitignore` 中——若要把 `launch.json`/`tasks.json` 作为共享工程配置纳入版本管理，需在 `.gitignore` 里为这两个文件加 `!` 例外。

## 调试/运行 task（VSCode）

- `.vscode/launch.json`：**Debug sumu (dev)** —— debugpy 起 `scripts/play.py`（提示输入视频，默认 test_video.mp4）。
- `.vscode/tasks.json`：`sumu: run (dev)` / `sumu: build native` / `sumu: apply patches` / `sumu: build dist` —— 均为薄转发到 scripts/ + native/。
