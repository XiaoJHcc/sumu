#!/usr/bin/env bash
# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Re-apply the third-party runtime patches to sumu's .venv site-packages.
# Ported from lada-realtime's dev-install step (docs/windows_install.md §5).
# These patch pinned deps (ultralytics==8.4.4, mmengine==0.10.7) and MUST be
# re-run after any `uv sync` that reinstalls those packages, otherwise:
#   - mmengine can't load the BasicVSR++ checkpoint on torch>=2.6
#     (torch.load defaults to weights_only=True -> load fails)  [REQUIRED]
#   - ultralytics phones home telemetry / sentry                 [privacy]
#   - YOLO NMS truncates on large frames (max_time_img 0.05->0.3)[quality]
#
# The 4th staged patch (remove_use_of_torch_dist_in_mmengine.patch) is a Windows
# torch.distributed shim that is NOT applied: mmengine 0.10.7 imports cleanly on
# this stack (torch 2.8.0+cu128, py3.13) without it. Apply it only if a future
# mmengine/torch bump reintroduces the ReduceOp/fsdp import error.
set -euo pipefail
cd "$(dirname "$0")/.."
SP=".venv/Lib/site-packages"
uv pip install patch >/dev/null
for p in increase_mms_time_limit.patch remove_ultralytics_telemetry.patch fix_loading_mmengine_weights_on_torch26_and_higher.diff; do
  echo "applying $p"
  uv run --no-project python -m patch -p1 -d "$SP" "patches/$p" || echo "  (already applied or failed — check target file)"
done
uv pip uninstall patch >/dev/null
echo "done. verify: grep weights_only=False $SP/mmengine/runner/checkpoint.py"
