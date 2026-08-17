// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#include "widgets.h"

#include <algorithm>
#include <cstring>

namespace ui {

namespace {

constexpr ImVec4 white(float a) { return ImVec4(1.0f, 1.0f, 1.0f, a); }

// Runtime DPI scale proxy: the standard frame is 28px at 96 DPI (FramePadding (10,5) +
// 18px base font) and ScaleAllSizes/FontScaleDpi scale both terms together, so the ratio
// recovers the monitor scale for self-drawn geometry (same role as Player::ui_s, but
// style-derived so this layer stays Player-free).
float ui_scale() { return ImGui::GetFrameHeight() / 28.0f; }

} // namespace

// ---- Button --------------------------------------------------------------------------------

bool Button(const char* label, ButtonVariant v, ControlSize s, float width){
    int pushed_cols = 0;
    int pushed_vars = 0;
    if (s == ControlSize::Small) {
        const float dpi = ui_scale(); // (8,2) is a 96-DPI base value; scale like everything else
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * dpi, 2.0f * dpi));
        ++pushed_vars;
    }
    switch (v) {
    case ButtonVariant::Primary:
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::kAccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text, white(0.95f));
        pushed_cols = 4;
        break;
    case ButtonVariant::Danger:
        ImGui::PushStyleColor(ImGuiCol_Button, white(0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, white(0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, white(0.14f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kError);
        pushed_cols = 4;
        break;
    case ButtonVariant::Secondary:
    default:
        // Neutral fill, one step under the FrameBg hover ramp; border stroked below.
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kButtonBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, white(0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, white(0.16f));
        pushed_cols = 3;
        break;
    }
    const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
    if (v == ButtonVariant::Secondary) {
        // macOS secondary buttons carry a hairline stroke (FrameBorderSize stays 0 so
        // other frame widgets don't inherit it).
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            theme::border_u32(), ImGui::GetStyle().FrameRounding, 0, 1.0f);
    }
    ImGui::PopStyleColor(pushed_cols);
    if (pushed_vars) ImGui::PopStyleVar(pushed_vars);
    return clicked;
}

// ---- field wrappers -------------------------------------------------------------------------

bool TextInput(const char* label, char* buf, size_t cap, const char* hint, float width){
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    if (hint)
        return ImGui::InputTextWithHint(label, hint, buf, cap);
    return ImGui::InputText(label, buf, cap);
}

bool IntInput(const char* label, int* v, float width){
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    return ImGui::InputInt(label, v);
}

bool Combo(const char* label, const char* const items[], int count, int* idx, float width){
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    return ImGui::Combo(label, idx, items, count);
}

bool Checkbox(const char* label, bool* v){
    return ImGui::Checkbox(label, v);
}

bool Radio(const char* label, bool active){
    return ImGui::RadioButton(label, active);
}

bool SliderInt(const char* label, int* v, int mn, int mx, float width){
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    return ImGui::SliderInt(label, v, mn, mx);
}

bool SliderFloat(const char* label, float* v, float mn, float mx, const char* fmt, float width){
    if (width != 0.0f) ImGui::SetNextItemWidth(width);
    return ImGui::SliderFloat(label, v, mn, mx, fmt);
}

bool OptionalSlider(const char* check_label, bool* enabled, const char* slider_label,
                    int* v, int mn, int mx){
    bool changed = ImGui::Checkbox(check_label, enabled);
    ImGui::SameLine();
    if (!*enabled) ImGui::BeginDisabled();
    // Style-derived width (~150px @ 96 DPI), matching the export editor's old ui_s(150).
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 5.4f);
    if (ImGui::SliderInt(slider_label, v, mn, mx)) changed = true;
    ImGui::SameLine();
    ImGui::Text("%d", *v);
    if (!*enabled) ImGui::EndDisabled();
    return changed;
}

// ---- text / feedback -------------------------------------------------------------------------

void SectionHeader(const char* text){
    const float s = ui_scale();
    ImGui::Dummy(ImVec2(0.0f, theme::kSpaceL * s - ImGui::GetStyle().ItemSpacing.y)); // top gap
    ImGui::PushFont(nullptr, theme::kFontSizeSm);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextSecondary);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    // 4px gap, then a 1px border-colored rule across the content column.
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float x1 = x0 + ImGui::GetContentRegionAvail().x;
    const float y = ImGui::GetCursorScreenPos().y + theme::kSpaceS * s - ImGui::GetStyle().ItemSpacing.y;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y + 0.5f), ImVec2(x1, y + 0.5f),
        theme::border_u32(), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, theme::kSpaceS * s));
}

void ProgressBar(float frac, float width, float height){
    if (height == 0.0f) height = ImGui::GetFrameHeight();
    // -FLT_MIN = fill the content region (ImGui's ProgressBar reads size.x < 0 that way).
    ImGui::ProgressBar(frac, ImVec2(width != 0.0f ? width : -FLT_MIN, height), "");
}

