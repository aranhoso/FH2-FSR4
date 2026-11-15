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
