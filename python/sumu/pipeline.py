# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# AI model build pipeline, factored out of scripts/run_player.py so both the verification
# scaffolding (run_player.py) and the daily-use player entry point (sumu.app) share one
# definition instead of the latter reaching across into a script module.


def default_restoration_model_path():
    """Absolute path to the default BasicVSR++ restoration weights. Shared by build_models() and
    the daily player's on-demand TRT compile (app.py needs the path to compile engines for it)."""
    from sumu.ai import ModelFiles
    return ModelFiles.get_restoration_model_by_name("basicvsrpp-v1.2").path


def build_models(device, fp16, allow_trt_compile=True):
    from sumu.ai import ModelFiles
    from sumu.ai.restorationpipeline import load_models

    rp = default_restoration_model_path()
    dp = ModelFiles.get_detection_model_by_name("v4-fast").path
    return load_models(
        device, "basicvsrpp-v1.2", rp, None, dp,
        fp16=fp16, detect_face_mosaics=False, allow_trt_compile=allow_trt_compile,
    )
