# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Lightweight UI i18n for the daily player. Message catalogs in locales/<code>.json
# are the single source of truth for copy (stdlib json only -- same isolation spirit
# as settings.py). Python owns translation; native ImGui labels are pushed once via
# Player.set_ui_strings(native_strings()). No embedded full-text fallbacks -- a
# missing/corrupt catalog is a packaging/dev error and surfaces as the bare key.
from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from typing import Iterable, Optional

# Source catalog language -- also fills any missing key in other locales.
DEFAULT_LANG = "zh-CN"
SUPPORTED_LANGS = ("zh-CN", "en", "ja")
LANGUAGE_AUTO = "auto"
LANGUAGE_CHOICES = (LANGUAGE_AUTO,) + SUPPORTED_LANGS

# Canonical key list (values live only in locales/*.json). Keep NATIVE_KEYS a
# subset of REQUIRED_KEYS; player.cpp set_ui_strings() field names must match.
REQUIRED_KEYS: tuple[str, ...] = (
    "splash_loading",
    "open_prompt",
    "open_file",
    "open_url",
    "open_url_title",
    "open_url_hint",
    "open_url_ok",
    "open_url_cancel",
    "open_url_invalid",
    "compile_retry",
    "compile_engine",
    "settings_title",
    "lead_label",
    "lead_tooltip",
    "clip_length_label",
    "clip_length_tooltip",
    "max_regions_label",
    "max_regions_tooltip",
    "cold_start_label",
    "cold_start_tooltip",
    "target_fps_label",
    "target_fps_original",
    "target_fps_tooltip",
    "diagnostics_title",
    "ai_speed",
    "ai_speed_unknown",
    "dialog_video_files",
    "dialog_all_files",
    "open_failed",
    "open_failed_named",
    "warmup_failed",
    "warmup_status",
    "compile_running",
    "compile_failed",
    "compile_preparing",
    "compile_prompt",
)

NATIVE_KEYS: tuple[str, ...] = (
    "splash_loading",
    "open_prompt",
    "open_file",
    "open_url",
    "open_url_title",
    "open_url_hint",
    "open_url_ok",
    "open_url_cancel",
    "open_url_invalid",
    "compile_retry",
    "compile_engine",
    "settings_title",
    "lead_label",
    "lead_tooltip",
    "clip_length_label",
    "clip_length_tooltip",
    "max_regions_label",
    "max_regions_tooltip",
    "cold_start_label",
    "cold_start_tooltip",
    "target_fps_label",
    "target_fps_original",
    "target_fps_tooltip",
    "diagnostics_title",
    "ai_speed",
    "ai_speed_unknown",
    "dialog_video_files",
    "dialog_all_files",
)

_catalog: dict[str, str] = {}
_active_lang: str = DEFAULT_LANG
_preference: str = LANGUAGE_AUTO
_json_cache: dict[str, dict[str, str]] = {}


def locales_dir() -> Path:
    """Resolve the on-disk locales directory (dev tree or frozen bundle)."""
    candidates: list[Path] = [Path(__file__).resolve().parent / "locales"]
    if getattr(sys, "frozen", False):
        exe_dir = Path(sys.executable).resolve().parent
        candidates.append(exe_dir / "sumu" / "locales")
        candidates.append(exe_dir / "locales")
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            candidates.append(Path(meipass) / "sumu" / "locales")
            candidates.append(Path(meipass) / "locales")
    for c in candidates:
        if c.is_dir():
            return c
    return candidates[0]


def detect_system_lang() -> str:
    """Map the OS UI language to a supported catalog. Unknown → en."""
    name = ""
    if sys.platform == "win32":
        try:
            import ctypes
            buf = ctypes.create_unicode_buffer(85)
            n = ctypes.windll.kernel32.GetUserDefaultLocaleName(buf, 85)
            if n > 0:
                name = buf.value or ""
        except Exception:  # noqa: BLE001 -- detection must never crash startup
            name = ""
    if not name:
        for env in ("LC_ALL", "LC_MESSAGES", "LANG"):
            v = os.environ.get(env) or ""
            if v and v != "C" and not v.startswith("C."):
                name = v
                break
    norm = name.replace("_", "-").split(".")[0].strip()
    low = norm.lower()
    if low.startswith("zh"):
        return "zh-CN"
    if low.startswith("ja"):
        return "ja"
    if low.startswith("en"):
        return "en"
    for code in SUPPORTED_LANGS:
        if low == code.lower():
            return code
    return "en"


