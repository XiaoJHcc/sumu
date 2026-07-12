# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Headless i18n checks: catalog key parity, t()/format, language clamp, system-auto
# resolve, native_strings subset. No GPU/Player required.
# Run: .venv/Scripts/python.exe scripts/verify_i18n.py
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))

from sumu import i18n as i18n_mod  # noqa: E402
from sumu import settings as settings_mod  # noqa: E402

failures = []


def check(name, condition):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}")
    if not condition:
        failures.append(name)


def main():
    required = i18n_mod.required_keys()
    check("required keys non-empty", len(required) >= 20)

    for lang in i18n_mod.SUPPORTED_LANGS:
        missing = i18n_mod.missing_keys(lang)
        check(f"{lang} catalog complete (missing={missing})", not missing)

    # Active catalog after explicit set
    i18n_mod.set_language("en")
    check("active en", i18n_mod.active_lang() == "en")
    check("en open_file", i18n_mod.t("open_file") == "Open file")
    check(
        "en open_failed_named format",
        i18n_mod.t("open_failed_named", name="a.mp4") == "Could not open: a.mp4",
    )
    check(
        "en compile_running format",
        "2/6" in i18n_mod.t("compile_running", step=2, total=6),
    )

    i18n_mod.set_language("zh-CN")
    check("active zh-CN", i18n_mod.active_lang() == "zh-CN")
    check("zh open_file", i18n_mod.t("open_file") == "打开文件")
    check(
        "zh open_failed_named format",
        i18n_mod.t("open_failed_named", name="a.mp4") == "无法打开：a.mp4",
    )

    i18n_mod.set_language("ja")
    check("active ja", i18n_mod.active_lang() == "ja")
    check("ja open_file", i18n_mod.t("open_file") == "ファイルを開く")
    check(
        "ja open_failed_named format",
        i18n_mod.t("open_failed_named", name="a.mp4") == "開けませんでした：a.mp4",
    )
    check(
        "ja compile_running format",
        "2/6" in i18n_mod.t("compile_running", step=2, total=6),
    )

    # auto preference resolves to a supported code
    resolved = i18n_mod.set_language("auto")
    check("auto resolves supported", resolved in i18n_mod.SUPPORTED_LANGS)
    check("preference stays auto", i18n_mod.language_preference() == "auto")
    print(f"  (system-detected lang = {resolved!r})")

    # native subset covers every NATIVE_KEYS entry and is non-empty
    i18n_mod.set_language("en")
    ns = i18n_mod.native_strings()
    check("native_strings size", len(ns) == len(i18n_mod.NATIVE_KEYS))
    check("native_strings all present", all(k in ns and ns[k] for k in i18n_mod.NATIVE_KEYS))
    check("native open_file en", ns["open_file"] == "Open file")

    # settings clamp agrees with i18n clamp table
    check("settings clamp fr->auto", settings_mod.clamp_language("fr") == "auto")
    check("settings clamp EN->en", settings_mod.clamp_language("EN") == "en")
    check("settings clamp JA->ja", settings_mod.clamp_language("JA") == "ja")
    check("i18n clamp fr->auto", i18n_mod.clamp_language("fr") == "auto")
    check("i18n clamp ja", i18n_mod.clamp_language("ja") == "ja")

    # missing key falls back to key name (not crash); no embedded full-text catalog
    check("unknown key returns key", i18n_mod.t("__no_such_key__") == "__no_such_key__")
    check("no embedded zh fallback table", not hasattr(i18n_mod, "_FALLBACK_ZH_CN"))
    check("no embedded en fallback table", not hasattr(i18n_mod, "_FALLBACK_EN"))

    # locales dir exists in dev tree; JSON is the only copy source
    check("locales_dir exists", i18n_mod.locales_dir().is_dir())
    check("NATIVE_KEYS subset of REQUIRED_KEYS",
          set(i18n_mod.NATIVE_KEYS).issubset(i18n_mod.required_keys()))

    if failures:
        print(f"\n{len(failures)} failure(s): {failures}")
        return 1
    print("\nall i18n checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
