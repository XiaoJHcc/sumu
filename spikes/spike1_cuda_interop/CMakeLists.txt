# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 1: torch CUDA tensor -> D3D11 present face, zero-copy interop. Independent build,
# does not touch the rest of the sumu repo. See ../README.md for the measurement contract.
#
# Deliberately: no CUDA language enabled, no nvcc. This links against the CUDA DRIVER API
# (cuda.lib -> nvcuda.dll) only, called from plain C++ -- there is no device code to
# compile here, only host-side driver-API calls for the D3D11 interop bridge.

cmake_minimum_required(VERSION 3.20)
project(spike1_cuda_interop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /EHsc)
endif()

set(CUDA_TOOLKIT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3" CACHE PATH "CUDA toolkit root (driver API headers/import lib only)")

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(sumu_present src/presenter.cpp)

target_include_directories(sumu_present PRIVATE "${CUDA_TOOLKIT_ROOT}/include")

target_link_libraries(sumu_present PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    winmm
    "${CUDA_TOOLKIT_ROOT}/lib/x64/cuda.lib"
)

# Land the .pyd directly in the source dir so spike1_driver.py can just sys.path.append(__file__ dir).
set_target_properties(sumu_present PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG_UPPER)
    set_target_properties(sumu_present PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endforeach()
