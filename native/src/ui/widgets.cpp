// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#include "widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace ui {

namespace {

constexpr ImVec4 white(float a) { return ImVec4(1.0f, 1.0f, 1.0f, a); }

// Same hue, different alpha (button fill ramps etc.).
constexpr ImVec4 with_alpha(ImVec4 c, float a) { c.w = a; return c; }

// Runtime DPI scale: the AUTHORITATIVE monitor scale pushed by Player::apply_ui_dpi
// (set_ui_scale), so fixed-metric arithmetic here matches Player::ui_s() exactly. Do NOT
// re-derive it from GetFrameHeight()/kControlHeight: the font rasterizer snaps the pixel
// size (27px requested -> 26px rasterized at 150%), so that proxy reads ~1.469 instead of
// 1.5 -- and any widget whose window width is computed with the proxy while its content
// is laid out with the true scale overflows the content column and ends up flush against
// the window's right edge (the recurring modal right-padding bug).
float g_ui_scale = 1.0f;

} // namespace

void set_ui_scale(float scale) { g_ui_scale = scale; }

namespace {

float ui_scale() { return g_ui_scale; }

// Visible part of an ImGui "label##id" string. (ImGui::FindRenderedTextEnd lives in
// imgui_internal.h; this layer stays public-API-only, so split on the first "##" by hand --
// identical semantics for our labels, which never embed "##" in the visible text.)
struct LabelParts {
    const char* text;     // visible text begin (never null)
    const char* text_end; // visible text end
};
LabelParts split_label(const char* label){
    const char* end = std::strstr(label, "##");
    return { label, end ? end : label + std::strlen(label) };
}

// Apply the disabled fade (style.Alpha) to a theme color for custom-drawn parts.
ImVec4 faded(ImVec4 c){
    c.w *= ImGui::GetStyle().Alpha;
    return c;
}

} // namespace

// ---- Button --------------------------------------------------------------------------------

bool Button(const char* label, ButtonVariant v, ControlSize s, float width){
    int pushed_cols = 0;
    int pushed_vars = 0;
    if (s == ControlSize::Small) {
        const float dpi = ui_scale(); // (8,4) is a 96-DPI base value; scale like everything else
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * dpi, 4.0f * dpi));
        ++pushed_vars;
    }
    switch (v) {
    case ButtonVariant::Primary:
        // Soft-blue chip: translucent accent fill + accent text, no border (buttons are
        // borderless in this design; only floating windows keep a stroke).
        ImGui::PushStyleColor(ImGuiCol_Button, with_alpha(theme::kAccent, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, with_alpha(theme::kAccent, 0.26f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(theme::kAccent, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kAccent);
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
        // Neutral fill, one step under the FrameBg hover ramp; borderless.
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kButtonBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, white(0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, white(0.16f));
        pushed_cols = 3;
        break;
    }
    const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleColor(pushed_cols);
    if (pushed_vars) ImGui::PopStyleVar(pushed_vars);
    return clicked;
}

// ---- labels ---------------------------------------------------------------------------------

// True while the cursor still sits at the very top of its container (window/card padding):
// headers and labels then skip their top gap so card padding alone sets the inset.
static bool at_container_top(){
    return ImGui::GetCursorPosY() <= ImGui::GetStyle().WindowPadding.y + 1.0f;
}

void SectionHeader(const char* text){
    const float s = ui_scale();
    const float gap_top = theme::kSpaceL * s - ImGui::GetStyle().ItemSpacing.y;
    if (!at_container_top())
        ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, gap_top)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kText);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const float gap_bottom = theme::kSpaceM * s - ImGui::GetStyle().ItemSpacing.y;
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, gap_bottom)));
}

