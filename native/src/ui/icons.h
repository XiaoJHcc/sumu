// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Design-system icon layer: lucide glyphs (ISC, assets/icons/lucide/) rasterized into one
// embedded RGBA strip (scripts/gen_icon_atlas.py -> assets/generated/icons_atlas.rgba ->
// icons_atlas.h). Rendering quality: the strip is a 4x (96px) straight-alpha source; at
// runtime each glyph is area-filter resampled on the CPU to the EXACT physical pixel size
// being drawn (once per size, cached as its own texture) and then drawn point-to-point on
// integer coordinates. No GPU mipmaps -- power-of-two mip levels essentially never match
// the actual draw size (at 150% the glyph would blend a 1.78x and a 0.89x level), whereas
// the CPU area resample integrates ~13 source pixels per output pixel and always hits 1:1.
// Best-effort: if the device is missing, draw() no-ops and IconButton degrades to a bare
// hit area.
#pragma once

// Forward-declared so this header stays free of d3d11.h (and windows.h's min/max macros,
// which otherwise leak into every widgets.cpp caller and break std::min/std::max).
struct ID3D11Device;

#include "imgui.h"

namespace ui {

// Atlas cell order -- MUST match ICONS in scripts/gen_icon_atlas.py.
enum class AppIcon {
    Settings = 0,  // settings-2
    OpenFile,      // folder-open
    OpenUrl,       // tv-minimal-play
    WebServer,     // cast
    Export,        // clapperboard
    WinMinimize,   // minus
    WinMaximize,   // square
    WinRestore,    // copy
    Fullscreen,    // maximize
    Close,         // x
    Play,          // play
    Pause,         // pause
    Volume,        // volume-2
    VolumeMute,    // volume-x
    Trash,         // trash (export preset/queue delete)
    FolderInput,   // folder-input (export path picker)
};

namespace icons {

// Source strip layout (scripts/gen_icon_atlas.py): kIconCount tightly packed square cells
// of kIconSrcPx (4x the 24px lucide grid, so the 2.5px stroke is a whole 10px -- the runtime
// area resample always starts from a clean, integer-stroked image).
inline constexpr int kIconSrcPx = 96;
inline constexpr int kIconCount = static_cast<int>(AppIcon::FolderInput) + 1;

// Records the D3D11 device; per-size glyph textures are built lazily on first draw. Call
// once after the device exists (Player::ui_init), pair with shutdown() before device
// teardown (Player::ui_shutdown).
void init(ID3D11Device* device);
void shutdown();
bool available();

// Tinted glyph draw into a rect (white source x `col`). Corners are snapped to whole
// physical pixels and the texture matches the snapped size 1:1. No-op when unavailable.
void draw(ImDrawList* dl, const ImVec2& p_min, const ImVec2& p_max, AppIcon icon, ImU32 col);

} // namespace icons
} // namespace ui
