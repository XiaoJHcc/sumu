# sumu UI 设计规范（`native/src/ui/`）

本文档描述阶段二建立的 ImGui 设计系统：色板/尺寸层 `ui/theme.h`、控件层
`ui/widgets.h`，以及各页面（player_ui_*.cpp）的使用约定。目标是 **macOS 风格的深色
极简** 观感：低亮度中性底色、细发丝描边（hairline）、统一的 6/8px 圆角语言、单一强调色
（macOS dark system blue），控件本身不携带任何调用点的样式代码。

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
  player_ui_bars.cpp     顶栏/底栏（手绘 seekbar、音量条、图标）+ 设置面板
  player_ui_export.cpp   离线导出页 + 预设编辑器
  player_pybind.cpp      pybind11 模块定义
  ui_util.h/.cpp         纯函数工具（basename、elide、mmss、seekbar 映射），可单测
  ui/theme.h/.cpp        设计系统色板 + 尺寸层（namespace ui::theme）
  ui/widgets.h/.cpp      设计系统控件层（namespace ui），见下文
```

UI 行为约束不变：所有 build_* 只**记录 intent**（`ui_intents_.*` / `record_*()`），
由 Python 主线程统一执行；UI 代码不得直接触碰 transport/scheduler。

## 色板（ui::theme，均为 ImVec4 常量 + 同名 `_u32()` 帮助函数）

| 常量 | 值 | 用途 |
|---|---|---|
| `kWindowBg` | #1E1E20 | 主窗口背景 |
| `kPanelBg` | #262628 | 卡片/弹窗/面板/顶栏底色，比窗口背景亮一档 |
| `kBorder` | 白 @ 12% | 发丝描边（分隔线、Secondary 按钮描边） |
| `kBorderStrong` | 白 @ 22% | 浮层 chrome 描边（modal/loading 卡片外框） |
| `kText` / `kTextSecondary` / `kTextDim` | 白 @ 88% / 55% / 32% | 正文 / 次级说明 / 弱化 |
| `kAccent` / `kAccentHover` / `kAccentActive` | #0A84FF / hover / active | 唯一强调色（Primary 按钮、CheckMark、链接、进度条填充） |
| `kError` / `kWarning` / `kSuccess` | #FF453A / #FFD60A / #32D74B | 错误 / 警告 / 成功文本与状态 |
| `kControlBg` | 白 @ 7% | 输入框/Combo/Checkbox 的 FrameBg |
| `kTrackBg` | 白 @ 10% | 滑杆轨道 |
| `kButtonBg` | 白 @ 8%（hover 12% / active 16%） | Secondary（中性）按钮填充 |
| `kHoverFill` | 白 @ 12% | 手绘命中区（图标按钮）的 hover 浅底 |
| `kIconColor` / `kIconColorDim` | (230,230,230) / (110,110,110) | 手绘 chrome 图标 glyph 色 / 禁用态；seekbar/音量旋钮同用 `kIconColor` |
| `kMediaFill` / `kMediaTrack` | (200,200,60) / (90,90,90) | 手绘媒体条（seekbar/音量）的已播放填充 / 轨道，既定观感，值冻结 |
| `kOverlayBgAlpha` | 0.55（float） | 半透明浮窗（状态浮窗/底栏/设置面板）喂给 `SetNextWindowBgAlpha` |

`apply_theme(ImGuiStyle&)` 在 `Player::ui_init()` 中替代 `StyleColorsDark()`，且在
`ui_style_base_` 快照**之前**调用，保证 `apply_ui_dpi()` 的 `ScaleAllSizes` 重建总是从
这套尺寸出发。

## 尺寸刻度（96 DPI 基准，ScaleAllSizes 统一放大）

- 圆角：`kRadiusControl = 6`（按钮/输入框/Combo/小浮窗）、`kRadiusWindow = 8`（窗口/modal）
- 间距：`kSpaceS/M/L/XL = 4/8/12/16`
- 次级字号：`kFontSizeSm = 16`（配合 `PushFont(nullptr, kFontSizeSm)`；不要用它乘
  `GetFontSize()`，会重复应用 FontScaleDpi）
- 标准控件高 28px（FramePadding (10,5) + 18px 基础字体）

### DPI 规则

- **控件几何走 ImGui style 推导**（GetFrameHeight/CalcTextSize/style vars），
  `apply_ui_dpi()` 已用 `ScaleAllSizes` + `FontScaleDpi` 缩放，控件层自己**不乘** DPI
  因子。
- **布局定宽走 `Player::ui_s()`**（96 DPI 基准 × 缩放），经由各控件尾部的 `width`
  参数传入（`0` = 内容自适应 / 保持调用方的 item width）。
- widgets 层内部需要缩放时用 `ui_scale() = GetFrameHeight() / 28` 作代理。

## 控件 API（namespace ui，`ui/widgets.h`）

所有控件**自行完成全部 PushStyleColor/Var + Pop**，调用点零样式代码。`width` 参数一律
是"调用方已 ui_s 过的像素宽"，`0` 表示不动 item width。

```cpp
enum class ButtonVariant { Primary, Secondary, Danger };
enum class ControlSize { Regular, Small };
```

- `bool Button(label, variant = Secondary, size = Regular, width = 0)`
  按钮层级：Primary = 强调色填充（**每个视图至多一个**，默认动作）；Secondary = 中性
  填充 + hairline 描边；Danger = 破坏性操作（删除/移除），安静底色 + `kError` 文字。
  `Small` 用于紧凑行（如导出队列的每行小按钮）。
  ```cpp
  if (ui::Button("开始导出", ui::ButtonVariant::Primary, ui::ControlSize::Regular, ui_s(120.0f)))
      ui_intents_.export_start = true;
  ```
- `bool TextInput(label, buf, cap, hint = nullptr, width = 0)` /
  `bool IntInput(label, int*, width = 0)` /
  `bool Combo(label, items, count, int* idx, width = 0)`
  标签前置的输入/下拉包装（`label##id` 约定）。注意 `TextInput` 暂无 flags 参数——
  需要 `EnterReturnsTrue` 之类行为时保留裸 `ImGui::InputText` 并注释原因（现有唯一
  一例：open-URL 弹窗的 URL 输入框）。
