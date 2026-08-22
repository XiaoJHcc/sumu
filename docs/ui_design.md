# sumu UI 设计规范（`native/src/ui/`）

本文档描述 ImGui 设计系统：色板/尺寸层 `ui/theme.h`、控件层 `ui/widgets.h`，以及各
页面（player_ui_*.cpp）的使用约定。目标是 **macOS 风格的深色极简** 观感：低亮度
中性底色、统一的 6/8px 圆角语言、单一背景填充蓝（#3965A8，拉条/勾选框/Primary 按钮
底色共用，蓝底上的文字与勾用白色），控件本身不携带任何
调用点的样式代码。

**描边规则：按钮和卡片一律无描边**（靠填充对比分层）。仅存的描边有两类——浮窗
外框（`kBorderStrong`：modal/loading 卡片/设置面板/下拉弹窗，用于表示显著的层级
区分）和标题栏下边缘/标题条分隔线（`kBorder`，划分标题栏与窗口主体）。

**核心原则：彻底重绘，不打样式补丁。** 所有表单型控件（输入框/下拉/勾选框/拉条/
按钮）的观感与行为由 widgets 层自绘实现并统一度量；页面代码只组合控件，不出现任何
裸 `ImGui::Slider*/Checkbox/Combo/InputInt` 或就地色值。

## 文件布局（阶段一拆分回顾）

`player.cpp` 单文件已在阶段一拆分。`docs/native_core.md` 等旧文档中 "src/player.cpp
晋升后的主体" 的描述已过时，当前布局为：

```
native/src/
  player.h               Player 类完整声明（成员、UiIntents、各 build_* 原型）
  player_session.cpp     会话生命周期：open/reopen/close、ui_init/apply_ui_dpi、ui_tick
  player_window.cpp      Win32 窗口/WndProc、全屏、DWM chrome、present loop 入口
  player_engine.cpp      解码/调度/缩略图 scrub 等引擎侧逻辑
  player_ui_overlays.cpp build_ui() 调度 + 首屏提示/打开 URL/Web 串流弹窗/状态浮窗
  player_ui_bars.cpp     顶栏/底栏（手绘 seekbar、音量条）+ 设置面板
  player_ui_export.cpp   离线导出页（四段卡片式布局）+ 预设管理器/编辑器
  player_pybind.cpp      pybind11 模块定义
  ui_util.h/.cpp         纯函数工具（basename、elide、mmss、seekbar 映射），可单测
  ui/theme.h/.cpp        设计系统色板 + 尺寸层（namespace ui::theme）
  ui/widgets.h/.cpp      设计系统控件层（namespace ui），见下文
  ui/icons.h/.cpp        设计系统图标层：lucide 图集纹理 + ui::AppIcon，见下文
```

UI 行为约束不变：所有 build_* 只**记录 intent**（`ui_intents_.*` / `record_*()`），
由 Python 主线程统一执行；UI 代码不得直接触碰 transport/scheduler。

## 色板（ui::theme，均为 ImVec4 常量 + 同名 `_u32()` 帮助函数）

