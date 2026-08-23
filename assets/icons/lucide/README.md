# Icon sources: lucide

The SVGs in this directory are from [lucide](https://lucide.dev)
(<https://github.com/lucide-icons/lucide>), licensed under the ISC License
(see `LICENSES/ISC.txt`).

SPDX-License-Identifier: ISC

They are the source of truth for the app's design-system icons. Do not edit them
in place; bump them from upstream instead. `scripts/gen_icon_atlas.py` rasterizes
them into `assets/generated/icons_atlas.rgba`, which CMake embeds into the native
pyd (`native/src/ui/icons.cpp` draws the glyphs tinted via ImGui).

Current set (order matches `enum class AppIcon` in `native/src/ui/icons.h`):
settings-2, folder-open, link, hard-drive-upload, clapperboard, minus, square, copy,
maximize, x, play, pause, volume-2, volume-x, trash, folder-input, folder, plus.

Note: `scripts/gen_icon_atlas.py` bakes a stroke-width of 2.5 (lucide default is 2) at
raster time; the SVGs here keep their upstream stroke-width="2".