void LineLabel(const char* text){
    const float s = ui_scale();
    // Gap above the previous row's control == the container's inner padding
    // (kPaddingContainer), so a label sits exactly one padding beat below the last input --
    // the same distance the card edge keeps from the controls. The spacer Dummy attracts
    // ItemSpacing.y on BOTH sides, so the dummy itself is the remainder after two spacings.
    const float gap_top = theme::kPaddingContainer * s - 2.0f * ImGui::GetStyle().ItemSpacing.y;
    if (!at_container_top())
        ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, gap_top)));
    ImGui::PushFont(nullptr, theme::kFontSizeSm);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void InlineLabel(const char* text, float width){
    // Both paths draw the text vertically centered on a frame-height rect: an auto-width
    // label still shares its row with 32px controls and must not sit at the row top.
    if (width <= 0.0f) width = ImGui::CalcTextSize(text).x;
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, frame_h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float th = ImGui::GetTextLineHeight();
    dl->PushClipRect(p, ImVec2(p.x + width, p.y + frame_h), true);
    dl->AddText(ImVec2(p.x, p.y + (frame_h - th) * 0.5f), theme::to_u32(faded(theme::kText)), text);
    dl->PopClipRect();
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
}

void UnitText(const char* text){
    // Vertically centered on the control row, secondary color; terminates the row (no
    // trailing SameLine -- chaining the next row into this one is exactly the class of
    // layout bug this helper exists to prevent).
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::Dummy(ImVec2(w, frame_h));
    const float th = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (frame_h - th) * 0.5f),
        theme::to_u32(faded(theme::kTextSecondary)), text);
}

// ---- single-line field controls ---------------------------------------------------------------

bool TextInput(const char* label, char* buf, size_t cap, const char* hint,
               float width, ImGuiInputTextFlags flags){
    LabelParts lp = split_label(label);
    const char* id = lp.text_end; // "##..." portion keeps the full ID
    if (lp.text != lp.text_end) InlineLabel(label);
    if (width > 0.0f)
        ImGui::SetNextItemWidth(width);
    else if (lp.text != lp.text_end)
        ImGui::SetNextItemWidth(-FLT_MIN); // fill the rest of the row after an inline label
    if (hint)
        return ImGui::InputTextWithHint(id, hint, buf, cap, flags);
    return ImGui::InputText(id, buf, cap, flags);
}

bool IntInput(const char* label, int* v, float width){
    LabelParts lp = split_label(label);
    const char* id = lp.text_end;
    if (lp.text != lp.text_end) InlineLabel(label);
    if (width > 0.0f)
        ImGui::SetNextItemWidth(width);
    else if (lp.text != lp.text_end)
        ImGui::SetNextItemWidth(-FLT_MIN);
    // step == 0 -> ImGui hides the +/- steppers (InputScalar gets a null step pointer).
    return ImGui::InputInt(id, v, 0, 0);
}

// ---- combo (custom) ----------------------------------------------------------------------------

