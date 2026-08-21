// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Export screen + export preset editor.
//
// Layout contract (docs/ui_design.md): the export screen is a two-column layout --
// left column cards [设置] [预设]; the right column is one full-height [视频队列] card
// whose pinned footer rows [添加文件] + [开始导出] side by side. Queue items
// are cards themselves: col1 = status/filename/output path, col2 = preset + output-mode
// combos, col3 = a gray delete X (red on hover). Reordering is manual gap-based drag:
// hold the card's left strip and the list opens an empty slot under the cursor; release
// drops the item into it. The preset manager is the first-class 导出预设 card; only
// the preset EDITOR remains a ui::BeginModal.
#include "player.h"
#include "ui_util.h"

#include <algorithm>

namespace {

// Card outer height for one queue item: content block (text lines / combo pair / 72px
// minimum) + 2*pad. Shared by the item renderer and the drag-slot math so both agree.
float export_item_card_h(const ExportItemView& item, float s){
    const float sp_y = ImGui::GetStyle().ItemSpacing.y;
    const float frame_h = ImGui::GetFrameHeight();
    const float base_lh = ImGui::GetTextLineHeight();
    ImGui::PushFont(nullptr, Player::kFontSizeSm);
    const float sm_lh = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const bool has_progress = (item.status == "running" || item.status == "done");
    float col1 = sm_lh + sp_y + base_lh + sp_y + sm_lh;
    if (has_progress) col1 += sp_y + 6.0f * s;
    if (!item.error.empty()) col1 += sp_y + base_lh;
    const float ch = std::max(std::max(2.0f * frame_h + sp_y, 72.0f * s), col1);
    return ch + 2.0f * ui::theme::kPaddingContainer * s;
}

// Drag grip: two columns of three dots.
void draw_grip_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col){
    const float r = 1.3f * s;
    const float dx = 2.5f * s, dy = 4.0f * s;
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; j += 2)
            dl->AddCircleFilled(ImVec2(c.x + j * dx, c.y + i * dy), r, col);
}

} // namespace

const char* Player::export_status_label(const std::string& s) const{
    if (s == "running") return ui_str_.export_status_running.c_str();
    if (s == "done") return ui_str_.export_status_done.c_str();
    if (s == "failed") return ui_str_.export_status_failed.c_str();
    if (s == "cancelled") return ui_str_.export_status_cancelled.c_str();
    if (s == "interrupted") return ui_str_.export_status_interrupted.c_str();
    return ui_str_.export_status_pending.c_str();
}

std::string Player::export_preset_summary(const ExportPresetView& p){
    std::string s = p.name;
    s += " · ";
    if (p.cq_enabled) s += "CQ " + std::to_string(p.cq);
    if (p.bitrate_enabled)
        s += (p.cq_enabled ? " + " : "") + std::to_string(p.bitrate) + "k";
    if (p.maxrate_enabled)
        s += " max " + std::to_string(p.maxrate) + "k";
    s += " · " + p.preset;
    s += p.codec == "h264" ? " · H.264" : " · HEVC";
    return s;
}

int Player::export_quality_idx_of(const std::string& preset){
    if (preset.size() >= 2 && preset[0] == 'p') {
        int n = preset[1] - '0';
        if (n >= 1 && n <= 7) return n - 1;
    }
    return 6;
}

