// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// macOS-style dark design system -- widget layer (Phase 2A). Every function does ALL its
// own PushStyleColor/Var + Pop internally: call sites carry zero styling code. Pure ImGui +
// theme.h dependency (never player.h), so it can be unit-exercised without a Player.
//
// DPI principle: control geometry is derived from the ImGui style (GetFrameHeight(),
// CalcTextSize(), style vars) which apply_ui_dpi() already scaled via ScaleAllSizes +
// FontScaleDpi -- nothing here multiplies by a DPI factor itself. Callers that need a fixed
// width keep wrapping with Player::ui_s() and pass it via the trailing `width` argument
// (0 = content-sized / ImGui default).

#pragma once

#include "imgui.h"
#include "theme.h"

#include <cstddef>

namespace ui {

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

// Label-in-front wrappers (ImGui "label##id" convention). width == 0 keeps the caller's
// item width (SetNextItemWidth / PushItemWidth) untouched. FrameBg comes from the theme.
bool TextInput(const char* label, char* buf, size_t cap, const char* hint = nullptr,
               float width = 0.0f);
bool IntInput(const char* label, int* v, float width = 0.0f);
bool Combo(const char* label, const char* const items[], int count, int* idx,
           float width = 0.0f);
bool Checkbox(const char* label, bool* v);
bool Radio(const char* label, bool active);
bool SliderInt(const char* label, int* v, int mn, int mx, float width = 0.0f);
bool SliderFloat(const char* label, float* v, float mn, float mx,
                 const char* fmt = "%.2f", float width = 0.0f);

// "enable checkbox + slider" coupling (the export preset editor's CQ/bitrate/maxrate
// pattern): the slider is greyed (BeginDisabled) while the checkbox is unchecked, and the
// current value is echoed after the slider. Returns true if EITHER control changed.
bool OptionalSlider(const char* check_label, bool* enabled, const char* slider_label,
                    int* v, int mn, int mx);

// Section heading: small font + secondary color, 12px gap above / 4px below, and a 1px
// border-colored rule underneath (replaces the PushFont(kFontSizeSm)+TextUnformatted+
// Separator pattern).
void SectionHeader(const char* text);

// frac in [0,1] (-1 = indeterminate, ImGui's anim). width == 0 fills the content region;
// height == 0 uses the standard frame height.
void ProgressBar(float frac, float width = 0.0f, float height = 0.0f);

// Rotating-arc spinner (extracted from the open-URL modal's loading card). Radius is
// frame-height derived; `id` only feeds the layout item's ID.
void Spinner(const char* id);

// Unified modal chrome (replaces the duplicated title-strip/close-X/border blocks of the
// open-URL and web-stream popups). Caller still owns OpenPopup() on its "###id" (same
// sticky-popup flow as today):
//     if (want_open) { ImGui::OpenPopup(id); want_open = false; }
//     if (ui::BeginModal(title + "###" + id, &open, ImVec2(w_base, 0))) {
//         ...body...
//         ui::EndModal();
//     }
// Centers every frame (AlwaysAutoResize settles over a frame), panel background,
// kRadiusWindow corners, self-drawn 1px border and title strip with the title text and an
// accent-on-hover close X. size_base is a 96-DPI base total window size (x==0 keeps
// AlwaysAutoResize width). The body is auto-indented below the strip; Esc and the X both
// close (via CloseCurrentPopup + *open=false when `open` is given). Returns true while the
// body should be emitted; EndModal() must then be called exactly once.
bool BeginModal(const char* title, bool* open, ImVec2 size_base = ImVec2(0.0f, 0.0f));
void EndModal();

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
