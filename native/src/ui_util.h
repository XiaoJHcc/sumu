// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#pragma once

#include <cstdint>
#include <string>

namespace sumu_ui {

// ---- seekbar x<->frame mapping (M3) -------------------------------------------------------
// Pure, no ImGui/Player/D3D dependency -- unit-testable in isolation. Placed here (in the
// anonymous namespace, above class Player) is sufficient for Player's own member functions to
// call them: C++ defers compiling member-function BODIES until after the class's closing
// brace has been seen ("complete-class context"), so any free function visible earlier in the
// translation unit -- including here -- is visible to every Player method body, regardless of
// where in the class the method itself is declared.
//
// Boundary behavior: frame_count<=0 (not-yet-opened/empty video) always maps to/from frame 0.
// x is clamped to [x0,x1] before mapping, so a drag past either edge clamps to frame 0 /
// frame_count-1, never out of bounds. float->frame uses round (llround), not truncation, so a
// pixel exactly between two frame boundaries maps to the CLOSER frame rather than always the
// earlier one.
int64_t frame_for_seekbar_x(float mx, float x0, float x1, int64_t frame_count);
float seekbar_x_for_frame(int64_t frame, float x0, float x1, int64_t frame_count);
std::string basename_of(const std::string& path);
std::string percent_decode(const std::string& s);
// Title-bar filename elide: keep a UTF-8-safe prefix that fits max_w, append "..." when clipped.
// Uses current ImGui font metrics (must be called during an active ImGui frame).
std::string elide_text_to_width(const std::string& text, float max_w);
std::string format_mmss(double seconds);

} // namespace sumu_ui
