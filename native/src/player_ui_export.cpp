// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "ui_util.h"

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

void Player::build_export_screen(float top_bar_h){
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, top_bar_h));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, std::max(0.0f, io.DisplaySize.y - top_bar_h)));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    // Background/padding come straight from the theme (kWindowBg / WindowPadding = (12,10)
    // @ 96 DPI, already DPI-scaled) -- no per-call-site styling here.
    ImGui::Begin("##sumu_export_screen", nullptr, flags);

    const char* t_presets = ui_str_.export_presets_title.empty() ? "Presets" : ui_str_.export_presets_title.c_str();
    const char* t_start = ui_str_.export_start.empty() ? "Start" : ui_str_.export_start.c_str();

    // ---- action row (replaces the old full-window header's 返回/title): [开始导出] [预设设置]
    // "开始导出" is greyed until the engine is warm (nothing to run yet).
    {
        if (!export_engine_ready_) ImGui::BeginDisabled();
        if (ui::Button(t_start, ui::ButtonVariant::Primary, ui::ControlSize::Regular, ui_s(120.0f)))
            ui_intents_.export_start = true;
        if (!export_engine_ready_) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button(t_presets, ui::ButtonVariant::Secondary, ui::ControlSize::Regular, ui_s(120.0f)))
            export_presets_open_ = true;
    }
    ImGui::Separator();

    const float left_w = ui_s(280.0f);
    const float gap = ui_s(16.0f);

    // ---- left column: AI 管线设置 + 导出路径 ----
    ImGui::BeginChild("##export_left", ImVec2(left_w, 0.0f), false);
    {
        ui::SectionHeader(ui_str_.export_section_ai.c_str());
        ImGui::TextUnformatted(ui_str_.export_clip_length_label.c_str());
        if (!export_clip_edit_init_) {
            export_clip_length_edit_ = export_clip_length_;
            export_clip_edit_init_ = true;
        }
        ui::SliderInt("##export_clip_length", &export_clip_length_edit_, 30, 180,
            left_w - ui_s(24.0f));
        if (ImGui::IsItemDeactivatedAfterEdit() &&
            export_clip_length_edit_ != export_clip_length_)
            ui_intents_.export_clip_length = export_clip_length_edit_;

        ui::SectionHeader(ui_str_.export_section_path.c_str());
        ImGui::TextUnformatted(ui_str_.export_global_dir_label.c_str());
        ImGui::TextWrapped("%s", export_global_dir_.empty() ? "-" : export_global_dir_.c_str());
        if (ui::Button(ui_str_.export_pick_dir.empty() ? "Choose" : ui_str_.export_pick_dir.c_str(),
                ui::ButtonVariant::Secondary, ui::ControlSize::Regular, ui_s(96.0f)))
            ui_intents_.export_pick_global = true;
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, gap);

    // ---- main: 视频队列 ----
    ImGui::BeginChild("##export_queue", ImVec2(0.0f, 0.0f), false);
    {
        // Small-font label + SameLine "add files" button (ui::SectionHeader draws a
        // full-width rule, which cannot share a line with the button -- label kept manual).
        ImGui::PushFont(nullptr, kFontSizeSm);
        ImGui::TextUnformatted(ui_str_.export_section_queue.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ui::Button(ui_str_.export_add_files.empty() ? "Add files" : ui_str_.export_add_files.c_str(),
                ui::ButtonVariant::Secondary, ui::ControlSize::Regular, ui_s(96.0f)))
            ui_intents_.export_add_files = true;
        ImGui::Separator();

        if (export_items_.empty()) {
            ImGui::TextWrapped("%s", ui_str_.export_empty.c_str());
        }

        std::vector<const char*> preset_names;
        preset_names.reserve(export_presets_.size());
        for (const auto& p : export_presets_) preset_names.push_back(p.name.c_str());
        const char* out_modes[] = {
            ui_str_.export_out_auto.c_str(),
            ui_str_.export_out_global.c_str(),
            ui_str_.export_out_custom.c_str(),
        };

        for (auto& item : export_items_) {
            std::string id = std::to_string(item.id);
            ImGui::PushID(item.id);
            ImGui::TextUnformatted(export_status_label(item.status));
            ImGui::SameLine();
            float avail = ImGui::GetContentRegionAvail().x;
            const float btns_w = ui_s(4.0f * 26.0f + 3.0f * 4.0f);
            const float name_w = std::max(ui_s(120.0f), avail - btns_w - ui_s(250.0f));
            std::string shown = sumu_ui::elide_text_to_width(sumu_ui::basename_of(item.source), name_w);
            ImGui::TextUnformatted(shown.c_str());
            if (!item.source.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", item.source.c_str());

            if (item.status == "running" || item.status == "done") {
                float frac = (item.progress >= 0.0f) ? item.progress : -1.0f;
                ui::ProgressBar(frac, 0.0f, ui_s(6.0f));
            }

            int preset_idx = item.preset_idx;
            if (preset_idx < 0 || preset_idx >= (int)preset_names.size()) preset_idx = 0;
            if (!preset_names.empty() &&
                ui::Combo(("##preset_" + id).c_str(), preset_names.data(),
                    (int)preset_names.size(), &preset_idx, ui_s(140.0f))) {
                ui_intents_.export_item_preset_id = item.id;
                ui_intents_.export_item_preset_idx = preset_idx;
            }
            ImGui::SameLine();
            int out_idx = (item.out_mode == "global") ? 1 : (item.out_mode == "custom") ? 2 : 0;
            if (ui::Combo(("##out_" + id).c_str(), out_modes, 3, &out_idx, ui_s(110.0f))) {
                ui_intents_.export_item_out_id = item.id;
                ui_intents_.export_item_out_mode = out_idx;
                if (out_idx == 2)
                    ui_intents_.export_pick_custom = item.id;
            }
            ImGui::SameLine();
            const float item_btn_w = ui_s(26.0f);
            if (ui::Button(ui_str_.export_up.c_str(), ui::ButtonVariant::Secondary,
                    ui::ControlSize::Small, item_btn_w)) {
                ui_intents_.export_move_id = item.id;
                ui_intents_.export_move_dir = -1;
            }
            ImGui::SameLine();
            if (ui::Button(ui_str_.export_down.c_str(), ui::ButtonVariant::Secondary,
                    ui::ControlSize::Small, item_btn_w)) {
                ui_intents_.export_move_id = item.id;
                ui_intents_.export_move_dir = 1;
            }
            ImGui::SameLine();
            if (ui::Button(ui_str_.export_remove.c_str(), ui::ButtonVariant::Danger,
                    ui::ControlSize::Small, item_btn_w))
                ui_intents_.export_remove = item.id;
            ImGui::SameLine();
            if (item.status == "running" || item.status == "pending") {
                if (ui::Button(ui_str_.cancel.c_str(), ui::ButtonVariant::Secondary,
                        ui::ControlSize::Small, item_btn_w))
                    ui_intents_.export_cancel = item.id;
            }
            if (!item.out_path.empty()) {
                ImGui::TextUnformatted(("  → " + item.out_path).c_str());
            }
            if (!item.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kError);
                ImGui::TextWrapped("%s", item.error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (!export_engine_ready_)
        ImGui::TextUnformatted(ui_str_.export_not_ready.c_str());

    ImGui::End();
}

void Player::build_export_preset_editor(){
    if (!export_presets_open_) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Non-modal plain window (not OpenPopup-driven), so ui::BeginModal does not apply; the
    // theme supplies WindowBg/rounding (kRadiusWindow == the old ui_s(8)). Only the roomier
    // padding stays local.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_s(14.0f), ui_s(12.0f)));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoTitleBar;
    if (!ImGui::Begin("##sumu_export_presets", nullptr, flags)) {
        ImGui::PopStyleVar();
        return;
    }
    const float w = ui_s(460.0f);
    ImGui::PushItemWidth(w);

    ImGui::TextUnformatted(ui_str_.export_presets_title.c_str());
    ImGui::Separator();

    if (export_preset_edit_idx_ == -1) {
        // ---- manager: preset cards (click to edit) + delete + default radio ----
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float del_w = ui_s(48.0f);
        const float def_w = ui_s(28.0f);
        for (int i = 0; i < (int)export_presets_.size(); ++i) {
            const auto& p = export_presets_[i];
            ImGui::PushID(i);
            std::string summary = export_preset_summary(p);
            const float card_w = w - del_w - def_w - spacing * 2.0f;
            if (ui::Button(summary.c_str(), ui::ButtonVariant::Secondary,
                    ui::ControlSize::Regular, card_w))
                open_export_preset_editor(i); // card = edit its params
            ImGui::SameLine();
            if (ui::Button(ui_str_.export_preset_delete.c_str(), ui::ButtonVariant::Danger,
                    ui::ControlSize::Regular, del_w)) {
                ui_intents_.export_preset_delete = true;
                ui_intents_.export_preset_edit_idx = i;
            }
            ImGui::SameLine();
            // "default" marker: radio-like -- exactly one preset is the queue default.
            // ui::Radio is the bool form; mirror the old RadioButton(int*) immediate write
            // so the marker moves this frame, then record the intent.
            if (ui::Radio(("##def_" + std::to_string(i)).c_str(),
                    export_default_preset_idx_ == i)) {
                export_default_preset_idx_ = i;
                ui_intents_.export_set_default = i;
            }
            if (ImGui::IsItemHovered() && !ui_str_.export_preset_default.empty())
                ImGui::SetTooltip("%s", ui_str_.export_preset_default.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ui::Button(ui_str_.export_preset_new.c_str(), ui::ButtonVariant::Secondary,
                ui::ControlSize::Regular, w))
            open_export_preset_editor(-2);
        ImGui::Spacing();
        if (ui::Button(ui_str_.cancel.c_str(), ui::ButtonVariant::Secondary,
                ui::ControlSize::Regular, w))
            export_presets_open_ = false;
    } else {
        // ---- editor: staged fields, seeded once per open ----
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

        ImGui::TextUnformatted(ui_str_.export_preset_name_label.c_str());
        ui::TextInput("##ep_name", export_preset_name_buf_, sizeof(export_preset_name_buf_));
        const char* codec_items[] = { "HEVC", "H.264" };
        ImGui::TextUnformatted(ui_str_.export_preset_codec_label.c_str());
        ui::Combo("##ep_codec", codec_items, 2, &export_preset_codec_idx_);

        // CQ / bitrate / maxrate are INDEPENDENT, each enabled by its own checkbox.
        // CQ uses ui::OptionalSlider (checkbox + greyed-while-disabled slider + value echo).
        // Bitrate/maxrate stay Checkbox + IntInput: they are unbounded numeric fields, and
        // squeezing them into OptionalSlider would clamp values to an invented slider range.
        ui::OptionalSlider(ui_str_.export_preset_cq_label.c_str(), &export_preset_cq_enabled_,
            "##ep_cq", &export_preset_cq_, 0, 51);
        ui::Checkbox(ui_str_.export_preset_bitrate_label.c_str(), &export_preset_bitrate_enabled_);
        if (export_preset_bitrate_enabled_) {
            ImGui::SameLine();
            ui::IntInput("##ep_bitrate", &export_preset_bitrate_, ui_s(90.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted("kbps");
        }
        ui::Checkbox(ui_str_.export_preset_maxrate_label.c_str(), &export_preset_maxrate_enabled_);
        if (export_preset_maxrate_enabled_) {
            ImGui::SameLine();
            ui::IntInput("##ep_maxrate", &export_preset_maxrate_, ui_s(90.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted("kbps");
        }

        const char* quality_items[] = { "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
        ImGui::TextUnformatted(ui_str_.export_preset_quality_label.c_str());
        ui::Combo("##ep_quality", quality_items, 7, &export_preset_quality_idx_);

        const char* audio_items[] = {
            ui_str_.export_preset_audio_copy.c_str(),
            ui_str_.export_preset_audio_encode.c_str(),
        };
        ImGui::TextUnformatted(ui_str_.export_preset_audio_label.c_str());
        int audio_mode = export_preset_audio_copy_ ? 0 : 1;
        if (ui::Combo("##ep_audio", audio_items, 2, &audio_mode, ui_s(160.0f)))
            export_preset_audio_copy_ = (audio_mode == 0);
        if (!export_preset_audio_copy_) {
            ImGui::SameLine();
            ui::IntInput("##ep_audio_bitrate", &export_preset_audio_bitrate_, ui_s(90.0f));
            ImGui::SameLine();
            ImGui::TextUnformatted("kbps");
        }

        ui::Checkbox(ui_str_.export_preset_subtitle_label.c_str(), &export_preset_subtitle_);
        ImGui::TextUnformatted(ui_str_.export_preset_suffix_label.c_str());
        ui::TextInput("##ep_suffix", export_preset_suffix_buf_, sizeof(export_preset_suffix_buf_));

        ImGui::Spacing();
        ImGui::Separator();
        if (ui::Button(ui_str_.export_preset_save.c_str(), ui::ButtonVariant::Primary,
                ui::ControlSize::Regular, w)) {
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
        }
        ImGui::SameLine();
        if (ui::Button(ui_str_.cancel.c_str(), ui::ButtonVariant::Secondary,
                ui::ControlSize::Regular, w))
            export_preset_edit_idx_ = -1;
    }

    ImGui::PopItemWidth();
    ImGui::End();
    ImGui::PopStyleVar();
}

void Player::open_export_preset_editor(int idx){
    export_preset_edit_idx_ = idx;
    export_preset_edit_init_ = false; // seed buffers on the next frame
}

