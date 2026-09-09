// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "imgui_internal.h" // ImGui::GetInputTextState -- URL field right-click menu

// ---- product UI (M3) -- built on the main thread inside ui_tick(), between NewFrame()
// and Render(). Only RECORDS intents (record_toggle_play()/record_seek()/ui_intents_.* on
// settings release / combo change) -- never calls play()/pause()/seek()/anything that
// touches transport or scheduler config directly. Python's main thread is the sole
// executor (see take_ui_intents() and scripts/run_player.py).
void Player::build_ui(){
    // Offline-export mode keeps the normal title bar (build_top_bar) and replaces only the
    // client area below it with the export screen. The bar's settings button is disabled in
    // export mode, and open/URL/web + the export toggle are disabled while an export is
    // running (see build_top_bar) -- that gating replaces the old "返回" exit button.
    if (export_mode_) {
        float top_bar_h = 0.0f;
        build_top_bar(top_bar_h);
        build_export_screen(top_bar_h);
        build_export_preset_editor(); // modal above the screen (when open)
        build_status_float();
        return;
    }
    // M-C1 / warmup-UX: no session open yet (or one was just closed) -- the normal
    // bottom bar / seekbar read session-scoped state (frame_count_, seekbar position, ...)
    // that isn't meaningful yet. Two sub-cases: never opened a file at all (full title bar
    // + interactive "drop/open a file" prompt so the user isn't staring at a dead window;
    // settings sidebar is available here too) vs. a reopen()-driven brief teardown/rebuild
    // of an already-opened file (plain "loading" splash -- opened_ stays true across reopen()).
    bool active = session_active_.load(std::memory_order_relaxed);
    if (!active) {
        if (!opened_) {
            float top_bar_h = 0.0f;
            build_top_bar(top_bar_h);
            // Open prompt first so the settings panel (when open) stacks above it and
            // receives clicks -- same chrome layering as the live-session branch below.
            build_open_prompt_overlay(top_bar_h);
            if (ui_settings_open_) build_settings_panel(top_bar_h);
        } else {
            build_splash_overlay();
        }
    } else {
        float top_bar_h = 0.0f;
        build_top_bar(top_bar_h);
        if (ui_settings_open_) build_settings_panel(top_bar_h);
        build_bottom_bar();
    }
    build_open_url_popup(); // modal above chrome; first-screen + live session both use it
    build_stream_popup(); // web-stream server modal (port + root + start/stop)
    build_status_float(); // model-warmup status line -- drawn in every branch above
}

// M-C1: centered "loading" text overlay, shown while !session_active_ (see build_ui()'s
// branch above and present_loop()'s draw_splash() call). Built the same way as every other
// window here -- main thread, inside ui_tick()'s NewFrame()/Render() bracket -- and never
// touches transport/session state, only ImGui + io.DisplaySize.
void Player::build_splash_overlay(){
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##sumu_splash", nullptr, flags);
    const char* text = ui_str_.splash_loading.c_str();
    ImVec2 tsize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - tsize.x) * 0.5f, (io.DisplaySize.y - tsize.y) * 0.5f));
    ImGui::TextUnformatted(text);
    ImGui::End();
}

