// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "ui_util.h"

Player::IconButtonResult Player::icon_button(const char* str_id, ImVec2 size, bool disabled){
    // Promoted to the design-system widget layer (ui/widgets.cpp). The hover-wash rounding
    // there reads GetStyle().FrameRounding -- apply_theme sets it to kRadiusControl (6px @
    // 96 DPI) and apply_ui_dpi()'s ScaleAllSizes scales it, so it equals the old ui_s(6.0f).
    return ui::IconButton(str_id, size, disabled);
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    // Panel tone (theme kPanelBg), one step above the window background, so the chrome
    // reads as a bar against the video/splash.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(4.0f), 0.0f));
    ImGui::Begin("##sumu_top_bar", nullptr, flags);

    const float btn_w = ui_s(28.0f);
    const float btn_h = bar_h - ui_s(4.0f);
    const ImU32 icon_col = ui::theme::icon_color_u32();
    const ImU32 icon_col_dim = ui::theme::icon_color_dim_u32();
    const float icon_th = ui_s(1.5f);
    // Export-mode gating (Phase 2): settings is disabled while the export page is up (its own
    // AI-pipeline section replaces the settings panel); open/URL/web + the export toggle are
    // disabled while an export is running so the user cannot navigate away mid-export. This
    // gating replaces the export screen's old "返回" exit button.
    const bool settings_disabled = export_mode_;
    const bool export_nav_disabled = export_mode_ && export_running_;
    // Vertically center every control in the bar (ImGui default is top-aligned).
    ImGui::SetCursorPosY((bar_h - btn_h) * 0.5f);

    { // settings toggle: hamburger icon (three horizontal bars), replaces the old text button
        IconButtonResult r = icon_button("##settings_btn", ImVec2(btn_w, btn_h), settings_disabled);
        if (r.clicked) {
            ui_settings_open_ = !ui_settings_open_;
            if (ui_settings_open_) settings_edit_init_ = false; // resync edit buffers to latest cfg on open
        }
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const float w = ui_s(14.0f);
        const ImU32 col = settings_disabled ? icon_col_dim : icon_col;
        for (int i = -1; i <= 1; ++i) {
            float y = cy + i * ui_s(5.0f);
            r.dl->AddLine(ImVec2(cx - w * 0.5f, y), ImVec2(cx + w * 0.5f, y), col, icon_th);
        }
    }
    ImGui::SameLine();

    { // M-C2: "open file" button (folder glyph: tab + body, same hollow-rect visual weight
      // as the maximize/fullscreen icons below) -- records open_dialog, drained by Python's
      // take_ui_intents() which responds by calling the blocking pick_open_file() dialog
      // (see UiIntents' header comment; present keeps showing the current video meanwhile).
        IconButtonResult r = icon_button("##open_btn", ImVec2(btn_w, btn_h), export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // open exits the export page first
            record_open_dialog();
        }
        if (ImGui::IsItemHovered() && !ui_str_.open_file.empty())
            ImGui::SetTooltip("%s", ui_str_.open_file.c_str());
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const ImU32 col = export_nav_disabled ? icon_col_dim : icon_col;
        r.dl->AddRect(ImVec2(cx - ui_s(7.0f), cy - ui_s(3.0f)), ImVec2(cx + ui_s(7.0f), cy + ui_s(6.0f)), col, 1.0f, 0, icon_th);
        r.dl->AddRect(ImVec2(cx - ui_s(7.0f), cy - ui_s(6.0f)), ImVec2(cx - ui_s(1.0f), cy - ui_s(3.0f)), col, 1.0f, 0, icon_th);
    }
    ImGui::SameLine();
    { // Network URL open: chain-link glyph next to the folder button. Opens the ImGui
      // URL popup (build_open_url_popup); on confirm writes open_path for Python.
        IconButtonResult r = icon_button("##open_url_btn", ImVec2(btn_w, btn_h), export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // URL open exits the export page first
            request_open_url_popup();
        }
        if (ImGui::IsItemHovered() && !ui_str_.open_url.empty())
            ImGui::SetTooltip("%s", ui_str_.open_url.c_str());
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        // Two small overlapping rings (link icon), hollow-rect weight matching neighbors.
        const float r_link = ui_s(4.2f);
        const float dx = ui_s(2.4f);
        const float dy = ui_s(2.0f);
        const ImU32 col = export_nav_disabled ? icon_col_dim : icon_col;
        r.dl->AddCircle(ImVec2(cx - dx, cy + dy), r_link, col, 12, icon_th);
        r.dl->AddCircle(ImVec2(cx + dx, cy - dy), r_link, col, 12, icon_th);
    }
    ImGui::SameLine();
    { // Web-stream server: globe glyph (circle + equator + meridian), hollow-line weight.
        IconButtonResult r = icon_button("##stream_btn", ImVec2(btn_w, btn_h), export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true; // web server exits the export page first
            request_stream_popup();
        }
        if (ImGui::IsItemHovered() && !ui_str_.stream_server.empty())
            ImGui::SetTooltip("%s", ui_str_.stream_server.c_str());
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const float rr = ui_s(6.0f);
        const ImU32 col = export_nav_disabled ? icon_col_dim : icon_col;
        r.dl->AddCircle(ImVec2(cx, cy), rr, col, 24, icon_th);
        r.dl->AddLine(ImVec2(cx - rr, cy), ImVec2(cx + rr, cy), col, icon_th);
        r.dl->AddLine(ImVec2(cx, cy - rr), ImVec2(cx, cy + rr), col, icon_th);
    }
    ImGui::SameLine();
    { // Offline export (Phase 2 extension): download-into-tray glyph. Toggles the export page
      // (enter when idle, exit when already in it); disabled while an export is running.
        IconButtonResult r = icon_button("##export_btn", ImVec2(btn_w, btn_h), export_nav_disabled);
        if (r.clicked) {
            if (export_mode_) ui_intents_.export_exit = true;
            else ui_intents_.export_enter = true;
        }
        if (ImGui::IsItemHovered() && !ui_str_.export_video.empty())
            ImGui::SetTooltip("%s", ui_str_.export_video.c_str());
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const ImU32 col = export_nav_disabled ? icon_col_dim : icon_col;
        r.dl->AddLine(ImVec2(cx, cy - ui_s(5.0f)), ImVec2(cx, cy + ui_s(3.0f)), col, icon_th);
        r.dl->AddLine(ImVec2(cx - ui_s(3.0f), cy), ImVec2(cx, cy + ui_s(3.0f)), col, icon_th);
        r.dl->AddLine(ImVec2(cx + ui_s(3.0f), cy), ImVec2(cx, cy + ui_s(3.0f)), col, icon_th);
        r.dl->AddLine(ImVec2(cx - ui_s(6.0f), cy + ui_s(5.0f)), ImVec2(cx + ui_s(6.0f), cy + ui_s(5.0f)), col, icon_th);
    }
    ImGui::SameLine();
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

    { // minimize: single horizontal bar
        IconButtonResult r = icon_button("##min_btn", ImVec2(btn_w, btn_h));
        if (r.clicked) ShowWindow(hwnd_, SW_MINIMIZE);
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        r.dl->AddLine(ImVec2(cx - ui_s(6.0f), cy), ImVec2(cx + ui_s(6.0f), cy), icon_col, icon_th);
    }
    ImGui::SameLine();

    // While borderless-fullscreen, IsZoomed is forced false (WS_MAXIMIZE cleared on enter).
    // The button still means "leave FS into a maximized window" rather than a no-op.
    bool zoomed = !fullscreen_.load(std::memory_order_relaxed) && IsZoomed(hwnd_) != 0;
    { // maximize/restore: hollow square (not maximized) / overlapping double square (maximized)
        IconButtonResult r = icon_button("##max_btn", ImVec2(btn_w, btn_h));
        if (r.clicked) {
            if (fullscreen_.load(std::memory_order_relaxed)) {
                // Exit FS first (restores pre-FS placement), then maximize if still windowed.
                toggle_fullscreen();
                if (hwnd_ && !IsZoomed(hwnd_)) ShowWindow(hwnd_, SW_MAXIMIZE);
            } else {
                ShowWindow(hwnd_, zoomed ? SW_RESTORE : SW_MAXIMIZE);
            }
        }
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        if (!zoomed) {
            r.dl->AddRect(ImVec2(cx - ui_s(5.0f), cy - ui_s(5.0f)), ImVec2(cx + ui_s(5.0f), cy + ui_s(5.0f)), icon_col, 0.0f, 0, icon_th);
        } else {
            r.dl->AddRect(ImVec2(cx - ui_s(5.0f), cy - ui_s(3.0f)), ImVec2(cx + ui_s(3.0f), cy + ui_s(5.0f)), icon_col, 0.0f, 0, icon_th);
            r.dl->AddRect(ImVec2(cx - ui_s(3.0f), cy - ui_s(5.0f)), ImVec2(cx + ui_s(5.0f), cy + ui_s(3.0f)), icon_col, 0.0f, 0, icon_th);
        }
    }
    ImGui::SameLine();

    { // fullscreen toggle: four corner brackets -- deliberately distinct from the maximize
      // square above so the two aren't visually confusable
        IconButtonResult r = icon_button("##fullscreen_btn", ImVec2(btn_w, btn_h));
        if (r.clicked) toggle_fullscreen();
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const float half = ui_s(6.0f), leg = ui_s(3.5f);
        const ImVec2 corners[4] = {
            ImVec2(cx - half, cy - half), ImVec2(cx + half, cy - half),
            ImVec2(cx - half, cy + half), ImVec2(cx + half, cy + half),
        };
        const float dirx[4] = { 1.0f, -1.0f, 1.0f, -1.0f };
        const float diry[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
        for (int i = 0; i < 4; ++i) {
            r.dl->AddLine(corners[i], ImVec2(corners[i].x + dirx[i] * leg, corners[i].y), icon_col, icon_th);
            r.dl->AddLine(corners[i], ImVec2(corners[i].x, corners[i].y + diry[i] * leg), icon_col, icon_th);
        }
    }
    ImGui::SameLine();

    { // close: X (two diagonals)
        IconButtonResult r = icon_button("##close_btn", ImVec2(btn_w, btn_h));
        if (r.clicked) PostMessageA(hwnd_, WM_CLOSE, 0, 0);
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const float half = ui_s(5.0f);
        r.dl->AddLine(ImVec2(cx - half, cy - half), ImVec2(cx + half, cy + half), icon_col, icon_th);
        r.dl->AddLine(ImVec2(cx - half, cy + half), ImVec2(cx + half, cy - half), icon_col, icon_th);
    }

    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
    ImGui::PopStyleColor(); // WindowBg
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    // Match build_status_float(): translucent so the video shows through.
    ImGui::SetNextWindowBgAlpha(ui::theme::kOverlayBgAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(4.0f), 0.0f));
    ImGui::Begin("##sumu_bottom_bar", nullptr, flags);

    // Row height for every control -- vertically centered in the bar.
    const float row_h = bar_h - ui_s(4.0f);
    ImGui::SetCursorPosY((bar_h - row_h) * 0.5f);

    { // play/pause: icon shows the ACTION a click performs (matches the old "Pause"/"Play"
      // text label semantics) -- playing state draws a pause glyph (two bars), paused state
      // draws a play glyph (solid triangle).
        IconButtonResult r = icon_button("##play_pause_btn", ImVec2(ui_s(40.0f), row_h));
        if (r.clicked) record_toggle_play();
        float cx = (r.min.x + r.max.x) * 0.5f;
        float cy = (r.min.y + r.max.y) * 0.5f;
        const ImU32 icon_col = ui::theme::icon_color_u32();
        if (is_playing()) {
            const float bar_w = ui_s(4.0f), bar_gap = ui_s(4.0f), bar_hh = ui_s(14.0f);
            r.dl->AddRectFilled(ImVec2(cx - bar_gap * 0.5f - bar_w, cy - bar_hh * 0.5f),
                ImVec2(cx - bar_gap * 0.5f, cy + bar_hh * 0.5f), icon_col);
            r.dl->AddRectFilled(ImVec2(cx + bar_gap * 0.5f, cy - bar_hh * 0.5f),
                ImVec2(cx + bar_gap * 0.5f + bar_w, cy + bar_hh * 0.5f), icon_col);
        } else {
            const float tri_w = ui_s(12.0f), tri_hh = ui_s(14.0f);
            r.dl->AddTriangleFilled(
                ImVec2(cx - tri_w * 0.4f, cy - tri_hh * 0.5f),
                ImVec2(cx - tri_w * 0.4f, cy + tri_hh * 0.5f),
                ImVec2(cx + tri_w * 0.6f, cy),
                icon_col);
        }
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
    const float vol_icon_w = ui_s(28.0f);
    const float vol_slider_w = ui_s(70.0f);
    const float right_pad = ui_s(ui::theme::kPaddingContainer); // standard container inset
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

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float track_half = ui_s(2.0f);
    dl->AddRectFilled(ImVec2(x0, bar_y - track_half), ImVec2(x1, bar_y + track_half),
        ui::theme::media_track_u32());
    float played_x = sumu_ui::seekbar_x_for_frame(current_frame(), x0, x1, fc);
    dl->AddRectFilled(ImVec2(x0, bar_y - track_half), ImVec2(played_x, bar_y + track_half),
        ui::theme::media_fill_u32());
    dl->AddCircleFilled(ImVec2(played_x, bar_y), ui_s(5.0f), ui::theme::icon_color_u32());

    if (track_hovered || track_active) {
        float hx = std::clamp(ImGui::GetIO().MousePos.x, x0, x1);
        dl->AddLine(ImVec2(hx, track_pos.y - ui_s(4.0f)), ImVec2(hx, track_pos.y + track_h + ui_s(4.0f)),
            IM_COL32(255, 255, 255, 160));

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
            const float pad = ui_s(3.0f);
            fg->AddRectFilled(ImVec2(tmin.x - pad, tmin.y - pad), ImVec2(tmax.x + pad, tmax.y + pad),
                IM_COL32(20, 20, 20, 230), pad);
            fg->AddImage(thumb, tmin, tmax);
            fg->AddRect(ImVec2(tmin.x - pad, tmin.y - pad), ImVec2(tmax.x + pad, tmax.y + pad),
                IM_COL32(230, 230, 230, 200), pad);
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
        { // mute icon: speaker body (small filled rect) + flare (a trapezoid, not a plain
          // triangle -- AddTriangleFilled can't produce a correct cone shape here) with
          // sound-wave arcs when unmuted, or a diagonal slash across the whole glyph when
          // muted. Click toggles native mute state directly (no UiIntents -- see
          // set_muted()'s header comment), same style language as the top-bar icons above.
            IconButtonResult r = icon_button("##mute_btn", ImVec2(vol_icon_w, row_h));
            if (r.clicked) toggle_mute();
            float cx = (r.min.x + r.max.x) * 0.5f;
            float cy = (r.min.y + r.max.y) * 0.5f;
            const ImU32 icon_col = ui::theme::icon_color_u32();
            ImVec2 body_min(cx - ui_s(9.0f), cy - ui_s(4.0f));
            ImVec2 body_max(cx - ui_s(5.0f), cy + ui_s(4.0f));
            r.dl->AddRectFilled(body_min, body_max, icon_col);
            ImVec2 flare[4] = {
                ImVec2(body_max.x, cy - ui_s(2.0f)), ImVec2(body_max.x, cy + ui_s(2.0f)),
                ImVec2(body_max.x + ui_s(6.0f), cy + ui_s(7.0f)), ImVec2(body_max.x + ui_s(6.0f), cy - ui_s(7.0f)),
            };
            r.dl->AddConvexPolyFilled(flare, 4, icon_col);
            if (is_muted()) {
                r.dl->AddLine(ImVec2(cx - ui_s(10.0f), cy - ui_s(9.0f)), ImVec2(cx + ui_s(10.0f), cy + ui_s(9.0f)),
                    IM_COL32(230, 80, 80, 255), ui_s(1.5f));
            } else {
                for (int i = 0; i < 2; ++i) {
                    float radius = ui_s(4.0f) + i * ui_s(4.0f);
                    r.dl->PathArcTo(ImVec2(body_max.x + ui_s(6.0f), cy), radius, -0.6f, 0.6f, 8);
                    r.dl->PathStroke(icon_col, 0, ui_s(1.5f));
                }
            }
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
            float filled_x = vx0 + (vx1 - vx0) * std::clamp(vol, 0.0f, 1.0f);
            ImDrawList* vdl = ImGui::GetWindowDrawList();
            vdl->AddRectFilled(ImVec2(vx0, vbar_y - track_half), ImVec2(vx1, vbar_y + track_half),
                ui::theme::media_track_u32());
            vdl->AddRectFilled(ImVec2(vx0, vbar_y - track_half), ImVec2(filled_x, vbar_y + track_half),
                ui::theme::media_fill_u32());
            vdl->AddCircleFilled(ImVec2(filled_x, vbar_y), ui_s(5.0f), ui::theme::icon_color_u32());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(); // WindowPadding
}

void Player::build_settings_panel(float top_bar_h){
    ImGuiIO& io = ImGui::GetIO();
    // Wider than the old 200px: rows are now [number box][slider track] compounds.
    const float panel_w = ui_s(260.0f);
    ImGui::SetNextWindowPos(ImVec2(0, top_bar_h));
    // Auto-height to content (AlwaysAutoResize) so the panel does not cover the bottom bar.
    // Cap max height so a tiny window still scrolls rather than overflowing the client.
    // bottom bar height == title bar height (kTopBarHBase).
    const float max_h = std::max(0.0f, io.DisplaySize.y - top_bar_h - ui_s(kTopBarHBase));
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

    // Title: plain standard-font text (no divider rules anywhere in the new design).
    ImGui::TextUnformatted(ui_str_.settings_title.c_str());

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

    // Phase 6 M-D: instant native toggle -- each frame the local bool is re-seeded from the
    // atomic present_loop() reads, and a change is stored straight back; see ai_enabled_'s
    // member comment.
    // bool local_ai = ai_enabled_.load(std::memory_order_relaxed);
    // if (ImGui::Checkbox(u8"去码", &local_ai))
    //     ai_enabled_.store(local_ai, std::memory_order_relaxed);

    // Net BasicVSR restore throughput from Python Scheduler (restore_clip wall time only;
    // excludes frontier-gate sleeps). Pushed each tick via set_ui_config; <0 means no
    // restore has finished yet (or no scheduler).
    ImGui::Dummy(ImVec2(0.0f, ui_s(ui::theme::kSpaceL)));
    ImGui::PushFont(nullptr, kFontSizeSm);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
    if (ui_cfg_ai_restore_fps_ >= 0.0f)
        ImGui::Text(ui_str_.ai_speed.c_str(), ui_cfg_ai_restore_fps_);
    else
        ImGui::TextUnformatted(ui_str_.ai_speed_unknown.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar(2); // WindowRounding + WindowPadding
    ImGui::PopStyleColor(); // WindowBg
}