| 常量 | 值 | 用途 |
|---|---|---|
| `kWindowBg` | #1E1E20 | 主窗口背景 |
| `kPanelBg` | #262628 | 卡片/弹窗/面板底色，比窗口背景亮一档；**所有容器背景统一用它**（设置面板等浮窗也已改为实心 `kPanelBg`，不再半透明） |
| `kBorder` | 白 @ 12% | 发丝线（标题栏下边缘、modal 标题条分隔线、下拉弹窗外框、未勾选 Checkbox 描边） |
| `kBorderStrong` | 白 @ 22% | 浮窗外框（modal / loading 卡片 / 设置面板，表示层级区分） |
| `kText` / `kTextSecondary` / `kTextDim` | 白 @ 88% / 55% / 32% | 正文 / 次级说明（hint、拉条端文字、摘要） / 弱化 |
| `kAccent` / `kAccentHover` / `kAccentActive` | #3965A8 / hover / active | 背景填充蓝（拉条填充、勾选框勾选态、Primary 按钮底色共用；另有 Combo 选中勾、链接）。蓝底上的标记（按钮文字、勾选框的勾）一律用白色 |
| `kError` / `kWarning` / `kSuccess` | #E65050 / #FFD60A / #32D74B | 错误 / 警告 / 成功文本与状态 |
| `kControlBg` | 白 @ 7% | 输入框/Combo/未勾选 Checkbox 的填充，**比卡片背景浅一档** |
| `kRowCardBg` | 白 @ 5% | 嵌套列表行卡片填充（导出预设行、队列项卡片——位于 `kPanelBg` 卡片之内，只亮一丝） |
| `kTrackBg` | 白 @ 10% | 拉条轨道 |
| `kButtonBg` | 白 @ 8%（hover 12% / active 16%） | Secondary（中性）按钮填充 |
| `kHoverFill` | 白 @ 12% | 手绘命中区（图标按钮、Combo 下拉行）的 hover 浅底 |
| `kIconColor` / `kIconColorDim` | (230,230,230) / (110,110,110) | 手绘 chrome 图标 glyph 色 / 弱化态；seekbar/音量旋钮同用 `kIconColor` |
| `kMediaFill` / `kMediaTrack` | (200,200,60) / (90,90,90) | 手绘媒体条（seekbar/音量）的已播放填充 / 轨道，既定观感，值冻结 |
| `kOverlayBgAlpha` | 0.55（float） | 状态浮窗与开屏编译卡片的半透明 alpha |
| `kDimBg` | 黑 @ 50% | 模态遮罩（`ModalWindowDimBg`/`NavWindowingDimBg`；`apply_theme` 已装为全局默认，页面无需再 push） |

`apply_theme(ImGuiStyle&)` 在 `Player::ui_init()` 中替代 `StyleColorsDark()`，且在
`ui_style_base_` 快照**之前**调用，保证 `apply_ui_dpi()` 的 `ScaleAllSizes` 重建总是从
这套尺寸出发。

## 尺寸刻度（96 DPI 基准，ScaleAllSizes 统一放大）

### 容器

- **`kPaddingContainer = 12`：所有卡片/弹窗/面板必须保证的最小内边距**（四边）。
  控件永远不得贴容器边缘（旧 URL 弹窗输入框右侧贴边即反例，已由 BeginModal 的钉宽
  机制根治）。
- **卡片** = `ui::BeginCard`：`kPanelBg` 底 + `kRadiusWindow` 圆角 + 四边 12px padding，
  **无描边**（靠与窗口底的填充对比分层）。
- 弹窗 = `ui::BeginModal`：宽度钉死为 `内容宽 + 2×12`，内容列右缘距窗口右缘恒为
  12px。标准内容宽 `kModalContentW = 440`；宽表单（预设编辑器）用
  `kModalContentWLg = 480`。**新弹窗一律从这两个宽度里选。** 标题条高度统一为
  `kModalTitleH = 36`（与主窗口顶栏 `kTopBarHBase` 同高，全 App 一种标题条；内容区
  右缘由 WindowPadding 机制内缩，任何"填满剩余宽"的控件都不可能贴边）。

### 标题栏

- 高度 `kTopBarHBase = 36`（modal 标题条 `kModalTitleH` 同高）；图标按钮 32×32，
  四边等距 `kSpaceXS = 2`（边距、按钮间距、垂直居中全部读这一个刻度）。
  底栏高度与标题栏一致（共用 `top_bar_h()`）。

### 图标按钮（标准规格）

- **全 App 一种图标按钮：`kControlHeight`（32px @ 96 DPI）方块 + 7/16 glyph（14px）**。
  顶栏、底栏、modal 关闭、导出页删除/文件夹按钮全部遵守；
  chrome 代码写 `ui_s(kControlHeight)`（顶栏 btn_h 从 36px 栏高减 2×`kSpaceXS` 推出，
  同为 32），行内场景用 `GetFrameHeight()`（同值，且与所在行对齐）。
- 底色两种场合：**chrome/列表行内**用裸命中区 + hover 浅底（`kHoverFill`）；
  **表单行内**（与输入框等带框控件同行，如路径选择按钮）用 `ui::IconButtonFramed`，
  底色 = Secondary 按钮梯度（`kButtonBg` / hover 白 12% / active 白 16%）。