bool Combo(const char* label, const char* const items[], int count, int* idx, float width){
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = ui_scale();
    bool changed = false;
    ImGui::PushID(label);

    LabelParts lp = split_label(label);
    if (lp.text != lp.text_end) InlineLabel(label);

    const float h = ImGui::GetFrameHeight();
    float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    w = std::max(w, h); // never narrower than tall

    // ---- closed frame: input-styled rect + preview text + small triangle ----
    const ImVec2 fpos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##combo_frame", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        ImGui::OpenPopup("##combo_popup");
    const bool popup_open = ImGui::IsPopupOpen("##combo_popup", ImGuiPopupFlags_None);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 fill = theme::kControlBg;
    if (hovered) fill.w = 0.11f;
    if (popup_open) fill.w = 0.15f;
    dl->AddRectFilled(fpos, ImVec2(fpos.x + w, fpos.y + h),
        theme::to_u32(faded(fill)), theme::kRadiusControl * s);

    const float th = ImGui::GetTextLineHeight();
    const float aw = theme::kComboArrowW * s;
    const float ah = theme::kComboArrowH * s;
    const float ax = fpos.x + w - style.FramePadding.x - aw;
    const float ay = fpos.y + (h - ah) * 0.5f;
    dl->AddTriangleFilled(ImVec2(ax, ay), ImVec2(ax + aw, ay), ImVec2(ax + aw * 0.5f, ay + ah),
        theme::to_u32(faded(theme::kTextSecondary)));

    const char* preview = (*idx >= 0 && *idx < count) ? items[*idx] : "";
    dl->PushClipRect(fpos, ImVec2(ax - 4.0f * s, fpos.y + h), true);
    dl->AddText(ImVec2(fpos.x + style.FramePadding.x, fpos.y + (h - th) * 0.5f),
        theme::to_u32(faded(theme::kText)), preview);
    dl->PopClipRect();

    // ---- popup: rounded panel, rows flat until hovered, check on the current item ----
    ImGui::SetNextWindowPos(ImVec2(fpos.x, fpos.y + h + 2.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * s, 4.0f * s));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanelBg);
    if (ImGui::BeginPopup("##combo_popup")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const float row_h = h;
        const float row_w = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(row_w, row_h));
            const bool rhover = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                *idx = i;
                changed = true;
                ImGui::CloseCurrentPopup();
            }
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            if (rhover)
                pdl->AddRectFilled(rmin, rmax, theme::hover_fill_u32(), theme::kRadiusControl * s);
            const float ty = rmin.y + (row_h - th) * 0.5f;
            if (i == *idx) {
                // Leading check, same glyph language as ui::Checkbox. It sits centered
                // in a row_h x row_h square, so its left/right margins match the row's
                // vertical padding and the text always starts one square in.
                const float bw = 10.0f * s;
                const float bx = rmin.x + (row_h - bw) * 0.5f;
                const float cx0 = bx + bw * 0.10f;
                const float cth = std::max(1.2f, 1.5f * s);
                const ImU32 cc = theme::accent_u32();
                pdl->AddLine(ImVec2(cx0, ty + th * 0.55f), ImVec2(cx0 + bw * 0.30f, ty + th * 0.82f), cc, cth);
                pdl->AddLine(ImVec2(cx0 + bw * 0.30f, ty + th * 0.82f), ImVec2(cx0 + bw, ty + th * 0.18f), cc, cth);
            }
            pdl->PushClipRect(rmin, rmax, true);
            pdl->AddText(ImVec2(rmin.x + row_h, ty),
                theme::to_u32(faded(theme::kText)), items[i]);
            pdl->PopClipRect();
            ImGui::PopID();
        }
        // hairline outline, same language as the modal border
        const ImVec2 pmin = ImGui::GetWindowPos();
        const ImVec2 pmax(pmin.x + ImGui::GetWindowSize().x, pmin.y + ImGui::GetWindowSize().y);
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(pmin.x + 0.5f, pmin.y + 0.5f), ImVec2(pmax.x - 0.5f, pmax.y - 0.5f),
            theme::border_u32(), theme::kRadiusWindow * s, 0, 1.0f);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

// ---- checkbox / radio ----------------------------------------------------------------------------

bool Checkbox(const char* label, bool* v){
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = ui_scale();
    const float frame_h = ImGui::GetFrameHeight();
    const float box = theme::kCheckboxSize * s;
    LabelParts lp = split_label(label);
    const bool has_text = lp.text != lp.text_end;
    const float text_w = has_text ? ImGui::CalcTextSize(lp.text, lp.text_end).x : 0.0f;
    const float total_w = box + (has_text ? style.ItemInnerSpacing.x + text_w : 0.0f);

    const bool clicked = ImGui::InvisibleButton(label, ImVec2(total_w, frame_h));
    if (clicked) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();

    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float by = p.y + (frame_h - box) * 0.5f;
    const float rounding = theme::kRadiusCheckbox * s;
    if (*v) {
        // Soft accent fill + dark check (the accent is too light for a white check now).
        dl->AddRectFilled(ImVec2(p.x, by), ImVec2(p.x + box, by + box),
            theme::to_u32(faded(hovered ? theme::kAccentHover : theme::kAccent)), rounding);
        const float cth = std::max(1.5f, box * 0.11f);
        const ImU32 ck = theme::to_u32(faded(theme::kWindowBg));
        const ImVec2 a(p.x + box * 0.24f, by + box * 0.54f);
        const ImVec2 b(p.x + box * 0.44f, by + box * 0.72f);
        const ImVec2 c(p.x + box * 0.77f, by + box * 0.30f);
        dl->AddLine(a, b, ck, cth);
        dl->AddLine(b, c, ck, cth);
    } else {
        // quiet fill + hairline border
        ImVec4 fillv = theme::kControlBg;
        if (hovered) fillv.w = 0.11f;
        dl->AddRectFilled(ImVec2(p.x, by), ImVec2(p.x + box, by + box),
            theme::to_u32(faded(fillv)), rounding);
        dl->AddRect(ImVec2(p.x, by), ImVec2(p.x + box, by + box),
            theme::to_u32(faded(theme::kBorder)), rounding, 0, 1.0f);
    }
    if (has_text) {
        dl->AddText(ImVec2(p.x + box + style.ItemInnerSpacing.x,
                p.y + (frame_h - ImGui::GetTextLineHeight()) * 0.5f),
            theme::to_u32(faded(theme::kText)), lp.text, lp.text_end);
    }
    return clicked;
}

