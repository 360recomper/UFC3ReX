// UFC NR-13: FSR 1 EASU, the picture (ResourceDescriptorIndex) scaled up into the intermediate
// target (its size packed in ResourceDescriptorIndex2).
#define FFX_FSR_EASU_FLOAT 1
#include "present_fsr.hlsli"
#include "third_party/fidelityfx/gpu/fsr1/ffx_fsr1.h"

FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p)
{
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .GatherRed(g_SamplerDescriptorHeap[0], p);
}
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p)
{
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .GatherGreen(g_SamplerDescriptorHeap[0], p);
}
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p)
{
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .GatherBlue(g_SamplerDescriptorHeap[0], p);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    uint w, h;
    g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex].GetDimensions(w, h);
    const uint2 out_size = UnpackPair(g_PushConstants.ResourceDescriptorIndex2);
    FfxUInt32x4 con0, con1, con2, con3;
    ffxFsrPopulateEasuConstants(con0, con1, con2, con3, float(w), float(h), float(w), float(h),
                                float(out_size.x), float(out_size.y));
    FfxFloat32x3 pix;
    ffxFsrEasuFloat(pix, FfxUInt32x2(position.xy), con0, con1, con2, con3);
    return float4(pix, 1.0);
}