// Warmup-UX: shown while !session_active_ AND !opened_, i.e. before the user has ever
// opened a file (build_ui()'s branch above). Title bar + optional settings panel are drawn
// by the caller (same chrome as a live session); this overlay only owns the center prompt.
// WM_DROPFILES already records an open_path intent unconditionally in WndProc regardless of
// what's on screen, so this overlay only needs to give the user a visible target plus an
// explicit "open file" button. top_bar_h is the strip already occupied by build_top_bar() so
// the prompt stays vertically centered in the remaining client area.
void Player::build_open_prompt_overlay(float top_bar_h){
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
        // (deliberately NOT ImGuiWindowFlags_NoInputs -- this overlay must accept clicks;
        //  and NOT NoBackground -- the first screen fills with the standard card color)
    ImGui::SetNextWindowPos(ImVec2(0, top_bar_h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, std::max(0.0f, io.DisplaySize.y - top_bar_h)));
    // Standard card color (kPanelBg) as the first-screen background, one step above the
    // window background the top bar still shows.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
    ImGui::Begin("##sumu_open_prompt", nullptr, flags);

    const float logo_draw = ui_s(120.0f);
    const float logo_gap = ui_s(ui::theme::kSpaceXL);
    bool have_logo = logo_srv_ != nullptr;

    const char* prompt = ui_str_.open_prompt.c_str();
    ImVec2 tsize = ImGui::CalcTextSize(prompt);
    const float open_btn_w = ui_s(120.0f), open_btn_h = ui_s(ui::theme::kControlHeight);
    const float open_btn_gap = ui_s(ui::theme::kSpaceM);
    const float gap = ui_s(ui::theme::kSpaceXL);
    // Single row of four entries: local file (Primary accent) + network URL / web-stream /
    // offline export (Secondary neutral).
    const float open_row_w = open_btn_w * 4.0f + open_btn_gap * 3.0f;
    const float open_grid_h = open_btn_h;

    // Optional TRT-compile region below the open button (set_compile_ui state != 0). Measure
    // it first so the whole prompt+button+compile block stays vertically centered.
    const float compile_cw = ui_s(380.0f);      // content column width for the compile region
    const float region_gap = ui_s(2.0f * ui::theme::kSpaceXL); // gap between open button and the compile region
    const float ctrl_gap = ui_s(ui::theme::kSpaceL);           // gap between the compile text line and its control
    const float compile_btn_w = ui_s(200.0f), compile_btn_h = ui_s(ui::theme::kControlHeight),
        compile_bar_h = ui_s(18.0f);
    bool show_compile = compile_ui_state_ != 0;
    float compile_text_h = 0.0f, compile_block_h = 0.0f;
    if (show_compile) {
        compile_text_h = ImGui::CalcTextSize(compile_ui_text_.c_str(), nullptr, false, compile_cw).y;
        float ctrl_h = (compile_ui_state_ == 2) ? compile_bar_h : compile_btn_h;
        compile_block_h = region_gap + compile_text_h + ctrl_gap + ctrl_h;
    }

    float content_h = std::max(0.0f, io.DisplaySize.y - top_bar_h);
    float logo_block = have_logo ? (logo_draw + logo_gap) : 0.0f;
    float block_h = logo_block + tsize.y + gap + open_grid_h + compile_block_h;
    float top = (content_h - block_h) * 0.5f;
    if (top < 0.0f) top = 0.0f;

    float y = top;
    if (have_logo) {
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - logo_draw) * 0.5f, y));
        ImGui::Image((ImTextureID)(intptr_t)logo_srv_.Get(),
            ImVec2(logo_draw, logo_draw));

        y += logo_draw + logo_gap;
    }

    ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - tsize.x) * 0.5f, y));
    ImGui::TextUnformatted(prompt);

    const float open_row_x = (io.DisplaySize.x - open_row_w) * 0.5f;
    const float open_row_y = y + tsize.y + gap;
    // "open file" is the primary (accent) action; URL / web-stream / export are secondary.
    // All four carry their top-bar counterpart's glyph (icon-text button standard).
    ImGui::SetCursorPos(ImVec2(open_row_x, open_row_y));
    if (ui::Button(ui_str_.open_file.c_str(), ui::AppIcon::OpenFile, ui::ButtonVariant::Primary,
            ui::ControlSize::Regular, open_btn_w)) record_open_dialog();
    ImGui::SameLine(0.0f, open_btn_gap);
    if (ui::Button(ui_str_.open_url.c_str(), ui::AppIcon::OpenUrl, ui::ButtonVariant::Secondary,
            ui::ControlSize::Regular, open_btn_w)) request_open_url_popup();
    ImGui::SameLine(0.0f, open_btn_gap);
    if (ui::Button(ui_str_.stream_server.c_str(), ui::AppIcon::WebServer, ui::ButtonVariant::Secondary,
            ui::ControlSize::Regular, open_btn_w)) request_stream_popup();
    ImGui::SameLine(0.0f, open_btn_gap);
    if (ui::Button(ui_str_.export_video.c_str(), ui::AppIcon::Export, ui::ButtonVariant::Secondary,
            ui::ControlSize::Regular, open_btn_w)) ui_intents_.export_enter = true;

    if (show_compile) {
        float region_x = (io.DisplaySize.x - compile_cw) * 0.5f;
        float cy = y + tsize.y + gap + open_grid_h + region_gap;


        // Frame the whole compile block (text + button/progress-bar) as one visual unit,
        // same translucent filled-bg language as the seekbar's hover thumbnail card above,
        // plus the settings float's 1px border (kBorderStrong) for separation on the
        // now-card-colored background.
        // DrawList is screen-space; region_x/cy are window-local (SetCursorPos), so add
        // WindowPos -- without it the box sits top_bar_h above the text/button.
        const float box_pad = ui_s(ui::theme::kPaddingContainer);
        float ctrl_h = (compile_ui_state_ == 2) ? compile_bar_h : compile_btn_h;
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 box_min(wpos.x + region_x - box_pad, wpos.y + cy - box_pad);
        ImVec2 box_max(wpos.x + region_x + compile_cw + box_pad,
            wpos.y + cy + compile_text_h + ctrl_gap + ctrl_h + box_pad);
        ImDrawList* box_dl = ImGui::GetWindowDrawList();
        ImVec4 box_bg = ui::theme::kPanelBg;
        box_bg.w = ui::theme::kOverlayBgAlpha; // translucent card, same alpha as the overlay floats
        box_dl->AddRectFilled(box_min, box_max, ui::theme::to_u32(box_bg),
            ui_s(ui::theme::kRadiusWindow));
        box_dl->AddRect(ImVec2(box_min.x + 0.5f, box_min.y + 0.5f),
            ImVec2(box_max.x - 0.5f, box_max.y - 0.5f),
            ui::theme::border_strong_u32(), ui_s(ui::theme::kRadiusWindow), 0, 1.0f);

        // Center each wrapped line (ImGui::TextUnformatted is left-aligned in the wrap column).
        bool failed = compile_ui_state_ == 3;
        if (failed) ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
        ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
        ImFont* font = ImGui::GetFont();
        const float font_sz = ImGui::GetFontSize();
        const float line_h = ImGui::GetTextLineHeight();
        const char* s = compile_ui_text_.c_str();
        const char* end = s + compile_ui_text_.size();
        float draw_y = wpos.y + cy;
        while (s < end) {
            const char* para_end = s;
            while (para_end < end && *para_end != '\n') ++para_end;
            const char* line = s;
            while (line < para_end) {
                const char* wrap = font->CalcWordWrapPosition(font_sz, line, para_end, compile_cw);
                if (wrap <= line && para_end > line) {
                    // Force one UTF-8 codepoint so we always advance.
                    const unsigned char c = static_cast<unsigned char>(*line);
                    int nbytes = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                        : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
                    wrap = line + nbytes;
                    if (wrap > para_end) wrap = para_end;
                }
                ImVec2 lsz = font->CalcTextSizeA(font_sz, FLT_MAX, 0.0f, line, wrap);
                float lx = wpos.x + region_x + (compile_cw - lsz.x) * 0.5f;
                box_dl->AddText(font, font_sz, ImVec2(lx, draw_y), text_col, line, wrap);
                draw_y += line_h;
                if (wrap >= para_end) break;
                line = wrap;
                while (line < para_end && (*line == ' ' || *line == '\t')) ++line;
            }
            s = (para_end < end && *para_end == '\n') ? para_end + 1 : para_end;
        }
        if (failed) ImGui::PopStyleColor();
        // Reserve layout space so the button/progress bar below still lands at cy2.
        ImGui::SetCursorPos(ImVec2(region_x, cy));
        ImGui::Dummy(ImVec2(compile_cw, compile_text_h));

        float cy2 = cy + compile_text_h + ctrl_gap;
        if (compile_ui_state_ == 2) {
            // running: progress bar (Python drives compile_ui_progress_ = step/6)
            ImGui::SetCursorPos(ImVec2(region_x, cy2));
            ui::ProgressBar(compile_ui_progress_, compile_cw, compile_bar_h);
        } else {
            // idle (1) or failed (3): a click records the compile/retry intent
            const char* label = failed ? ui_str_.compile_retry.c_str()
                                       : ui_str_.compile_engine.c_str();
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - compile_btn_w) * 0.5f, cy2));
            if (ui::Button(label, ui::ButtonVariant::Primary, ui::ControlSize::Regular,
                    compile_btn_w)) record_compile_engine();
        }
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg (kPanelBg first-screen background)
}

