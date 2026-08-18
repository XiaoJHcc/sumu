// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// macOS-style dark design system -- widget layer. Every function does ALL its own
// PushStyleColor/Var + Pop internally: call sites carry zero styling code. Pure ImGui +
// theme.h dependency (never player.h), so it can be unit-exercised without a Player.
//
// Design contract (see docs/ui_design.md):
//   - Single-line controls (TextInput / IntInput / Combo / Button) share one height,
//     rounding, fill and border language (theme::kControlHeight / kRadiusControl /
//     kControlBg), 32px @ 96 DPI.
//   - Sliders / checkboxes / combo popups are CUSTOM-DRAWN (macOS language: thin track +
//     big round knob, small accent checkbox, hover-only popup rows). ImGui's stock
//     rendering for these is not used anywhere.
//   - Labels come in three kinds: SectionHeader (standard font, no rule), LineLabel
//     (small font above a field, wide top / narrow bottom gap), InlineLabel (standard
//     font, same row as the control).
//
// DPI principle: control geometry is derived from the ImGui style (GetFrameHeight(),
// CalcTextSize(), style vars) which apply_ui_dpi() already scaled via ScaleAllSizes +
// FontScaleDpi -- nothing here multiplies by a DPI factor itself, except the ui_scale()
// proxy for the few fixed pixel metrics. Callers that need a fixed width keep wrapping
// with Player::ui_s() and pass it via the trailing `width` argument (0 = fill the
// remaining content width).

#pragma once

#include "imgui.h"
#include "theme.h"

#include <cstddef>

