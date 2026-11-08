// UFC 3's XDK D3DDevice layout: UFC's counterpart of reblue/gpu/d3d.h's SvR 2010 D3DDevice.
//
// UFC links a later XDK than SvR 2010 (FINDINGS-nr1-d3d-map.md §4). The constants block and the
// register shadows match SvR (to +0x2AC0); everything past that is re-derived from UFC's own
// stores, not offset from SvR. Device = 0x5F80 bytes (Direct3D_CreateDevice sub_82374F80,
// li r3,24448). Each offset names the function whose store proves it. "census" = checked at run
// time by the NR-6 census (src/dev/ufc_d3d_census.cpp, FINDINGS-native-renderer.md "NR-6 census").

#pragma once

#include <cstdint>

namespace ufc3::d3d {

constexpr uint32_t kDeviceSize = 0x5F80;

// Dirty masks and the ring (KickOff / MakeSpace sub_82370460).
constexpr uint32_t kDirtyMasks = 0x000;         // be_u64[5]
constexpr uint32_t kRing = 0x030;               // write pointer; BeginCommandBuffer resets it to 4
constexpr uint32_t kRingLimit = 0x034;
constexpr uint32_t kRingGuarantee = 0x038;

// Constants block: same as SvR 2010.
constexpr uint32_t kFetchConstants = 0x480;     // SetTexture sub_8236E9D8: (sampler + 48) * 24
constexpr uint32_t kVsFloatConstants = 0x780;   // SetVertexShaderConstantF sub_82371440
constexpr uint32_t kPsFloatConstants = 0x1780;  // SetPixelShaderConstantF sub_82371518
constexpr uint32_t kVsBoolConstants = 0x2780;   // sub_823715F0
constexpr uint32_t kPsBoolConstants = 0x2790;   // sub_82371650
constexpr uint32_t kRbDepthControl = 0x2934;    // register shadow (SvR RB_BLENDCONTROL0 at +0x2938)
constexpr uint32_t kPaScWindowScissor = 0x28C4; // SetScissorRect sub_82377610 writes +0x28C4/+0x28C8

// Past +0x2AC0: UFC-only offsets.
constexpr uint32_t kStateFlags = 0x2B3C;        // byte +0x2B3C (11068): BeginZPass / RunCommandBuffer test bit 0x20
constexpr uint32_t kVertexDeclaration = 0x2F58; // SetVertexDeclaration sub_82371BB8 (SvR +0x2ED8); census
constexpr uint32_t kIndexBuffer = 0x318C;       // SetIndices sub_82377900 (SvR +0x3144)
constexpr uint32_t kRenderTarget0 = 0x3190;     // SetRenderTarget sub_82378088: (idx + 3172) * 4, 4 slots
constexpr uint32_t kDepthStencil = 0x31A0;      // SetDepthStencilSurface sub_82378418 (SvR +0x3158)
constexpr uint32_t kStreamSource = 0x31A4;      // SetStreamSource sub_82377758: (3177 + s) * 4, 16 streams
constexpr uint32_t kStreamStrideDiv4 = 0x31E8;  // u8 per stream; BeginVertices writes stream 0's
constexpr uint32_t kTexture = 0x31F8;           // SetTexture: (3198 + s) * 4
constexpr uint32_t kViewport = 0x3260;          // SetViewport -> sub_82377CF0; GetViewport reads to +0x3274
constexpr uint32_t kViewportFlags = 0x3278;
constexpr uint32_t kScissorRect = 0x327C;       // SetScissorRect (SvR +0x3234)
constexpr uint32_t kPixelShader = 0x328C;       // SetPixelShader sub_82371798 (SvR +0x3244); census
constexpr uint32_t kVertexShader = 0x3290;      // SetVertexShader sub_823719A0 (SvR +0x3248); census
constexpr uint32_t kTileCount = 0x32C4;         // BeginTiling sub_82380408: stw r5 (count), rects follow
constexpr uint32_t kZPassActive = 0x32A8;       // BeginZPass sub_82380778: stw 1 at 12968 (low confidence)
constexpr uint32_t kSavedRingEnd = 0x3594;      // EndVertices sub_823832D8: ring = saved end
constexpr uint32_t kRecordingCommandBuffer = 0x35E0;  // BeginCommandBuffer sub_82372A00: stw cb,13792; census
constexpr uint32_t kPresentParams = 0x365C;     // Swap sub_8237BD60
// SetShaderGPRAllocation sub_82371D40 emits three INDIRECT_BUFFERs, each {size dwords (low 24 bits,
// flags above), physical address}, all built by device init sub_82371EA0 (NR-6b):
constexpr uint32_t kGprBaseIb = 0x36EC;         // sub_823710E0's state buffer (no draws)
constexpr uint32_t kGprTableIb = 0x36F4;        // + vs_gprs * 8, vs_gprs 16..112 (sub_823713D0; no draws)
constexpr uint32_t kGprFlushIb = 0x3AF4;        // 24 x DRAW_INDX_2 point list, 1 auto index (pipe flush)
constexpr uint32_t kFrontBuffer = 0x3B00;       // Swap
constexpr uint32_t kBackBuffer = 0x3B04;        // Present: SetRenderTarget(0, +0x3B04)
constexpr uint32_t kCreateFlags = 0x5EC8;

// Command buffer object (RunCommandBuffer sub_82374010): flags +108, segment list +116
// (node {next, count, entries[count] {size dwords in the low 24 bits, physical address}}).
constexpr uint32_t kCbFlags = 108;
constexpr uint32_t kCbSegments = 116;

// D3DQuery (D3DQuery_Issue sub_82379540): device +0, type +4 (9 = occlusion / ZPD, 10 = event).
constexpr uint32_t kQueryDevice = 0;
constexpr uint32_t kQueryType = 4;

static_assert(kVertexShader == kPixelShader + 4);
static_assert(kStreamSource == kDepthStencil + 4);
static_assert(kRecordingCommandBuffer < kDeviceSize && kCreateFlags < kDeviceSize);

}  // namespace ufc3::d3d
