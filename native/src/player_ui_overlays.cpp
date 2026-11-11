// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "browser_view.h" // BrowserView (page-view placement + cookie lookup)
#include "imgui_internal.h" // ImGui::GetInputTextState -- URL field right-click menu

// ---- product UI (M3) -- built on the main thread inside ui_tick(), between NewFrame()
// and Render(). Only RECORDS intents (record_toggle_play()/record_seek()/ui_intents_.* on
// settings release / combo change) -- never calls play()/pause()/seek()/anything that
// touches transport or scheduler config directly. Python's main thread is the sole
// executor (see take_ui_intents() and scripts/run_player.py).
void Player::build_ui(){
    // Offline-export mode keeps the normal title bar (build_top_bar) and replaces only the
    // client area below it with the export screen -- it is an EXCLUSIVE OVERLAY, not a
    // navigation: the URL page (if open) stays alive underneath and build_browser_page() picks
    // it up again the moment export mode is left. The bar's settings button is disabled in
    // export mode, and open/URL/web + the export toggle are disabled while an export is
    // running (see build_top_bar).
    if (export_mode_) {
        float top_bar_h = 0.0f;
        build_top_bar(top_bar_h);
        build_export_screen(top_bar_h);
        build_export_preset_editor(); // modal above the screen (when open)
        build_status_float();
        return;
    }
    // Embedded browser page: the title bar stays, the page owns everything below it (its own
    // address bar + the WebView2 child window / first-screen form). Nothing else may draw
    // into that area -- the page view is a real child window and would cover it.
    if (browser_page_) {
        build_browser_page();
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

// ---- shared single-line URL field ---------------------------------------------------------
// The browser page's address bar and its first-screen form each own a buffer + ImGui id and
// both render through this one helper: the right-click menu (Cut/Copy/Paste/Paste&Open/Select
// All, drawn in the combo dropdown language -- the stock MenuItem popup looks off-theme) is
// written once instead of twice. Two separate buffers (not one shared field) is deliberate:
// a single buffer would give the two fields two ImGuiInputTextStates over the same text, so a
// cursor/selection edit in one would be clobbered by the other's stale state in the same frame.
// Right-click does NOT activate an ImGui field, so a null state falls back to whole-buffer
// operations; selection indices are byte offsets into the buffer.
Player::UrlFieldResult Player::url_field(const char* id, char* buf, size_t cap, float width,
                                         bool* focus_pending, bool* select_all_pending){
    UrlFieldResult out;
    const bool want_select_all = select_all_pending && *select_all_pending;
    // "Select All" from an inactive field's menu: focus now, select next frame (the state only
    // exists once the field is active) -- the same two-frame pattern the focus flag uses.
    if ((focus_pending && *focus_pending) || want_select_all) {
        // ImGui's focus request only moves its own active id; the Win32 focus may still sit in
        // the page view's child window (which would swallow every keystroke). Take it back.
        if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
        ImGui::SetKeyboardFocusHere();
    }
    if (focus_pending) *focus_pending = false;

    bool enter = ui::TextInput(id, buf, cap, nullptr, width, ImGuiInputTextFlags_EnterReturnsTrue);
    const ImGuiID item_id = ImGui::GetItemID();
    out.active = ImGui::GetActiveID() == item_id;
    if (want_select_all) {
        *select_all_pending = false;
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(item_id)) st->SelectAll();
    }

    const char* cut_label = ui_str_.input_cut.empty() ? "Cut" : ui_str_.input_cut.c_str();
    const char* copy_label = ui_str_.input_copy.empty() ? "Copy" : ui_str_.input_copy.c_str();
    const char* paste_label = ui_str_.input_paste.empty() ? "Paste" : ui_str_.input_paste.c_str();
    const char* paste_open_label =
        ui_str_.input_paste_open.empty() ? "Paste & Open" : ui_str_.input_paste_open.c_str();
    const char* sel_all_label =
        ui_str_.input_select_all.empty() ? "Select All" : ui_str_.input_select_all.c_str();
    ImGuiInputTextState* st = ImGui::GetInputTextState(item_id);
    const bool has_sel = st && st->HasSelection();
    const size_t buf_len = std::strlen(buf);
    const char* clip = ImGui::GetClipboardText();
    const bool clip_avail = clip && clip[0];

    // Only currently-actionable items are listed (unavailable ones are hidden, not greyed), so
    // the menu simply does not open when nothing applies.
    struct CtxItem { const char* label; int action; };
    CtxItem items[5];
    int item_count = 0;
    if (has_sel) items[item_count++] = { cut_label, 0 };
    if (has_sel || buf_len > 0) items[item_count++] = { copy_label, 1 };
    if (clip_avail) {
        items[item_count++] = { paste_label, 2 };
        items[item_count++] = { paste_open_label, 4 };
    }
    if (buf_len > 0) items[item_count++] = { sel_all_label, 3 };

    // Replace the selection (or the whole field for 粘贴并打开; append when the field was never
    // activated, since InputText re-reads the buffer every frame in that case).
    auto paste_clipboard = [&](bool replace_all) {
        std::string ins(clip);
        ins.erase(std::remove_if(ins.begin(), ins.end(),
                      [](char c) { return c == '\r' || c == '\n'; }),
            ins.end());
        std::string s(buf);
        size_t cursor;
        if (replace_all) {
            s = ins;
            cursor = s.size();
        } else if (st) {
            const int a = st->GetSelectionStart(), b = st->GetSelectionEnd();
            s.replace(static_cast<size_t>(a), static_cast<size_t>(b - a), ins);
            cursor = static_cast<size_t>(a) + ins.size();
        } else {
            s += ins;
            cursor = s.size();
        }
        if (s.size() >= cap) {
            s.resize(cap - 1);
            cursor = std::min(cursor, s.size());
        }
        std::memcpy(buf, s.c_str(), s.size() + 1);
        if (st) {
            st->WantReloadUserBuf = true;
            st->ReloadSelectionStart = st->ReloadSelectionEnd = static_cast<int>(cursor);
        }
    };

    const float ctx_pad_x = ui_s(10.0f);
    float ctx_label_w = 0.0f;
    for (int i = 0; i < item_count; ++i)
        ctx_label_w = std::max(ctx_label_w, ImGui::CalcTextSize(items[i].label).x);
    const float ctx_menu_w = ctx_label_w + ctx_pad_x * 2.0f + ui_s(4.0f) * 2.0f; // + WindowPadding
    // The menu's width is pinned through ImGui's "next window" hints (SetNextWindowSizeConstraints
    // + style pushes). Those hints stick until the NEXT Begin() of the frame, so pushing them on a
    // frame where the popup cannot appear clamps whatever window happens to be begun next -- in
    // the URL page that is the page-content window, which then collapses to the menu's width and
    // drags the whole Home form out of view (horizontal auto-scroll to the focused field). So they
    // are only pushed when the menu can really open (right-click on the field) or already is open.
    const bool ctx_open = ImGui::IsPopupOpen("##url_ctx");
    out.ctx_open = ctx_open;
    const bool ctx_active = ctx_open ||
        (item_count > 0 && ImGui::IsItemHovered() &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
             ImGui::IsMouseReleased(ImGuiMouseButton_Right)));
    if (ctx_active) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(ctx_menu_w, 0.0f), ImVec2(ctx_menu_w, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(4.0f), ui_s(4.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, ui_s(ui::theme::kRadiusWindow));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f); // border self-drawn below
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::theme::kPanelBg);
    }
    int ctx_action = -1; // 0=cut 1=copy 2=paste 3=select-all 4=paste&open; applied below
    if (ctx_active && ImGui::BeginPopupContextItem("##url_ctx")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const float row_h = ImGui::GetFrameHeight();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float th = ImGui::GetTextLineHeight();
        for (int i = 0; i < item_count; ++i) {
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(row_w, row_h));
            const bool rhover = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                ctx_action = items[i].action;
                ImGui::CloseCurrentPopup();
            }
            const ImVec2 rmin = ImGui::GetItemRectMin();
            if (rhover)
                pdl->AddRectFilled(rmin, ImGui::GetItemRectMax(),
                    ui::theme::hover_fill_u32(), ui_s(ui::theme::kRadiusControl));
            pdl->AddText(ImVec2(rmin.x + ctx_pad_x, rmin.y + (row_h - th) * 0.5f),
                ui::theme::text_u32(), items[i].label);
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
    if (ctx_active) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    if (ctx_action == 0) { // cut (only offered with a selection, so st is valid)
        const int a = st->GetSelectionStart(), b = st->GetSelectionEnd();
        std::string s(buf);
        ImGui::SetClipboardText(s.substr(a, b - a).c_str());
        s.erase(static_cast<size_t>(a), static_cast<size_t>(b - a));
        std::memcpy(buf, s.c_str(), s.size() + 1);
        st->WantReloadUserBuf = true;
        st->ReloadSelectionStart = st->ReloadSelectionEnd = a;
    } else if (ctx_action == 1) { // copy
        if (has_sel) {
            const int a = st->GetSelectionStart(), b = st->GetSelectionEnd();
            ImGui::SetClipboardText(std::string(buf).substr(a, b - a).c_str());
        } else {
            ImGui::SetClipboardText(buf);
        }
    } else if (ctx_action == 2) { // paste
        paste_clipboard(false);
    } else if (ctx_action == 3) { // select all -- defer, see above
        if (select_all_pending) *select_all_pending = true;
    } else if (ctx_action == 4) { // paste & open: replace the field, then act as Enter
        paste_clipboard(true);
        out.submit_paste = true;
    }
    out.submit = enter;
    return out;
}