namespace ui {

// ---- buttons ---------------------------------------------------------------------------------

// Button hierarchy: Primary = accent-filled (one per view, the default action),
// Secondary = neutral fill + hairline border, Danger = destructive (delete/remove),
// error-tinted text on a quiet fill (macOS destructive-button language).
enum class ButtonVariant { Primary, Secondary, Danger };
// Small = compact rows like the export queue's per-item buttons.
enum class ControlSize { Regular, Small };

// Click -> true. width == 0 sizes to the label + FramePadding; pass an explicit (already
// ui_s-scaled by the caller) width otherwise.
bool Button(const char* label, ButtonVariant v = ButtonVariant::Secondary,
            ControlSize s = ControlSize::Regular, float width = 0.0f);

// ---- single-line field controls ---------------------------------------------------------------
// All take the ImGui "label##id" convention. width == 0 fills the remaining content-region
// width (SetNextItemWidth(-FLT_MIN) equivalent); a visible (non-"##") label part is drawn
// as an InlineLabel to the LEFT of the control.

bool TextInput(const char* label, char* buf, size_t cap, const char* hint = nullptr,
               float width = 0.0f, ImGuiInputTextFlags flags = 0);
// No +/- steppers anywhere (step == 0 hides ImGui's buttons).
bool IntInput(const char* label, int* v, float width = 0.0f);
// Custom combo: input-styled frame + small triangle; popup = rounded panel whose rows are
// flat (same fill as the popup background) until hovered (hover-fill lights up); the
// current item carries a small leading check. Click-select only.
bool Combo(const char* label, const char* const items[], int count, int* idx,
           float width = 0.0f);

// Custom macOS checkbox: 16px rounded square about as tall as the label text -- accent
// fill + white check when on, quiet fill + hairline border when off.
bool Checkbox(const char* label, bool* v);
bool Radio(const char* label, bool active);

// ---- sliders (custom, macOS style) -------------------------------------------------------------
// Compound row: [number input][pure slider track] -- thin gray track, accent fill left of
// the knob, large round light knob. Returns true while the value changes;
// `committed` (optional out) turns true exactly once when the edit FINISHES (mouse release
// after a drag, or the number box losing focus / Enter) -- this replaces the old
// IsItemDeactivatedAfterEdit() pattern, which cannot span a compound widget.
bool SliderInt(const char* label, int* v, int mn, int mx, float width = 0.0f,
               bool* committed = nullptr);
bool SliderFloat(const char* label, float* v, float mn, float mx,
                 const char* fmt = "%.2f", float width = 0.0f, bool* committed = nullptr);
// End-labelled variant for sliders whose direction needs explaining (CQ: low number ==
// high quality). `left`/`right` are small-font texts pinned to the track's two ends
// (macOS volume-slider icon positions, text instead of icons).
bool SliderIntEnds(const char* label, int* v, int mn, int mx,
                   const char* left, const char* right, float width = 0.0f,
                   bool* committed = nullptr);

// ---- labels -------------------------------------------------------------------------------------

// Section heading (卡片/面板内的小节标题): standard font, full-opacity text, NO divider
// rule, 12px gap above / 8px below.
void SectionHeader(const char* text);
// Label above a field (行前 label, for long labels like 缓冲窗口（帧）): small font,
// wide gap above (12px), narrow gap below (4px).
void LineLabel(const char* text);
// Label on the same row as its control (行内 label, for short labels like 名称/编码格式):
// standard font. width == 0 sizes to the text; pass a shared column width to align the
// controls of consecutive rows (the preset editor's mixed rows).
// InlineLabel(const char*, float width = 0) — see above; row-chaining: it ENDS with
// SameLine (its job is to precede a control). For a trailing unit text (kbps 等) use
// UnitText instead -- centered, secondary color, NO trailing SameLine (row terminator).
void InlineLabel(const char* text, float width = 0.0f);
void UnitText(const char* text);

// ---- containers ----------------------------------------------------------------------------------

// Card: panel-bg rounded child with a hairline border and kPaddingContainer padding on all
// sides. height == 0 auto-fits the content. Pair every BeginCard with EndCard.
bool BeginCard(const char* id, float height = 0.0f);
void EndCard();

// ---- feedback --------------------------------------------------------------------------------------

// frac in [0,1] (-1 = indeterminate, ImGui's anim). width == 0 fills the content region;
// height == 0 uses the standard frame height.
void ProgressBar(float frac, float width = 0.0f, float height = 0.0f);

// Rotating-arc spinner (extracted from the open-URL modal's loading card). Radius is
// frame-height derived; `id` only feeds the layout item's ID.
void Spinner(const char* id);

// ---- modal chrome -----------------------------------------------------------------------------------

// Unified modal chrome: title strip (title text + close X) + content, two sections, with a
// guaranteed kPaddingContainer inset on ALL four sides. The window width is PINNED to
// content_w_base + 2*pad (height auto-fits), so the content column's right edge always
// lands exactly pad inside the window -- inputs can never end up flush with the edge.
// Caller still owns OpenPopup() on the "###id" (same sticky-popup flow as today):
//     if (want_open) { ImGui::OpenPopup(id); want_open = false; }
//     if (ui::BeginModal(title + "###" + id, &open)) { ...body...; ui::EndModal(); }
// The body starts with a full-column item width already set (PushItemWidth(content_w));
// Esc and the X both close. Returns true while the body should be emitted; EndModal()
// must then be called exactly once.
bool BeginModal(const char* title, bool* open, float content_w_base = theme::kModalContentW);
void EndModal();

// ---- icon button -------------------------------------------------------------------------------------

// Self-drawn hit area for glyph icon buttons (promoted verbatim from Player::icon_button;
// Player keeps a same-signature delegating member so existing call sites are untouched).
// InvisibleButton asserts on zero-size axes, so callers always pass an explicit nonzero
// size. Returns the hit-test result plus the screen-space rect + draw list so the caller
// paints its glyph centered on it; draws a faint hover wash (theme kHoverFill, rounded by
// the style's FrameRounding == 6px @ 96 DPI).
struct IconButtonResult {
    bool clicked;
    ImVec2 min, max;
    ImDrawList* dl;
};
IconButtonResult IconButton(const char* str_id, ImVec2 size, bool disabled = false);

} // namespace ui
