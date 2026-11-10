# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# sumu's official native present kernel (pybind11 module `sumu_core`, class `Player`).
# Promoted from spikes/spike2_clock_mixing (three-thread clock-driven mixing kernel with a
# single explicit d3d_mutex_, see src/player.h's header comment) with a new seek=reposition
# capability (I6). Reuses spike0's already-extracted FFmpeg dev package and spike1/2's CUDA
# driver-API interop pattern -- does NOT re-download or duplicate FFmpeg.

cmake_minimum_required(VERSION 3.20)
project(sumu_core CXX)

# CMake 4.3's MSVC platform injects /LTCG (CMAKE_CXX_LINK_OPTIONS_IPO) into the link flags even
# when no interprocedural-optimization is requested, which makes link.exe write multi-hundred-MB
# temp objects under %TEMP% (hanging the link where %TEMP% is unwritable to child processes).
# Force IPO off globally (plus the per-target override below).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "Disable IPO/LTCG" FORCE)

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

# ---- WebView2 SDK (build-time only; the app ships NO WebView2 DLL) ---------------------------
# The URL page's embedded browser uses the system Evergreen WebView2 Runtime at run time and
# the NuGet SDK only for headers + the STATIC loader at link time. Resolution order (first hit
# wins) keeps a clean tree reproducible without pinning one user's absolute path:
#   1. -DWEBVIEW2_ROOT=<extracted nupkg>
#   2. native/third_party/webview2/package            (gitignored fetch target, see below)
#   3. %NUGET_PACKAGES% / %USERPROFILE%\.nuget\packages \ microsoft.web.webview2\<version>
#   4. download + extract that exact nupkg into native/third_party/webview2/package
set(WEBVIEW2_VERSION "1.0.3179.45" CACHE STRING "Microsoft.Web.WebView2 NuGet package version")
set(WEBVIEW2_ROOT "" CACHE PATH "Extracted Microsoft.Web.WebView2 package root (empty = auto-resolve)")
if(NOT WEBVIEW2_ROOT)
  set(_wv2_fetch "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
  if(EXISTS "${_wv2_fetch}/package/build/native/include/WebView2.h")
    set(WEBVIEW2_ROOT "${_wv2_fetch}/package")
  endif()
  if(NOT WEBVIEW2_ROOT)
    foreach(_root "$ENV{NUGET_PACKAGES}" "$ENV{USERPROFILE}/.nuget/packages" "$ENV{HOME}/.nuget/packages")
      if(_root AND EXISTS "${_root}/microsoft.web.webview2/${WEBVIEW2_VERSION}/build/native/include/WebView2.h")
        set(WEBVIEW2_ROOT "${_root}/microsoft.web.webview2/${WEBVIEW2_VERSION}")
        break()
      endif()
    endforeach()
  endif()
  if(NOT WEBVIEW2_ROOT)
    set(_wv2_nupkg "${_wv2_fetch}.nupkg")
    message(STATUS "WebView2 SDK not found locally -- downloading ${WEBVIEW2_VERSION}")
    file(DOWNLOAD
      "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}"
      "${_wv2_nupkg}" STATUS _wv2_status SHOW_PROGRESS TLS_VERIFY ON)
    list(GET _wv2_status 0 _wv2_code)
    if(NOT _wv2_code EQUAL 0)
      list(GET _wv2_status 1 _wv2_msg)
      file(REMOVE "${_wv2_nupkg}")
      message(FATAL_ERROR "WebView2 SDK download failed (${_wv2_msg}). Install the "
        "Microsoft.Web.WebView2 ${WEBVIEW2_VERSION} NuGet package or pass "
        "-DWEBVIEW2_ROOT=<extracted package dir>.")
    endif()
    file(MAKE_DIRECTORY "${_wv2_fetch}/package")
    file(ARCHIVE_EXTRACT INPUT "${_wv2_nupkg}" DESTINATION "${_wv2_fetch}/package")
    file(REMOVE "${_wv2_nupkg}")
    set(WEBVIEW2_ROOT "${_wv2_fetch}/package")
  endif()
endif()
if(NOT EXISTS "${WEBVIEW2_ROOT}/build/native/include/WebView2.h")
  message(FATAL_ERROR "WEBVIEW2_ROOT does not look like an extracted Microsoft.Web.WebView2 "
    "package: ${WEBVIEW2_ROOT}")
endif()
message(STATUS "WebView2 SDK: ${WEBVIEW2_ROOT}")
set(WEBVIEW2_INCLUDE_DIR "${WEBVIEW2_ROOT}/build/native/include")
set(WEBVIEW2_LOADER_LIB "${WEBVIEW2_ROOT}/build/native/x64/WebView2LoaderStatic.lib")

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

# ---- icon atlas embedding (assets/generated/icons_atlas.rgba -> icons_atlas.h) ----
# Source of truth is assets/icons/lucide/*.svg; scripts/gen_icon_atlas.py rasterizes them
# into this .rgba strip (21 tightly packed 96px cells; native resamples per-DPI at runtime).
# Same rationale as the logo embed: no image decoder in native.
set(ICONS_RGBA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../assets/generated/icons_atlas.rgba")
set(ICONS_RGBA_HEADER "${GENERATED_DIR}/icons_atlas.h")
add_custom_command(
    OUTPUT "${ICONS_RGBA_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DIN=${ICONS_RGBA_SRC}
        -DOUT=${ICONS_RGBA_HEADER}
        -DVARNAME=IconsAtlas
        -DWIDTH=2016
        -DHEIGHT=96
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    DEPENDS "${ICONS_RGBA_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    COMMENT "Embedding icons_atlas.rgba -> icons_atlas.h"
)
add_custom_target(generate_shaders DEPENDS "${PRESENT_HLSL_HEADER}" "${LOGO_RGBA_HEADER}" "${ICONS_RGBA_HEADER}")


pybind11_add_module(sumu_core
    src/player_session.cpp
    src/player_window.cpp
    src/player_engine.cpp
    src/player_audio.cpp
    src/player_ui_overlays.cpp
    src/player_ui_bars.cpp
    src/player_ui_export.cpp
    src/player_pybind.cpp
    src/browser_view.cpp
    src/ui_util.cpp
    src/ui/theme.cpp
    src/ui/widgets.cpp
    src/ui/icons.cpp
    src/decoder.cpp
    src/video_normalizer.cpp
    src/headless_decode.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
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
    "${WEBVIEW2_INCLUDE_DIR}"
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
    oleaut32
    advapi32
    shlwapi
    version
    comctl32
    # WIC: decode the WebView2 page snapshot that backs the settings overlay.
    windowscodecs
    avcodec
    avformat
    avutil
    swresample
    "${CUDA_TOOLKIT_ROOT}/lib/x64/cuda.lib"
    # Static WebView2 loader: no runtime DLL to stage next to the .pyd/onedir bundle.
    "${WEBVIEW2_LOADER_LIB}"
)

# Disable MSVC link-time code generation. /LTCG offers nothing for a pybind11 module but makes
# the linker emit large temp objects under %TEMP% -- which hangs the link step in environments
# where %TEMP% is not writable by child processes (observed: the sandboxed dev harness).
set_property(TARGET sumu_core PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)

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