// One preset row card (shared by the 预设 card's scrolling list): default checkbox,
// click-to-edit name + summary, trash icon at the right edge.
void Player::build_export_preset_card(int i, float width){
    const auto& p = export_presets_[i];
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::theme::kRowCardBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_s(ui::theme::kRadiusControl));
    // Row height is the single-row container standard (kTopBarHBase, shared with the
    // title bar); pad is what remains around the 32px frame controls, giving the trash
    // button equal top/right/bottom margins (title-bar's uniform kSpaceXS beat).
    const float row_h = ui_s(kTopBarHBase);
    const float pad = std::max(0.0f, (row_h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    // NoScrollbar: the row fits exactly; rounding overflow must clip, never scroll.
    // AlwaysUseWindowPadding is MANDATORY: ImGui zeroes a borderless child window's
    // padding otherwise (imgui.cpp Begin), silently discarding the push above.
    ImGui::BeginChild("##preset_card", ImVec2(width, row_h), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float gap = ImGui::GetStyle().ItemSpacing.x;

    // The checkbox's VISIBLE box is 16px, not the 32px frame the trash button uses:
    // its equal-margin inset is (row_h - box)/2 = 10px. Vertically that already falls
    // out of the box being centered in its frame-height item; horizontally the cursor
    // must move past the window pad, and the right gap takes the same inset.
    const float cb_inset = std::max(0.0f,
        (row_h - ui::theme::kCheckboxSize * ui_s(1.0f)) * 0.5f);

    // Default marker: exactly one preset is the queue default. Checkbox is the standard
    // control; clicking the already-default card's box is a no-op.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cb_inset - pad);
    bool is_def = (export_default_preset_idx_ == i);
    if (ui::Checkbox("##def", &is_def) && is_def) {
        export_default_preset_idx_ = i;
        ui_intents_.export_set_default = i;
    }
    if (ImGui::IsItemHovered() && !ui_str_.export_preset_default.empty())
        ImGui::SetTooltip("%s", ui_str_.export_preset_default.c_str());
    ImGui::SameLine(0.0f, cb_inset); // checkbox right margin == its other three margins

    // Name + summary text column, plain drawing: the WHOLE card is the click-to-edit
    // target (see below), so there is no inner hover rect for the text to sit in.
    const float icon_sz = ImGui::GetFrameHeight(); // delete size unchanged: 32px square
    const float text_w = std::max(ui_s(40.0f),
        ImGui::GetContentRegionAvail().x - icon_sz - gap);
    ImGui::Dummy(ImVec2(text_w, ImGui::GetFrameHeight())); // reserve the column, advance the line
    {
        const ImVec2 tmin = ImGui::GetItemRectMin();
        const ImVec2 tmax = ImGui::GetItemRectMax();
        // Elide the WHOLE summary to the text width first -- hard clipping cut glyphs in
        // half; the name keeps the primary color, the "· CQ … · p7 · HEVC" tail secondary.
        std::string summary = sumu_ui::elide_text_to_width(export_preset_summary(p), text_w);
        const bool name_intact = summary.size() >= p.name.size() &&
            summary.compare(0, p.name.size(), p.name) == 0;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float th = ImGui::GetTextLineHeight();
        const float ty = tmin.y + (tmax.y - tmin.y - th) * 0.5f;
        dl->PushClipRect(tmin, tmax, true);
        if (name_intact) {
            const float name_w = ImGui::CalcTextSize(p.name.c_str()).x;
            dl->AddText(ImVec2(tmin.x, ty), ui::theme::text_u32(), p.name.c_str());
            if (summary.size() > p.name.size())
                dl->AddText(ImVec2(tmin.x + name_w, ty),
                    ui::theme::text_secondary_u32(), summary.c_str() + p.name.size());
        } else {
            dl->AddText(ImVec2(tmin.x, ty), ui::theme::text_u32(), summary.c_str());
        }
        dl->PopClipRect();
    }

    // Trash: square at the right edge (inset pad on top/right/bottom); glyph red on hover.
    ImGui::SameLine(0.0f, gap);
    ui::IconButtonResult dr = ui::IconButton("##del", ImVec2(icon_sz, icon_sz));
    ui::DrawIconButtonGlyph(dr, ui::AppIcon::Trash, ImGui::IsItemHovered()
        ? ui::theme::error_u32() : ui::theme::icon_color_dim_u32());
    if (dr.clicked) {
        ui_intents_.export_preset_delete = true;
        ui_intents_.export_preset_edit_idx = i;
    }

    // Whole card = click-to-edit target: hot only when no sub-widget (checkbox/trash)
    // claims the mouse. The highlight is drawn INSIDE the child (on top of its bg);
    // the parent draw list would render UNDER the child and be invisible.
    const bool card_hot = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    if (card_hot) {
        const ImVec2 cmin = ImGui::GetWindowPos();
        const ImVec2 cmax(cmin.x + ImGui::GetWindowSize().x, cmin.y + ImGui::GetWindowSize().y);
        ImGui::GetWindowDrawList()->AddRectFilled(cmin, cmax,
            ui::theme::hover_fill_u32(), ui_s(ui::theme::kRadiusControl));
    }

    ImGui::EndChild();

    if (card_hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        open_export_preset_editor(i);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

// One queue-item card: [drag strip+grip | status/filename/path (+progress) | preset+out
// combos | X]. Flat layout inside the card child: the old nested col1/col2 child windows
// inherited the card's pushed ChildBg (double-painted inner panels) and their clip rects
// caused phantom margins / clipped combos, so everything is placed absolutely instead.
void Player::build_export_item_card(ExportItemView& item, float width,
                                    const std::vector<const char*>& preset_names,
                                    const char* const out_modes[]){
    ImGui::PushID(item.id);

    const float s = ui_s(1.0f);
    const float pad = ui_s(ui::theme::kPaddingContainer);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float frame_h = ImGui::GetFrameHeight();
    const float sp_y = ImGui::GetStyle().ItemSpacing.y;

    const float card_h = export_item_card_h(item, s);
    const float ch = card_h - 2.0f * pad;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::theme::kRowCardBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_s(ui::theme::kRadiusControl));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    // NoScrollbar: a fixed-size card must clip, never grow its own scrollbar.
    // AlwaysUseWindowPadding: without it ImGui forces a borderless child's padding to 0,
    // which used to pin every column to the card's top-left corner.
    const ImGuiWindowFlags card_wflags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::BeginChild("##item_card", ImVec2(width, card_h), ImGuiChildFlags_AlwaysUseWindowPadding, card_wflags);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float inner_w = width - 2.0f * pad;
    const float grip_w = ui_s(14.0f);
    const float col2_w = ui_s(150.0f);
    const float col3_w = frame_h; // delete button: same row-height square as the path card's
    const float col1_w = std::max(ui_s(60.0f),
        inner_w - grip_w - col2_w - col3_w - 3.0f * gap);

    // Absolute column placement. SameLine chains at child boundaries have repeatedly
    // produced wrapped/misaligned columns here, so every column is pinned with
    // SetCursorScreenPos relative to the card's content origin instead.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float col1_x = origin.x + grip_w + gap;
    const float col2_x = col1_x + col1_w + gap;
    const float col3_x = col2_x + col2_w + gap;
    const float mid_y = origin.y + ch * 0.5f;

    // ---- col 0: drag strip. Everything left of the text (card edge .. col1) initiates
    // the drag; plain InvisibleButton, no hover fill by design. Pending items only.
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x - pad, origin.y));
        ImGui::InvisibleButton("##grip", ImVec2(pad + grip_w + gap, ch));
        if (item.status == "pending" && ImGui::IsItemActivated())
            export_drag_id_ = item.id;
        draw_grip_glyph(dl, ImVec2(origin.x + grip_w * 0.5f, mid_y), s,
            export_drag_id_ == item.id ? ui::theme::accent_u32() : ui::theme::icon_color_dim_u32());
    }

    // ---- col 1: status / filename / output path (+ progress while running) ----
    // Lines absolutely placed; the whole block is vertically centered on the card.
    {
        const float base_lh = ImGui::GetTextLineHeight();
        ImGui::PushFont(nullptr, kFontSizeSm);
        const float sm_lh = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const bool has_progress = (item.status == "running" || item.status == "done");
        float content_h = sm_lh + sp_y + base_lh + sp_y + sm_lh;
        if (has_progress) content_h += sp_y + ui_s(6.0f);
        if (!item.error.empty()) content_h += sp_y + base_lh;
        float ty = origin.y + std::max(0.0f, (ch - content_h) * 0.5f);

        ImGui::SetCursorScreenPos(ImVec2(col1_x, ty));
        ImGui::PushFont(nullptr, kFontSizeSm);
        // 完成 status goes green, matching the progress bar.
        ImGui::PushStyleColor(ImGuiCol_Text,
            item.status == "done" ? ui::theme::kSuccess : ui::theme::kTextSecondary);
        ImGui::TextUnformatted(export_status_label(item.status));
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ty += sm_lh + sp_y;
        ImGui::SetCursorScreenPos(ImVec2(col1_x, ty));
        std::string shown = sumu_ui::elide_text_to_width(
            sumu_ui::basename_of(item.source), col1_w);
        ImGui::TextUnformatted(shown.c_str());
        if (!item.source.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", item.source.c_str());

        ty += base_lh + sp_y;
        ImGui::SetCursorScreenPos(ImVec2(col1_x, ty));
        ImGui::PushFont(nullptr, kFontSizeSm);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
        if (!item.out_path.empty()) {
            std::string out = sumu_ui::elide_text_to_width("→ " + item.out_path, col1_w);
            ImGui::TextUnformatted(out.c_str());
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();

        if (has_progress) {
            ty += sm_lh + sp_y;
            ImGui::SetCursorScreenPos(ImVec2(col1_x, ty));
            float frac = (item.progress >= 0.0f) ? item.progress : -1.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::theme::kSuccess);
            ui::ProgressBar(frac, col1_w, ui_s(6.0f));
            ImGui::PopStyleColor();
        }
        if (!item.error.empty()) {
            ty += (has_progress ? ui_s(6.0f) : sm_lh) + sp_y;
            ImGui::SetCursorScreenPos(ImVec2(col1_x, ty));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
            ImGui::TextUnformatted(item.error.c_str());
            ImGui::PopStyleColor();
        }
    }

    // ---- col 2: preset + output-mode combos (the pair centered as one block) ----
    {
        const float pair_h = 2.0f * frame_h + sp_y;
        const float top = origin.y + std::max(0.0f, (ch - pair_h) * 0.5f);
        int preset_idx = item.preset_idx;
        if (preset_idx < 0 || preset_idx >= (int)preset_names.size()) preset_idx = 0;
        ImGui::SetCursorScreenPos(ImVec2(col2_x, top));
        if (!preset_names.empty() &&
            ui::Combo("##preset", preset_names.data(), (int)preset_names.size(), &preset_idx, col2_w)) {
            ui_intents_.export_item_preset_id = item.id;
            ui_intents_.export_item_preset_idx = preset_idx;
        }
        int out_idx = (item.out_mode == "global") ? 1 : (item.out_mode == "custom") ? 2 : 0;
        ImGui::SetCursorScreenPos(ImVec2(col2_x, top + frame_h + sp_y));
        if (ui::Combo("##out", out_modes, 3, &out_idx, col2_w)) {
            ui_intents_.export_item_out_id = item.id;
            ui_intents_.export_item_out_mode = out_idx;
            if (out_idx == 2)
                ui_intents_.export_pick_custom = item.id;
        }
    }

    // ---- col 3: delete, gray X, red on hover, vertically centered on the card ----
    ImGui::SetCursorScreenPos(ImVec2(col3_x, mid_y - col3_w * 0.5f));
    {
        ui::IconButtonResult dr = ui::IconButton("##del", ImVec2(col3_w, col3_w));
        ui::DrawIconButtonGlyph(dr, ui::AppIcon::Close, ImGui::IsItemHovered()
            ? ui::theme::error_u32() : ui::theme::icon_color_dim_u32());
        if (dr.clicked)
            ui_intents_.export_remove = item.id;
        if (!ui_str_.export_remove.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ui_str_.export_remove.c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void Player::build_export_screen(float top_bar_h){
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, top_bar_h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, std::max(0.0f, io.DisplaySize.y - top_bar_h)));
    // Two columns scroll independently; the outer window itself never scrolls.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##sumu_export_screen", nullptr, flags);

    const char* t_start = ui_str_.export_start.empty() ? "Start" : ui_str_.export_start.c_str();
    // Vertical rhythm between stacked cards: the real gap is spacing + filler + spacing
    // (ItemSpacing.y applies on BOTH sides of the Dummy), so the filler is what's left
    // after two spacings. Getting this wrong by one spacing overflows the column and
    // spawns a stray column-level scrollbar.
    const float card_gap = std::max(0.0f,
        ui_s(ui::theme::kSpaceL) - 2.0f * ImGui::GetStyle().ItemSpacing.y);

    const float left_w = ui_s(320.0f);
    const float col_gap = ui_s(ui::theme::kSpaceL);

    // ================= left column: 设置 / 预设 ==================
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##export_left", ImVec2(left_w, 0.0f), ImGuiChildFlags_None);
    {
        // ---- card: 设置 (AI pipeline + export path merged; small LineLabels only) ----
        if (ui::BeginCard("##export_card_settings")) {
            ui::SectionHeader(ui_str_.export_section_settings.c_str());
            ui::LineLabel(ui_str_.export_clip_length_label.c_str());
            if (!export_clip_edit_init_) {
                export_clip_length_edit_ = export_clip_length_;
                export_clip_edit_init_ = true;
            }
            bool committed = false;
            ui::SliderInt("##export_clip_length", &export_clip_length_edit_, 30, 180, 0.0f, &committed);
            if (committed && export_clip_length_edit_ != export_clip_length_)
                ui_intents_.export_clip_length = export_clip_length_edit_;

            // Row: read-only path input + folder icon button (label is the LineLabel above).
            ui::LineLabel(ui_str_.export_global_dir_label.c_str());
            const float icon_sz = ImGui::GetFrameHeight();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float input_w = std::max(ui_s(40.0f),
                ImGui::GetContentRegionAvail().x - icon_sz - gap);
            // Read-only view of the configured directory; hint shows when unset.
            char empty_buf[1] = { '\0' };
            char* dir_buf = export_global_dir_.empty()
                ? empty_buf : const_cast<char*>(export_global_dir_.c_str());
            ui::TextInput("##export_global_dir", dir_buf,
                export_global_dir_.empty() ? 1 : export_global_dir_.size() + 1,
                "-", input_w, ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            // Framed icon button: form-row pickers carry the standard button fill ramp,
            // not the bare hover wash (design standard, see ui::IconButtonFramed).
            ui::IconButtonResult r = ui::IconButtonFramed("##export_pick_dir",
                ImVec2(icon_sz, icon_sz), ui::AppIcon::FolderInput);
            if (r.clicked)
                ui_intents_.export_pick_global = true;
        }
        ui::EndCard();
        ImGui::Dummy(ImVec2(0.0f, card_gap));

        // ---- card: 预设 (manager as a first-class card; editor stays a modal) ----
        // Height = whatever the column has left, so the left column itself never
        // scrolls (only this card's preset list does).
        const float presets_h = std::max(ui_s(120.0f),
            floorf(ImGui::GetContentRegionAvail().y) - 1.0f);
        if (ui::BeginCard("##export_card_presets", presets_h, ImGuiWindowFlags_NoScrollbar)) {
            ui::SectionHeader(ui_str_.export_presets_title.empty()
                ? "Presets" : ui_str_.export_presets_title.c_str());
            // Scrolling list above a pinned full-width "new preset" button. The list
            // height is exactly what remains below it minus the button row and the standard
            // gap between them. That gap is kPaddingContainer (the card's inner padding
            // width -- the same as the edge insets and the button's bottom inset), not the
            // smaller ItemSpacing. floorf rounds the freed height down so fractional-DPI
            // math can never spill the footer past the card edge.
            const float btn_h = ImGui::GetFrameHeight();
            const float gap_to_btn = ui_s(ui::theme::kPaddingContainer);
            const float list_h = std::max(ImGui::GetFrameHeight(),
                floorf(ImGui::GetContentRegionAvail().y - gap_to_btn - btn_h));
            // Scroll region: kWindowBg (#1E1E20 -- the app's base-background standard,
            // same as the scrollbar track) with a kSpaceM inner inset. The scrollbar hugs
            // the container's right edge; the right padding keeps the gap between it and
            // the cards, so a card's visible margin is equal on all four sides.
            // AlwaysUseWindowPadding keeps ImGui from zeroing this borderless child's
            // padding (same Begin() caveat as the row cards).
            // ItemSpacing.y is zeroed inside the list so the inter-card gap is the Dummy
            // alone (one kSpaceM, same as the edge inset) -- otherwise ItemSpacing would
            // apply on BOTH sides of the Dummy and double-count the gap.
            const float list_pad = ui_s(ui::theme::kSpaceM);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::theme::kWindowBg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_s(ui::theme::kRadiusWindow));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(list_pad, list_pad));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
            ImGui::BeginChild("##preset_list", ImVec2(0.0f, list_h), ImGuiChildFlags_AlwaysUseWindowPadding);
            const float list_w = ImGui::GetContentRegionAvail().x;
            for (int i = 0; i < (int)export_presets_.size(); ++i) {
                if (i > 0) ImGui::Dummy(ImVec2(0.0f, list_pad));
                build_export_preset_card(i, list_w);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
            // Gap above the pinned button == kPaddingContainer. The list child's
            // ItemSpacing.y was zeroed above (for the Dummy-only inter-card gap), so its
            // EndChild leaves NO ItemSpacing beat behind it -- the Dummy below carries the
            // ENTIRE gap, not a residual on top of a beat (the old `- sp_y` under-counted,
            // floating the button up and opening a wide bottom margin). Zero ItemSpacing
            // for this row: the button's own trailing spacing would otherwise push the
            // content past the card, even with NoScrollbar.
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
            ImGui::Dummy(ImVec2(0.0f, gap_to_btn));
            if (ui::Button(ui_str_.export_preset_new.c_str(), ui::ButtonVariant::Secondary,
                    ui::ControlSize::Regular, -1.0f))
                open_export_preset_editor(-2);
            ImGui::PopStyleVar();
        }
        ui::EndCard();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding(0,0)
    ImGui::SameLine(0.0f, col_gap);

    // ============== right column: 视频队列 card (开始导出 lives in its footer now) ==============
    // Own BeginChild, symmetric with the left column: after a child ends, ImGui returns
    // the cursor X to the LINE start (not the child's X), so laying a button out in the
    // parent window would teleport it back under the left column. Inside this child the
    // cursor math stays local.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##export_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    // The queue card fills the whole column height; its pinned footer carries the
    // 开始导出 button next to 添加文件 (no cardless action below the card anymore).
    const float queue_h = std::max(ui_s(120.0f),
        floorf(ImGui::GetContentRegionAvail().y) - 1.0f);
    if (ui::BeginCard("##export_card_queue", queue_h, ImGuiWindowFlags_NoScrollbar)) {
        ui::SectionHeader(ui_str_.export_section_queue.c_str());
        // Queue scrolls above a pinned footer row ([添加文件] + [开始导出]) at the card
        // bottom -- the same two-beat arrangement as the preset card's "新建预设": gap
        // above the buttons = kPaddingContainer, bottom inset = the card's own padding.
        const float btn_h = ImGui::GetFrameHeight();
        const float gap_to_btn = ui_s(ui::theme::kPaddingContainer);
        const float list_h = std::max(ImGui::GetFrameHeight(),
            floorf(ImGui::GetContentRegionAvail().y - gap_to_btn - btn_h) - 1.0f);
        // Scroll region mirrors the preset list: kWindowBg + kSpaceM inset + slim
        // scrollbar, so both columns read as the same "list panel + pinned action" card.
        // ItemSpacing.y stays at the global value here -- the queue item cards read it for
        // their own line layout, so do NOT zero it (the inter-card gap is instead nudge-
        // positioned in block_gap() below).
        const float list_pad = ui_s(ui::theme::kSpaceM);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::theme::kWindowBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ui_s(ui::theme::kRadiusWindow));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(list_pad, list_pad));
        ImGui::BeginChild("##queue_list", ImVec2(0.0f, list_h), ImGuiChildFlags_AlwaysUseWindowPadding);

        if (export_items_.empty()) {
            ImGui::Dummy(ImVec2(0.0f, ui_s(4.0f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kTextSecondary);
            ImGui::TextWrapped("%s", ui_str_.export_empty.c_str());
            ImGui::PopStyleColor();
        }

        std::vector<const char*> preset_names;
        preset_names.reserve(export_presets_.size());
        for (const auto& p : export_presets_) preset_names.push_back(p.name.c_str());
        const char* out_modes[] = {
            ui_str_.export_out_auto.c_str(),
            ui_str_.export_out_global.c_str(),
            ui_str_.export_out_custom.c_str(),
        };

        const float list_w = ImGui::GetContentRegionAvail().x;

        // ---- manual gap-based drag-reorder ----
        // export_drag_id_ is armed by the item card's left drag strip; export_drag_slot_
        // tracks the open gap's position. Slot math runs against the RENDERED geometry
        // (gap provisionally inserted at the previous slot), so hovering the gap itself
        // is stable instead of oscillating. Release drops the item into the slot.
        const ExportItemView* dragged = nullptr;
        if (export_drag_id_ >= 0) {
            for (const auto& it : export_items_)
                if (it.id == export_drag_id_) { dragged = &it; break; }
            if (!dragged) { export_drag_id_ = -1; export_drag_slot_ = -1; } // vanished -> cancel
        }
        if (export_drag_id_ >= 0) {
            const float s1 = ui_s(1.0f);
            const float pitch_gap = list_pad; // inter-card gap == kSpaceM (block_gap()'s cursor nudge)
            const float drag_card_h = export_item_card_h(*dragged, s1);
            if (export_drag_slot_ < 0) {
                // initial slot = the dragged card's own index (gap opens where it was)
                export_drag_slot_ = 0;
                for (const auto& it : export_items_) {
                    if (it.id == export_drag_id_) break;
                    ++export_drag_slot_;
                }
            }
            const float mouse_y = io.MousePos.y;
            float y = ImGui::GetCursorScreenPos().y;
            int slot = 0, r = 0;
            for (const auto& it : export_items_) {
                if (it.id == export_drag_id_) continue;
                if (r == export_drag_slot_) y += drag_card_h + pitch_gap; // the open gap
                const float h = export_item_card_h(it, s1);
                if (mouse_y > y + h * 0.5f) slot = r + 1;
                y += h + pitch_gap;
                ++r;
            }
            export_drag_slot_ = slot;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                ImGui::SetTooltip("%s", sumu_ui::basename_of(dragged->source).c_str());
            } else {
                // Drop: map the slot to "insert before this item id" (-1 = queue end).
                int target = -1; r = 0;
                for (const auto& it : export_items_) {
                    if (it.id == export_drag_id_) continue;
                    if (r++ == export_drag_slot_) { target = it.id; break; }
                }
                // No-op if the card lands back where it already sits.
                bool noop = false;
                for (size_t k = 0; k + 1 < export_items_.size(); ++k)
                    if (export_items_[k].id == export_drag_id_ &&
                        export_items_[k + 1].id == target) { noop = true; break; }
                if (!noop && !(target == -1 && export_items_.back().id == export_drag_id_)) {
                    ui_intents_.export_move_id = export_drag_id_;
                    ui_intents_.export_move_to = target;
                    // Apply the reorder locally right away: the authoritative snapshot
                    // arrives a frame or two later, and without this the card flashes
                    // back at its old position in between.
                    size_t from = 0;
                    while (from < export_items_.size() && export_items_[from].id != export_drag_id_)
                        ++from;
                    if (from < export_items_.size()) {
                        ExportItemView moved = std::move(export_items_[from]);
                        export_items_.erase(export_items_.begin() + from);
                        size_t to = export_items_.size();
                        for (size_t k = 0; k < export_items_.size(); ++k)
                            if (export_items_[k].id == target) { to = k; break; }
                        export_items_.insert(export_items_.begin() + to, std::move(moved));
                    }
                }
                export_drag_id_ = -1;
                export_drag_slot_ = -1;
            }
        }

        int rendered = 0;
        bool first_block = true;
        auto block_gap = [&](){
            if (!first_block) {
                // Inter-card gap == kSpaceM (8px). The previous card's EndChild already
                // advanced the cursor by one ItemSpacing.y beat (6px); nudge it the rest of
                // the way to 8px. Do this via the cursor rather than a Dummy so we neither
                // zero ItemSpacing.y for the list (the item cards read it for their own
                // line layout) nor double-count the beat around a Dummy.
                ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                    - ImGui::GetStyle().ItemSpacing.y + list_pad);
            }
            first_block = false;
        };
        auto render_slot = [&](){
            block_gap();
            ImGui::Dummy(ImVec2(list_w, export_item_card_h(*dragged, ui_s(1.0f))));
            // accent outline marks the open slot the dragged card will drop into
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ui::theme::accent_u32(), ui_s(ui::theme::kRadiusControl), 0, 1.5f * ui_s(1.0f));
        };
        for (size_t ii = 0; ii < export_items_.size(); ++ii) {
            ExportItemView& item = export_items_[ii];
            if (export_drag_id_ >= 0 && item.id == export_drag_id_) continue; // hidden mid-drag
            if (rendered == export_drag_slot_ && export_drag_id_ >= 0) render_slot();
            block_gap();
            build_export_item_card(item, list_w, preset_names, out_modes);
            ++rendered;
        }
        // slot past the last card = drop at the queue end (replaces the old trailing strip)
        if (export_drag_id_ >= 0 && rendered == export_drag_slot_) render_slot();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        // Gap above the pinned footer row == kPaddingContainer. The list child's EndChild
        // already leaves one ItemSpacing beat (ItemSpacing.y stays global here), so the
        // residual Dummy + that beat = the padding width. Zero ItemSpacing for this row:
        // the buttons' own trailing spacing would otherwise push the content past the card,
        // even with NoScrollbar. Capturing ItemSpacing.y before the push keeps the Dummy's
        // math against the real global spacing.
        const float sp_y = ImGui::GetStyle().ItemSpacing.y;
        const float gap_above_btn = gap_to_btn - sp_y;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, gap_above_btn)));
        // Footer row: [添加文件] + [开始导出] side by side, splitting the column width.
        // The inter-button gap reads the global kPaddingContainer margin (12px) -- the
        // same value as the card's four-sided inset and the preset card's pinned 新建预设
        // button margins, so the whole footer sits on one unified rhythm. 开始导出 stays
        // disabled while the engine is still warming up.
        const float gap = ui_s(ui::theme::kPaddingContainer);
        const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ui::Button(ui_str_.export_add_files.empty() ? "Add files" : ui_str_.export_add_files.c_str(),
                ui::ButtonVariant::Secondary, ui::ControlSize::Regular, half))
            ui_intents_.export_add_files = true;
        ImGui::SameLine(0.0f, gap);
        if (!export_engine_ready_) ImGui::BeginDisabled();
        if (ui::Button(t_start, ui::ButtonVariant::Primary, ui::ControlSize::Regular, half))
            ui_intents_.export_start = true;
        if (!export_engine_ready_) ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    ui::EndCard();
    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding(0,0)

    ImGui::End();
}

