# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Embeds a text file (HLSL shader source) into a generated C++ header as a raw string
# literal, so shaders live as real, editable, syntax-highlightable .hlsl files under
# native/shaders/ instead of C++ string literals, while the built .pyd stays fully
# self-contained (no runtime file path to resolve wherever the .pyd ends up copied).
#
# Usage: cmake -DIN=<path> -DOUT=<path> -DVARNAME=<name> -P embed_shader.cmake
#
# Relies on HLSL never containing the literal delimiter `)HLSL"` -- true for any shader
# source that doesn't itself embed that exact token, which none of sumu's do.

if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED VARNAME)
    message(FATAL_ERROR "embed_shader.cmake requires -DIN=, -DOUT=, -DVARNAME=")
endif()

file(READ "${IN}" _shader_content)

set(_header_content
"// Auto-generated from ${IN} by native/cmake/embed_shader.cmake -- do not edit directly.
#pragma once

inline const char* ${VARNAME} = R\"HLSL(
${_shader_content}
)HLSL\";
")

file(WRITE "${OUT}" "${_header_content}")
