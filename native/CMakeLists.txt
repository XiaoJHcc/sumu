# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# sumu's official native present kernel (pybind11 module `sumu_core`, class `Player`).
# Promoted from spikes/spike2_clock_mixing (three-thread clock-driven mixing kernel with a
# single explicit d3d_mutex_, see src/player.cpp's header comment) with a new seek=reposition
# capability (I6). Reuses spike0's already-extracted FFmpeg dev package and spike1/2's CUDA
# driver-API interop pattern -- does NOT re-download or duplicate FFmpeg.

cmake_minimum_required(VERSION 3.20)
project(sumu_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  # /utf-8: source files now contain literal UTF-8 Chinese text (settings panel labels).
  # Without this, MSVC decodes narrow string literals using the system ANSI code page,
  # which would silently corrupt those bytes at compile time (no build error, wrong text).
  add_compile_options(/W3 /EHsc /utf-8)
endif()

set(CUDA_TOOLKIT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3" CACHE PATH "CUDA toolkit root (driver API headers/import lib only, no nvcc)")
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../spikes/spike0_d3d11_present/third_party/ffmpeg" CACHE PATH "FFmpeg dev package root (reused from spike0, not duplicated)")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Vendored Dear ImGui root (see third_party/imgui/README.sumu.md)")

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

# ---- shader embedding (native/shaders/*.hlsl -> generated header, see cmake/embed_shader.cmake) ----
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(PRESENT_HLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present.hlsl")
set(PRESENT_HLSL_HEADER "${GENERATED_DIR}/present_hlsl.h")

add_custom_command(
    OUTPUT "${PRESENT_HLSL_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${PRESENT_HLSL_SRC}
        -DOUT=${PRESENT_HLSL_HEADER}
        -DVARNAME=kShaderSrc
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake"
    DEPENDS "${PRESENT_HLSL_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shader.cmake"
    COMMENT "Embedding present.hlsl -> present_hlsl.h"
)

# ---- logo embedding (assets/generated/sumu-logo-256.rgba -> logo_rgba.h) ----
# Source of truth is assets/sumu-logo-1024.png; scripts/gen_logo_assets.py resizes it to this
# .rgba dump. Native has no PNG decoder -- embed raw RGBA so the first-screen logo is
# self-contained inside the pyd (same rationale as the HLSL embed above).
set(LOGO_RGBA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../assets/generated/sumu-logo-256.rgba")
set(LOGO_RGBA_HEADER "${GENERATED_DIR}/logo_rgba.h")
add_custom_command(
    OUTPUT "${LOGO_RGBA_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${LOGO_RGBA_SRC}
        -DOUT=${LOGO_RGBA_HEADER}
        -DVARNAME=Logo
        -DWIDTH=256
        -DHEIGHT=256
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    DEPENDS "${LOGO_RGBA_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    COMMENT "Embedding sumu-logo-256.rgba -> logo_rgba.h"
)
add_custom_target(generate_shaders DEPENDS "${PRESENT_HLSL_HEADER}" "${LOGO_RGBA_HEADER}")


pybind11_add_module(sumu_core
    src/player.cpp
    src/decoder.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)
add_dependencies(sumu_core generate_shaders)

target_include_directories(sumu_core PRIVATE
    "${GENERATED_DIR}"
    "${CUDA_TOOLKIT_ROOT}/include"
    "${FFMPEG_ROOT}/include"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_link_directories(sumu_core PRIVATE "${FFMPEG_ROOT}/lib")

target_link_libraries(sumu_core PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    winmm
    dwmapi
    comdlg32
    ole32
    avcodec
    avformat
    avutil
    swresample
    "${CUDA_TOOLKIT_ROOT}/lib/x64/cuda.lib"
)

# Land the .pyd directly in python/sumu/ so `import sumu_core` (or sumu's own package code)
# can find it without any sys.path surgery.
set(SUMU_PY_PKG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../python/sumu")
set_target_properties(sumu_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SUMU_PY_PKG_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SUMU_PY_PKG_DIR}"
)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG_UPPER)
    set_target_properties(sumu_core PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${SUMU_PY_PKG_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${SUMU_PY_PKG_DIR}"
    )
endforeach()

# Copy FFmpeg's shared DLLs next to the .pyd so it loads without touching PATH.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
foreach(dll ${FFMPEG_DLLS})
    add_custom_command(TARGET sumu_core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${SUMU_PY_PKG_DIR}"
    )
endforeach()

# Window/taskbar icon for the Win32 class (LoadImage from module_dir). Same file is also
# staged next to the frozen exe by scripts/build_dist.ps1 for the PyInstaller icon=.
add_custom_command(TARGET sumu_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/generated/sumu.ico"
        "${SUMU_PY_PKG_DIR}/sumu.ico"
)

