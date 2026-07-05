# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 3: D3D11 NV12 decoded texture -> CUDA -> torch, zero main-memory round trip (the
# reverse direction of spike 1's CUDA->D3D11 bridge). Independent build, does not touch the
# rest of the sumu repo. Reuses native/src/decoder.{h,cpp} (copied, not symlinked, matching
# spike2's own precedent) and spike0's already-extracted FFmpeg dev package.
#
# No window/swapchain here -- this spike is a pure compute bridge (decode -> CUDA tensor),
# nothing is presented on screen, so unlike spikes 0/1/2 there's no Win32 window boilerplate.

cmake_minimum_required(VERSION 3.20)
project(spike3_nv12_interop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /EHsc)
endif()

set(CUDA_TOOLKIT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3" CACHE PATH "CUDA toolkit root (driver API headers/import lib only)")
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../spike0_d3d11_present/third_party/ffmpeg" CACHE PATH "FFmpeg dev package root (reused from spike0, not duplicated)")

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(sumu_nv12interop
    src/interop.cpp
    src/decoder.cpp
)

target_include_directories(sumu_nv12interop PRIVATE
    "${CUDA_TOOLKIT_ROOT}/include"
    "${FFMPEG_ROOT}/include"
)
target_link_directories(sumu_nv12interop PRIVATE "${FFMPEG_ROOT}/lib")

target_link_libraries(sumu_nv12interop PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    avcodec
    avformat
    avutil
    "${CUDA_TOOLKIT_ROOT}/lib/x64/cuda.lib"
)

# Land the .pyd directly in the source dir so spike3_driver.py can just sys.path.append(__file__ dir).
set_target_properties(sumu_nv12interop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG_UPPER)
    set_target_properties(sumu_nv12interop PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_CURRENT_SOURCE_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endforeach()

# Copy FFmpeg's shared DLLs next to the .pyd so it loads without touching PATH.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
foreach(dll ${FFMPEG_DLLS})
    add_custom_command(TARGET sumu_nv12interop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endforeach()