// ---- embedded browser page -----------------------------------------------------------------
// First-screen "打开 URL" and the top bar's link icon both call request_open_url_popup(), which
// ENTERS this page (no modal). Layout: the shared title bar on top, then the page content; the
// address bar is laid out between them as PERMANENT chrome (Home/Web/Loading/Unavailable) or as
// FLOATING chrome over the video (Native). Content states (Player::browser_content_kind(), same
// priority here as in browser_state()):
//
//   Loading      native open in flight -> spinner + Cancel over the page background
//   Native       the playing session IS the content: the video uses exactly the ordinary
//                viewport (fit_viewport() reserves only the title bar; the address bar reserves
//                nothing) and the normal bottom bar stays available. The address bar is shown
//                only while the cursor is in the top reveal strip or the field is in use.
//   Web          the WebView2 child window (a REAL HWND: nothing ImGui draws can appear on top
//                of it, so the chrome strips are laid out above its rect and it steps aside
//                while an overlay, the settings snapshot or the loading card is up)
//   Unavailable  WebView2 missing/failed -> one centered line
//   Home         first-screen form (nothing loaded yet)
//
// Address bar: [back] [forward] [reload] [URL field] [go] [play] [star]. A submitted http(s) URL
// is a NAVIGATION -- the page view shows an ordinary web page immediately, with no probing and no
// fallback. The play button is the explicit manual native entry: it hands the bar's URL to the
// decoder (any container FFmpeg can demux, no extension gate), and a failure there is reported as
// an open error instead of being re-routed to the page. The other way into Native is the page
// view's own handover: when the navigation's document IS a video, the page switches to Native by
// itself (BrowserView::MediaNavigation). The star favorites the COMMITTED page URL; the dropdown
// under the field lists the favorites (see build_favorites_dropdown). Tooltips and that dropdown
// reach into the page area, where the WebView2 child window would cover them -- they are lifted
// into the popup layers (popup_layer.h) while the live page view is up. Nothing here leaves the
// page implicitly: settings, the web-stream modal, the file picker, the export overlay and a
// successful native open all keep it alive.
void Player::build_browser_page(){
    ImGuiIO& io = ImGui::GetIO();
    sync_browser_page();

    float top_bar_h = 0.0f;
    build_top_bar(top_bar_h);

    const float addr_h = addr_bar_h();
    const float pad = ui_s(ui::theme::kSpaceXS);
    const float ctrl_h = std::max(1.0f, addr_h - 2.0f * pad);
    const float ctrl_w = ctrl_h; // square icon buttons, same beat as the title bar
    const bool busy = open_url_loading_;
    const int kind = browser_content_kind();
    const bool is_web = kind == 1;
    const bool is_native = kind == 3;

    // The address bar is PERMANENT chrome while the page shows a page/form/loading, but FLOATING
    // chrome while it shows the native video: then it reserves no space (the video viewport is
    // exactly the ordinary one) and only appears with the top strip -- same reveal rule as the
    // title bar in fullscreen -- or while the user is actually using it (typing, right-click
    // menu). Showing/hiding it never changes layout or resizes anything.
    const bool addr_pinned = browser_addr_active_ || browser_addr_focus_ ||
        browser_addr_select_all_ || browser_addr_ctx_open_;
    const bool addr_visible = !is_native || busy || mouse_in_top_reveal_zone() || addr_pinned;
    browser_addr_visible_ = addr_visible;
    const float content_y = top_bar_h + (addr_visible ? addr_h : 0.0f);
    const float content_h = std::max(0.0f, io.DisplaySize.y - content_y);

    // ---- settings overlay ------------------------------------------------------------------
    // The panel is drawn ON the page, not instead of it: the page view is captured to a texture
    // (BrowserView::request_snapshot), which backs the panel while the live view steps aside.
    // The page session, URL and history are untouched; closing the panel restores the same
    // controller. While the capture is in flight the live view stays up and the panel waits a
    // frame; if it fails the panel falls back to the plain player background.
    const bool settings_wanted = ui_settings_open_ && is_web;
    if (!ui_settings_open_ && browser_backdrop_requested_) reset_page_backdrop_state();
    if (settings_wanted && !browser_backdrop_requested_) {
        browser_backdrop_requested_ = true;
        browser_backdrop_state_ = 1; // pending
        browser_backdrop_url_ = browser_ ? browser_->page_url() : std::string();
        if (browser_) browser_->request_snapshot();
    }
    if (browser_backdrop_state_ == 1 && browser_) {
        std::vector<uint8_t> png;
        const BrowserView::Snapshot snap = browser_->take_snapshot(png);
        if (snap == BrowserView::Snapshot::Ready)
            browser_backdrop_state_ = upload_page_backdrop(png) ? 2 : 3;
        else if (snap == BrowserView::Snapshot::Failed)
            browser_backdrop_state_ = 3;
    }
    const bool backdrop_ready = settings_wanted && browser_backdrop_state_ == 2 &&
        browser_backdrop_srv_ != nullptr;
    const bool backdrop_failed = settings_wanted && browser_backdrop_state_ == 3;
    const bool settings_pending = settings_wanted && browser_backdrop_state_ == 1;

    // ---- address bar -------------------------------------------------------------------
    if (addr_visible) {
        ImGui::SetNextWindowPos(ImVec2(0, top_bar_h));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, addr_h));
        // NoFocusOnAppearing: this chrome appears/disappears with the cursor position, so it can
        // come back in the same frame a title-bar button is pressed -- an appearing window taking
        // focus clears that button's ActiveId, and the release then lands nowhere (the click is
        // silently dropped). Nothing here needs focus-on-appear: the URL field is focused
        // explicitly (url_field's SetKeyboardFocusHere) when the user actually types.
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(pad, ImGui::GetStyle().ItemSpacing.y));
        ImGui::Begin("##sumu_browser_bar", nullptr, flags);
        { // Bar fill + bottom hairline: same panel tone as the title bar above it.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetWindowPos();
            const ImVec2 p1(p0.x + ImGui::GetWindowSize().x,
                p0.y + ImGui::GetWindowSize().y);
            dl->AddRectFilled(p0, p1, ui::theme::panel_bg_u32());
            dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f),
                ui::theme::border_u32());
        }
        ImGui::SetCursorPosY((addr_h - ctrl_h) * 0.5f);
        // History buttons follow the engine's live state; from Native they are the way back to
        // the page that produced the media, and doing so pauses is handled by browser_enter_web().
        const bool can_back = browser_ && !busy && browser_->can_go_back();
        const bool can_forward = browser_ && !busy && browser_->can_go_forward();
        // Reload needs something loaded to re-fetch (Home with no page has nothing) and stays
        // disabled on the Native content, where it would drop the URL the session was opened
        // against.
        const bool can_reload = browser_ && browser_->ready() && !busy && !is_native &&
            !browser_->page_url().empty();
        {
            IconButtonResult r = icon_button("##browser_back", ImVec2(ctrl_w, ctrl_h),
                ui::AppIcon::NavBack, !can_back);
            if (r.clicked) browser_go_back();
        }
        ImGui::SameLine();
        {
            IconButtonResult r = icon_button("##browser_forward", ImVec2(ctrl_w, ctrl_h),
                ui::AppIcon::NavForward, !can_forward);
            if (r.clicked) browser_go_forward();
        }
        ImGui::SameLine();
        {
            IconButtonResult r = icon_button("##browser_reload", ImVec2(ctrl_w, ctrl_h),
                ui::AppIcon::Reload, !can_reload);
            if (r.clicked) browser_reload();
        }
        ImGui::SameLine();
        // Three right-side buttons: go, play, star (the field takes the remaining width).
        const float right_w = 3.0f * (ctrl_w + pad);
        float field_w = ImGui::GetContentRegionAvail().x - right_w;
        if (field_w < ui_s(80.0f)) field_w = ui_s(80.0f);
        UrlFieldResult field;
        ImGui::BeginDisabled(busy);
        field = url_field("##browser_addr", browser_addr_buf_, sizeof(browser_addr_buf_),
            field_w, &browser_addr_focus_, &browser_addr_select_all_);
        browser_addr_active_ = field.active;
        browser_addr_ctx_open_ = field.ctx_open;
        { // the favorites dropdown is pinned to the field's own rect
            const ImVec2 fmin = ImGui::GetItemRectMin();
            const ImVec2 fmax = ImGui::GetItemRectMax();
            browser_addr_field_rect_[0] = fmin.x;
            browser_addr_field_rect_[1] = fmin.y;
            browser_addr_field_rect_[2] = fmax.x;
            browser_addr_field_rect_[3] = fmax.y;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        { // go: navigate / open the address
            IconButtonResult r = icon_button("##browser_go", ImVec2(ctrl_w, ctrl_h),
                ui::AppIcon::OpenUrl, busy);
            if (r.clicked) field.submit = true;
            // Tooltip over the page view goes through the popup layer (see popup_layer.h):
            // ImGui draws it in this swapchain, the layer lifts it above the page view.
            if (ImGui::IsItemHovered() && !ui_str_.browser_go.empty())
                ImGui::SetTooltip("%s", ui_str_.browser_go.c_str());
        }
        ImGui::SameLine();
        { // native play: hands the URL in the bar to the decoder (no extension/container gate)
            const bool disabled = busy || browser_addr_text().empty() || browser_cookie_pending_;
            IconButtonResult r = icon_button("##browser_play", ImVec2(ctrl_w, ctrl_h),
                ui::AppIcon::Play, disabled);
            if (r.clicked) browser_play_native();
            if (ImGui::IsItemHovered() && !ui_str_.browser_play.empty())
                ImGui::SetTooltip("%s", ui_str_.browser_play.c_str());
        }
        ImGui::SameLine();
        { // star: favorite / unfavorite the COMMITTED page URL (never the unsubmitted text)
            const std::string committed = browser_committed_url();
            const bool favorited = browser_favorited(committed);
            const bool disabled = busy || committed.empty();
            IconButtonResult r = icon_button("##browser_star", ImVec2(ctrl_w, ctrl_h), disabled);
            const ImU32 tint = disabled ? ui::theme::icon_color_dim_u32()
                : (favorited ? ui::theme::accent_u32() : ui::theme::icon_color_u32());
            ui::DrawIconButtonGlyph(r, favorited ? ui::AppIcon::StarFilled : ui::AppIcon::Star,
                tint);
            if (r.clicked && !disabled) browser_toggle_favorite();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (field.submit || field.submit_paste) {
            fav_dropdown_open_ = false; // Enter / submit is one of the dropdown's close conditions
            browser_submit(browser_addr_buf_);
        }
    }

    // Failure of a native open: on Home the form shows the line itself; on Web/Native it is a
    // strip under the address bar (on Native the video stays behind it -- ImGui draws over it).
    const bool show_err_strip = open_url_show_load_error_ && (is_web || is_native);
    float err_h = 0.0f;
    if (show_err_strip) {
        ImGui::PushFont(nullptr, kFontSizeSm);
        err_h = ImGui::GetTextLineHeight() + 2.0f * pad;
        ImGui::PopFont();
        ImGui::SetNextWindowPos(ImVec2(0, content_y));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, err_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(ui_s(ui::theme::kPaddingContainer), pad));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
        ImGuiWindowFlags err_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoInputs;
        ImGui::Begin("##sumu_browser_err", nullptr, err_flags);
        ImGui::PushFont(nullptr, kFontSizeSm);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
        ImGui::TextUnformatted(ui_str_.open_url_load_failed.empty()
            ? "Could not open this URL" : ui_str_.open_url_load_failed.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleColor(); // WindowBg
        ImGui::PopStyleVar();
    }

    // ---- page content -------------------------------------------------------------------
    const float page_y = content_y + err_h;
    const float page_h = std::max(0.0f, content_h - err_h);
    // Settings overlay: the captured page, 1:1 over the area the live view used to cover.
    if (backdrop_ready) {
        ImGui::SetNextWindowPos(ImVec2(0, page_y));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, page_h));
        ImGuiWindowFlags bd_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoInputs;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##sumu_settings_backdrop", nullptr, bd_flags);
        ImGui::Image((ImTextureID)(intptr_t)browser_backdrop_srv_.Get(),
            ImVec2(io.DisplaySize.x, page_h));
        ImGui::End();
        ImGui::PopStyleVar();
    }
    if (!is_web && !is_native) {
        ImGui::SetNextWindowPos(ImVec2(0, page_y));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, page_h));
        // NoFocusOnAppearing, same reason as the address bar above: chrome that can appear while
        // a title-bar button is being pressed must not steal that button's ActiveId.
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoFocusOnAppearing;
        // Same card color as the first screen (kPanelBg) -- the page area is one surface.
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
        const bool page_visible = ImGui::Begin("##sumu_browser_page", nullptr, flags);
        if (page_visible) { // observable for scripts: the page surface must span the client
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            browser_page_rect_[0] = wp.x;
            browser_page_rect_[1] = wp.y;
            browser_page_rect_[2] = ws.x;
            browser_page_rect_[3] = ws.y;
        }

        if (busy) {
            // Loading state (native open in flight): spinner + Cancel, centered. Cancel
            // interrupts the blocked FFmpeg open (cancel_open_url -> AVIOInterruptCB); the
            // button greys out until Python calls notify_open_url_finished().
            const float r = ImGui::GetFrameHeight() * 0.65f;
            const float d = r * 2.0f + std::max(1.5f, r / 6.0f);
            const float btn_w = ui_s(88.0f);
            const float block_h = d + ui_s(ui::theme::kSpaceL) * 2.0f +
                ui::theme::kControlHeight * ui_s(1.0f);
            float y = (page_h - block_h) * 0.5f;
            if (y < 0.0f) y = 0.0f;
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - d) * 0.5f, y));
            ui::Spinner("##browser_loading");
            const bool cancelling = open_cancel_requested_.load(std::memory_order_relaxed);
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - btn_w) * 0.5f,
                y + d + ui_s(ui::theme::kSpaceL) * 2.0f));
            ImGui::BeginDisabled(cancelling);
            if (ui::Button(ui_str_.open_url_cancel.empty() ? "Cancel"
                                                          : ui_str_.open_url_cancel.c_str(),
                    ui::ButtonVariant::Secondary, ui::ControlSize::Regular, btn_w))
                cancel_open_url();
            ImGui::EndDisabled();
        } else if (browser_failed_ || (browser_ && browser_->failed())) {
            const char* text = ui_str_.browser_unavailable.empty()
                ? "Embedded browser unavailable" : ui_str_.browser_unavailable.c_str();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
            const ImVec2 tsize = ImGui::CalcTextSize(text);
            ImGui::SetCursorPos(ImVec2(std::max(0.0f, (io.DisplaySize.x - tsize.x) * 0.5f),
                std::max(0.0f, (page_h - tsize.y) * 0.5f)));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        } else {
            // First-screen form: title, hint, field, open/cancel. Cancel is the page's other
            // explicit exit (the address bar's close button is the first).
            const float form_w = ui_s(ui::theme::kModalContentW);
            const float hint_w = form_w;
            const float btn_w = ui_s(100.0f);
            const float row_w = btn_w * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            const float gap = ui_s(ui::theme::kSpaceM);

            const char* title = ui_str_.open_url_title.empty() ? "Open network URL"
                                                              : ui_str_.open_url_title.c_str();
            ImGui::PushFont(nullptr, kFontSizeBase);
            const ImVec2 tsize = ImGui::CalcTextSize(title);
            ImGui::PopFont();
            float hint_h = 0.0f;
            if (!ui_str_.open_url_hint.empty()) {
                ImGui::PushFont(nullptr, kFontSizeSm);
                hint_h = ImGui::CalcTextSize(ui_str_.open_url_hint.c_str(), nullptr, false,
                    hint_w).y;
                ImGui::PopFont();
            }
            const float field_h = ImGui::GetFrameHeight();
            const float err_line_h = (open_url_show_error_ || open_url_show_load_error_)
                ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
            const float block_h = tsize.y + gap + hint_h + gap + field_h + err_line_h +
                gap * 2.0f + ui_s(ui::theme::kControlHeight);
            float y = (page_h - block_h) * 0.5f;
            if (y < 0.0f) y = 0.0f;

            const float x0 = (io.DisplaySize.x - form_w) * 0.5f;
            // Observable layout of the Home block (see browser_state()'s "home_rect"): the form
            // must sit inside the page surface and be horizontally centered in it.
            browser_home_rect_[0] = x0;
            browser_home_rect_[1] = y;
            browser_home_rect_[2] = form_w;
            browser_home_rect_[3] = block_h;
            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - tsize.x) * 0.5f, y));
            ImGui::TextUnformatted(title);
            y += tsize.y + gap;

            if (hint_h > 0.0f) {
                ImGui::PushFont(nullptr, kFontSizeSm);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
                ImGui::PushTextWrapPos(x0 + hint_w);
                ImGui::SetCursorPos(ImVec2(x0, y));
                ImGui::TextUnformatted(ui_str_.open_url_hint.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();
                y += hint_h + gap;
            }

            ImGui::SetCursorPos(ImVec2(x0, y));
            UrlFieldResult field = url_field("##browser_home", browser_home_buf_,
                sizeof(browser_home_buf_), form_w, &browser_home_focus_,
                &browser_home_select_all_);
            y += field_h;

            if (open_url_show_error_) {
                ImGui::PushFont(nullptr, kFontSizeSm);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
                ImGui::SetCursorPos(ImVec2(x0, y));
                ImGui::TextUnformatted(ui_str_.open_url_invalid.empty()
                    ? "Enter a URL starting with http:// or https://"
                    : ui_str_.open_url_invalid.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            } else if (open_url_show_load_error_) {
                ImGui::PushFont(nullptr, kFontSizeSm);
                ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
                ImGui::SetCursorPos(ImVec2(x0, y));
                ImGui::TextUnformatted(ui_str_.open_url_load_failed.empty()
                    ? "Could not open this URL" : ui_str_.open_url_load_failed.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
            }
            y += err_line_h + gap * 2.0f;

            ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - row_w) * 0.5f, y));
            bool submit = field.submit || field.submit_paste;
            if (ui::Button(ui_str_.open_url_ok.empty() ? "Open" : ui_str_.open_url_ok.c_str(),
                    ui::ButtonVariant::Primary, ui::ControlSize::Regular, btn_w))
                submit = true;
            ImGui::SameLine();
            if (ui::Button(ui_str_.open_url_cancel.empty() ? "Cancel"
                                                          : ui_str_.open_url_cancel.c_str(),
                    ui::ButtonVariant::Secondary, ui::ControlSize::Regular, btn_w))
                exit_browser_page();
            if (submit) {
                // The form submits through the address bar: same validation, same
                // navigate-vs-native-open decision, and the bar mirrors what was submitted.
                const size_t n = std::min(std::strlen(browser_home_buf_),
                    sizeof(browser_addr_buf_) - 1);
                std::memcpy(browser_addr_buf_, browser_home_buf_, n);
                browser_addr_buf_[n] = '\0';
                browser_submit(browser_addr_buf_);
            }
        }

        ImGui::End();
        ImGui::PopStyleColor(); // WindowBg
    } else if (is_native) {
        // Native: the session video is already on the swapchain behind this page (the ordinary
        // viewport -- the floating address bar takes no space), so the page paints nothing here;
        // only the usual bottom bar, so playback controls keep working inside the page.
        build_bottom_bar();
    }

    // ---- in-page overlays ---------------------------------------------------------------
    // Both live INSIDE the page (neither navigates back to the player): the settings panel over
    // the captured page snapshot (skipped for the one frame that capture is still in flight --
    // the live page is what the user sees then), and the web-stream modal over the plain page
    // background (the live page view steps aside for it and comes back unchanged).
    if (ui_settings_open_ && !settings_pending)
        build_settings_panel(top_bar_h + (addr_visible ? addr_h : 0.0f));
    build_stream_popup();

    // ---- favorites dropdown --------------------------------------------------------------
    // Built last so it sits above the page content and the chrome in the main swapchain (where
    // the page view is not up, this is the only way it is drawn); while the live page view IS up
    // the popup layer lifts it into its own window instead.
    build_favorites_dropdown();

    // ---- page view placement -------------------------------------------------------------
    // The child window covers exactly the page area, below the chrome strips, and only when the
    // page itself is what should be visible: not loading, not native, not covered by an overlay,
    // not while the export screen owns the window. Bounds/visibility are pushed every frame
    // (both are no-ops when unchanged) so resize, DPI change and every transition land on the
    // next tick without extra plumbing.
    if (browser_) {
        // The live page stays up while the settings capture is in flight (no blank flash, and
        // the engine needs visible frames to capture); once the backdrop is ready -- or the
        // capture failed -- it steps aside for the panel.
        const bool web_visible = is_web && !browser_overlay_open() && !export_mode_ &&
            !backdrop_ready && !backdrop_failed;
        browser_->set_bounds(0, static_cast<int>(page_y),
            static_cast<int>(io.DisplaySize.x), static_cast<int>(page_h));
        browser_->set_visible(web_visible);
    }
}