- `bool Checkbox(label, bool*)` / `bool Radio(label, bool active)`
  Radio 是 bool 形式；替代旧 `RadioButton(int*)` 时需在返回 true 的分支里自己做立即
  赋值（见 player_ui_export.cpp 默认预设标记）。
- `bool SliderInt(label, int*, min, max, width = 0)` /
  `bool SliderFloat(label, float*, min, max, fmt = "%.2f", width = 0)`
- `bool OptionalSlider(check_label, bool* enabled, slider_label, int* v, min, max)`
  "启用 checkbox + 滑杆"耦合（导出预设 CQ 的模式）：未勾选时滑杆灰显
  （BeginDisabled），当前值回显在滑杆后。任一控件变化都返回 true。**注意**它只适合
  有明确区间的值；无界数值（如码率 kbps）仍用 Checkbox + IntInput，不要为它发明区间。
- `void SectionHeader(text)`
  小节标题：小字号 + 次级色，上方 12px 间距、下方 4px + 1px 发丝分隔线。替代
  `PushFont(kFontSizeSm)+TextUnformatted+Separator` 模式。它画整行宽的分隔线，**不能
  与 SameLine 的按钮共行**——那种行（如导出队列标题 + "添加文件"按钮）保持手写。
- `void ProgressBar(frac, width = 0, height = 0)`
  `frac ∈ [0,1]`，`-1` = 不定态动画；`width=0` 填满内容区，`height=0` 用标准框高。
- `void Spinner(id)`
  旋转弧线加载指示（提取自打开 URL 的加载卡片），半径由框高推导。
- `IconButtonResult IconButton(str_id, size, disabled = false)`
  手绘 glyph 图标按钮的自绘命中区（顶/底栏图标）；返回点击结果 + 屏幕矩形 +
  draw list，调用方在其上居中画 glyph，hover 浅底自动绘制。`Player::icon_button()`
  是同签名委托。

### BeginModal / EndModal（统一模态框 chrome）

替代各弹窗重复的自绘标题条 + 关闭 X + 边框。调用方仍持有 `OpenPopup("###id")`
（sticky popup 流程不变）：

```cpp
if (want_open) { ImGui::OpenPopup("###my_popup"); want_open = false; }
bool open = true;
const std::string title = display_title + "###my_popup"; // ### 后的是 ID，不显示
if (ui::BeginModal(title.c_str(), &open, ImVec2(w_base, 0))) {
    // ...body（自动缩进到标题条下方，左 pad = 12px@96DPI）...
    ui::EndModal();
}
```

特性：每帧居中（AlwaysAutoResize 需要一帧稳定内容尺寸）；`kPanelBg` 底、
`kRadiusWindow` 圆角、自绘 1px `kBorderStrong` 边框；标题条含标题文本与
hover 变强调色的关闭 X；Esc 与 X 都会 `CloseCurrentPopup` 并把 `*open` 置 false
（需要清理表单错误标志时在 `BeginModal` 返回后检查 `!open`）。`size_base` 是 96-DPI
基准整体尺寸，`x==0` 保持自动宽度。已有用例：`build_open_url_popup`（含 chromeless
加载卡片的特例）、`build_stream_popup`。

## Dos / Don'ts（新增 UI 时）

- **Do**：一切颜色取 `ui::theme::*` 常量；新颜色先入 theme.h，用语义化命名并注明
  用途，再在页面使用。
- **Do**：一切标准控件走 `ui::` 包装（Button/TextInput/Combo/...）；主操作 Primary、
  破坏性 Danger、其余 Secondary。
- **Do**：模态弹窗走 `ui::BeginModal/EndModal`；小节标题走 `ui::SectionHeader`。
- **Do**：布局定宽用 `ui_s()` 后传给控件的 `width`；控件内部几何不乘 DPI。
- **Don't**：禁止在页面代码写就地 `IM_COL32(...)` / `ImVec4` 色值字面量，禁止就地
  `PushStyleColor` 配色（窗口级的 bg/rounding/padding 覆盖除外，且色值必须来自
  theme）。
- **Don't**：不改行为——intents 记录、校验、字段读写、可见性条件保持原样；设计系统
  只负责"调用哪个函数、用哪个颜色"。
- **例外（现存且合理）**：顶/底栏的手绘图标、seekbar/音量条、缩略图卡片是既定手绘
  媒体观感，保留 ImDrawList 画法；open-URL 的 URL 输入框保留裸
  `ImGui::InputText`（`EnterReturnsTrue` 无包装参数）。
