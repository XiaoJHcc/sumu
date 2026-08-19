// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// macOS-style dark design system -- palette + metrics layer (Phase 2A).
// Pure ImGui dependency (no player.h): apply_theme() is called from Player::ui_init() in
// place of ImGui::StyleColorsDark(), BEFORE ui_style_base_ is snapshotted, so the
// apply_ui_dpi() ScaleAllSizes rebuild always restarts from these metrics (see
// player_session.cpp). All sizes below are 96-DPI bases; ScaleAllSizes multiplies them.

#pragma once

#include "imgui.h"

namespace ui::theme {

// ---- palette (ImVec4; matching ImU32 helpers below) ---------------------------------------

// #1E1E20 -- main window background.
inline constexpr ImVec4 kWindowBg{ 30.0f / 255.0f, 30.0f / 255.0f, 32.0f / 255.0f, 1.0f };
// #262628 -- cards / popups / panels, one step above the window background.
inline constexpr ImVec4 kPanelBg{ 38.0f / 255.0f, 38.0f / 255.0f, 40.0f / 255.0f, 1.0f };
// Hairline strokes: white at 12% alpha.
inline constexpr ImVec4 kBorder{ 1.0f, 1.0f, 1.0f, 0.12f };
// Stronger stroke for floating chrome (modal/popup/floating-panel outlines): white at 22%
// (== the previous IM_COL32(120,120,120,230)-ish popup border language).
inline constexpr ImVec4 kBorderStrong{ 1.0f, 1.0f, 1.0f, 0.22f };

inline constexpr ImVec4 kText{ 1.0f, 1.0f, 1.0f, 0.88f };
inline constexpr ImVec4 kTextSecondary{ 1.0f, 1.0f, 1.0f, 0.55f };
inline constexpr ImVec4 kTextDim{ 1.0f, 1.0f, 1.0f, 0.32f };

// Soft accent blue (#6FA8F7) with hover/active steps. Used for the Primary button chip,
// checkbox on-state, slider fill, combo check, links -- the single accent everywhere.
inline constexpr ImVec4 kAccent{ 111.0f / 255.0f, 168.0f / 255.0f, 247.0f / 255.0f, 1.0f };
inline constexpr ImVec4 kAccentHover{ 137.0f / 255.0f, 187.0f / 255.0f, 249.0f / 255.0f, 1.0f };
inline constexpr ImVec4 kAccentActive{ 93.0f / 255.0f, 150.0f / 255.0f, 230.0f / 255.0f, 1.0f };

inline constexpr ImVec4 kError{ 230.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f }; // #E65050
inline constexpr ImVec4 kWarning{ 1.0f, 214.0f / 255.0f, 10.0f / 255.0f, 1.0f }; // #FFD60A
inline constexpr ImVec4 kSuccess{ 50.0f / 255.0f, 215.0f / 255.0f, 75.0f / 255.0f, 1.0f }; // #32D74B

// Input / combo / checkbox frame background: white 7%.
inline constexpr ImVec4 kControlBg{ 1.0f, 1.0f, 1.0f, 0.07f };
// Nested list-row card fill (rows inside a kPanelBg card -- export preset rows, queue
// item cards): white 5%, one faint step above the parent card.
inline constexpr ImVec4 kRowCardBg{ 1.0f, 1.0f, 1.0f, 0.05f };
// Slider track: white 10%.
inline constexpr ImVec4 kTrackBg{ 1.0f, 1.0f, 1.0f, 0.10f };
// Neutral (secondary) button fill: white 8% (hover 12% / active 16% -- see widgets).
inline constexpr ImVec4 kButtonBg{ 1.0f, 1.0f, 1.0f, 0.08f };
// Faint hover wash for self-drawn hit areas (icon buttons etc.), white 12%
// (== the previous IM_COL32(255,255,255,30) used by Player::icon_button).
inline constexpr ImVec4 kHoverFill{ 1.0f, 1.0f, 1.0f, 0.12f };

// Self-drawn glyph color for the hand-painted chrome icons (top bar / bottom bar /
// modal close X): (230,230,230) -- the established media-chrome look. Dim step is
// the disabled state (110,110,110). The seekbar/volume knob reuses kIconColor.
inline constexpr ImVec4 kIconColor{ 230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f };
inline constexpr ImVec4 kIconColorDim{ 110.0f / 255.0f, 110.0f / 255.0f, 110.0f / 255.0f, 1.0f };

// Hand-drawn media bars (seekbar / volume): warm-yellow played fill (200,200,60) on a
// quiet gray track (90,90,90) -- established look, values frozen from the original code.
inline constexpr ImVec4 kMediaFill{ 200.0f / 255.0f, 200.0f / 255.0f, 60.0f / 255.0f, 1.0f };
inline constexpr ImVec4 kMediaTrack{ 90.0f / 255.0f, 90.0f / 255.0f, 90.0f / 255.0f, 1.0f };

// Alpha for translucent overlay windows (status float / bottom bar / settings panel)
// fed to ImGui::SetNextWindowBgAlpha so the video shows through.
inline constexpr float kOverlayBgAlpha = 0.55f;

// Modal backdrop dim: pure black at 50%. apply_theme() installs it as
// ImGuiCol_ModalWindowDimBg / NavWindowingDimBg; ui::BeginModal re-pushes the same value
// around its popup so a dim changed at runtime cannot leak between modals.
inline constexpr ImVec4 kDimBg{ 0.0f, 0.0f, 0.0f, 0.50f };

// ImU32 forms for ImDrawList calls.
inline ImU32 to_u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32 window_bg_u32() { return to_u32(kWindowBg); }
inline ImU32 panel_bg_u32() { return to_u32(kPanelBg); }
inline ImU32 border_u32() { return to_u32(kBorder); }
inline ImU32 border_strong_u32() { return to_u32(kBorderStrong); }
inline ImU32 text_u32() { return to_u32(kText); }
inline ImU32 text_secondary_u32() { return to_u32(kTextSecondary); }
inline ImU32 text_dim_u32() { return to_u32(kTextDim); }
inline ImU32 accent_u32() { return to_u32(kAccent); }
inline ImU32 accent_hover_u32() { return to_u32(kAccentHover); }
inline ImU32 accent_active_u32() { return to_u32(kAccentActive); }
inline ImU32 error_u32() { return to_u32(kError); }
inline ImU32 warning_u32() { return to_u32(kWarning); }
inline ImU32 success_u32() { return to_u32(kSuccess); }
inline ImU32 control_bg_u32() { return to_u32(kControlBg); }
inline ImU32 track_bg_u32() { return to_u32(kTrackBg); }
inline ImU32 button_bg_u32() { return to_u32(kButtonBg); }
inline ImU32 hover_fill_u32() { return to_u32(kHoverFill); }
inline ImU32 icon_color_u32() { return to_u32(kIconColor); }
inline ImU32 icon_color_dim_u32() { return to_u32(kIconColorDim); }
inline ImU32 media_fill_u32() { return to_u32(kMediaFill); }
inline ImU32 media_track_u32() { return to_u32(kMediaTrack); }

// ---- metrics (96-DPI bases) ---------------------------------------------------------------

inline constexpr float kRadiusControl = 6.0f; // buttons, inputs, combos, popups, children
inline constexpr float kRadiusWindow = 8.0f;  // windows / modals / cards
inline constexpr float kRadiusCheckbox = 4.0f;

// Guaranteed minimum inner padding of every container (cards, modals, panels): the distance
// between a container's edge and any control inside it (window↔slider, modal↔input, ...).
inline constexpr float kPaddingContainer = 12.0f;

// kSpaceXS is the title-bar beat: 36px bar / 32px buttons -> a uniform 2px inset on all
// four sides (button gap, bar edge padding, vertical centering all read this).
inline constexpr float kSpaceXS = 2.0f;
inline constexpr float kSpaceS = 4.0f;
inline constexpr float kSpaceM = 8.0f;
inline constexpr float kSpaceL = 12.0f;
inline constexpr float kSpaceXL = 16.0f;

// Single-line control standard (inputs, combos, buttons): 32px = 18px font + 2*7px
// FramePadding.y (set in apply_theme). All three read the same height/rounding/colors.
// This is ALSO the icon-button standard: every icon button (title bar, bottom bar, modal
// close, export page) is a kControlHeight square -- in chrome code use ui_s(kControlHeight)
// (title-bar btn_h derives from the 36px bar minus 2*kSpaceXS, which is the same 32px);
// inside rows GetFrameHeight() is the same value and keeps the button matched to its row.
// The glyph inside is 7/16 of the edge (ui::IconButton), 14px at 96 DPI.
inline constexpr float kControlHeight = 32.0f;
// Compact numeric column used inside mixed rows and next to sliders.
inline constexpr float kNumericInputW = 72.0f;

// macOS slider: thin gray track, accent fill, large round knob.
inline constexpr float kSliderTrackH = 4.0f;
inline constexpr float kSliderKnobR = 9.0f; // radius; diameter 18px ≈ label cap height

// macOS checkbox: small rounded square about as tall as the label text.
inline constexpr float kCheckboxSize = 16.0f;

// Combo dropdown arrow (small filled triangle at the frame's right edge).
inline constexpr float kComboArrowW = 9.0f;
inline constexpr float kComboArrowH = 5.0f;

// Standard modal content-column width (window width = this + 2 * kPaddingContainer).
// The preset editor and other wide forms may opt into kModalContentWLg.
inline constexpr float kModalContentW = 440.0f;
inline constexpr float kModalContentWLg = 480.0f;
// Modal title-strip height, identical for every ui::BeginModal (title text + close X).
// Matches the main window's top bar (Player::kTopBarHBase = 36) so every title strip
// in the app shares one height standard.
inline constexpr float kModalTitleH = 36.0f;

// Secondary-copy font size (unscaled base @ 96 DPI) -- mirrors Player::kFontSizeSm.
// Duplicated here so this layer stays player.h-free; pass to PushFont(nullptr, size),
// never GetFontSize() (would double-apply FontScaleDpi).
inline constexpr float kFontSizeSm = 16.0f;

// Apply the full theme: every relevant ImGuiCol_* plus the style vars (rounding, padding,
// spacing, scrollbar/grab sizes). Standard controls then read macOS-dark with zero
// per-call-site styling. Call once at context setup, before snapshotting the style for
// DPI scaling.
void apply_theme(ImGuiStyle& style);

} // namespace ui::theme