void Spinner(const char* id){
    const float r = ImGui::GetFrameHeight() * 0.65f; // ~18px @ 96 DPI
    const float th = std::max(1.5f, r / 6.0f);
    ImGui::InvisibleButton(id, ImVec2(r * 2.0f + th, r * 2.0f + th));
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const ImVec2 center((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Soft track + rotating head arc (verbatim from the open-URL loading card).
    constexpr float kPi = 3.14159265f;
    dl->PathClear();
    dl->PathArcTo(center, r, 0.0f, kPi * 2.0f, 48);
    dl->PathStroke(theme::to_u32(white(0.22f)), ImDrawFlags_None, th);

    const float t = static_cast<float>(ImGui::GetTime());
    const float a0 = t * 5.5f;
    const float a1 = a0 + kPi * 1.35f;
    dl->PathClear();
    dl->PathArcTo(center, r, a0, a1, 32);
    dl->PathStroke(theme::text_u32(), ImDrawFlags_None, th);
}

// ---- modal chrome -----------------------------------------------------------------------------

// Body indent / title-strip geometry, shared by BeginModal/EndModal.
namespace {
constexpr float kModalBodyPadBase = theme::kSpaceL; // 12px @ 96 DPI
}

bool BeginModal(const char* title, bool* open, ImVec2 size_base){
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = ui_scale();

    // Always re-center: AlwaysAutoResize takes a frame to settle the content size, so
    // Appearing-only centering lands once with a stale size (same fix the old popups had).
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (size_base.x > 0.0f || size_base.y > 0.0f)
        ImGui::SetNextWindowSize(ImVec2(size_base.x * s, size_base.y * s), ImGuiCond_Appearing);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f)); // custom title strip
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f); // border self-drawn in EndModal
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.50f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::BeginPopupModal(title, open, flags)) {
        if (open) *open = false;
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return false;
    }

    if (open && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        *open = false;
        ImGui::CloseCurrentPopup();
    }

    // ---- title strip: panel-toned cap + separator + title text + close X ----
    const float bar_h = ImGui::GetFrameHeight() + 2.0f * style.FramePadding.y * 0.8f; // ~36px@96
    const float pad = kModalBodyPadBase * s;
    const float rounding = theme::kRadiusWindow * s;
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float strip_w = ImGui::GetWindowSize().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The strip fill runs BEFORE content sizing settles on the first frame (strip_w == 0);
    // AlwaysAutoResize redraws next frame, so a one-frame narrow cap is acceptable here
    // (same behavior the old popups had).
    dl->AddRectFilled(ImVec2(wpos.x + 1.0f, wpos.y + 1.0f),
        ImVec2(wpos.x + strip_w - 1.0f, wpos.y + bar_h),
        theme::to_u32(white(0.05f)), std::max(0.0f, rounding - 1.0f),
        ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wpos.x + 1.0f, wpos.y + bar_h),
        ImVec2(wpos.x + strip_w - 1.0f, wpos.y + bar_h),
        theme::border_u32(), 1.0f);

    // Reserve the strip in the layout, then draw title + X over it.
    ImGui::Dummy(ImVec2(0.0f, bar_h));

    // Title text, vertically centered, inset by the body pad. Strip the "###id" suffix.
    {
        const char* text_end = std::strstr(title, "###");
        if (!text_end) text_end = title + std::strlen(title);
        const float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorPos(ImVec2(pad, (bar_h - th) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
        ImGui::TextUnformatted(title, text_end);
        ImGui::PopStyleColor();
    }

    if (open) {
        const float btn_w = 28.0f * s;
        const float btn_h = bar_h - 4.0f * s;
        ImGui::SetCursorPos(ImVec2(strip_w - btn_w - 4.0f * s, (bar_h - btn_h) * 0.5f));
        IconButtonResult r = IconButton("##modal_close", ImVec2(btn_w, btn_h));
        const float cx = (r.min.x + r.max.x) * 0.5f;
        const float cy = (r.min.y + r.max.y) * 0.5f;
        const float half = 5.0f * s;
        const float icon_th = std::max(1.0f, 1.5f * s);
        const bool hovered = ImGui::IsItemHovered();
        const ImU32 icon_col = hovered ? theme::accent_hover_u32() : theme::text_u32();
        r.dl->AddLine(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), icon_col, icon_th);
        r.dl->AddLine(ImVec2(cx - half, cy + half), ImVec2(cx + half, cy - half), icon_col, icon_th);
        if (r.clicked) {
            *open = false;
            ImGui::CloseCurrentPopup();
        }
    }

    // Body starts below the strip, indented by the pad (EndModal mirrors with Unindent).
    ImGui::SetCursorPosY(bar_h + pad);
    ImGui::Indent(pad);
    return true;
}

void EndModal(){
    const float s = ui_scale();
    const float pad = kModalBodyPadBase * s;
    // Bottom inset; the width Dummy keeps a right pad (= left Indent) under AlwaysAutoResize.
    ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x), pad));
    ImGui::Unindent(pad);

    // Self-drawn 1px border over everything (foreground list so the title-strip fill and
    // corner arcs stay underneath), matching the old popups' chrome.
    const ImVec2 a = ImGui::GetWindowPos();
    const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f),
        theme::border_strong_u32(), theme::kRadiusWindow * s, 0, 1.0f);

    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ---- icon button ------------------------------------------------------------------------------

IconButtonResult IconButton(const char* str_id, ImVec2 size, bool disabled){
    IconButtonResult r{};
    if (disabled) ImGui::BeginDisabled();
    r.clicked = ImGui::InvisibleButton(str_id, size);
    if (disabled) ImGui::EndDisabled();
    r.min = ImGui::GetItemRectMin();
    r.max = ImGui::GetItemRectMax();
    r.dl = ImGui::GetWindowDrawList();
    if (!disabled && ImGui::IsItemHovered())
        r.dl->AddRectFilled(r.min, r.max, theme::hover_fill_u32(), ImGui::GetStyle().FrameRounding);
    return r;
}

} // namespace ui