bool Radio(const char* label, bool active){
    return ImGui::RadioButton(label, active);
}

// ---- sliders (custom, macOS style) -----------------------------------------------------------------

namespace {

// Track interaction + rendering. `t` is the normalized value in [0,1]; on drag the new
// position is written back and drag_changed set. released == the mouse went up after an
// active drag this frame (commit signal).
void slider_track(const char* id, float* t, float track_w,
                  bool* drag_changed, bool* released){
    const float s = ui_scale();
    const float h = ImGui::GetFrameHeight();
    *drag_changed = false;
    *released = false;

    ImGui::InvisibleButton(id, ImVec2(track_w, h));
    const ImVec2 p = ImGui::GetItemRectMin();
    const float inset = 4.0f * s; // knob-center travel inset from the track ends
    const float span = std::max(1.0f, track_w - 2.0f * inset);

    if (ImGui::IsItemActive()) {
        float nt = (ImGui::GetIO().MousePos.x - (p.x + inset)) / span;
        nt = std::clamp(nt, 0.0f, 1.0f);
        if (nt != *t) { *t = nt; *drag_changed = true; }
    }
    if (ImGui::IsItemDeactivated()) *released = true;

    // ---- render: thin gray track, accent fill, large round light knob ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h * 0.5f;
    const float track_h = theme::kSliderTrackH * s;
    const float x0 = p.x, x1 = p.x + track_w;
    const ImU32 track_col = theme::to_u32(faded(theme::kTrackBg));
    dl->AddRectFilled(ImVec2(x0, cy - track_h * 0.5f), ImVec2(x1, cy + track_h * 0.5f),
        track_col, track_h * 0.5f);

    const float knob_x = x0 + inset + (*t) * span;
    if (knob_x > x0 + track_h)
        dl->AddRectFilled(ImVec2(x0, cy - track_h * 0.5f), ImVec2(knob_x, cy + track_h * 0.5f),
            theme::to_u32(faded(theme::kAccent)), track_h * 0.5f);

    const float r = theme::kSliderKnobR * s;
    dl->AddCircleFilled(ImVec2(knob_x, cy), r,
        theme::to_u32(faded(white(0.93f))));
    dl->AddCircle(ImVec2(knob_x, cy), r, IM_COL32(0, 0, 0, (int)(64.0f * ImGui::GetStyle().Alpha)), 0, 1.0f);
}

// End label (small font, secondary color) pinned beside the track, vertically centered on
// the control row. No SameLine bookkeeping -- the caller orchestrates the row layout, so a
// trailing SameLine can never leak the NEXT row into this one.
void slider_end_text(const char* text){
    const float frame_h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(nullptr, theme::kFontSizeSm);
    const float tw = ImGui::CalcTextSize(text).x;
    ImGui::Dummy(ImVec2(tw, frame_h));
    const float th = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (frame_h - th) * 0.5f),
        theme::to_u32(faded(theme::kTextSecondary)), text);
    ImGui::PopFont();
}

float slider_ends_width(const char* left, const char* right){
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushFont(nullptr, theme::kFontSizeSm);
    const float w = (left ? ImGui::CalcTextSize(left).x + gap : 0.0f) +
                    (right ? ImGui::CalcTextSize(right).x + gap : 0.0f);
    ImGui::PopFont();
    return w;
}

