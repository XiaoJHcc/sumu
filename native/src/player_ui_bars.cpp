// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "ui_util.h"

namespace {

// Shared renderer for the bottom bar's two media sliders (seekbar / volume). Visual
// language mirrors the settings-panel slider_track() in ui/widgets.cpp minus the knob:
// a thin fully rounded track with a rounded yellow fill (hover/drag brightens it).
void draw_media_bar(ImDrawList* dl, float x0, float x1, float cy, float t,
                    float track_h, bool emphasized){
    const float half = track_h * 0.5f;
    dl->AddRectFilled(ImVec2(x0, cy - half), ImVec2(x1, cy + half),
        ui::theme::media_track_u32(), half);
    const float fill_x = x0 + (x1 - x0) * std::clamp(t, 0.0f, 1.0f);
    if (fill_x > x0 + track_h) // guard: a hair-thin fill would round into a blob
        dl->AddRectFilled(ImVec2(x0, cy - half), ImVec2(fill_x, cy + half),
            emphasized ? ui::theme::media_fill_hover_u32() : ui::theme::media_fill_u32(),
            half);
}

} // namespace

Player::IconButtonResult Player::icon_button(const char* str_id, ImVec2 size, bool disabled){
    // Promoted to the design-system widget layer (ui/widgets.cpp). The hover-wash rounding
    // there reads GetStyle().FrameRounding -- apply_theme sets it to kRadiusControl (6px @
    // 96 DPI) and apply_ui_dpi()'s ScaleAllSizes scales it, so it equals the old ui_s(6.0f).
    return ui::IconButton(str_id, size, disabled);
}
Player::IconButtonResult Player::icon_button(const char* str_id, ImVec2 size, ui::AppIcon icon, bool disabled){
    return ui::IconButton(str_id, size, icon, disabled);
}