def clamp_language(value) -> str:
    """'auto' | supported code. Unknown / non-str → 'auto'."""
    if not isinstance(value, str):
        return LANGUAGE_AUTO
    v = value.strip()
    if v == LANGUAGE_AUTO:
        return LANGUAGE_AUTO
    for code in SUPPORTED_LANGS:
        if v.lower() == code.lower():
            return code
    return LANGUAGE_AUTO


def resolve_lang(preference: Optional[str] = None) -> str:
    pref = clamp_language(preference if preference is not None else _preference)
    if pref == LANGUAGE_AUTO:
        return detect_system_lang()
    return pref


def _load_json_catalog(lang: str, *, use_cache: bool = True) -> dict[str, str]:
    if use_cache and lang in _json_cache:
        return dict(_json_cache[lang])
    path = locales_dir() / f"{lang}.json"
    out: dict[str, str] = {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001 -- missing/corrupt → empty + stderr
        print(f"[sumu.i18n] failed to load {path}: {e!r}", file=sys.stderr)
        _json_cache[lang] = out
        return out
    if not isinstance(data, dict):
        print(f"[sumu.i18n] {path} is not a JSON object", file=sys.stderr)
        _json_cache[lang] = out
        return out
    for k, v in data.items():
        if isinstance(k, str) and isinstance(v, str):
            out[k] = v
    _json_cache[lang] = dict(out)
    return out


def _merged_catalog(lang: str) -> dict[str, str]:
    """Active locale on top of DEFAULT_LANG so partial translations still work."""
    base = _load_json_catalog(DEFAULT_LANG)
    if lang != DEFAULT_LANG:
        base.update(_load_json_catalog(lang))
    return base


def set_language(preference: str = LANGUAGE_AUTO) -> str:
    """Install preference ('auto'|code), load catalog, return resolved active code."""
    global _catalog, _active_lang, _preference
    # Drop cache so a hand-edited JSON is picked up without restarting the process
    # mid-session (dev); production only calls this at startup.
    _json_cache.clear()
    _preference = clamp_language(preference)
    _active_lang = resolve_lang(_preference)
    _catalog = _merged_catalog(_active_lang)
    return _active_lang


def active_lang() -> str:
    return _active_lang


def language_preference() -> str:
    return _preference


def t(key: str, **kwargs) -> str:
    """Translate key; optional str.format_map kwargs (e.g. name=, step=).

    Missing key → bare key name (visible, never crash). JSON is the only copy source.
    """
    s = _catalog.get(key, key)
    if kwargs:
        try:
            return s.format_map(kwargs)
        except (KeyError, ValueError):
            return s
    return s


def native_strings() -> dict[str, str]:
    """Subset of the catalog for Player.set_ui_strings()."""
    return {k: t(k) for k in NATIVE_KEYS}


def required_keys() -> frozenset[str]:
    return frozenset(REQUIRED_KEYS)


def catalog_keys(lang: str) -> set[str]:
    return set(_load_json_catalog(lang, use_cache=False).keys())


def missing_keys(lang: str, keys: Optional[Iterable[str]] = None) -> list[str]:
    want = set(keys) if keys is not None else set(REQUIRED_KEYS)
    have = catalog_keys(lang)
    return sorted(want - have)


def apply_to_player(player, preference: Optional[str] = None) -> str:
    """set_language + push native string table. Returns resolved lang."""
    lang = set_language(preference if preference is not None else _preference)
    setter = getattr(player, "set_ui_strings", None)
    if callable(setter):
        setter(native_strings())
    return lang


# Load default catalog at import so t() works before an explicit set_language().
set_language(LANGUAGE_AUTO)
