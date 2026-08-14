// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Registration entry for the HeadlessDecode class (see headless_decode.cpp) -- a windowless
// d3d11va decode -> CUDA NV12 bridge for the transcode / web-streaming / offline-export
// pipeline. Bound into the existing `sumu_core` module by player.cpp's PYBIND11_MODULE block.
#pragma once

#include <pybind11/pybind11.h>

namespace py = pybind11;

// Called from player.cpp's PYBIND11_MODULE(sumu_core, m) block.
void init_headless_decode(py::module_& m);
