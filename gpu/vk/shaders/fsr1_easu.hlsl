// (Evil Hookshot) AMD FidelityFX Super Resolution 1, EASU (edge-adaptive spatial upscaling): the frame to the window's size
// for the Insert menu's Upscaling = AMD FSR 1 (vk_framegen.cpp framegen_scale). Then fsr1_rcas.hlsl sharpens it. AMD's code in
// shaders/fsr1 (MIT). Compiled with the Vulkan SDK's dxc (scripts in the header made from it, fsr1_easu_comp_spv.h).
#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_FSR_EASU_FLOAT 1
#include "fsr1/ffx_core.h"

[[vk::binding(0)]] [[vk::combinedImageSampler]] Texture2D<float4> src;
[[vk::binding(0)]] [[vk::combinedImageSampler]] SamplerState smp;
[[vk::binding(1)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> dst;

struct Push { uint4 size; };   // (input w, h; output w, h)
[[vk::push_constant]] Push pc;

FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { return src.GatherRed(smp, p); }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { return src.GatherGreen(smp, p); }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { return src.GatherBlue(smp, p); }

#include "fsr1/ffx_fsr1.h"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= pc.size.zw)) return;
    FfxUInt32x4 c0, c1, c2, c3;
    ffxFsrPopulateEasuConstants(c0, c1, c2, c3, pc.size.x, pc.size.y, pc.size.x, pc.size.y, pc.size.z, pc.size.w);
    FfxFloat32x3 c;
    ffxFsrEasuFloat(c, id.xy, c0, c1, c2, c3);
    dst[id.xy] = float4(c, 1.0);
}