// ---- address-bar favorites dropdown ---------------------------------------------------------
// Rows under the URL field: one line each -- title (the entry's label, URL when the page never
// reported one) truncated with an ellipsis, the URL in secondary text when it still fits, and an
// X at the row's right edge. Clicking a row puts its URL in the field and navigates through the
// address bar's own submit path; the X removes that favorite and keeps the list open.
//
// Visibility is a STICKY flag (Player::fav_dropdown_open_): pressing a row/X makes that button
// the ActiveId and deactivates the InputText on PRESS, while the click itself fires on RELEASE,
// so a "derived from the field's active state" test would close the list in the very frame the
// user clicks a row. It opens when the field takes the caret and then stays open while the field
// is active or the list itself is hovered.
void Player::build_favorites_dropdown(){
    const bool field_active = browser_addr_active_ && browser_addr_visible_;
    const bool can_open = browser_addr_visible_ && !open_url_loading_ &&
        !browser_favorites_.empty();
    if (can_open && field_active && !fav_field_was_active_) fav_dropdown_open_ = true;
    fav_field_was_active_ = field_active;
    if (!fav_dropdown_open_) return;
    if (!can_open) {
        fav_dropdown_open_ = false;
        return;
    }

    constexpr int kMaxVisibleRows = 8;
    const float row_h = ui_s(ui::theme::kControlHeight);
    const float pad = ui_s(ui::theme::kSpaceS);
    const float x_w = row_h; // square hit area for the row's X
    const int rows = static_cast<int>(browser_favorites_.size());
    const int visible_rows = std::min(rows, kMaxVisibleRows);
    const float field_x0 = browser_addr_field_rect_[0];
    const float w = std::max(ui_s(120.0f), browser_addr_field_rect_[2] - field_x0);

    ImGui::SetNextWindowPos(ImVec2(field_x0, browser_addr_field_rect_[3] + ui_s(2.0f)));
    ImGui::SetNextWindowSize(ImVec2(w, static_cast<float>(visible_rows) * row_h + 2.0f * pad));
    // No NoBringToFrontOnFocus: the list must come to the front when it is clicked. It is built
    // after the page content (see build_browser_page), so it also starts above the address bar.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;
    const ImVec2 win_pad(pad, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, win_pad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_s(ui::theme::kRadiusWindow));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); // border self-drawn below
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::theme::kPanelBg);
    ImGui::Begin("##sumu_favorites", nullptr, flags);

    { // hairline outline, same floating-surface language as the modal / URL context menu
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        dl->AddRect(ImVec2(p0.x + 0.5f, p0.y + 0.5f), ImVec2(p1.x - 0.5f, p1.y - 0.5f),
            ui::theme::border_u32(), ui_s(ui::theme::kRadiusWindow), 0, 1.0f);
    }

    std::string navigate_url;
    std::string remove_url;
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
        ImGuiHoveredFlags_ChildWindows);
    // The scroll region paints its own ChildBg (the theme's list standard, kWindowBg) at Begin;
    // the dropdown is one panel surface, so it takes the panel tone the window and the popup's
    // backdrop already use.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::theme::kPanelBg);
    ImGui::BeginChild("##fav_rows", ImVec2(0.0f, static_cast<float>(visible_rows) * row_h));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float text_pad = ui_s(ui::theme::kSpaceM);
    for (int i = 0; i < rows; ++i) {
        const BrowserFavorite& fav = browser_favorites_[i];
        ImGui::PushID(i);
        const ImVec2 row_pos = ImGui::GetCursorScreenPos();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const bool row_clicked = ImGui::InvisibleButton("##row", ImVec2(row_w - x_w, row_h));
        const bool row_hovered = ImGui::IsItemHovered();
        if (row_hovered) // the wash spans the whole row, the X included
            dl->AddRectFilled(row_pos, ImVec2(row_pos.x + row_w, row_pos.y + row_h),
                ui::theme::hover_fill_u32(), ui_s(ui::theme::kRadiusControl));
        const float th = ImGui::GetTextLineHeight();
        const float ty = row_pos.y + (row_h - th) * 0.5f;
        const float text_w = row_w - x_w - 2.0f * text_pad;
        const ImVec2 title_sz = ImGui::CalcTextSize(fav.title.c_str());
        const ImVec2 url_sz = ImGui::CalcTextSize(fav.url.c_str());
        const float gap = ui_s(ui::theme::kSpaceM);
        const bool show_url = fav.title != fav.url; // an untitled entry's label already is its URL
        if (show_url && title_sz.x + gap + url_sz.x <= text_w) { // label + URL on one line
            dl->AddText(ImVec2(row_pos.x + text_pad, ty), ui::theme::text_u32(),
                fav.title.c_str());
            dl->AddText(ImVec2(row_pos.x + text_pad + title_sz.x + gap, ty),
                ui::theme::text_secondary_u32(), fav.url.c_str());
        } else {
            const float text_x1 = row_pos.x + text_pad + text_w;
            ImGui::RenderTextEllipsis(dl, ImVec2(row_pos.x + text_pad, ty),
                ImVec2(text_x1, ty + th), text_x1, fav.title.c_str(), nullptr, &title_sz);
        }
        ImGui::SameLine(0.0f, 0.0f);
        ui::IconButtonResult xr{};
        xr.clicked = ImGui::InvisibleButton("##del", ImVec2(x_w, row_h));
        xr.min = ImGui::GetItemRectMin();
        xr.max = ImGui::GetItemRectMax();
        xr.dl = ImGui::GetWindowDrawList();
        const bool x_hovered = ImGui::IsItemHovered();
        ui::DrawIconButtonGlyph(xr, ui::AppIcon::Close,
            x_hovered ? ui::theme::error_u32() : ui::theme::text_secondary_u32());
        if (row_clicked) navigate_url = fav.url;
        if (xr.clicked) remove_url = fav.url; // keeps the list open
        ImGui::PopID();
    }
    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg
    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(3);

    if (!navigate_url.empty()) {
        const size_t n = std::min(navigate_url.size(), sizeof(browser_addr_buf_) - 1);
        std::memcpy(browser_addr_buf_, navigate_url.c_str(), n);
        browser_addr_buf_[n] = '\0';
        fav_dropdown_open_ = false;
        browser_submit(navigate_url);
        return;
    }
    if (!remove_url.empty()) {
        browser_remove_favorite(remove_url);
        return;
    }
    // Any click outside the field and the list closes it (Esc/submit/leaving the page are handled
    // by their own paths); a list nobody is interacting with does not linger.
    if (!field_active && !hovered) fav_dropdown_open_ = false;
}

// Web-stream server modal (Phase 2): port + video-root folder + 启动/停止. Chrome is the
// shared ui::BeginModal/EndModal (same chrome as every other modal: title strip + close X,
// 8px rounding + 1px border, centered footer buttons). While running it shows the access URL
// as a clickable link (TextLinkOpenURL → default browser) instead of the form -- no
// persistent status float.
void Player::build_stream_popup(){
    stream_popup_open_ = false;
    if (stream_popup_) { ImGui::OpenPopup("###sumu_stream"); stream_popup_ = false; }
    bool open = true;
    // ID after ### matches OpenPopup("###sumu_stream"); BeginModal strips it for display and
    // re-centers every frame (AlwaysAutoResize takes a frame to settle the content size).
    const std::string modal_title =
        (ui_str_.stream_title.empty() ? std::string("Web stream") : ui_str_.stream_title)
        + "###sumu_stream";
    if (!ui::BeginModal(modal_title.c_str(), &open)) return;
    stream_popup_open_ = true;

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