bool slider_int_impl(const char* label, int* v, int mn, int mx,
                     const char* left, const char* right, float width, bool* committed){
    const float s = ui_scale();
    if (committed) *committed = false;
    ImGui::PushID(label);
    bool changed = false;

    LabelParts lp = split_label(label);
    if (lp.text != lp.text_end) InlineLabel(label);

    float avail = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float box_w = theme::kNumericInputW * s;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float track_w = avail - box_w - gap - slider_ends_width(left, right);
    track_w = std::max(track_w, 40.0f * s);

    // [number box] -- step 0 hides the +/- steppers; edits clamp to [mn,mx].
    ImGui::SetNextItemWidth(box_w);
    if (ImGui::InputInt("##box", v, 0, 0)) {
        *v = std::clamp(*v, mn, mx);
        changed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        *v = std::clamp(*v, mn, mx);
        if (committed) *committed = true;
    }

    // [left end text][track][right end text] -- SameLine orchestrated here, row terminates
    // with the track/right text (no trailing SameLine leaks into the next row).
    ImGui::SameLine();
    if (left) {
        slider_end_text(left);
        ImGui::SameLine();
    }
    float t = (mx > mn) ? float(*v - mn) / float(mx - mn) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    bool drag_changed, released;
    slider_track("##track", &t, track_w, &drag_changed, &released);
    if (drag_changed) {
        const int nv = mn + (int)std::lround(t * float(mx - mn));
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (released && committed) *committed = true;
    if (right) {
        ImGui::SameLine();
        slider_end_text(right);
    }

    ImGui::PopID();
    return changed;
}

} // namespace

bool SliderInt(const char* label, int* v, int mn, int mx, float width, bool* committed){
    return slider_int_impl(label, v, mn, mx, nullptr, nullptr, width, committed);
}

bool SliderIntEnds(const char* label, int* v, int mn, int mx,
                   const char* left, const char* right, float width, bool* committed){
    return slider_int_impl(label, v, mn, mx, left, right, width, committed);
}

bool SliderFloat(const char* label, float* v, float mn, float mx,
                 const char* fmt, float width, bool* committed){
    const float s = ui_scale();
    if (committed) *committed = false;
    ImGui::PushID(label);
    bool changed = false;

    LabelParts lp = split_label(label);
    if (lp.text != lp.text_end) InlineLabel(label);

    float avail = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float box_w = theme::kNumericInputW * s;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    float track_w = std::max(avail - box_w - gap, 40.0f * s);

    ImGui::SetNextItemWidth(box_w);
    if (ImGui::InputFloat("##box", v, 0.0f, 0.0f, fmt)) {
        *v = std::clamp(*v, mn, mx);
        changed = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        *v = std::clamp(*v, mn, mx);
        if (committed) *committed = true;
    }

    ImGui::SameLine();
    float t = (mx > mn) ? (*v - mn) / (mx - mn) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    bool drag_changed, released;
    slider_track("##track", &t, track_w, &drag_changed, &released);
    if (drag_changed) {
        *v = mn + t * (mx - mn);
        changed = true;
    }
    if (released && committed) *committed = true;

    ImGui::PopID();
    return changed;
}

// ---- containers --------------------------------------------------------------------------------

bool BeginCard(const char* id, float height, ImGuiWindowFlags flags){
    const float s = ui_scale();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kPanelBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusWindow * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
        ImVec2(theme::kPaddingContainer * s, theme::kPaddingContainer * s));
    // Borderless (cards separate by fill contrast alone), so AlwaysUseWindowPadding is
    // MANDATORY -- ImGui zeroes a borderless child's padding otherwise.
    ImGuiChildFlags child_flags = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (height == 0.0f) child_flags |= ImGuiChildFlags_AutoResizeY;
    return ImGui::BeginChild(id, ImVec2(0.0f, height), child_flags, flags);
}

