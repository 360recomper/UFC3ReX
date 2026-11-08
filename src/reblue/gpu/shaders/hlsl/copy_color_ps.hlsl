#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

static const uint kMaxBoxTaps = 8u;

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float3 rgb;
    float a;
    if (g_PushConstants.Param1 < -1.5)
    {
        // UFC target fill (NR-12a): a guest-size level stretched to the host size packed into the
        // second descriptor slot (two 16-bit values), nearest: the texel the CPU fill picks.
        uint dstW = g_PushConstants.ResourceDescriptorIndex2 & 0xFFFFu;
        uint dstH = g_PushConstants.ResourceDescriptorIndex2 >> 16;
        uint srcW, srcH;
        tex.GetDimensions(srcW, srcH);
        uint2 p = uint2(position.xy);
        float4 s = tex.Load(int3(min(p.x * srcW / dstW, srcW - 1u), min(p.y * srcH / dstH, srcH - 1u), 0));
        rgb = s.rgb;
        a = s.a;
    }
    else if (g_PushConstants.Param1 < -0.5)
    {
        // UFC rect resolve: 1:1, source texel = destination pixel + a signed 16-bit offset pair
        // packed into the second descriptor slot (resolve.cpp, ResolveRegion).
        int offX = ((int)(g_PushConstants.ResourceDescriptorIndex2 << 16)) >> 16;
        int offY = ((int)g_PushConstants.ResourceDescriptorIndex2) >> 16;
        float4 s = tex.Load(int3((int)position.x + offX, (int)position.y + offY, 0));
        rgb = s.rgb;
        a = s.a;
    }
    else
    {
        // Param1 >= 0 from here: a negative one cast to uint is undefined.
        uint ratio = (uint)(g_PushConstants.Param1 + 0.5);
        if (ratio >= 2u)
        {
            uint taps = min(ratio, kMaxBoxTaps);
            int2 base = int2(position.xy) * (int)ratio;
            float4 acc = 0.0;
            [loop] for (uint y = 0u; y < taps; ++y)
            {
                int oy = (int)((y * ratio + ratio / 2u) / taps);
                [loop] for (uint x = 0u; x < taps; ++x)
                {
                    int ox = (int)((x * ratio + ratio / 2u) / taps);
                    acc += tex.Load(int3(base.x + ox, base.y + oy, 0));
                }
            }
            rgb = acc.rgb / (float)(taps * taps);
            a = acc.a / (float)(taps * taps);
        }
        else
        {
            float4 s = tex.Sample(g_SamplerDescriptorHeap[0], texCoord);
            rgb = s.rgb;
            a = s.a;
        }
    }
    return float4(rgb * g_PushConstants.Param0, a);
}