- **破坏性操作（关闭/删除）hover 变红**：glyph hover 时翻 `kError`——预设删除、队列
  移除、主窗口关闭、modal 关闭 X 全部遵守（平时 chrome 用 `kIconColor`，列表行内删除
  用 `kIconColorDim`）。
- 新图标按钮不允许自定义尺寸；确需例外时先在这里登记理由。

### 单行控件（输入框 / 下拉 / 按钮共用）

- **`kControlHeight = 32`**（FramePadding (10,7) + 18px 基础字体），圆角
  `kRadiusControl = 6`，填充 `kControlBg`，无边框。按钮与输入框同高——比旧版 28px 更高。
- 窗口圆角 `kRadiusWindow = 8`。
- 数值输入小列宽 `kNumericInputW = 72`（拉条旁的数字框、混排行输入框）。

### 拉条（macOS 风格）

- 轨道 `kSliderTrackH = 4`（细），填充 `kAccent`（拉出蓝），拉杆为
  `kSliderKnobR = 9` 的大圆形浅色钮（带 1px 暗描边）。
- 默认形态 = **数字输入框 + 纯拉条** 的组合行；需要指明两端方向的拉条（CQ）用
  `SliderIntEnds`，左右两端为**小字号文字**（不用图标）。

### 勾选框

- `kCheckboxSize = 16`、圆角 `kRadiusCheckbox = 4`，约与 label 文字同高。
- 勾选态：`kAccent` 底 + 深色对勾（柔和蓝太浅，白对勾对比不足）；未勾选：`kControlBg`
  底 + hairline 描边。

### 下拉菜单

- 关闭态与输入框完全一致，仅右侧一个小三角（`kComboArrowW/H = 9×5`）。
- 弹出层 = `kPanelBg` 圆角面板 + hairline 描边，逐行卡片：正常态与面板同色（**无
  填充**），仅 hover 行亮起 `kHoverFill`；当前项行首有强调色小勾。

### 间距与字号

- 间距：`kSpaceXS/S/M/L/XL = 2/4/8/12/16`。`ItemSpacing = (kSpaceM, kSpaceM)`，即行内
  间距与行间距同为 8px——行间距不得小于行内间距，否则纵向堆叠的行会糊成一团；
  拉条行内"输入框→轨道"的间距单独用 `kSpaceL`（12px）。
- 字号：标准 = `kFontSizeBase = 18`（小节标题、行内 label、控件文字）；小型 =
  `kFontSizeSm = 16`（行前 label、hint、摘要、拉条端文字）。配合
  `PushFont(nullptr, size)`；不要乘 `GetFontSize()`（会重复应用 FontScaleDpi）。
- 目前字体只载入了常规字重，小节标题靠"标准字号 + 全不透明白 + 间距层级"区分，
  **不使用分割线**。如未来需要加粗，需额外加载粗体字重。

### DPI 规则

- **控件几何走 ImGui style 推导**（GetFrameHeight/CalcTextSize/style vars），
  `apply_ui_dpi()` 已用 `ScaleAllSizes` + `FontScaleDpi` 缩放，控件层自己**不乘**
  DPI 因子。
- **布局定宽走 `Player::ui_s()`**（96 DPI 基准 × 缩放），经由各控件尾部的 `width`
  参数传入（`0` = 填满剩余内容宽 / 保持调用方的 item width）。
- widgets 层内部需要固定像素度量时用 `ui_scale()` —— 它返回的是
  `Player::apply_ui_dpi()` 通过 `ui::set_ui_scale()` 推入的**权威显示器缩放**。
  **禁止**再从 `GetFrameHeight() / 32` 之类的光栅化字体度量反推：字体光栅化会把
  27px 请求对齐成 26px，代理值（≈1.469）与真实缩放（1.5）混用会使"窗口宽用代理、
  内容宽用真值"的算式相差十几像素，控件直接贴到窗口右缘（模态框右边距屡修屡错的
  根因）。

## 标签三分法

