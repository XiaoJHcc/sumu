# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Proves both branches of sumu.ai._default_model_weights_dir() without importing torch:
#   - frozen  : sys.frozen=True + sys.executable set -> <exe dir>/model_weights
#   - dev     : sys.frozen absent -> sibling lada-realtime/model_weights (if it exists)
#               else the CWD-relative "model_weights" fallback
# MODEL_WEIGHTS_DIR is cached at import time, so we call the resolver function directly
# under each controlled sys/env state rather than re-importing the module.
import importlib
import os
import sys
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_REPO, "python"))

ai = importlib.import_module("sumu.ai")
resolve = ai._default_model_weights_dir

failures = []


def _clear_env():
    for var in ("SUMU_MODEL_WEIGHTS_DIR", "LADA_MODEL_WEIGHTS_DIR"):
        os.environ.pop(var, None)


# ── frozen case ──────────────────────────────────────────────────────────────
_saved_frozen = getattr(sys, "frozen", None)
_saved_exe = sys.executable
try:
    _clear_env()
    with tempfile.TemporaryDirectory() as tmp:
        fake_exe = os.path.join(tmp, "sumu.exe")
        sys.frozen = True
        sys.executable = fake_exe
        got = resolve()
        want = os.path.join(tmp, "model_weights")
        if got == want:
            print(f"PASS frozen: {got}")
        else:
            print(f"FAIL frozen: got={got!r} want={want!r}")
            failures.append("frozen")
finally:
    if _saved_frozen is None:
        if hasattr(sys, "frozen"):
            del sys.frozen
    else:
        sys.frozen = _saved_frozen
    sys.executable = _saved_exe

# ── dev case ─────────────────────────────────────────────────────────────────
_clear_env()
if hasattr(sys, "frozen"):
    del sys.frozen
# Recompute the expected dev path the same way the resolver does, from the module file.
_here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(ai.__file__)))))
_sibling = os.path.join(os.path.dirname(_here), "lada-realtime", "model_weights")
want_dev = _sibling if os.path.isdir(_sibling) else "model_weights"
got_dev = resolve()
if got_dev == want_dev:
    print(f"PASS dev: {got_dev}")
else:
    print(f"FAIL dev: got={got_dev!r} want={want_dev!r}")
    failures.append("dev")

print(f"EXIT:{1 if failures else 0}")
sys.exit(1 if failures else 0)
