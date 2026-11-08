// UFC NR-13: FSR 1 RCAS, the EASU output (ResourceDescriptorIndex) sharpened into the back
// buffer's fit rect (its top-left packed in ResourceDescriptorIndex2). Param0 = sharpness in stops
// (0 = most).
#define FSR_RCAS_F 1
#include "present_fsr.hlsli"
#include "third_party/fidelityfx/gpu/fsr1/ffx_fsr1.h"

static int2 g_RcasMax;

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p)
{
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .Load(int3(clamp(p, int2(0, 0), g_RcasMax), 0));
}
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    uint w, h;
    g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex].GetDimensions(w, h);
    g_RcasMax = int2(w, h) - 1;
    const uint2 offset = UnpackPair(g_PushConstants.ResourceDescriptorIndex2);
    // FsrRcasCon's con is passed by value in HLSL (an array out-parameter only in C), so its
    // float path (con.x) is filled here.
    const FfxUInt32x4 con = FfxUInt32x4(asuint(exp2(-g_PushConstants.Param0)), 0, 0, 0);
    float r, g, b;
    FsrRcasF(r, g, b, FfxUInt32x2(position.xy) - offset, con);
    return float4(r, g, b, 1.0);
}