| 种类 | 控件 | 字号 | 用法 |
|---|---|---|---|
| 小节标题 | `ui::SectionHeader(text)` | 标准 18，`kText` | 卡片/面板内的小节名（如"设置"、"预设"）。上间距 12、下间距 8，**无分割线**。 |
| 行前 label | `ui::LineLabel(text)` | 小型 16 | 较长 label，独占一行位于控件上方（缓冲窗口（帧）、粘贴 https 直链）。**上间距宽（12）、下间距窄**。 |
| 行内 label | `ui::InlineLabel(text, width = 0)` | 标准 18 | 短 label，与控件同行（名称、编码格式）。`width > 0` 时占固定列宽（文字垂直居中、裁切），用于多行对齐。**以 SameLine 收尾**（职责是引出后续控件）。 |
| 单位文本 | `ui::UnitText(text)` | 标准 18，次级色 | 行尾的单位（kbps 等），垂直居中，**不拖 SameLine**——行终结符。 |

**行契约（防止整行被顶出可视区的链式泄漏）**：一行内除 `InlineLabel` 外，任何控件
/辅助文本都不得以挂起的 SameLine 收尾；行间的新行由下一行的第一个控件自然开始。
混合行的垂直居中一律用"占位 Dummy + 文本居中绘制"或 `SetCursorPosY` 原位下移，
**禁止用 Dummy 做行内垂直偏移**（Dummy 会结束 SameLine 行，把后续控件换行顶出
卡片）。定高卡片子窗口一律带 `NoScrollbar | NoScrollWithMouse`：内容必须恰好容纳，
溢出宁可裁切也不得长出卡内滚动条。

`SectionHeader`/`LineLabel` 位于容器顶部时自动跳过上方间距（卡片 padding 已提供
内边距）。小节标题要与按钮共行时（如"视频队列 + 添加文件"），用定宽
`InlineLabel` + 尾随按钮实现，不要再手写 PushFont。

## 控件 API（namespace ui，`ui/widgets.h`）

所有控件**自行完成全部 PushStyleColor/Var + Pop**，调用点零样式代码。`width` 参数一律
是"调用方已 ui_s 过的像素宽"，`0` 表示填满剩余内容宽。所有控件遵循 `label##id`
约定：可见部分非空时作为**行内 label 画在控件左侧**。

```cpp
enum class ButtonVariant { Primary, Secondary, Danger };
enum class ControlSize { Regular, Small };
```

- `bool Button(label, variant = Secondary, size = Regular, width = 0)`
  Primary = 柔和蓝 chip（半透明 `kAccent` 底 + `kAccent` 文字，**每个视图至多一个**，
  默认动作）；Secondary = 中性填充；Danger = 破坏性操作，安静底色 + `kError` 文字。
  **全部无描边。** `Small` 用于紧凑行（导出队列的每行小按钮）。
- `bool TextInput(label, buf, cap, hint = nullptr, width = 0, flags = 0)`
  输入框。`flags` 直传 ImGui（`EnterReturnsTrue`、`ReadOnly` 等）。
- `bool IntInput(label, int*, width = 0)`
  数值输入框，**无 +/- 步进按钮**（全站统一去掉）。
- `bool Combo(label, items, count, int* idx, width = 0)`
  自绘下拉（见"下拉菜单"一节）。
- `bool Checkbox(label, bool*)` / `bool Radio(label, bool active)`
  自绘 macOS 勾选框（见"勾选框"一节）。
- `bool SliderInt(label, int*, min, max, width = 0, bool* committed = nullptr)` /
  `bool SliderFloat(label, float*, min, max, fmt = "%.2f", width = 0, committed = nullptr)` /
  `bool SliderIntEnds(label, int*, min, max, left_text, right_text, width = 0, committed = nullptr)`
  自绘 macOS 拉条：数字框 + 细轨道 + 大圆钮。`SliderIntEnds` 两端带小字（仅用于
  CQ 这类方向易混淆的拉条）。返回值 = 值本帧变化；**`committed` 在编辑结束的那一帧
  置 true**（拖动松手 / 数字框失焦或回车），替代旧的 `IsItemDeactivatedAfterEdit()`
  ——后者无法覆盖组合控件。提交语义一律用：
  ```cpp
  bool committed = false;
  ui::SliderInt("##lead", &edit_, 1, 180, 0.0f, &committed);
  if (committed && edit_ != cfg_) ui_intents_.lead = edit_;
  ```
