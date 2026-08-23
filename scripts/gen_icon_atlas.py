# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
"""
Icon atlas pipeline (lucide -> embedded RGBA strip).

Source of truth (edit these when an icon changes):
  assets/icons/lucide/*.svg    (lucide, ISC license -- see assets/icons/lucide/README.md)

Output (consumed by native/CMakeLists.txt -> cmake/embed_binary.cmake -> icons_atlas.h):
  assets/generated/icons_atlas.rgba   raw RGBA, one horizontal strip, N cells of
                                      ICON_PX x ICON_PX, tightly packed (no gutter)

Quality notes: the strip is a high-res (4x the 24px lucide grid, so the 2px stroke is a
whole 8px) straight-alpha source. The native side (ui/icons.cpp) does NOT mip it -- power
of two mips essentially never match the actual draw size. Instead it area-filter resamples
each glyph on the CPU to the exact physical pixel size at the current DPI (once per size,
cached) and draws point-to-point.

The cell order below MUST match `enum class AppIcon` in native/src/ui/icons.h.

Usage:
  .venv/Scripts/python.exe scripts/gen_icon_atlas.py
"""
from __future__ import annotations

from pathlib import Path

import pymupdf

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "assets" / "icons" / "lucide"
OUT = ROOT / "assets" / "generated" / "icons_atlas.rgba"

# (AppIcon enum order) -> lucide svg file
ICONS = [
    "settings-2",      # Settings
    "folder-open",     # OpenFile
    "link",            # OpenUrl
    "hard-drive-upload", # WebServer
    "clapperboard",    # Export
    "minus",           # WinMinimize
    "square",          # WinMaximize
    "copy",            # WinRestore
    "maximize",        # Fullscreen
    "x",               # Close
    "play",            # Play
    "pause",           # Pause
    "volume-2",        # Volume
    "volume-x",        # VolumeMute
    "trash",           # Trash (export preset/queue delete)
    "folder-input",    # FolderInput (export path picker)
    "folder",          # Folder (web-server root picker)
    "plus",            # Plus (icon-text button standard: 新建预设/添加文件)
]

ICON_PX = 96    # raster size of one glyph (lucide grid is 24 -> 4x, stroke = whole 10px)
LUCIDE_GRID = 24
STROKE_WIDTH = 2.5  # lucide default is 2; 2.5 reads better at our 14-28px draw sizes
                    # (at 4x zoom this is a whole 10px, so the source stays crisp)


def render_icon(svg_path: Path) -> bytes:
    """Rasterize one lucide SVG to ICON_PX x ICON_PX RGBA (straight alpha, white strokes)."""
    svg = svg_path.read_text(encoding="utf-8")
    # lucide strokes use currentColor; bake white so the native side can tint via ImGui.
    # Also bake the design-system stroke width (upstream files stay untouched).
    svg = svg.replace('stroke="currentColor"', 'stroke="#ffffff"')
    svg = svg.replace('stroke-width="2"', f'stroke-width="{STROKE_WIDTH}"')
    doc = pymupdf.open(stream=svg.encode("utf-8"), filetype="svg")
    try:
        page = doc[0]
        zoom = ICON_PX / LUCIDE_GRID
        pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), alpha=True)
        if pix.width != ICON_PX or pix.height != ICON_PX:
            raise RuntimeError(f"{svg_path.name}: got {pix.width}x{pix.height}, want {ICON_PX}")
        return pix.samples
    finally:
        doc.close()


def main() -> None:
    strip_w = ICON_PX * len(ICONS)
    strip = bytearray(strip_w * ICON_PX * 4)
    for i, name in enumerate(ICONS):
        src = SRC_DIR / f"{name}.svg"
        if not src.exists():
            raise SystemExit(f"missing icon source: {src}")
        rgba = render_icon(src)
        for row in range(ICON_PX):
            dst = (row * strip_w + i * ICON_PX) * 4
            strip[dst:dst + ICON_PX * 4] = rgba[row * ICON_PX * 4:(row + 1) * ICON_PX * 4]

    data = bytes(strip)
    if OUT.exists() and OUT.read_bytes() == data:
        print(f"icons_atlas.rgba unchanged ({strip_w}x{ICON_PX})")
        return
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({strip_w}x{ICON_PX}, {len(ICONS)} icons, {len(data)} bytes)")


if __name__ == "__main__":
    main()
