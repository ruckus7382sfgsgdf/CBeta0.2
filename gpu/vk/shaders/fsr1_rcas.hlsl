// (Evil Hookshot) AMD FidelityFX Super Resolution 1, RCAS (robust contrast-adaptive sharpening), after fsr1_easu.hlsl: the
// Insert menu's Sharpness, 0% the softest (2 stops below the most), 100% the most AMD allows (its limit keeps it free of halos).
// AMD's code in shaders/fsr1 (MIT).
#define FFX_GPU 1
#define FFX_HLSL 1
#define FSR_RCAS_F 1
#include "fsr1/ffx_core.h"

[[vk::binding(0)]] [[vk::combinedImageSampler]] Texture2D<float4> src;
[[vk::binding(0)]] [[vk::combinedImageSampler]] SamplerState smp;
[[vk::binding(1)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> dst;

struct Push { uint2 size; float sharpness; float pad; };   // (output w, h; sharpness 0..1)
[[vk::push_constant]] Push pc;

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) { return src.Load(int3(p, 0)); }
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "fsr1/ffx_fsr1.h"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= pc.size)) return;
    // (FsrRcasCon's constant by hand: AMD's takes it by value in HLSL. Its first word: 2^-stops, stops 0 = the most)
    const float stops = 2.0 * (1.0 - saturate(pc.sharpness));
    const FfxUInt32x4 con = FfxUInt32x4(asuint(exp2(-stops)), 0, 0, 0);
    FfxFloat32 r, g, b;
    FsrRcasF(r, g, b, id.xy, con);
    dst[id.xy] = float4(r, g, b, 1.0);
}
