#pragma once
// UFC NR-13: shared by the FSR 1 present passes (AMD FidelityFX FSR 1, MIT, third_party/fidelityfx).
// Their input already has the present colour change on it (the bilinear pass's shader, drawn at the
// picture's own size first).
#include "copy_common.hlsli"

#define FFX_GPU 1
#define FFX_HLSL 1
#include "third_party/fidelityfx/gpu/ffx_core.h"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// ResourceDescriptorIndex2 carries two 16-bit values (a size or an offset in pixels).
uint2 UnpackPair(uint v) { return uint2(v & 0xFFFFu, v >> 16); }
