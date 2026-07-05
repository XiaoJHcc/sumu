# SPDX-FileCopyrightText: Lada Authors
# SPDX-License-Identifier: AGPL-3.0

def register_all_modules():
    from sumu.ai.models.basicvsrpp.mmagic import register_all_modules
    register_all_modules()
    from sumu.ai.models.basicvsrpp.basicvsrpp_gan import BasicVSRPlusPlusGanNet, BasicVSRPlusPlusGan
    # NOTE (sumu port): lada also imported MosaicVideoDataset here (training dataset,
    # registry key DATASETS). It is intentionally NOT vendored: it's training-only and
    # its import chain drags in lada.utils.video_utils (the VideoReader/PyAV/pynvc
    # production-decode path) + lada.datasetcreation, both explicitly out of scope for
    # this AI-core port. Nothing on the load_models / restore inference path needs it.