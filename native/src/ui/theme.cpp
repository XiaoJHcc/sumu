// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#include "theme.h"

namespace ui::theme {

namespace {
constexpr ImVec4 white(float a) { return ImVec4(1.0f, 1.0f, 1.0f, a); }
}

void apply_theme(ImGuiStyle& style){
    // ---- metrics (96-DPI bases; ScaleAllSizes applies the monitor scale) ----
    // FramePadding (10,7) + 18px base font -> 32px standard control height (kControlHeight),
    // shared by every single-line control: inputs, combos, buttons.
    style.WindowPadding = ImVec2(kPaddingContainer, 10.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(kSpaceM, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, kSpaceS);
    style.WindowRounding = kRadiusWindow;
    style.ChildRounding = kRadiusControl;
    style.FrameRounding = kRadiusControl;
    style.PopupRounding = kRadiusControl;
    style.ScrollbarRounding = kRadiusControl;
    style.GrabRounding = kRadiusControl;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowBorderSize = 0.0f;   // modal/popup strokes are self-drawn (see ui::BeginModal)
    style.PopupBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;    // controls are borderless (only floating windows stroke)

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_TextLink] = kAccent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);

    c[ImGuiCol_WindowBg] = kWindowBg;
    c[ImGuiCol_ChildBg] = kWindowBg;
    c[ImGuiCol_PopupBg] = kPanelBg;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = kControlBg;
    c[ImGuiCol_FrameBgHovered] = white(0.11f);
    c[ImGuiCol_FrameBgActive] = white(0.15f);
    c[ImGuiCol_InputTextCursor] = kText;

    c[ImGuiCol_TitleBg] = kPanelBg;
    c[ImGuiCol_TitleBgActive] = kPanelBg;
    c[ImGuiCol_TitleBgCollapsed] = kWindowBg;
    c[ImGuiCol_MenuBarBg] = kPanelBg;

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.20f);
    c[ImGuiCol_ScrollbarGrab] = white(0.20f);
    c[ImGuiCol_ScrollbarGrabHovered] = white(0.30f);
    c[ImGuiCol_ScrollbarGrabActive] = white(0.40f);

    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_CheckboxSelectedBg] = kControlBg;
    // Slider: unfilled track reads as FrameBg-derived in ImGui; the grab is the light knob.
    c[ImGuiCol_SliderGrab] = white(0.85f);
    c[ImGuiCol_SliderGrabActive] = white(1.0f);

    // Bare ImGui::Button reads as the neutral (secondary) button; ui::Button pushes the
    // accent/danger variants explicitly.
    c[ImGuiCol_Button] = kButtonBg;
    c[ImGuiCol_ButtonHovered] = white(0.12f);
    c[ImGuiCol_ButtonActive] = white(0.16f);

    c[ImGuiCol_Header] = white(0.08f);
    c[ImGuiCol_HeaderHovered] = white(0.12f);
    c[ImGuiCol_HeaderActive] = white(0.16f);

    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_SeparatorHovered] = kAccentHover;
    c[ImGuiCol_SeparatorActive] = kAccent;

    c[ImGuiCol_ResizeGrip] = white(0.10f);
    c[ImGuiCol_ResizeGripHovered] = white(0.20f);
    c[ImGuiCol_ResizeGripActive] = white(0.30f);

    c[ImGuiCol_Tab] = white(0.05f);
    c[ImGuiCol_TabHovered] = white(0.14f);
    c[ImGuiCol_TabSelected] = white(0.10f);
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = white(0.03f);
    c[ImGuiCol_TabDimmedSelected] = white(0.06f);
    c[ImGuiCol_TabDimmedSelectedOverline] = white(0.20f);

    c[ImGuiCol_PlotLines] = kAccent;
    c[ImGuiCol_PlotLinesHovered] = kAccentHover;
    c[ImGuiCol_PlotHistogram] = kAccent; // ProgressBar fill
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;

    c[ImGuiCol_TableHeaderBg] = kPanelBg;
    c[ImGuiCol_TableBorderStrong] = white(0.18f);
    c[ImGuiCol_TableBorderLight] = kBorder;
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = white(0.03f);

    c[ImGuiCol_TreeLines] = kBorder;
    c[ImGuiCol_DragDropTarget] = kAccent;
    c[ImGuiCol_DragDropTargetBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.20f);
    c[ImGuiCol_UnsavedMarker] = kWarning;

    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_NavWindowingHighlight] = white(0.70f);
    c[ImGuiCol_NavWindowingDimBg] = kDimBg;
    c[ImGuiCol_ModalWindowDimBg] = kDimBg;
}

} // namespace ui::theme