void Player::build_top_bar(float& out_height){
    ImGuiIO& io = ImGui::GetIO();
    const float bar_h = top_bar_h();
    out_height = bar_h;

    // Fullscreen: the title bar auto-hides and only appears when the mouse nears the TOP edge
    // -- the same mouse-zone reveal the bottom bar uses at the bottom (build_bottom_bar()).
    // In fullscreen the video fills the whole screen (fit_viewport() reserves no strip), so a
    // hidden bar means nothing is drawn and the caption drag-rect must be cleared, otherwise
    // WM_NCHITTEST would still report HTCAPTION over the (now invisible) top strip. In
    // windowed mode the bar is always shown and its strip is reserved (task: title bar
    // outside the video), so this whole block is skipped.
    if (fullscreen_.load(std::memory_order_relaxed)) {
        const float show_zone_y = io.DisplaySize.y * 0.10f; // top 10%
        bool mouse_in_zone = io.MousePos.y >= 0.0f && io.MousePos.y <= show_zone_y &&
            io.MousePos.x >= 0.0f && io.MousePos.x <= io.DisplaySize.x;
        if (!mouse_in_zone) {
            caption_drag_x0_ = caption_drag_x1_ = 0.0f; // disable phantom HTCAPTION while hidden
            return;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_h));
    // NoBackground: the bg is self-drawn right after Begin() with per-corner rounding --
    // square TOP corners + rounded BOTTOM corners (WindowRounding is all-four-corners only).
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(ui::theme::kSpaceXS), 0.0f));
    // Uniform 2px gap between the bar's buttons (the same kSpaceXS beat as the edge padding
    // and the vertical inset: 36px bar / 32px buttons). Pushed for the whole bar so the bare
    // SameLine() calls and the ItemSpacing.x reads below (filename reserve, drag-region
    // width) all pick it up.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
        ImVec2(ui_s(ui::theme::kSpaceXS), ImGui::GetStyle().ItemSpacing.y));
    ImGui::Begin("##sumu_top_bar", nullptr, flags);

    { // Bar background: panel tone (theme kPanelBg), one step above the window background so
      // the chrome reads as a bar against the video/splash. TOP corners are SQUARE -- the
      // theme's kRadiusWindow rounding there leaves two transparent notches at the screen's
      // top corners in fullscreen (DWM chrome disabled / DONOTROUND); square corners instead
      // get clipped by DWM's ~8px window rounding in windowed mode -- both states end up
      // flush with zero leakage. BOTTOM corners keep the theme rounding.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        dl->AddRectFilled(p0, p1, ui::theme::panel_bg_u32(), ui_s(ui::theme::kRadiusWindow),
            ImDrawFlags_RoundCornersBottom);
    }

    const float btn_w = ui_s(ui::theme::kControlHeight); // icon button = control-height square
    const float btn_h = bar_h - ui_s(2.0f * ui::theme::kSpaceXS); // 36 - 2*2 = 32, centered
    // Export-mode gating (Phase 2): settings is disabled while the export page is up (its own
    // AI-pipeline section replaces the settings panel); open/URL/web + the export toggle are
    // disabled while an export is running so the user cannot navigate away mid-export. This
    // gating replaces the export screen's old "返回" exit button.
    const bool settings_disabled = export_mode_;
    const bool export_nav_disabled = export_mode_ && export_running_;
    // Vertically center every control in the bar (ImGui default is top-aligned).
    ImGui::SetCursorPosY((bar_h - btn_h) * 0.5f);

    { // settings toggle: sliders-horizontal glyph from the lucide atlas (ui::AppIcon)
        IconButtonResult r = icon_button("##settings_btn", ImVec2(btn_w, btn_h), ui::AppIcon::Settings, settings_disabled);
        if (r.clicked) {
            ui_settings_open_ = !ui_settings_open_;
            if (ui_settings_open_) settings_edit_init_ = false; // resync edit buffers to latest cfg on open
        }
    }
    ImGui::SameLine();

    { // M-C2: "open file" button -- records open_dialog, drained by Python's
      // take_ui_intents() which responds by calling the blocking pick_open_file() dialog
      // (see UiIntents' header comment; present keeps showing the current video meanwhile).
        IconButtonResult r = icon_button("##open_btn", ImVec2(btn_w, btn_h), ui::AppIcon::OpenFile, export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // open exits the export page first
            record_open_dialog();
        }
        if (ImGui::IsItemHovered() && !ui_str_.open_file.empty())
            ImGui::SetTooltip("%s", ui_str_.open_file.c_str());
    }
    ImGui::SameLine();
    { // Network URL open: opens the ImGui URL popup (build_open_url_popup); on confirm
      // writes open_path for Python.
        IconButtonResult r = icon_button("##open_url_btn", ImVec2(btn_w, btn_h), ui::AppIcon::OpenUrl, export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // URL open exits the export page first
            request_open_url_popup();
        }
        if (ImGui::IsItemHovered() && !ui_str_.open_url.empty())
            ImGui::SetTooltip("%s", ui_str_.open_url.c_str());
    }
    ImGui::SameLine();
    { // Web-stream server popup (request_stream_popup).
        IconButtonResult r = icon_button("##stream_btn", ImVec2(btn_w, btn_h), ui::AppIcon::WebServer, export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // web server exits the export page first
            request_stream_popup();
        }
        if (ImGui::IsItemHovered() && !ui_str_.stream_server.empty())
            ImGui::SetTooltip("%s", ui_str_.stream_server.c_str());
    }
    ImGui::SameLine();
    { // Offline export (Phase 2 extension). Toggles the export page (enter when idle, exit
      // when already in it); disabled while an export is running.
        IconButtonResult r = icon_button("##export_btn", ImVec2(btn_w, btn_h), ui::AppIcon::Export, export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true;
            else ui_intents_.export_enter = true;
        }
        if (ImGui::IsItemHovered() && !ui_str_.export_video.empty())
            ImGui::SetTooltip("%s", ui_str_.export_video.c_str());
    }
    // Title text: wider kSpaceM gap after the icon cluster (the bar's uniform 2px beat is
    // right between buttons but reads cramped before text).
    ImGui::SameLine(0.0f, ui_s(ui::theme::kSpaceM));
    {
        // Cap the filename width so a long basename cannot shove the right-side chrome
        // (min/max/fullscreen/close) off the bar. Leave room for those 4 buttons + a thin
        // drag gap; elide with "..." (UTF-8-safe) and expose the full name on hover.
        float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY((bar_h - th) * 0.5f);
        const int n_right = 4;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float reserve_right = (btn_w + spacing) * n_right + spacing;
        float max_title_w = ImGui::GetContentRegionAvail().x - reserve_right;
        if (max_title_w < 0.0f) max_title_w = 0.0f;
        const std::string full_name = export_mode_ ? ui_str_.export_title : sumu_ui::basename_of(video_path_);
        const std::string shown = sumu_ui::elide_text_to_width(full_name, max_title_w);
        ImGui::TextUnformatted(shown.c_str());
        if (!full_name.empty() && shown != full_name && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", full_name.c_str());
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY((bar_h - btn_h) * 0.5f);

    // Blank draggable region (native-titlebar-like drag), sized to leave room for the
    // four right-side buttons so it doesn't eat their clicks.
    const int n_btns = 4; // minimize, maximize/restore, fullscreen, close
    float drag_w = ImGui::GetContentRegionAvail().x - (btn_w + ImGui::GetStyle().ItemSpacing.x) * n_btns;
    if (drag_w < 0.0f) drag_w = 0.0f;
    ImGui::InvisibleButton("##drag_region", ImVec2(drag_w, btn_h));
    // Publish this frame's drag rect (client-space) for WndProc's WM_NCHITTEST to consume
    // (see point_in_caption_drag()'s header comment) -- WM_NCHITTEST reporting HTCAPTION
    // here gives OS-native drag/double-click-maximize/Aero-Snap, so no synthetic
    // WM_NCLBUTTONDOWN SendMessage is needed (that used to leave ImGui's mouse-down state
    // stuck until an extra click).
    {
        ImVec2 dmin = ImGui::GetItemRectMin();
        ImVec2 dmax = ImGui::GetItemRectMax();
        caption_drag_x0_ = dmin.x; caption_drag_x1_ = dmax.x; caption_drag_y1_ = bar_h;
    }
    ImGui::SameLine();

    { // minimize
        IconButtonResult r = icon_button("##min_btn", ImVec2(btn_w, btn_h), ui::AppIcon::WinMinimize);
        if (r.clicked) ShowWindow(hwnd_, SW_MINIMIZE);
    }
    ImGui::SameLine();

    // While borderless-fullscreen, IsZoomed is forced false (WS_MAXIMIZE cleared on enter).
    // The button still means "leave FS into a maximized window" rather than a no-op.
    bool zoomed = !fullscreen_.load(std::memory_order_relaxed) && IsZoomed(hwnd_) != 0;
    { // maximize/restore
        IconButtonResult r = icon_button("##max_btn", ImVec2(btn_w, btn_h),
            zoomed ? ui::AppIcon::WinRestore : ui::AppIcon::WinMaximize);
        if (r.clicked) {
            if (fullscreen_.load(std::memory_order_relaxed)) {
                // Exit FS first (restores pre-FS placement), then maximize if still windowed.
                toggle_fullscreen();
                if (hwnd_ && !IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_MAXIMIZE);
            } else {
                ShowWindow(hwnd_, zoomed ? SW_RESTORE : SW_MAXIMIZE);
            }
        }
    }
    ImGui::SameLine();

    { // fullscreen toggle
        IconButtonResult r = icon_button("##fullscreen_btn", ImVec2(btn_w, btn_h), ui::AppIcon::Fullscreen);
        if (r.clicked) toggle_fullscreen();
    }
    ImGui::SameLine();

    { // close -- destructive-action standard: glyph flips to kError on hover (same as the
      // export preset trash / queue remove buttons).
        IconButtonResult r = icon_button("##close_btn", ImVec2(btn_w, btn_h));
        ui::DrawIconButtonGlyph(r, ui::AppIcon::Close, ImGui::IsItemHovered()
            ? ui::theme::error_u32() : ui::theme::icon_color_u32());
        if (r.clicked) PostMessageA(hwnd_, WM_CLOSE, 0, 0);
    }

    // Bottom hairline: the title-bar/body division -- one of the two surviving strokes in
    // this borderless design (the other is floating-window borders).
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float y = wp.y + bar_h - 0.5f;
        dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowSize().x, y),
            ui::theme::border_u32());
    }

    ImGui::End();
    ImGui::PopStyleVar(2); // ItemSpacing + WindowPadding
}

