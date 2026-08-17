// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#include "ui_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h" // ImGui::CalcTextSize (elide_text_to_width)

namespace sumu_ui {

int64_t frame_for_seekbar_x(float mx, float x0, float x1, int64_t frame_count)
{
    if (frame_count <= 0) return 0;
    if (x1 <= x0) return 0;
    float t = (mx - x0) / (x1 - x0);
    t = std::clamp(t, 0.0f, 1.0f);
    int64_t last = frame_count - 1;
    int64_t f = static_cast<int64_t>(std::llround(static_cast<double>(t) * static_cast<double>(last)));
    return std::clamp<int64_t>(f, 0, last);
}

float seekbar_x_for_frame(int64_t frame, float x0, float x1, int64_t frame_count)
{
    if (frame_count <= 1) return x0; // avoids a div-by-zero on `last` below when there's <=1 frame
    int64_t last = frame_count - 1;
    frame = std::clamp<int64_t>(frame, 0, last);
    float t = static_cast<float>(frame) / static_cast<float>(last);
    return x0 + t * (x1 - x0);
}

std::string basename_of(const std::string& path)
{
    // Strip query/fragment so "http://host/a/b.mp4?token=x" shows as "b.mp4", not the token.
    std::string p = path;
    size_t cut = p.find_first_of("?#");
    if (cut != std::string::npos)
        p = p.substr(0, cut);
    size_t pos = p.find_last_of("/\\");
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

std::string elide_text_to_width(const std::string& text, float max_w)
{
    if (text.empty() || max_w <= 0.0f)
        return {};
    if (ImGui::CalcTextSize(text.c_str()).x <= max_w)
        return text;
    const char* ellipsis = "...";
    const float ell_w = ImGui::CalcTextSize(ellipsis).x;
    if (ell_w >= max_w)
        return std::string(ellipsis);
    const float budget = max_w - ell_w;
    const char* s = text.c_str();
    const char* end = s + text.size();
    const char* cut = s;
    while (cut < end) {
        const unsigned char c = static_cast<unsigned char>(*cut);
        int nbytes = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
            : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (cut + nbytes > end)
            nbytes = static_cast<int>(end - cut);
        const char* next = cut + nbytes;
        if (ImGui::CalcTextSize(s, next).x > budget)
            break;
        cut = next;
    }
    return std::string(s, cut) + ellipsis;
}

std::string format_mmss(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    int64_t total = static_cast<int64_t>(seconds + 0.5);
    int64_t h = total / 3600;
    int64_t m = (total % 3600) / 60;
    int64_t s = total % 60;
    char buf[32];
    if (h > 0)
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld",
            static_cast<long long>(h), static_cast<long long>(m), static_cast<long long>(s));
    else
        snprintf(buf, sizeof(buf), "%lld:%02lld",
            static_cast<long long>(m), static_cast<long long>(s));
    return std::string(buf);
}

} // namespace sumu_ui
