// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// NV12 (BT.709 limited range) -> RGB, full-screen triangle, no vertex/index buffers.
// Source luma/chroma are Texture2DArray because the FFmpeg d3d11va decoder pool is a
// single ID3D11Texture2D with N array slices; arraySlice picks the current frame's slice.
//
// This file is the human-readable copy of the shader; main.cpp embeds an identical
// string literal (kShaderSrc) that is compiled at runtime via D3DCompile so the exe has
// no runtime dependency on this file's path. Keep both in sync if you edit either.

Texture2DArray<float>  texY  : register(t0);
Texture2DArray<float2> texUV : register(t1);
SamplerState           samp0 : register(s0);

cbuffer SliceCB : register(b0)
{
    uint arraySlice;
    uint3 _pad;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // Classic fullscreen triangle trick: 3 vertices that cover the whole clip volume.
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3]  = { float2(0.0, 1.0),   float2(0.0, -1.0), float2(2.0, 1.0) };

    VSOut o;
    o.pos = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 uvw = float3(i.uv, (float)arraySlice);
    float y = texY.Sample(samp0, uvw).r * 255.0;
    float2 cbcr = texUV.Sample(samp0, uvw).rg * 255.0;

    // BT.709 limited-range YCbCr -> RGB (integer coefficients scaled by 256, standard formula)
    float c = y - 16.0;
    float d = cbcr.x - 128.0; // Cb / U
    float e = cbcr.y - 128.0; // Cr / V

    float r = clamp((298.082 * c + 408.583 * e) / 256.0, 0.0, 255.0);
    float g = clamp((298.082 * c - 100.291 * d - 208.120 * e) / 256.0, 0.0, 255.0);
    float b = clamp((298.082 * c + 516.412 * d) / 256.0, 0.0, 255.0);

    return float4(r / 255.0, g / 255.0, b / 255.0, 1.0);
}