// The preset EDITOR modal (the manager lives in the export screen's 导出预设 card now).
// export_preset_edit_idx_ != -1 means "open": open_export_preset_editor() arms the
// one-shot OpenPopup via export_presets_open_.
void Player::build_export_preset_editor(){
    if (export_presets_open_) {
        ImGui::OpenPopup("###sumu_export_preset_edit");
        export_presets_open_ = false;
    }
    if (export_preset_edit_idx_ == -1) return;
    bool open = true;
    const std::string modal_title =
        (ui_str_.export_presets_title.empty() ? std::string("Presets") : ui_str_.export_presets_title)
        + "###sumu_export_preset_edit";
    // Closed via X/Esc (or never opened): drop back to the manager state.
    if (!ui::BeginModal(modal_title.c_str(), &open, ui::theme::kModalContentWLg)) {
        export_preset_edit_idx_ = -1;
        return;
    }

    const float content_w = ui_s(ui::theme::kModalContentWLg);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float inner = ImGui::GetStyle().ItemInnerSpacing.x;

    // ---- staged fields, seeded once per open ----
    if (!export_preset_edit_init_) {
        const ExportPresetView* src = nullptr;
        if (export_preset_edit_idx_ >= 0 && export_preset_edit_idx_ < (int)export_presets_.size())
            src = &export_presets_[export_preset_edit_idx_];
        if (src) {
            snprintf(export_preset_name_buf_, sizeof(export_preset_name_buf_), "%s", src->name.c_str());
            export_preset_codec_idx_ = (src->codec == "h264") ? 1 : 0;
            export_preset_cq_enabled_ = src->cq_enabled;
            export_preset_cq_ = src->cq;
            export_preset_bitrate_enabled_ = src->bitrate_enabled;
            export_preset_bitrate_ = src->bitrate;
            export_preset_maxrate_enabled_ = src->maxrate_enabled;
            export_preset_maxrate_ = src->maxrate;
            export_preset_quality_idx_ = export_quality_idx_of(src->preset);
            export_preset_audio_copy_ = src->audio_copy;
            export_preset_audio_bitrate_ = src->audio_bitrate;
            export_preset_subtitle_ = src->subtitle;
            snprintf(export_preset_suffix_buf_, sizeof(export_preset_suffix_buf_), "%s", src->suffix.c_str());
        } else {
            export_preset_name_buf_[0] = '\0';
            export_preset_codec_idx_ = 0;
            export_preset_cq_enabled_ = true;
            export_preset_cq_ = 33;
            export_preset_bitrate_enabled_ = false;
            export_preset_bitrate_ = 0;
            export_preset_maxrate_enabled_ = false;
            export_preset_maxrate_ = 0;
            export_preset_quality_idx_ = 6;
            export_preset_audio_copy_ = true;
            export_preset_audio_bitrate_ = 256;
            export_preset_subtitle_ = true;
            snprintf(export_preset_suffix_buf_, sizeof(export_preset_suffix_buf_), "_Decensored");
        }
        export_preset_edit_init_ = true;
    }

    // Two row shapes share ONE label column: plain rows (名称/编码格式/质量/音频/后缀)
    // draw a text label of `label_w`; checkbox rows (CQ/码率/最大码率) draw
    // [checkbox box][ItemSpacing][label of mix_label_w]. Making label_w exactly equal
    // to box + spacing + mix_label_w puts every row's control at the same x.
    const float mix_label_w = ui_s(80.0f);
    const float label_w = ui_s(ui::theme::kCheckboxSize) + ImGui::GetStyle().ItemSpacing.x
        + mix_label_w;
    const float num_w = ui_s(ui::theme::kNumericInputW);
    const float field_w = content_w - label_w - inner;

    ui::InlineLabel(ui_str_.export_preset_name_label.c_str(), label_w);
    ui::TextInput("##ep_name", export_preset_name_buf_, sizeof(export_preset_name_buf_),
        nullptr, field_w);

    const char* codec_items[] = { "HEVC", "H.264" };
    ui::InlineLabel(ui_str_.export_preset_codec_label.c_str(), label_w);
    ui::Combo("##ep_codec", codec_items, 2, &export_preset_codec_idx_, field_w);

    // CQ / bitrate / maxrate are INDEPENDENT, each enabled by its own checkbox.
    // Mixed-row order: checkbox, inline label, number input, slider / unit.
    // CQ's direction is counter-intuitive (LOW number == HIGH quality), so its slider
    // carries end texts (macOS volume-slider icon positions, text instead of icons).
    ui::Checkbox("##ep_cq_en", &export_preset_cq_enabled_);
    ImGui::SameLine();
    ui::InlineLabel(ui_str_.export_preset_cq_label.c_str(), mix_label_w);
    if (!export_preset_cq_enabled_) ImGui::BeginDisabled();
    ui::SliderIntEnds("##ep_cq", &export_preset_cq_, 0, 51,
        u8"高质量", u8"小体积");
    if (!export_preset_cq_enabled_) ImGui::EndDisabled();

    ui::Checkbox("##ep_br_en", &export_preset_bitrate_enabled_);
    ImGui::SameLine();
    ui::InlineLabel(ui_str_.export_preset_bitrate_label.c_str(), mix_label_w);
    if (!export_preset_bitrate_enabled_) ImGui::BeginDisabled();
    ui::IntInput("##ep_bitrate", &export_preset_bitrate_, num_w);
    ImGui::SameLine();
    ui::UnitText("kbps");
    if (!export_preset_bitrate_enabled_) ImGui::EndDisabled();

    ui::Checkbox("##ep_mr_en", &export_preset_maxrate_enabled_);
    ImGui::SameLine();
    ui::InlineLabel(ui_str_.export_preset_maxrate_label.c_str(), mix_label_w);
    if (!export_preset_maxrate_enabled_) ImGui::BeginDisabled();
    ui::IntInput("##ep_maxrate", &export_preset_maxrate_, num_w);
    ImGui::SameLine();
    ui::UnitText("kbps");
    if (!export_preset_maxrate_enabled_) ImGui::EndDisabled();

    const char* quality_items[] = { "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
    ui::InlineLabel(ui_str_.export_preset_quality_label.c_str(), label_w);
    ui::Combo("##ep_quality", quality_items, 7, &export_preset_quality_idx_, field_w);

    const char* audio_items[] = {
        ui_str_.export_preset_audio_copy.c_str(),
        ui_str_.export_preset_audio_encode.c_str(),
    };
    ui::InlineLabel(ui_str_.export_preset_audio_label.c_str(), label_w);
    int audio_mode = export_preset_audio_copy_ ? 0 : 1;
    if (ui::Combo("##ep_audio", audio_items, 2, &audio_mode, ui_s(160.0f)))
        export_preset_audio_copy_ = (audio_mode == 0);
    if (!export_preset_audio_copy_) {
        ImGui::SameLine();
        ui::IntInput("##ep_audio_bitrate", &export_preset_audio_bitrate_, num_w);
        ImGui::SameLine();
        ui::UnitText("kbps");
    }

    ui::Checkbox(ui_str_.export_preset_subtitle_label.c_str(), &export_preset_subtitle_);

    ui::InlineLabel(ui_str_.export_preset_suffix_label.c_str(), label_w);
    ui::TextInput("##ep_suffix", export_preset_suffix_buf_, sizeof(export_preset_suffix_buf_),
        nullptr, field_w);

    // Footer: save/cancel share the row as two equal halves.
    ImGui::Dummy(ImVec2(0.0f, ui_s(4.0f)));
    const float half = (content_w - gap) * 0.5f;
    if (ui::Button(ui_str_.export_preset_save.c_str(), ui::ButtonVariant::Primary,
            ui::ControlSize::Regular, half)) {
        ui_intents_.export_preset_save = true;
        ui_intents_.export_preset_edit_idx = export_preset_edit_idx_;
        ui_intents_.export_preset_name = export_preset_name_buf_;
        ui_intents_.export_preset_codec = export_preset_codec_idx_;
        ui_intents_.export_preset_cq_enabled = export_preset_cq_enabled_;
        ui_intents_.export_preset_cq = export_preset_cq_;
        ui_intents_.export_preset_bitrate_enabled = export_preset_bitrate_enabled_;
        ui_intents_.export_preset_bitrate = export_preset_bitrate_;
        ui_intents_.export_preset_maxrate_enabled = export_preset_maxrate_enabled_;
        ui_intents_.export_preset_maxrate = export_preset_maxrate_;
        ui_intents_.export_preset_quality = export_preset_quality_idx_;
        ui_intents_.export_preset_audio_copy = export_preset_audio_copy_;
        ui_intents_.export_preset_audio_bitrate = export_preset_audio_bitrate_;
        ui_intents_.export_preset_subtitle = export_preset_subtitle_;
        ui_intents_.export_preset_suffix = export_preset_suffix_buf_;
        export_preset_edit_idx_ = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button(ui_str_.cancel.c_str(), ui::ButtonVariant::Secondary,
            ui::ControlSize::Regular, half)) {
        export_preset_edit_idx_ = -1;
        ImGui::CloseCurrentPopup();
    }

    ui::EndModal();
}

void Player::open_export_preset_editor(int idx){
    export_preset_edit_idx_ = idx;
    export_preset_edit_init_ = false; // seed buffers on the next frame
    export_presets_open_ = true;      // arms the editor modal's one-shot OpenPopup
}
