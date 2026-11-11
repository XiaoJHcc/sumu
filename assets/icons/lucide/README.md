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
maximize, x, play-filled, pause-filled, volume-2, volume-x, trash, folder-input, folder, plus,
arrow-left, arrow-right, rotate-cw, star, star-filled.

`play-filled.svg`, `pause-filled.svg` and `star-filled.svg` are the corresponding upstream
shapes with `fill="currentColor"` on their paths. Play and pause stay solid in both the
address bar and the bottom playback bar; the address bar uses the outline star in `kWarning`
when the page is favorited.

Note: `scripts/gen_icon_atlas.py` bakes a stroke-width of 2.5 (lucide default is 2) at
raster time; the SVGs here keep their upstream stroke-width="2".