- `void ProgressBar(frac, width = 0, height = 0)`：`frac ∈ [0,1]`，`-1` = 不定态。
- `void Spinner(id)`：旋转弧线加载指示。
- `IconButtonResult IconButton(str_id, size, disabled = false)`
  图标按钮命中区（无 glyph 版）；返回点击结果 + 屏幕矩形 + draw list，hover 浅底自动
  绘制。仅配合 `DrawIconButtonGlyph` 用于自定义 tint 的场合（导出页删除按钮：
  平时 `kIconColorDim`、hover 变 `kError`）。
- `IconButtonResult IconButton(str_id, size, AppIcon, disabled = false)`（**首选**）
  同上，但自动居中绘制 lucide 图集 glyph（按钮边长的 7/16，32px 按钮 → 14px 图标），
  着色 `kIconColor` / 禁用态 `kIconColorDim`。图集不可用时退化为纯命中区。
  `Player::icon_button()` 是两个重载的同签名委托。
- `IconButtonResult IconButtonFramed(str_id, size, AppIcon, disabled = false)`
  带底 variant：Secondary 按钮梯度底色（`kButtonBg`/hover 白 12%/active 白 16%），
  用于表单行内的图标按钮（导出路径、web 串流根目录的文件夹选择按钮）。
- `void DrawIconButtonGlyph(result, AppIcon, tint)`：给无 glyph 版命中区补画图标，
  居中与 7/16 比例与首选重载一致，仅 tint 由调用方决定。

### 图标（ui/icons.*，lucide 图集）

- 图标一律来自 **lucide**（ISC，`assets/icons/lucide/*.svg`，署名见该目录 README），
  不再手绘。`scripts/gen_icon_atlas.py` 把 SVG 光栅化成白色 RGBA 图集
  （`assets/generated/icons_atlas.rgba`），CMake 嵌入 pyd，`ui::icons::draw()` 以
  tint 着色绘制——禁用/hover 变色都是 tint，SVG 永远保持白色。
- 渲染质量三原则（别回退）：**不做 GPU mipmap**——2 的幂次 mip 网格几乎永远对不上
  实际绘制尺寸（150% 时 27px 图标会混 1.78x 和 0.89x 两级），保证不了点对点也保证
  不了 2 倍超采；改为**运行时按当前物理尺寸 CPU 面积加权重采样**（96px 矢量光栅源
  → 精确目标尺寸，等价 ~13 源像素/目标像素的积分超采，每种尺寸一次并缓存纹理）；
  **绘制坐标吸附到整数物理像素**，纹理与绘制矩形 1:1（150% 等分数缩放下居中矩形
  会落在 .5 坐标上）。
- 新增图标：往 `assets/icons/lucide/` 放 SVG → 在脚本的 `ICONS` 列表和
  `ui::AppIcon` 枚举**同序**追加 → 重跑脚本 → 编译。
- 当前枚举：Settings / OpenFile / OpenUrl / WebServer / Export / WinMinimize /
  WinMaximize / WinRestore / Fullscreen / Close（顶栏 9 个 + modal 关闭 X 共用）+
  Play / Pause / Volume / VolumeMute（底栏播放与静音，播放键图标随状态切换，
  语义与顶栏一致：32px 按钮、7/16 glyph、2px 边距）+ Trash / FolderInput（导出页：
  预设删除、队列项删除（X 复用 Close）、路径选择文件夹，按钮统一为行高方块，
  删除类平时 `kIconColorDim`、hover `kError`，走 `DrawIconButtonGlyph`）。

### BeginCard / EndCard（卡片容器）

```cpp
if (ui::BeginCard("##card_id")) {   // kPanelBg + 8px 圆角 + 12px 四边 padding，无描边
    ui::SectionHeader("小节");
    // ...
}
ui::EndCard();
```

`height = 0` 时高度自适应内容。卡片之间用
`Dummy(0, ui_s(kSpaceL) - ItemSpacing.y)` 保持 12px 节奏。