void EndCard(){
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- text / feedback -------------------------------------------------------------------------

void ProgressBar(float frac, float width, float height){
    if (height == 0.0f) height = ImGui::GetFrameHeight();
    // -FLT_MIN = fill the content region (ImGui's ProgressBar reads size.x < 0 that way).
    ImGui::ProgressBar(frac, ImVec2(width != 0.0f ? width : -FLT_MIN, height), "");
}

void Spinner(const char* id){
    const float r = ImGui::GetFrameHeight() * 0.65f; // ~21px @ 96 DPI
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

bool BeginModal(const char* title, bool* open, float content_w_base){
    const ImGuiIO& io = ImGui::GetIO();
    const float s = ui_scale();
    const float pad = theme::kPaddingContainer * s;
    // Width pinned to content + 2*pad, height auto: the content column's right edge always
    // lands exactly `pad` inside the window (no flush-right inputs).
    const float win_w = content_w_base * s + 2.0f * pad;

    // Always re-center: AlwaysAutoResize takes a frame to settle the content size, so
    // Appearing-only centering lands once with a stale size (same fix the old popups had).
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.0f), ImVec2(win_w, FLT_MAX));

    // WindowPadding (pad, 0): the horizontal padding does double duty -- it insets the body
    // AND shrinks the content region so "fill the remaining width" widgets always stop
    // exactly `pad` from the window's right edge (the strip is drawn with absolute coords).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f); // border self-drawn in EndModal
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, theme::kDimBg);

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
    // Fixed theme height (kModalTitleH) -- identical across every modal.
    const float bar_h = theme::kModalTitleH * s;
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
        const float btn_w = theme::kControlHeight * s; // icon button = control-height square
        const float btn_h = bar_h - 2.0f * theme::kSpaceXS * s; // 36 - 2*2 = 32, centered
        ImGui::SetCursorPos(ImVec2(strip_w - btn_w - theme::kSpaceXS * s, (bar_h - btn_h) * 0.5f));
        // Destructive-action standard: the close glyph flips to kError on hover (same as
        // the main-window close / export delete buttons).
        IconButtonResult r = IconButton("##modal_close", ImVec2(btn_w, btn_h));
        DrawIconButtonGlyph(r, AppIcon::Close, ImGui::IsItemHovered()
            ? theme::error_u32() : theme::icon_color_u32());
        if (r.clicked) {
            *open = false;
            ImGui::CloseCurrentPopup();
        }
    }

    // Body starts below the strip (WindowPadding.x already insets it horizontally);
    // the item width defaults to the full content column.
    ImGui::SetCursorPosY(bar_h + pad);
    ImGui::PushItemWidth(content_w_base * s);
    return true;
}

void EndModal(){
    const float s = ui_scale();
    const float pad = theme::kPaddingContainer * s;
    ImGui::PopItemWidth();
    // Bottom inset (WindowPadding.y is 0): spacing + this dummy == pad.
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, pad - ImGui::GetStyle().ItemSpacing.y)));

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

void DrawIconButtonGlyph(const IconButtonResult& r, AppIcon icon, ImU32 tint){
    if (!icons::available()) return; // bare hit area + hover wash (see header contract)
    // Glyph at 7/16 of the button edge: 14px inside the standard 32px title-bar button.
    // 7/16 keeps the physical size whole at 100%/150%/200% (14/21/28px); the atlas draw
    // snaps to integer pixels, so non-integer multiples would shift the glyph off-grid.
    const float g = std::floorf(std::min(r.max.x - r.min.x, r.max.y - r.min.y) * 0.4375f);
    const ImVec2 gmin((r.min.x + r.max.x - g) * 0.5f, (r.min.y + r.max.y - g) * 0.5f);
    icons::draw(r.dl, gmin, ImVec2(gmin.x + g, gmin.y + g), icon, tint);
}

IconButtonResult IconButton(const char* str_id, ImVec2 size, AppIcon icon, bool disabled){
    IconButtonResult r = IconButton(str_id, size, disabled);
    DrawIconButtonGlyph(r, icon,
        disabled ? theme::icon_color_dim_u32() : theme::icon_color_u32());
    return r;
}

IconButtonResult IconButtonFramed(const char* str_id, ImVec2 size, AppIcon icon, bool disabled){
    // Hit area WITHOUT the bare wash: IconButton(str_id, size, disabled)'s own hover fill
    // is replaced below by the full button-fill ramp, so replicate it inline.
    IconButtonResult r{};
    if (disabled) ImGui::BeginDisabled();
    r.clicked = ImGui::InvisibleButton(str_id, size);
    r.dl = ImGui::GetWindowDrawList();
    // State colors BEFORE EndDisabled so the active/hover reads aren't alpha-faded twice.
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (disabled) ImGui::EndDisabled();
    r.min = ImGui::GetItemRectMin();
    r.max = ImGui::GetItemRectMax();
    // Secondary Button ramp (theme kButtonBg -> white 12% hover -> white 16% active).
    const ImU32 fill = theme::to_u32(faded(
        active ? white(0.16f) : hovered ? white(0.12f) : theme::kButtonBg));
    r.dl->AddRectFilled(r.min, r.max, fill, ImGui::GetStyle().FrameRounding);
    DrawIconButtonGlyph(r, icon,
        disabled ? theme::icon_color_dim_u32() : theme::icon_color_u32());
    return r;
}

} // namespace ui
