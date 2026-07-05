# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Spike 0: native D3D11 present + FFmpeg d3d11va HEVC decode. Independent build, does not
# touch the rest of the sumu repo. See ../README.md for the measurement contract.

cmake_minimum_required(VERSION 3.20)
project(spike0_d3d11_present CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /EHsc)
endif()

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg dev package root (BtbN win64-gpl-shared)")

add_executable(spike0
    src/main.cpp
    src/decoder.cpp
)

target_include_directories(spike0 PRIVATE "${FFMPEG_ROOT}/include")
target_link_directories(spike0 PRIVATE "${FFMPEG_ROOT}/lib")

target_link_libraries(spike0 PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    winmm
    avcodec
    avformat
    avutil
)

# Copy FFmpeg's shared DLLs next to the exe so it runs without touching PATH.
file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
foreach(dll ${FFMPEG_DLLS})
    add_custom_command(TARGET spike0 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:spike0>"
    )
endforeach()
