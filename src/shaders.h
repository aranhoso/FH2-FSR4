/* HLSL sources compiled at runtime with the system d3dcompiler_47. */
#pragma once

/* Full-resolution replacement for FH2's half-resolution motion-blur velocity pass
   (PS hash 8a12870f-60f7c6e9-21d28459-07b63a84 on Azimuth 1.0.3/1.0.4). Same registers as
   the game's shader so it runs under the game's own root signature and bindings. The
   game's version reads a 2x2 gather and clamps the vector to 0.1 NDC; this one reads the
   exact pixel and keeps the full vector.
   Output: UV-space offset from the current pixel to where it was last frame
   (current -> previous), the convention FSR expects. */
static const char MV_PS_HLSL[] =
    "Texture2D<float> txWorldDepth : register(t0);\n"
    "Texture2D<uint> txWorldStencil : register(t1);\n"
    "cbuffer ImageProcPostProcessConstBuffer : register(b1) { float4 cb1[557]; };\n"
    "cbuffer AzimuthDynamicIndex : register(b13) { uint4 cb13; };\n"
    "float4 row(uint i, uint sel) { return i < 542u ? cb1[i + sel] : float4(0, 0, 0, 0); }\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TexCoord0) : SV_TARGET {\n"
    "    uint w, h;\n"
    "    txWorldDepth.GetDimensions(w, h);\n"
    "    int2 p = min(int2(uv.xy * float2(w, h)), int2(w - 1, h - 1));\n"
    "    float d = txWorldDepth.Load(int3(p, 0));\n"
    "    uint s = txWorldStencil.Load(int3(p, 0)) << 2;\n"
    "    uint sel = (cb13.x >> 4) & 15u;\n"
    "    float2 ndc = uv.xy * float2(2, -2) + float2(-1, 1);\n"
    "    float3 r = ndc.x * row(s + 30u, sel).xyw + ndc.y * row(s + 31u, sel).xyw\n"
    "             + d * row(s + 32u, sel).xyw + row(s + 33u, sel).xyw;\n"
    "    float2 prev = r.z != 0 ? r.xy / r.z : ndc;\n"
    "    float2 m = prev - ndc;\n"
    "    return float4(m.x * 0.5, -m.y * 0.5, 0, 0);\n"
    "}\n";

/* Fullscreen tonemap pass used in upscale mode: point-samples the FSR output (HDR) and writes
   the tonemapped image into the back buffer. cbuffer: hdr_mode, texel.xy. */
static const char TONEMAP_HLSL[] =
    "Texture2D<float3> hdr : register(t0);\n"
    "cbuffer C : register(b0) { uint hdr_mode; float2 texel; };\n"
    "float3 pq(float3 c) {\n"
    "    c = max(c, 0.0);\n"
    "    float m1 = 0.159301758125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "    float3 p = pow(c, float3(m2, m2, m2));\n"
    "    return pow((c1 + c2 * p) / (1.0 + c3 * p), float3(m1, m1, m1));\n"
    "}\n"
    "void vs(uint id : SV_VertexID, out float4 pos : SV_POSITION, out float2 uv : TEXCOORD0) {\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    uv = t;\n"
    "    pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    int2 p = int2(pos.xy);\n"
    "    float3 c = hdr.Load(int3(p, 0));\n"
    "    if (hdr_mode == 2u) { return float4(c, 1.0); }\n"
    "    if (hdr_mode == 3u) { return float4(1, 0, 1, 1); }\n"
    "    c = 1.0 - exp(-c * 2.0);\n"
    "    if (hdr_mode != 0u) { return float4(pq(c), 1.0); }\n"
    "    return float4(pow(c, 1.0 / 2.2), 1.0);\n"
    "}\n";