// Auto-hides when the mouse leaves the bottom ~15% of the client area (simple mouse-Y-zone
// check, per the task brief). Self-drawn progress bar (InvisibleButton + ImDrawList, not
// SliderInt): click/drag -> frame_for_seekbar_x() -> record_seek(); playhead position ->
// seekbar_x_for_frame(current_frame()). Height matches the title bar (top_bar_h()).
void Player::build_bottom_bar(){
    ImGuiIO& io = ImGui::GetIO();
    const float bar_h = top_bar_h();
    const float show_zone_y = io.DisplaySize.y * 0.85f;
    bool mouse_in_zone = io.MousePos.y >= show_zone_y && io.MousePos.y <= io.DisplaySize.y &&
        io.MousePos.x >= 0.0f && io.MousePos.x <= io.DisplaySize.x;
    if (!mouse_in_zone) return; // auto-hide

    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bar_h));
    // NoBackground: the bg is self-drawn right after Begin() with per-corner rounding --
    // rounded TOP corners + square BOTTOM corners (WindowRounding is all-four-corners only),
    // mirroring the top bar's square-top treatment.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(ui::theme::kSpaceXS), 0.0f));
    ImGui::Begin("##sumu_bottom_bar", nullptr, flags);

    { // Bar background: same translucent wash the window bg used to provide
      // (kWindowBg @ kOverlayBgAlpha, video shows through), but with only the TOP corners
      // rounded -- square bottom corners sit flush against the window's bottom edge.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        const ImVec4 bg = ui::theme::kWindowBg; // same translucent wash the window bg used
        dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32( // to provide (video shows
            ImVec4(bg.x, bg.y, bg.z, ui::theme::kOverlayBgAlpha)), // through), top corners only
            ui_s(ui::theme::kRadiusWindow), ImDrawFlags_RoundCornersTop);
    }

    // Row height for every control -- vertically centered, same 2px-inset beat as the title
    // bar (36px bar / 32px controls, theme::kSpaceXS).
    const float row_h = bar_h - ui_s(2.0f * ui::theme::kSpaceXS);
    ImGui::SetCursorPosY((bar_h - row_h) * 0.5f);

    { // play/pause: icon shows the ACTION a click performs (matches the old "Pause"/"Play"
      // text label semantics) -- playing state shows the pause glyph, paused shows play.
        IconButtonResult r = icon_button("##play_pause_btn",
            ImVec2(ui_s(ui::theme::kControlHeight), row_h),
            is_playing() ? ui::AppIcon::Pause : ui::AppIcon::Play);
        if (r.clicked) record_toggle_play();
    }
    ImGui::SameLine();

    int64_t fc = frame_count();
    double fpsv = fps() > 0.0 ? fps() : 1.0;
    // Align time text with the row center (ImGui text is baseline-aligned by default).
    {
        float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY((bar_h - th) * 0.5f);
        ImGui::TextUnformatted(sumu_ui::format_mmss(static_cast<double>(current_frame()) / fpsv).c_str());
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY((bar_h - row_h) * 0.5f);

    // Reserve space for the trailing total-duration label so the track sizes itself first.
    std::string total_str = sumu_ui::format_mmss(static_cast<double>(std::max<int64_t>(fc - 1, 0)) / fpsv);
    float total_w = ImGui::CalcTextSize(total_str.c_str()).x;
    // Volume/mute controls (additive): shown only when has_audio(), trailing the
    // total-duration label -- reserve their width the same way total_w already reserves
    // room for the duration text, so the seekbar track shrinks to make room rather than
    // being overdrawn.
    const float vol_icon_w = ui_s(ui::theme::kControlHeight); // standard icon-button square
    const float vol_slider_w = ui_s(70.0f);
    // Right-edge clearance: kSpaceM (8px) of clear space past the track's end, minus the
    // window padding already eaten (kSpaceXS).
    const float right_pad = ui_s(ui::theme::kSpaceM - ui::theme::kSpaceXS);
    float vol_ctrl_w = has_audio()
        ? (ImGui::GetStyle().ItemSpacing.x + vol_icon_w + ImGui::GetStyle().ItemSpacing.x + vol_slider_w + right_pad)
        : right_pad;
    float track_w = ImGui::GetContentRegionAvail().x - total_w - vol_ctrl_w - ImGui::GetStyle().ItemSpacing.x;
    if (track_w < ui_s(10.0f)) track_w = ui_s(10.0f);
    const float track_h = row_h;

    ImVec2 track_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##seekbar", ImVec2(track_w, track_h));
    bool track_active = ImGui::IsItemActive();
    bool track_hovered = ImGui::IsItemHovered();
    float x0 = track_pos.x;
    float x1 = track_pos.x + track_w;
    float bar_y = track_pos.y + track_h * 0.5f;

    // Seek only on the initial click (IsItemActivated) or when the mapped frame changes
    // during a drag. Holding still on the bar must NOT re-fire seek every ui_tick -- that
    // repositions to the same frame repeatedly and makes playback twitch in place.
    if (track_active) {
        int64_t target = sumu_ui::frame_for_seekbar_x(ImGui::GetIO().MousePos.x, x0, x1, fc);
        if (ImGui::IsItemActivated() || target != seekbar_last_seek_frame_) {
            record_seek(target);
            seekbar_last_seek_frame_ = target;
        }
    } else {
        seekbar_last_seek_frame_ = -1; // next press always counts as a fresh click
    }

    // Slider-language rendering (the settings-panel 拉条 look): thin rounded track +
    // rounded yellow fill + white outlined knob; hover/drag brightens the fill.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float media_track_h = ui_s(ui::theme::kSliderTrackH);
    const float played_x = sumu_ui::seekbar_x_for_frame(current_frame(), x0, x1, fc);
    draw_media_bar(dl, x0, x1, bar_y, (played_x - x0) / std::max(x1 - x0, 1.0f),
        media_track_h, track_hovered || track_active);

    if (track_hovered || track_active) {
        float hx = std::clamp(ImGui::GetIO().MousePos.x, x0, x1);
        dl->AddLine(ImVec2(hx, track_pos.y - ui_s(4.0f)), ImVec2(hx, track_pos.y + track_h + ui_s(4.0f)),
            ui::theme::text_secondary_u32());

        ID3D11ShaderResourceView* thumb = get_thumbnail(sumu_ui::frame_for_seekbar_x(hx, x0, x1, fc));
        if (thumb) {
            // Draw on the FOREGROUND draw list, NOT this window's `dl`: the bottom bar window
            // is only bar_h tall, so its clip rect clips anything above it -- the
            // thumbnail sits ~100px above the track and would be entirely clipped away
            // (that was the M4 "thumbnail never shows" bug). The foreground list is clipped
            // only to the whole viewport and always renders on top.
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImVec2 tsize(ui_s(160.0f), ui_s(90.0f));
            float tx = std::clamp(hx - tsize.x * 0.5f, x0, std::max(x0, x1 - tsize.x));
            ImVec2 tmin(tx, track_pos.y - ui_s(12.0f) - tsize.y);
            ImVec2 tmax(tmin.x + tsize.x, tmin.y + tsize.y);
            const float pad = ui_s(ui::theme::kSpaceS);
            const float rounding = ui_s(ui::theme::kRadiusControl);
            // Floating over the video: panel fill one step above the window, held
            // near-opaque so the thumbnail stays readable. Borderless like every card.
            ImVec4 thumb_bg = ui::theme::kPanelBg;
            thumb_bg.w = 0.90f;
            fg->AddRectFilled(ImVec2(tmin.x - pad, tmin.y - pad), ImVec2(tmax.x + pad, tmax.y + pad),
                ui::theme::to_u32(thumb_bg), rounding);
            fg->AddImage(thumb, tmin, tmax);
        }
    }

    ImGui::SameLine();
    {
        float th = ImGui::GetTextLineHeight();
        ImGui::SetCursorPosY((bar_h - th) * 0.5f);
        ImGui::TextUnformatted(total_str.c_str());
    }

    if (has_audio()) {
        ImGui::SameLine();
        ImGui::SetCursorPosY((bar_h - row_h) * 0.5f);
        { // mute toggle: volume-2 / volume-x atlas glyph. Click toggles native mute state
          // directly (no UiIntents -- see set_muted()'s header comment).
            IconButtonResult r = icon_button("##mute_btn", ImVec2(vol_icon_w, row_h),
                is_muted() ? ui::AppIcon::VolumeMute : ui::AppIcon::Volume);
            if (r.clicked) toggle_mute();
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY((bar_h - row_h) * 0.5f);
        { // volume slider: same self-drawn InvisibleButton+track+fill+handle pattern as the
          // seekbar above, but drag writes volume_ directly via set_volume() -- pure native
          // state, no record_seek()/UiIntents involved (see set_volume()'s header comment).
            ImVec2 vs_pos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##vol_slider", ImVec2(vol_slider_w, track_h));
            bool vs_active = ImGui::IsItemActive();
            float vx0 = vs_pos.x;
            float vx1 = vs_pos.x + vol_slider_w;
            float vbar_y = vs_pos.y + track_h * 0.5f;
            if (vs_active) {
                float t = (ImGui::GetIO().MousePos.x - vx0) / std::max(vx1 - vx0, 1.0f);
                set_volume(std::clamp(t, 0.0f, 1.0f));
            }
            float vol = get_volume();
            ImDrawList* vdl = ImGui::GetWindowDrawList();
            draw_media_bar(vdl, vx0, vx1, vbar_y, std::clamp(vol, 0.0f, 1.0f),
                media_track_h, vs_active || ImGui::IsItemHovered());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
}

void Player::build_settings_panel(float top_bar_h){
    ImGuiIO& io = ImGui::GetIO();
    // Wider than the old 200px: rows are now [number box][slider track] compounds.
    const float panel_w = ui_s(260.0f);
    // Outer margin from the window edges == the panel's own inner padding
    // (kPaddingContainer), same beat as the export screen's cards.
    const float margin = ui_s(ui::theme::kPaddingContainer);
    ImGui::SetNextWindowPos(ImVec2(margin, top_bar_h + margin));
    // Auto-height to content (AlwaysAutoResize) so the panel does not cover the bottom bar.
    // Cap max height so a tiny window still scrolls rather than overflowing the client.
    // bottom bar height == title bar height (kTopBarHBase); margins kept above it too.
    const float max_h = std::max(0.0f,
        io.DisplaySize.y - top_bar_h - 2.0f * margin - ui_s(kTopBarHBase));
    ImGui::SetNextWindowSizeConstraints(ImVec2(panel_w, 0.0f), ImVec2(panel_w, max_h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize;
    // Unified solid panel background (was a translucent overlay showing the video through).
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
    // kPaddingContainer inset so labels/sliders aren't flush against the panel edge.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(ui::theme::kPaddingContainer), ui_s(ui::theme::kPaddingContainer)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_s(ui::theme::kRadiusWindow));
    ImGui::Begin("##sumu_settings", nullptr, flags);

    // Staged locally in settings_edit_*_, seeded from the Python-refreshed ui_cfg_*
    // mirror only once per open (not every frame -- that would clobber an in-progress
    // edit with Python's per-tick refresh of the ACTUAL committed config).
    // Commit on release (IsItemDeactivatedAfterEdit) / Combo selection change -- never on
    // every slider tick -- so Python rebuilds the scheduler once per finished edit, not
    // dozens of times while dragging.
    if (!settings_edit_init_) {
        settings_edit_clip_length_ = ui_cfg_clip_length_;
        settings_edit_max_regions_ = ui_cfg_max_regions_;
        settings_edit_cold_start_s_ = ui_cfg_cold_start_s_;
        settings_edit_lead_ = ui_cfg_lead_;
        // Combo index: 0=原始, 1=30, 2=60
        settings_edit_target_fps_idx_ =
            (ui_cfg_target_fps_ == 30) ? 1 : (ui_cfg_target_fps_ == 60) ? 2 : 0;
        settings_edit_init_ = true;
    }

    // (No panel title -- the rows start directly; the first LineLabel skips its top gap
    // via ui::at_container_top() so the WindowPadding alone insets it.)

    // Tooltips at kFontSizeSm (SetTooltip has no font arg -- BeginTooltip + PushFont).
    auto settings_tooltip = [&](const char* text) {
        if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || !text || !text[0]) return;
        ImGui::BeginTooltip();
        ImGui::PushFont(nullptr, kFontSizeSm);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndTooltip();
    };
    // ui::SliderInt/Float are compound rows ([number box][track]); `committed` replaces
    // the old IsItemDeactivatedAfterEdit() check, which cannot span a compound widget.
    bool committed = false;
    ui::LineLabel(ui_str_.lead_label.c_str());
    ui::SliderInt("##lead", &settings_edit_lead_, 1, 180, 0.0f, &committed);
    if (committed && settings_edit_lead_ != ui_cfg_lead_)
        ui_intents_.lead = settings_edit_lead_;
    settings_tooltip(ui_str_.lead_tooltip.c_str());
    ui::LineLabel(ui_str_.clip_length_label.c_str());
    ui::SliderInt("##clip_length", &settings_edit_clip_length_, 1, 180, 0.0f, &committed);
    if (committed && settings_edit_clip_length_ != ui_cfg_clip_length_)
        ui_intents_.clip_length = settings_edit_clip_length_;
    settings_tooltip(ui_str_.clip_length_tooltip.c_str());
    ui::LineLabel(ui_str_.max_regions_label.c_str());
    ui::SliderInt("##max_regions", &settings_edit_max_regions_, 1, 8, 0.0f, &committed);
    if (committed && settings_edit_max_regions_ != ui_cfg_max_regions_)
        ui_intents_.max_regions = settings_edit_max_regions_;
    settings_tooltip(ui_str_.max_regions_tooltip.c_str());
    ui::LineLabel(ui_str_.cold_start_label.c_str());
    ui::SliderFloat("##cold_start_s", &settings_edit_cold_start_s_, 0.0f, 3.0f, "%.1f", 0.0f, &committed);
    if (committed && settings_edit_cold_start_s_ != ui_cfg_cold_start_s_)
        ui_intents_.cold_start_s = settings_edit_cold_start_s_;
    settings_tooltip(ui_str_.cold_start_tooltip.c_str());
    {
        const char* target_fps_items[] = {
            ui_str_.target_fps_original.c_str(), "30", "60"
        };
        ui::LineLabel(ui_str_.target_fps_label.c_str());
        if (ui::Combo("##target_fps", target_fps_items, 3, &settings_edit_target_fps_idx_)) {
            // Combo is click-select (no drag) -- commit on the selection change itself.
            static const int kTargetFpsByIdx[] = { 0, 30, 60 };
            int idx = settings_edit_target_fps_idx_;
            if (idx < 0 || idx > 2) idx = 0;
            int fps = kTargetFpsByIdx[idx];
            if (fps != ui_cfg_target_fps_)
                ui_intents_.target_fps = fps;
        }
        settings_tooltip(ui_str_.target_fps_tooltip.c_str());
    }

    // ── Engine status + optional compile button / progress bar ──────────────────
    // Driven by Python's set_trt_engine_status() and set_compile_ui().
    // Net vertical gaps here are kSpaceL (12px), same as LineLabel. A spacer Dummy would
    // attract ItemSpacing.y on BOTH sides (2x8=16px before any own height, already
    // overshooting 12) and the separator is a non-item (spacing on one side only), so each
    // gap is made by nudging the cursor with SetCursorPosY to land exactly kSpaceL.
    const float is_px = ImGui::GetStyle().ItemSpacing.y;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
        std::max(0.0f, ui_s(ui::theme::kSpaceL) - is_px));
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float sep_w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(cp.x, cp.y + 2.0f), ImVec2(cp.x + sep_w, cp.y + 2.0f),
            ui::theme::border_u32());
        // Separator is not an item (no trailing ItemSpacing) -- land the status text
        // kSpaceL below it directly.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui_s(ui::theme::kSpaceL));
    }

    ImGui::PushFont(nullptr, kFontSizeSm);
    {
        // Status line text + color, plus whether a compile control (button/progress/retry)
        // follows. Driven solely by the Python-resolved engine status enum -- no re-derivation
        // from status_text_ or compile_ui_state_ here.
        const char* eng_text = nullptr;
        ImVec4 eng_color = ui::theme::kTextSecondary; // gray default
        int ctrl_kind = 0; // 0 none / 1 compile button / 2 progress bar / 3 retry button
        switch (trt_engine_status_) {
        case 0: // Warming
            eng_text = ui_str_.trt_warming.c_str();
            break;
        case 1: // WarmupFailed
            eng_text = ui_str_.warmup_failed.c_str();
            eng_color = ui::theme::kError;
            break;
        case 2: // Ready (TRT active)
            eng_text = ui_str_.trt_ready.c_str();
            eng_color = ui::theme::kText;
            break;
        case 3: // NotApplicable (non-Nvidia / non-fp16)
            eng_text = ui_str_.trt_not_applicable.c_str();
            break;
        case 4: // NotCompiled idle -> offer compile
            eng_text = ui_str_.trt_not_compiled.c_str();
            eng_color = ui::theme::kWarning;
            ctrl_kind = 1;
            break;
        case 5: // Compiling (or queued "preparing") -> progress bar
            eng_text = ui_str_.trt_compiling.c_str();
            eng_color = ui::theme::kWarning;
            ctrl_kind = 2;
            break;
        case 6: // CompileFailed -> offer retry
            eng_text = ui_str_.compile_failed.c_str();
            eng_color = ui::theme::kError;
            ctrl_kind = 3;
            break;
        default:
            break;
        }

        if (eng_text) {
            ImGui::PushStyleColor(ImGuiCol_Text, eng_color);
            ImGui::TextUnformatted(eng_text);
            ImGui::PopStyleColor();
        }

        if (ctrl_kind != 0) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                std::max(0.0f, ui_s(ui::theme::kSpaceL) - is_px));
            const float ctrl_w = ImGui::GetContentRegionAvail().x;
            const float ctrl_h = ImGui::GetFrameHeight();
            if (ctrl_kind == 2) {
                // Center "step/total" on the bar once a real count exists (total <= 0 while
                // queued/preparing -- leave the bar bare so we don't show a bogus "0/0").
                std::string overlay;
                if (compile_ui_total_ > 0)
                    overlay = std::to_string(compile_ui_step_) + "/" +
                        std::to_string(compile_ui_total_);
                ui::ProgressBar(compile_ui_progress_, ctrl_w, ctrl_h, overlay.c_str());
            } else {
                const char* btn_label = (ctrl_kind == 3) ? ui_str_.compile_retry.c_str()
                                                         : ui_str_.compile_engine.c_str();
                if (ui::Button(btn_label, ui::ButtonVariant::Primary, ui::ControlSize::Regular,
                        ctrl_w))
                    record_compile_engine();
            }
        }
    }
    ImGui::PopFont();

    // Net BasicVSR restore throughput from Python Scheduler (restore_clip wall time only;
    // excludes frontier-gate sleeps). Pushed each tick via set_ui_config; <0 means no
    // restore has finished yet (or no scheduler).
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
        std::max(0.0f, ui_s(ui::theme::kSpaceL) - is_px));
    ImGui::PushFont(nullptr, kFontSizeSm);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
    if (ui_cfg_ai_restore_fps_ >= 0.0f)
        ImGui::Text(ui_str_.ai_speed.c_str(), ui_cfg_ai_restore_fps_);
    else
        ImGui::TextUnformatted(ui_str_.ai_speed_unknown.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    // Floating-window stroke (kBorderStrong): layer separation over the video -- the same
    // language as ui::EndModal's border, one of the two surviving strokes in this design.
    // Rect captured BEFORE End() (afterwards the current window is the parent).
    const ImVec2 panel_min = ImGui::GetWindowPos();
    const ImVec2 panel_max(panel_min.x + ImGui::GetWindowSize().x,
        panel_min.y + ImGui::GetWindowSize().y);
    ImGui::End();
    ImGui::GetForegroundDrawList()->AddRect(
        ImVec2(panel_min.x + 0.5f, panel_min.y + 0.5f),
        ImVec2(panel_max.x - 0.5f, panel_max.y - 0.5f),
        ui::theme::border_strong_u32(), ui_s(ui::theme::kRadiusWindow), 0, 1.0f);
    ImGui::PopStyleVar(2); // WindowRounding + WindowPadding
    ImGui::PopStyleColor(); // WindowBg
}

