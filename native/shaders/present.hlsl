// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// sumu present shader -- promoted verbatim from spikes/spike2_clock_mixing/src/presenter.cpp's
// inline kShaderSrc. Embedded into the sumu_core module at build time (see
// native/cmake/embed_shader.cmake) rather than loaded at runtime, so the .pyd stays
// self-contained wherever it gets copied (python/sumu/, native/build/, ...).
//
// VSMain is shared by both pixel shaders (fullscreen triangle from SV_VertexID, no vertex
// buffer). Distinct registers (t0/t1 for NV12 Y/UV, t2 for the AI RGBA8 array) so both entry
// points can live in one compile unit with zero risk of register-binding conflicts.

Texture2DArray<float>  texY   : register(t0);
Texture2DArray<float2> texUV  : register(t1);
Texture2DArray<float4> texAI  : register(t2);
SamplerState           samp0  : register(s0);

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
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3]  = { float2(0.0, 1.0),   float2(0.0, -1.0), float2(2.0, 1.0) };
    VSOut o;
    o.pos = float4(pos[vid], 0.0, 1.0);
    o.uv = uv[vid];
    return o;
}

// passthrough source: decoder's NV12 -> RGB (BT.709, limited range). The decoder hands this
// shader BT.709 limited NV12 unconditionally (see Decoder::prepare_nv12_frame /
// VideoNormalizer), so the coefficients are the BT.709 ones: Kr/Kg/Kb = 0.2126/0.7152/0.0722,
// luma scaled by 255/219 and chroma by 255/224 (x256 fixed point, exact round trip of the
// 16/219 + 128/224 encode).
float4 PSMain_NV12(VSOut i) : SV_Target
{
    float3 uvw = float3(i.uv, (float)arraySlice);
    float y = texY.Sample(samp0, uvw).r * 255.0;
    float2 cbcr = texUV.Sample(samp0, uvw).rg * 255.0;

    float c = y - 16.0;
    float d = cbcr.x - 128.0;
    float e = cbcr.y - 128.0;

    float r = clamp((298.082 * c + 458.9417 * e) / 256.0, 0.0, 255.0);
    float g = clamp((298.082 * c - 54.5916 * d - 136.4248 * e) / 256.0, 0.0, 255.0);
    float b = clamp((298.082 * c + 540.7749 * d) / 256.0, 0.0, 255.0);

    return float4(r / 255.0, g / 255.0, b / 255.0, 1.0);
}

// AI source: ready-map's RGBA8 array, straight passthrough sample.
float4 PSMain_AI(VSOut i) : SV_Target
{
    float3 uvw = float3(i.uv, (float)arraySlice);
    return texAI.Sample(samp0, uvw);
}