> **裸 `BeginChild` 的 padding 陷阱**：ImGui 对无边框子窗口强制把 `WindowPadding`
> 归零（见 imgui.cpp `Begin`：`ChildWindow && !AlwaysUseWindowPadding &&
> WindowBorderSize == 0` → padding = 0），之前 push 的 padding 会被静默丢弃，内容
> 直接贴到子窗口顶角。凡是为裸 `BeginChild` push 了非零 WindowPadding 的地方，必须
> 传 `ImGuiChildFlags_AlwaysUseWindowPadding`（预设行卡、队列项卡即此模式；`BeginCard`
> 去描边后同样自带该 flag）。普通窗口/弹窗不受影响。

### BeginModal / EndModal（统一模态框 chrome）

标准两段式：标题条（标题文本 + 关闭 X）+ 内容区。宽度钉死、高度自适应，四边
12px 内边距由机制保证。调用方仍持有 `OpenPopup("###id")`（sticky popup 流程）：

```cpp
if (want_open) { ImGui::OpenPopup("###my_popup"); want_open = false; }
bool open = true;
const std::string title = display_title + "###my_popup"; // ### 后的是 ID，不显示
if (ui::BeginModal(title.c_str(), &open)) {   // 默认内容宽 kModalContentW = 440
    // ...body（item width 已预设为整个内容列）...
    ui::EndModal();
}
```

Esc 与 X 都会 `CloseCurrentPopup` 并把 `*open` 置 false（需要清理表单错误标志时在
`BeginModal` 返回后检查 `!open`）。已有用例：open-URL（含 chromeless 加载卡片特例）、
Web 串流、预设编辑器。

## 页面布局约定

### 导出页（`build_export_screen`）

左右分栏（左列定宽 320px，右列填满剩余宽；两列各自滚动，外窗不滚动）：

- **左列**，两张卡片 + 无卡操作区：
  1. **设置**（AI 管线 + 导出路径合并）—— SectionHeader"设置" + 两行小字 LineLabel
     （"去码模型处理片段长度（帧）"、"全局默认导出路径"）：片段长度 SliderInt；只读
     输入框（空时 hint "-"）+ 文件夹图标按钮同行。
  2. **预设**（一级卡片，高度 = 左列剩余空间，使左列整体永不滚动）—— 预设列表为
     卡片行（卡内：默认勾选框 / 名称+摘要点击进编辑 / 末尾垃圾桶 icon，全部以整行高
     为命中区垂直居中），行高 = 单行容器标准 36px（`kTopBarHBase`，与标题栏统一）。
     边距按各自可视尺寸定：垃圾桶按钮（32px）上右下 = (36-32)/2 = 2px 均等；勾选框
     可视盒子 16px，上左下右 = (36-16)/2 = 10px 均等。滚动区容器底色 = 底层背景标准
     `kWindowBg`（#1E1E20，滚动条轨道同值），内边距 `kSpaceM`（8px）四边均等——
     滚动条贴容器右边（`ScrollbarPadding=0`，滑块不再内缩，否则右边距会多读一个
     内缩量；`ScrollbarSize=8` 细滚动条），右 padding 留出它与卡片间的间距。卡片间
     距 = 单个 `kSpaceM`（列表内 `ItemSpacing.y` 归零，由 Dummy 单算，避免 Dummy 两侧
     各叠一次 ItemSpacing 的双重间距）。底部固定全宽"新建预设"按钮。
     **管理不再是弹窗**；只有预设编辑器仍是 `BeginModal`（内容宽 480）。
  - **右列**：**视频队列**整列高卡片。标题（SectionHeader）+ 滚动队列表格（`kWindowBg` +
  `kSpaceM` 内边距 + 细滚动条，卡片间距 = `kSpaceM` 8px，由 `block_gap` 对光标做
  nudge（`ItemSpacing.y` 保持全局值，队列项卡内部行距依赖它，不得归零）；镜像预设列表
  版面）+ 底部固定一行两按钮：**添加文件**
  Secondary 与 **[开始导出]** Primary 各占半列宽，按钮间距 = `kPaddingContainer` 12px
  （同卡片四边、新建预设按钮上下边距，全局统一节奏；引擎未就绪时灰显）——不再占用
  标题行，避免撑高标题行破坏边距。每个队列项一张卡：
  - 第 0 栏：拖拽 grip（六点 icon）——拖拽排序（`export_move_id` + `export_move_to`
    intent，Python `ExportQueue.move_to()`；落到某卡 = 插到它前面，落到底部拖放条 =
    移到队尾，拖动悬停的卡显示 accent 描边）。**已废弃上移/下移按钮。**
  - 第 1 栏：状态（小字次级色）/ 文件名（elide，hover 出全路径 tooltip）/ 导出路径
    （小字次级色，"→ "前缀）；进行中卡片底部加 6px 细进度条。
  - 第 2 栏：预设下拉 + 输出方式下拉（定宽 150px，堆叠）。
  - 第 3 栏：删除灰叉 icon（`kIconColorDim`，hover 变 `kError`），垂直居中。
    **已废弃每项的取消按钮。**

