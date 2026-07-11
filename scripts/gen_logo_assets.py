# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
"""
Single-source logo pipeline.

Source of truth (edit this one file when the logo changes):
  assets/sumu-logo-1024.png   (RGBA, 1024x1024)

Everything else is generated here and consumed by README / Win32 / ImGui / PyInstaller:

  assets/generated/sumu-logo-256.png   README + docs
  assets/generated/sumu-logo-128.png   mid size
  assets/generated/sumu-logo-64.png    small
  assets/generated/sumu.ico            Windows app / window icon (multi-size)
  assets/generated/sumu-logo-256.rgba  raw RGBA, embedded into the native pyd by CMake

Idempotent: skips rewrite when an output is already byte-identical to what would
be written (so a no-op regen doesn't dirty git or force a rebuild).

Usage:
  .venv/Scripts/python.exe scripts/gen_logo_assets.py
  .venv/Scripts/python.exe scripts/gen_logo_assets.py --check   # CI: fail if stale
"""
from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "assets" / "sumu-logo-1024.png"
OUT_DIR = ROOT / "assets" / "generated"

# (filename, size) -- PNG square variants for README / external use.
PNG_SIZES = (
    ("sumu-logo-256.png", 256),
    ("sumu-logo-128.png", 128),
    ("sumu-logo-64.png", 64),
)

# Multi-size ICO for the Windows executable + window class icon.
# 16/32/48 for shell chrome; 256 for Explorer large-icon view.
ICO_SIZES = (16, 32, 48, 256)
ICO_NAME = "sumu.ico"

# Raw RGBA dump consumed by native/cmake/embed_binary.cmake (no PNG decoder in the pyd).
LOGO_RGBA_SIZE = 256
LOGO_RGBA_NAME = "sumu-logo-256.rgba"


def _resize(src: Image.Image, size: int) -> Image.Image:
    return src.resize((size, size), Image.Resampling.LANCZOS)


def _png_bytes(im: Image.Image) -> bytes:
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _ico_bytes(src: Image.Image) -> bytes:
    # Pillow builds a multi-size ICO by DOWN-scaling a single base image to every
    # entry in sizes=. The base must be >= the largest requested size (otherwise
    # Pillow silently emits only the base size -- observed: a 16x16 base + sizes=
    # [(16,16),(32,32),...] produced a 1-entry 16x16 ICO).
    base = _resize(src, max(ICO_SIZES))
    buf = io.BytesIO()
    base.save(buf, format="ICO", sizes=[(s, s) for s in ICO_SIZES])
    return buf.getvalue()


def _rgba_bytes(src: Image.Image) -> bytes:
    return _resize(src, LOGO_RGBA_SIZE).tobytes("raw", "RGBA")


def _write_if_changed(path: Path, data: bytes) -> str:
    if path.is_file() and path.read_bytes() == data:
        return "unchanged"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return "wrote"


def generate() -> list[tuple[Path, str]]:
    if not SRC.is_file():
        raise SystemExit(f"source logo missing: {SRC}")
    src = Image.open(SRC).convert("RGBA")
    if src.size != (1024, 1024):
        print(f"warning: source is {src.size}, expected 1024x1024", file=sys.stderr)

    results: list[tuple[Path, str]] = []
    for name, size in PNG_SIZES:
        path = OUT_DIR / name
        status = _write_if_changed(path, _png_bytes(_resize(src, size)))
        results.append((path, status))

    ico_path = OUT_DIR / ICO_NAME
    status = _write_if_changed(ico_path, _ico_bytes(src))
    results.append((ico_path, status))

    rgba_path = OUT_DIR / LOGO_RGBA_NAME
    status = _write_if_changed(rgba_path, _rgba_bytes(src))
    results.append((rgba_path, status))
    return results


def check_fresh() -> int:
    """Return 0 if generated outputs match what generate() would write; else 1."""
    if not SRC.is_file():
        print(f"FAIL: source logo missing: {SRC}", file=sys.stderr)
        return 1
    src = Image.open(SRC).convert("RGBA")
    stale: list[str] = []
    for name, size in PNG_SIZES:
        path = OUT_DIR / name
        expected = _png_bytes(_resize(src, size))
        if not path.is_file() or path.read_bytes() != expected:
            stale.append(str(path.relative_to(ROOT)))
    ico_path = OUT_DIR / ICO_NAME
    if not ico_path.is_file() or ico_path.read_bytes() != _ico_bytes(src):
        stale.append(str(ico_path.relative_to(ROOT)))
    rgba_path = OUT_DIR / LOGO_RGBA_NAME
    if not rgba_path.is_file() or rgba_path.read_bytes() != _rgba_bytes(src):
        stale.append(str(rgba_path.relative_to(ROOT)))
    if stale:
        print("FAIL: logo assets stale (run scripts/gen_logo_assets.py):", file=sys.stderr)
        for s in stale:
            print(f"  {s}", file=sys.stderr)
        return 1
    print("OK: logo assets up to date")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if generated assets are missing or stale (no writes)",
    )
    args = ap.parse_args()
    if args.check:
        return check_fresh()
    for path, status in generate():
        print(f"  [{status}] {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