// Open-URL modal: first-screen "打开 URL" button and top-bar link icon both call
// request_open_url_popup(). Confirm trims the buffer, requires http(s)://, then writes
// ui_intents_.open_path so Python reuses the existing drop/open path (do_open/do_reopen).
// Popup stays open in a loading state until Python calls notify_open_url_finished -- keeps
// the float responsive during network open (no main-thread freeze). Loading-state Cancel
// interrupts the blocked FFmpeg open (cancel_open_url → Decoder AVIOInterruptCB); the worker
// then unwinds through the normal failure path and Python reaps it as a SILENT cancellation
// (open_url_cancel_requested()) -- back to the form with the URL kept, no load-error line.
// (No loading-state Esc: window-level WM_KEYDOWN VK_ESCAPE is a deliberate global quit/park
// escape hatch that must never be swallowed by a widget, see player_window.cpp.)
// Popup id uses ### so the i18n title can change without breaking OpenPopup matching.
// Form chrome is the shared ui::BeginModal/EndModal (title strip + close X + border);
// the loading state keeps its own compact chromeless card (deliberately no title strip).
void Player::build_open_url_popup(){
    if (open_url_popup_) {
        ImGui::OpenPopup("###sumu_open_url");
        open_url_popup_ = false; // OpenPopup is sticky until EndPopup; re-arm only on next request
    }

    // ---- loading: compact card -- centered spinner + Cancel (aborts the network open) ----
    if (open_url_loading_) {
        ImGuiIO& io = ImGui::GetIO();
        // Fixed width + auto height (Cancel button below the spinner grew the card beyond
        // the old 112px square); force center every frame or the card stays stuck at the
        // form's old top-left.
        const float card_w = ui_s(200.0f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(card_w, 0.0f), ImVec2(card_w, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_s(ui::theme::kRadiusWindow));
        // Horizontal padding stays 0 so the manual centering math below is in window coords.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, ui_s(28.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f); // border self-drawn below
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::theme::kPanelBg);
        // ModalWindowDimBg needs no push: apply_theme() already installed theme::kDimBg.
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_AlwaysAutoResize;
        // Same "###sumu_open_url" ID as the form's BeginModal title below (same window).
        if (ImGui::BeginPopupModal("###sumu_open_url", nullptr, flags)) {
            // Center the spinner; its layout item is 2r+th on a side (see ui::Spinner).
            const float r = ImGui::GetFrameHeight() * 0.65f;
            const float d = r * 2.0f + std::max(1.5f, r / 6.0f);
            ImGui::SetCursorPosX((card_w - d) * 0.5f);
            ui::Spinner("##open_url_spinner");
            ImGui::Spacing();
            ImGui::Spacing();

            // Cancel: interrupt the blocked FFmpeg open; the card stays up (button greyed)
            // until the worker unwinds and Python calls notify_open_url_finished().
            const bool cancelling = open_cancel_requested_.load(std::memory_order_relaxed);
            const float btn_w = ui_s(88.0f);
            ImGui::SetCursorPosX((card_w - btn_w) * 0.5f);
            ImGui::BeginDisabled(cancelling);
            if (ui::Button(
                    ui_str_.open_url_cancel.empty() ? "Cancel" : ui_str_.open_url_cancel.c_str(),
                    ui::ButtonVariant::Secondary, ui::ControlSize::Regular, btn_w)) {
                cancel_open_url();
            }
            ImGui::EndDisabled();

            // Same self-drawn 1px border language as ui::EndModal (foreground list).
            const ImVec2 a = ImGui::GetWindowPos();
            const ImVec2 b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
            ImGui::GetForegroundDrawList()->AddRect(
                ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f),
                ui::theme::border_strong_u32(), ui_s(ui::theme::kRadiusWindow), 0, 1.0f);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(3);
        return;
    }

    // ---- input form (title strip + field + buttons) via the shared modal chrome ----
    // ID after ### matches OpenPopup("###sumu_open_url"); BeginModal strips it for display.
    bool open = true;
    const std::string modal_title =
        (ui_str_.open_url_title.empty() ? std::string("Open URL") : ui_str_.open_url_title)
        + "###sumu_open_url";
    if (!ui::BeginModal(modal_title.c_str(), &open)) return;
    if (!open) { // closed via the strip's X or Esc
        open_url_show_error_ = false;
        open_url_show_load_error_ = false;
    }

    // Success path: close after the modal is open this frame (CloseCurrentPopup is frame-local).
    if (open_url_close_pending_) {
        open_url_close_pending_ = false;
        ImGui::CloseCurrentPopup();
    }

    // BeginModal pins the window width and insets the body by kPaddingContainer on all
    // four sides, and presets the item width to the full content column.
    const float pad = ui_s(ui::theme::kPaddingContainer);
    const float content_w = ui_s(ui::theme::kModalContentW);

    auto trim_url = [](const char* raw) -> std::string {
        std::string s(raw ? raw : "");
        // Strip leading/trailing whitespace and common surrounding quotes from paste.
        auto is_ws = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        size_t a = 0, b = s.size();
        while (a < b && is_ws(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && is_ws(static_cast<unsigned char>(s[b - 1]))) --b;
        s = s.substr(a, b - a);
        if (s.size() >= 2) {
            char q0 = s.front(), q1 = s.back();
            if ((q0 == '"' && q1 == '"') || (q0 == '\'' && q1 == '\''))
                s = s.substr(1, s.size() - 2);
        }
        return s;
    };
    auto looks_http = [](const std::string& s) {
        if (s.size() < 8) return false;
        // Case-insensitive scheme check without locale-dependent tolower on the whole string.
        auto eq_ci = [](char a, char b) {
            auto up = [](char c) {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return up(a) == up(b);
        };
        const char* http = "http://";
        const char* https = "https://";
        bool is_http = true, is_https = true;
        for (int i = 0; http[i]; ++i)
            if (i >= static_cast<int>(s.size()) || !eq_ci(s[static_cast<size_t>(i)], http[i]))
                is_http = false;
        for (int i = 0; https[i]; ++i)
            if (i >= static_cast<int>(s.size()) || !eq_ci(s[static_cast<size_t>(i)], https[i]))
                is_https = false;
        return is_http || is_https;
    };

    if (!ui_str_.open_url_hint.empty()) {
        ImGui::PushFont(nullptr, kFontSizeSm); // secondary copy (hint)
        ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + content_w);
        ImGui::TextUnformatted(ui_str_.open_url_hint.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Spacing();
    }

    if (open_url_focus_ || open_url_select_all_pending_) {
        ImGui::SetKeyboardFocusHere();
        open_url_focus_ = false;
    }
    bool enter = ui::TextInput("##open_url_input", open_url_buf_, sizeof(open_url_buf_),
        nullptr, 0.0f, ImGuiInputTextFlags_EnterReturnsTrue);
    const ImGuiID url_input_id = ImGui::GetItemID();
    if (open_url_select_all_pending_) {
        // Deferred "Select All" from the context menu on an inactive field: the keyboard
        // focus above just activated it this frame, so the state exists now.
        open_url_select_all_pending_ = false;
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(url_input_id))
            st->SelectAll();
    }

    // Right-click context menu: Cut/Copy/Paste/Paste&Open/Select All, drawn in the ui::Combo
    // dropdown language (rounded kPanelBg panel, 4px padding, flat rows with a hover fill,
    // self-drawn hairline border) -- the stock MenuItem popup looks off-theme. Only
    // currently-actionable items are listed (unavailable ones are hidden, not greyed), so
    // the menu simply does not open when nothing applies. ImGui's InputText has Ctrl+X/C/V
    // but no built-in menu; ImGuiInputTextState (imgui_internal.h) exposes the cursor and
    // selection for precise edits -- selection indices are byte offsets into the buffer.
    // Right-click does NOT activate the field (ImGui activates on left click only), so a
    // NULL state falls back to whole-buffer operations.
    const char* cut_label = ui_str_.input_cut.empty() ? "Cut" : ui_str_.input_cut.c_str();
    const char* copy_label = ui_str_.input_copy.empty() ? "Copy" : ui_str_.input_copy.c_str();
    const char* paste_label = ui_str_.input_paste.empty() ? "Paste" : ui_str_.input_paste.c_str();
    const char* paste_open_label =
        ui_str_.input_paste_open.empty() ? "Paste & Open" : ui_str_.input_paste_open.c_str();
    const char* sel_all_label =
        ui_str_.input_select_all.empty() ? "Select All" : ui_str_.input_select_all.c_str();
    ImGuiInputTextState* url_st = ImGui::GetInputTextState(url_input_id);
    const bool url_has_sel = url_st && url_st->HasSelection();
    const size_t url_buf_len = std::strlen(open_url_buf_);
    const char* url_clip = ImGui::GetClipboardText();
    const bool url_clip_avail = url_clip && url_clip[0];

    struct CtxItem { const char* label; int action; };
    CtxItem ctx_items[5];
    int ctx_item_count = 0;
    if (url_has_sel) ctx_items[ctx_item_count++] = { cut_label, 0 };
    if (url_has_sel || url_buf_len > 0) ctx_items[ctx_item_count++] = { copy_label, 1 };
    if (url_clip_avail) {
        ctx_items[ctx_item_count++] = { paste_label, 2 };
        ctx_items[ctx_item_count++] = { paste_open_label, 4 };
    }
    if (url_buf_len > 0) ctx_items[ctx_item_count++] = { sel_all_label, 3 };

    // Shared by 粘贴 (replace selection / append when inactive) and 粘贴并打开 (whole field).
    auto paste_clipboard = [&](bool replace_all) {
        // Single-line field: strip CR/LF from the clipboard text before inserting.
        std::string ins(url_clip);
        ins.erase(std::remove_if(ins.begin(), ins.end(),
                      [](char c) { return c == '\r' || c == '\n'; }),
            ins.end());
        std::string s(open_url_buf_);
        size_t cursor;
        if (replace_all) {
            s = ins;
            cursor = s.size();
        } else if (url_st) {
            const int a = url_st->GetSelectionStart(), b = url_st->GetSelectionEnd();
            s.replace(static_cast<size_t>(a), static_cast<size_t>(b - a), ins);
            cursor = static_cast<size_t>(a) + ins.size();
        } else {
            s += ins; // inactive field: append (InputText re-reads buf every frame)
            cursor = s.size();
        }
        if (s.size() >= sizeof(open_url_buf_)) {
            s.resize(sizeof(open_url_buf_) - 1);
            cursor = std::min(cursor, s.size());
        }
        std::memcpy(open_url_buf_, s.c_str(), s.size() + 1);
        if (url_st) {
            url_st->WantReloadUserBuf = true;
            url_st->ReloadSelectionStart = url_st->ReloadSelectionEnd = static_cast<int>(cursor);
        }
    };

    const float ctx_pad_x = ui_s(10.0f);
    float ctx_label_w = 0.0f;
    for (int i = 0; i < ctx_item_count; ++i)
        ctx_label_w = std::max(ctx_label_w, ImGui::CalcTextSize(ctx_items[i].label).x);
    const float ctx_menu_w = ctx_label_w + ctx_pad_x * 2.0f + ui_s(4.0f) * 2.0f; // + WindowPadding
    ImGui::SetNextWindowSizeConstraints(ImVec2(ctx_menu_w, 0.0f), ImVec2(ctx_menu_w, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(4.0f), ui_s(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ui_s(ui::theme::kRadiusWindow));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f); // border self-drawn below
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::theme::kPanelBg);
    int ctx_action = -1; // 0=cut 1=copy 2=paste 3=select-all 4=paste&open; applied below
    if (ctx_item_count > 0 && ImGui::BeginPopupContextItem("##open_url_ctx")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const float row_h = ImGui::GetFrameHeight();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float th = ImGui::GetTextLineHeight();
        for (int i = 0; i < ctx_item_count; ++i) {
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(row_w, row_h));
            const bool rhover = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                ctx_action = ctx_items[i].action;
                ImGui::CloseCurrentPopup();
            }
            const ImVec2 rmin = ImGui::GetItemRectMin();
            if (rhover)
                pdl->AddRectFilled(rmin, ImGui::GetItemRectMax(),
                    ui::theme::hover_fill_u32(), ui_s(ui::theme::kRadiusControl));
            pdl->AddText(ImVec2(rmin.x + ctx_pad_x, rmin.y + (row_h - th) * 0.5f),
                ui::theme::text_u32(), ctx_items[i].label);
            ImGui::PopID();
        }
        // Hairline outline, same language as the combo popup / modal border.
        const ImVec2 pmin = ImGui::GetWindowPos();
        const ImVec2 pmax(pmin.x + ImGui::GetWindowSize().x, pmin.y + ImGui::GetWindowSize().y);
        ImGui::GetForegroundDrawList()->AddRect(
            ImVec2(pmin.x + 0.5f, pmin.y + 0.5f), ImVec2(pmax.x - 0.5f, pmax.y - 0.5f),
            ui::theme::border_u32(), ui_s(ui::theme::kRadiusWindow), 0, 1.0f);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (ctx_action == 0) { // cut (only offered with a selection, so url_st is valid)
        const int a = url_st->GetSelectionStart(), b = url_st->GetSelectionEnd();
        std::string s(open_url_buf_);
        ImGui::SetClipboardText(s.substr(a, b - a).c_str());
        s.erase(static_cast<size_t>(a), static_cast<size_t>(b - a));
        std::memcpy(open_url_buf_, s.c_str(), s.size() + 1);
        url_st->WantReloadUserBuf = true;
        url_st->ReloadSelectionStart = url_st->ReloadSelectionEnd = a;
    } else if (ctx_action == 1) { // copy
        if (url_has_sel) {
            const int a = url_st->GetSelectionStart(), b = url_st->GetSelectionEnd();
            ImGui::SetClipboardText(std::string(open_url_buf_).substr(a, b - a).c_str());
        } else {
            ImGui::SetClipboardText(open_url_buf_);
        }
    } else if (ctx_action == 2) { // paste
        paste_clipboard(false);
    } else if (ctx_action == 3) { // select all
        // Always defer: the context menu takes focus, and a selection set on a field
        // that is about to deactivate would vanish with its InputTextState. Re-focus
        // and select on the next frame (same two-frame pattern as open_url_focus_).
        open_url_select_all_pending_ = true;
    } else if (ctx_action == 4) { // paste & open: replace the field, then act as Enter
        paste_clipboard(true);
        enter = true; // falls into the do_ok trim/validate/submit path below
    }
    if (open_url_show_error_ && !ui_str_.open_url_invalid.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
        ImGui::TextUnformatted(ui_str_.open_url_invalid.c_str());
        ImGui::PopStyleColor();
    } else if (open_url_show_load_error_) {
        const char* err = !ui_str_.open_url_load_failed.empty()
            ? ui_str_.open_url_load_failed.c_str()
            : "Could not open this URL";
        ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
        ImGui::TextUnformatted(err);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const float btn_w = ui_s(100.0f);
    const float row_w = btn_w * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    // Absolute X: left pad + center within content column.
    ImGui::SetCursorPosX(pad + std::max(0.0f, (content_w - row_w) * 0.5f));

    bool do_ok = enter;
    if (ui::Button(ui_str_.open_url_ok.empty() ? "Open" : ui_str_.open_url_ok.c_str(),
            ui::ButtonVariant::Primary, ui::ControlSize::Regular, btn_w)) {
        do_ok = true;
    }
    ImGui::SameLine();
    if (ui::Button(ui_str_.open_url_cancel.empty() ? "Cancel" : ui_str_.open_url_cancel.c_str(),
            ui::ButtonVariant::Secondary, ui::ControlSize::Regular, btn_w)) {
        open_url_show_error_ = false;
        open_url_show_load_error_ = false;
        ImGui::CloseCurrentPopup();
    }

    if (do_ok) {
        std::string url = trim_url(open_url_buf_);
        if (!looks_http(url)) {
            open_url_show_error_ = true;
            open_url_show_load_error_ = false;
        } else {
            // Keep popup open → spinner loading card; Python opens async and calls
            // notify_open_url_finished when done.
            open_url_show_error_ = false;
            open_url_show_load_error_ = false;
            open_url_loading_ = true;
            if (url.size() >= sizeof(open_url_buf_))
                url.resize(sizeof(open_url_buf_) - 1);
            std::memcpy(open_url_buf_, url.c_str(), url.size() + 1);
            record_open_path(url);
        }
    }
    // Esc is handled by ui::BeginModal (it closes the popup and clears `open`; the flags
    // are reset at the top of this body when that happens).

    ui::EndModal();
}

// Web-stream server modal (Phase 2): port + video-root folder + 启动/停止. Chrome is the
// shared ui::BeginModal/EndModal (same as the open-URL modal's form: title strip + close X,
// 8px rounding + 1px border, centered footer buttons). While running it shows the access URL
// as a clickable link (TextLinkOpenURL → default browser) instead of the form -- no
// persistent status float.
void Player::build_stream_popup(){
    if (stream_popup_) { ImGui::OpenPopup("###sumu_stream"); stream_popup_ = false; }
    bool open = true;
    // ID after ### matches OpenPopup("###sumu_stream"); BeginModal strips it for display and
    // re-centers every frame (AlwaysAutoResize takes a frame to settle the content size).
    const std::string modal_title =
        (ui_str_.stream_title.empty() ? std::string("Web stream") : ui_str_.stream_title)
        + "###sumu_stream";
    if (!ui::BeginModal(modal_title.c_str(), &open)) return;

    // BeginModal pins the window width and insets the body by kPaddingContainer on all
    // four sides, and presets the item width to the full content column.
    const float pad = ui_s(ui::theme::kPaddingContainer);
    const float content_w = ui_s(ui::theme::kModalContentW);

    const float label_w = ui_s(88.0f);
    // InlineLabel leaves the cursor past the label column + ItemInnerSpacing.
    const float field_w = content_w - label_w - ImGui::GetStyle().ItemInnerSpacing.x;

    if (stream_running_) {
        // Running: the access URL lives here (no persistent status float), clickable → browser.
        if (!ui_str_.stream_url_label.empty())
            ImGui::TextUnformatted(ui_str_.stream_url_label.c_str());
        if (!stream_url_.empty())
            ImGui::TextLinkOpenURL(stream_url_.c_str(), stream_url_.c_str());
    } else {
        // Form rows: InlineLabel (standard font, fixed column) + control filling the rest,
        // so every row's right edge lands at the same content-column edge.
        ui::InlineLabel(ui_str_.stream_port_label.empty() ? "Port" : ui_str_.stream_port_label.c_str(), label_w);
        ui::IntInput("##stream_port", &stream_port_edit_, field_w);
        if (stream_port_edit_ < 1) stream_port_edit_ = 1;
        if (stream_port_edit_ > 65535) stream_port_edit_ = 65535;

        ui::InlineLabel(ui_str_.stream_root_label.empty() ? "Root" : ui_str_.stream_root_label.c_str(), label_w);
        // Folder pick: framed icon button (lucide folder), same style as the export
        // page's path picker; the row stays label + input + square button.
        const float pick_sz = ImGui::GetFrameHeight();
        ui::TextInput("##stream_root", stream_root_buf_, sizeof(stream_root_buf_), nullptr,
            field_w - pick_sz - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SameLine();
        ui::IconButtonResult pick_r = ui::IconButtonFramed("##stream_pick",
            ImVec2(pick_sz, pick_sz), ui::AppIcon::Folder);
        if (!ui_str_.stream_pick.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ui_str_.stream_pick.c_str());
        if (pick_r.clicked) {
            std::string dir = pick_folder();
            if (!dir.empty()) {
                size_t n = std::min(dir.size(), sizeof(stream_root_buf_) - 1);
                std::memcpy(stream_root_buf_, dir.c_str(), n);
                stream_root_buf_[n] = '\0';
            }
        }

        // Token row: a "无 token" checkbox (unchecked = use a token for auth). While checked
        // the token input is hidden; while unchecked, an empty input means "generate a random
        // token each start".
        ui::InlineLabel(ui_str_.stream_no_token.empty() ? "No token" : ui_str_.stream_no_token.c_str(), label_w);
        ui::Checkbox("##stream_no_token", &stream_no_token_edit_);
        if (!stream_no_token_edit_) {
            ui::InlineLabel(ui_str_.stream_token_label.empty() ? "Token" : ui_str_.stream_token_label.c_str(), label_w);
            ui::TextInput("##stream_token", stream_token_buf_, sizeof(stream_token_buf_),
                ui_str_.stream_token_hint.empty() ? "" : ui_str_.stream_token_hint.c_str(),
                field_w);
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();
    const float btn_w = ui_s(100.0f);
    const float row_w = btn_w * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(pad + std::max(0.0f, (content_w - row_w) * 0.5f));

    if (stream_running_) {
        if (ui::Button(ui_str_.stream_stop.empty() ? "Stop" : ui_str_.stream_stop.c_str(),
                ui::ButtonVariant::Primary, ui::ControlSize::Regular, btn_w)) {
            ui_intents_.stream_stop = true;
            ImGui::CloseCurrentPopup();
        }
    } else {
        if (ui::Button(ui_str_.stream_start.empty() ? "Start" : ui_str_.stream_start.c_str(),
                ui::ButtonVariant::Primary, ui::ControlSize::Regular, btn_w)) {
            ui_intents_.stream_start = true;
            ui_intents_.stream_port = stream_port_edit_;
            ui_intents_.stream_root = stream_root_buf_;
            ui_intents_.stream_no_token = stream_no_token_edit_;
            ui_intents_.stream_token = stream_token_buf_;
            // Keep the popup open: the next tick flips stream_running_ and the body swaps to
            // the running view (URL) in place.
        }
    }
    ImGui::SameLine();
    if (ui::Button(ui_str_.cancel.empty() ? "Cancel" : ui_str_.cancel.c_str(),
            ui::ButtonVariant::Secondary, ui::ControlSize::Regular, btn_w))
        ImGui::CloseCurrentPopup();

    ui::EndModal();
}

// Model/engine status float (top-left, below the title bar). Shows only when the model
// state is abnormal: warmup in progress (status_text_), warmup failed (status 1), or TRT
// engines not compiled (status 4). Clicking the float opens the settings panel so the user
// can inspect details or trigger an on-demand compile. Hidden when the settings panel is
// already open (no duplicate info). The NotCompiled float auto-dismisses after 10 seconds
// and is suppressed on the first screen.
// Python drives status_text_ (warmup/open-error) and trt_engine_status_ (0..6 enum).
void Player::build_status_float(){
    if (ui_settings_open_) return;

    const char* text = nullptr;
    ImVec4 color = ui::theme::kTextSecondary; // default gray (warming)
    bool auto_dismiss = false;

    if (trt_engine_status_ == 1) {
        // WarmupFailed: always surface, red, even on the first screen.
        text = ui_str_.warmup_failed.c_str();
        color = ui::theme::kError;
    } else if (trt_engine_status_ == 4) {
        // NotCompiled idle: one-shot nudge on a live session (first screen has its own
        // compile region), auto-dismiss after 10s.
        if (!session_active_.load(std::memory_order_relaxed)) return;
        text = ui_str_.trt_not_compiled_hint.c_str();
        color = ui::theme::kText; // white
        auto_dismiss = true;
    } else {
        // Warming / open error ride status_text_ (Python). All other engine states
        // (Ready / NotApplicable / Compiling / CompileFailed) stay silent here.
        if (status_text_.empty()) return;
        text = status_text_.c_str();
    }
    if (!text || !text[0]) return;

    // Auto-dismiss timer: after 10 seconds the float hides. Reset when the state
    // changes away from auto-dismiss (warmup states use 0 to indicate "no timer").
    if (auto_dismiss) {
        float now = (float)ImGui::GetTime();
        if (status_float_shown_at_ == 0.0f) status_float_shown_at_ = now;
        if (now - status_float_shown_at_ > 10.0f) return;
    } else {
        status_float_shown_at_ = 0.0f;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    // Deliberately NOT NoInputs -- the float is clickable.

    const float margin = ui_s(12.0f);
    ImGui::SetNextWindowPos(ImVec2(margin, top_bar_h() + margin), ImGuiCond_Always,
        ImVec2(0.0f, 0.0f)); // top-left anchor
    ImGui::SetNextWindowBgAlpha(ui::theme::kOverlayBgAlpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_s(ui::theme::kRadiusControl));
    ImGui::Begin("##sumu_status_float", nullptr, flags);

    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();

    // Click anywhere on the float → open settings panel.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ui_settings_open_ = true;
        if (ui_settings_open_) settings_edit_init_ = false;
    }

    ImGui::End();
    ImGui::PopStyleVar(); // WindowRounding
}