### 预设编辑器（`build_export_preset_editor`）

- 标准 `BeginModal`（内容宽 480），标题条 + 内容两段式，无内部分割线。
  `open_export_preset_editor(idx)` 通过 `export_presets_open_` 单次触发 OpenPopup；
  保存/取消/X/Esc 都会关闭弹窗并把 `export_preset_edit_idx_` 归 -1。
- 编辑器：短 label 行（名称/编码格式/速度/音频/后缀）用定宽列 `InlineLabel` + 控件填满
  剩余宽。混排行（CQ/码率/最大码率）顺序固定为 **勾选框 → 行内 label → 数字输入框 →
  拉条/单位**，三行的勾选框列、label 列（80px）、输入框列（72px）严格对齐；未勾选时
  该行的输入/拉条 BeginDisabled 灰显。CQ 拉条两端文字"高质量 / 小体积"（低数值 = 高
  质量，易混淆）。保存/取消为等宽半行按钮（修复旧版宽度溢出）。

## Dos / Don'ts（新增 UI 时）

- **Do**：一切颜色取 `ui::theme::*` 常量；新颜色先入 theme.h，用语义化命名并注明
  用途，再在页面使用。
- **Do**：一切标准控件走 `ui::` 包装；主操作 Primary、破坏性 Danger、其余 Secondary。
- **Do**：容器走 `ui::BeginCard` / `ui::BeginModal`；标题/标签用三分法控件；宽度从
  `kModalContentW` / `kModalContentWLg` 中选。
- **Do**：布局定宽用 `ui_s()` 后传给控件的 `width`；控件内部几何不乘 DPI。
- **Do**：拉条提交用 `committed` 出参；下拉/勾选变更即时提交（它们没有拖拽态）。
- **Don't**：禁止在页面代码写就地 `IM_COL32(...)` / `ImVec4` 色值字面量，禁止就地
  `PushStyleColor` 配色（文字语义色如 `kError`/`kTextSecondary` 的 PushFont/Pop 组合
  除外）；禁止裸 `ImGui::Slider*/Checkbox/Combo/InputInt`；禁止 `ImGui::Separator()`
  （间距用 Dummy/间距刻度表达）。
- **Don't**：不改行为——intents 记录、校验、字段读写、可见性条件保持原样；设计系统
  只负责"调用哪个函数、用哪个颜色"。
- **例外（现存且合理）**：seekbar/音量条、导出队列的六点拖拽 grip 是既定手绘媒体
  观感，保留 ImDrawList 画法；颜色仍取主题常量。seekbar 悬停缩略图
  卡片同样保留手绘，但底色 = `kPanelBg` @ 0.90、圆角 = `kRadiusControl`、内边距 =
  `kSpaceS`，与其他卡片一样无描边。

## 布局验证

改了导出页/预设编辑器布局后，跑 `scripts/shot_export_ui.py` 实证：它开一个无视频、
无 AI 引擎的裸 Player 窗口，注入合成快照（6 预设 + 3 种状态的队列项），先截导出页
全图，再自动点击首个预设行截编辑器弹窗（`native/trace/export_ui_shot*.png`）。肉眼
检查：滚动条条数、垂直居中、label 列对齐、四边 12px 边距。
