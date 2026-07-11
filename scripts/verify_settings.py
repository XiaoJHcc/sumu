# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Phase 6 M-E verification: python/sumu/settings.py round-trip / corruption / push_recent /
# resume-gate / atomic-write checks. Stdlib + sumu.settings only -- no GPU/torch/Player needed,
# fast and headless. Run with: .venv/Scripts/python.exe scripts/verify_settings.py
import os
import json
import shutil
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))

from sumu import settings as settings_mod  # noqa: E402

failures = []


def check(name, condition):
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}")
    if not condition:
        failures.append(name)


def main():
    tmpdir = tempfile.mkdtemp(prefix="sumu_settings_test_")
    try:
        settings_path = os.path.join(tmpdir, "settings.json")

        # 1. Round-trip
        s = settings_mod.Settings(
            volume=0.5, muted=True, recent=["a", "b", "c"], positions={"x": 1234},
            cold_start_s=1.5, target_fps=30,
        )
        settings_mod.save(s, settings_path)
        loaded = settings_mod.load(settings_path)
        check("round-trip volume", loaded.volume == 0.5)
        check("round-trip muted", loaded.muted is True)
        check("round-trip recent", loaded.recent == ["a", "b", "c"])
        check("round-trip positions", loaded.positions == {"x": 1234})
        check("round-trip cold_start_s", loaded.cold_start_s == 1.5)
        check("round-trip target_fps", loaded.target_fps == 30)

        # 2. Corrupt/missing
        with open(settings_path, "wb") as f:
            f.write(b"\x00\x01not json{{{")
        corrupt_loaded = settings_mod.load(settings_path)
        check("corrupt file -> defaults (volume)", corrupt_loaded.volume == 1.0)
        check("corrupt file -> defaults (muted)", corrupt_loaded.muted is False)
        check("corrupt file -> defaults (recent)", corrupt_loaded.recent == [])
        check("corrupt file -> defaults (positions)", corrupt_loaded.positions == {})
        check("corrupt file -> defaults (cold_start_s)", corrupt_loaded.cold_start_s == 1.0)
        check("corrupt file -> defaults (target_fps)", corrupt_loaded.target_fps == 0)

        os.remove(settings_path)
        missing_loaded = settings_mod.load(settings_path)
        check("missing file -> defaults", missing_loaded == settings_mod.Settings())

        # 2b. cold_start_s clamp
        s_hi = settings_mod.Settings(cold_start_s=99.0)
        settings_mod.save(s_hi, settings_path)
        loaded_hi = settings_mod.load(settings_path)
        check("cold_start_s clamp high -> 3.0", loaded_hi.cold_start_s == 3.0)
        s_lo = settings_mod.Settings(cold_start_s=-1.0)
        settings_mod.save(s_lo, settings_path)
        loaded_lo = settings_mod.load(settings_path)
        check("cold_start_s clamp low -> 0.0", loaded_lo.cold_start_s == 0.0)

        # 2c. target_fps clamp + legacy fps_div migrate
        s_bad = settings_mod.Settings(target_fps=99)
        settings_mod.save(s_bad, settings_path)
        check("target_fps clamp junk -> 0", settings_mod.load(settings_path).target_fps == 0)
        with open(settings_path, "w", encoding="utf-8") as f:
            json.dump({"fps_div": 2}, f)
        check("legacy fps_div=2 -> target_fps 30", settings_mod.load(settings_path).target_fps == 30)
        with open(settings_path, "w", encoding="utf-8") as f:
            json.dump({"fps_div": 1}, f)
        check("legacy fps_div=1 -> target_fps 0", settings_mod.load(settings_path).target_fps == 0)

        # 2d. fps_div_for_target (nearest 1/N, never upsample)
        check("div: original -> 1", settings_mod.fps_div_for_target(60.0, 0) == 1)
        check("div: 60->30 uses 2", settings_mod.fps_div_for_target(60.0, 30) == 2)
        check("div: 50->30 uses 2 (25 closer than 50)", settings_mod.fps_div_for_target(50.0, 30) == 2)
        check("div: 30->30 keeps 1", settings_mod.fps_div_for_target(30.0, 30) == 1)
        check("div: 24->30 keeps 1", settings_mod.fps_div_for_target(24.0, 30) == 1)
        check("div: 120->60 uses 2", settings_mod.fps_div_for_target(120.0, 60) == 2)
        check("div: 120->30 uses 4", settings_mod.fps_div_for_target(120.0, 30) == 4)

        # 3. push_recent semantics. push_recent stores os.path.abspath(path) (case-preserved,
        # "a real usable absolute path" per the spec) and dedups/orders via the case-insensitive
        # normcase key -- so the expected list below runs the same tokens through abspath() to
        # match what push_recent actually stores, while still exercising move-to-front + dedup.
        s2 = settings_mod.Settings()
        for tok in ["A", "B", "C", "A"]:
            s2.push_recent(tok)
        expected_order = [os.path.abspath(tok) for tok in ["A", "C", "B"]]
        check("push_recent move-to-front + dedup", s2.recent == expected_order)

        s3 = settings_mod.Settings()
        for i in range(12):
            s3.push_recent(f"file{i}")
        check("push_recent cap at 10", len(s3.recent) == 10)
        expected_capped = [os.path.abspath(f"file{i}") for i in range(11, 1, -1)]
        check("push_recent keeps newest, drops oldest", s3.recent == expected_capped)

        # 4. Resume gate logic (pure predicate, settings_mod.is_resumable_frame)
        fps = 30.0
        frame_count = 3000  # 100s clip
        check("resume: just after start -> no resume",
              settings_mod.is_resumable_frame(10, fps, frame_count) is False)
        check("resume: near end -> no resume",
              settings_mod.is_resumable_frame(frame_count - 10, fps, frame_count) is False)
        check("resume: middle -> resumes",
              settings_mod.is_resumable_frame(1500, fps, frame_count) is True)
        check("resume: unknown fps -> no resume",
              settings_mod.is_resumable_frame(1500, 0, frame_count) is False)
        check("resume: unknown frame_count -> no resume",
              settings_mod.is_resumable_frame(1500, fps, 0) is False)
        check("resume: no stored position -> no resume",
              settings_mod.is_resumable_frame(None, fps, frame_count) is False)

        # 5. Atomic write: no leftover temp file, target parses as valid JSON.
        s4 = settings_mod.Settings(volume=0.7)
        settings_mod.save(s4, settings_path)
        leftover_tmp = [f for f in os.listdir(tmpdir) if f != "settings.json"]
        check("atomic write: no leftover temp files", leftover_tmp == [])
        with open(settings_path, encoding="utf-8") as f:
            data = json.load(f)
        check("atomic write: target parses as valid JSON", isinstance(data, dict))
        check("save writes target_fps not fps_div", "target_fps" in data and "fps_div" not in data)

    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if failures:
        print(f"RESULT: FAIL ({len(failures)} failing): {failures}")
        sys.exit(1)
    else:
        print("RESULT: PASS (all checks passed)")
        sys.exit(0)


if __name__ == "__main__":
    main()
