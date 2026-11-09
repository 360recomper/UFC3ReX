/**
 * @file    gpu/hooks/state.cpp
 * @brief   Guest hooks that set the draw state: targets, viewport, bindings,
 *          render state and the shader constant dirty marks.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_hook.h"
#include "gpu/constant_buffers.h"
#include "ufc_cb_mirror.h"
#include "ufc_resources.h"
#endif
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/physical_buffers.h"
#include "gpu/shaders/shader_constants.h"

namespace {

using bd::gpu::ResolveGuestBufferVa;

void D3DDevice_SetViewport_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice,
    rex::MappedPtr<bd::gpu::D3DViewport9> pViewport) {
  if (!pViewport) {
    return;
  }

  auto &s = bd::gpu::state();

  const float fx = static_cast<float>(u32(pViewport->X));
  const float fy = static_cast<float>(u32(pViewport->Y));
  const float fw = static_cast<float>(u32(pViewport->Width));
  const float fh = static_cast<float>(u32(pViewport->Height));
  const float fmin = float(pViewport->MinZ);
  const float fmax = float(pViewport->MaxZ);

  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.x,
                                       fx);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.y,
                                       fy);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport, s.viewport.width,
                                       fw);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.height, fh);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.minDepth, fmin);
  bd::gpu::Video::SetDirtyValue<float>(s.dirtyStates.viewport,
                                       s.viewport.maxDepth, fmax);

  s.dirtyStates.scissorRect =
      s.dirtyStates.scissorRect || s.dirtyStates.viewport;

  if (pDevice) {
    pDevice->viewport.X = u32(pViewport->X);
    pDevice->viewport.Y = u32(pViewport->Y);
    pDevice->viewport.Width = u32(pViewport->Width);
    pDevice->viewport.Height = u32(pViewport->Height);
    pDevice->viewport.MinZ = fmin;
    pDevice->viewport.MaxZ = fmax;
  }
}

void D3DDevice_SetRenderTarget_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice, u32 RenderTargetIndex,
    rex::MappedPtr<bd::gpu::D3DSurface> pRenderTarget) {
#if defined(SVR_NATIVE_RENDERER)
  auto *surface = bd::gpu::SvrResolveSurface(pRenderTarget.guest_address());
#else
  auto *surface = bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(
      pRenderTarget.guest_address());
#endif

  // Maintain the device RT shadow the recompiled D3DDevice_GetRenderTarget
  // reads: the RT stack push/pop (every Sofdec movie frame) saves via the
  // getter and restores via Set, so a stale shadow un-binds the real RT after
  // each stack pop.
#if !defined(SVR_NATIVE_RENDERER)  // SvR's own SetRenderTarget keeps its shadow
  if (pDevice && RenderTargetIndex < 4) {
    pDevice->renderTargetShadow[RenderTargetIndex] =
        pRenderTarget.guest_address();
  }
#endif

  // MRT is not modeled. BD issues SetRenderTarget(idx=1..3, NULL) every frame
  // after binding RT[0], and writing those into state would clobber the real
  // RT[0].
  if (RenderTargetIndex != 0)
    return;

  auto &s = bd::gpu::state();

  // FlushRenderState bakes the new RT's format into the PSO, so the cached
  // framebuffer holding the old RTV has to go with it.
  if (s.render_target != surface)
    s.draw_framebuffer_bound = false;

  bd::gpu::Video::SetDirtyValue<bd::gpu::GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.render_target, surface);
  bd::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat,
      surface != nullptr ? surface->format : plume::RenderFormat::UNKNOWN);
  bd::gpu::Video::SetDirtyValue<plume::RenderSampleCounts>(
      s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
      surface != nullptr ? surface->sampleCount
                         : plume::RenderSampleCount::COUNT_1);
  // Alpha test mode tied to sample count.
  bd::gpu::Video::SetAlphaTestMode(
      (s.pipelineState.specConstants & bd::gpu::kSpecConstantAlphaTest) != 0);

#if defined(SVR_NATIVE_RENDERER)
  // SvR's own SetRenderTarget/SetDepthStencilSurface already reset the device viewport
  // (sub_82240748); the guest device is the game's, so only the host state is touched.
  bd::gpu::Video::SetDefaultViewport(nullptr, surface);
#else
  bd::gpu::Video::SetDefaultViewport(pDevice, surface);
#endif
}

void D3DDevice_SetDepthStencilSurface_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> pDevice,
    rex::MappedPtr<bd::gpu::D3DSurface> pZStencilSurface) {
#if defined(SVR_NATIVE_RENDERER)
  auto *surface = bd::gpu::SvrResolveSurface(pZStencilSurface.guest_address());
#else
  auto *surface = bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestTexture>(
      pZStencilSurface.guest_address());
#endif
  // Shadow for the recompiled D3DDevice_GetDepthStencilSurface (+0x2F98). Raw
  // VA as given, the pop's re-Set re-applies the type filter below.
#if !defined(SVR_NATIVE_RENDERER)  // SvR's own SetDepthStencilSurface keeps its shadow
  if (pDevice) {
    pDevice->depthStencilShadow = pZStencilSurface.guest_address();
  }
#endif
  // FromGuest verifies registration, not ResourceType: reject non-depth so a
  if (surface) {
    const bool is_depth =
        surface->type == bd::gpu::ResourceType::DepthStencil ||
        (surface->type == bd::gpu::ResourceType::Texture &&
         bd::gpu::IsDepthFormat(surface->format));
    if (!is_depth)
      surface = nullptr;
  }

  auto &s = bd::gpu::state();

  // Depth surface change drops the cached framebuffer (same #613 reason as
  // SetRenderTarget above).
  if (s.depth_stencil != surface)
    s.draw_framebuffer_bound = false;

  bd::gpu::Video::SetDirtyValue<bd::gpu::GuestTexture *>(
      s.dirtyStates.renderTargetAndDepthStencil, s.depth_stencil, surface);
  bd::gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.depthStencilFormat,
      surface != nullptr ? surface->format : plume::RenderFormat::UNKNOWN);

  // Remember the most recent real depth surface for the enhanced DOF
  // downsample. BD rebinds the 1280 DOF depth here just before the post chain,
  // so this ends up naming the depth aligned with the 1280 scene resolve.
  if (surface)
    s.scene_depth = surface;

#if defined(SVR_NATIVE_RENDERER)
  // SvR's own SetRenderTarget/SetDepthStencilSurface already reset the device viewport
  // (sub_82240748); the guest device is the game's, so only the host state is touched.
  bd::gpu::Video::SetDefaultViewport(nullptr, surface);
#else
  bd::gpu::Video::SetDefaultViewport(pDevice, surface);
#endif
}

void D3DDevice_SetScissorRect_hook(
    rex::MappedPtr<bd::gpu::D3DDevice> /*pDevice*/,
    rex::MappedPtr<bd::gpu::D3DRect> pRect) {
  if (!pRect) {
    return;
  }
  // Scissor always tracks the viewport extent in FlushViewport, so the rect
  // values are unused, and only the dirty mark matters.
  bd::gpu::state().dirtyStates.scissorRect = true;
}

void D3DDevice_SetVertexShader_hook(u32 /*device*/, u32 shader_guest) {
#if defined(SVR_NATIVE_RENDERER)
  auto *shader = bd::gpu::SvrLookupShader(shader_guest);
#else
  auto *shader =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestShader>(shader_guest);
#endif
  bd::gpu::Video::SetVertexShader(shader);
}

void D3DDevice_SetPixelShader_hook(u32 /*device*/, u32 shader_guest) {
#if defined(SVR_NATIVE_RENDERER)
  auto *shader = bd::gpu::SvrLookupShader(shader_guest);
#else
  auto *shader =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestShader>(shader_guest);
#endif
  bd::gpu::Video::SetPixelShader(shader);
}

void D3DDevice_SetVertexDeclaration_hook(u32 /*device*/, u32 decl_guest) {
#if defined(SVR_NATIVE_RENDERER)
  auto *decl = bd::gpu::SvrLookupVertexDeclaration(decl_guest);
#else
  auto *decl =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestVertexDeclaration>(
          decl_guest);
#endif

  // The recompiled VS guards its R11G11B10/SNORM decode on this bit, else it
  // asfloat()s the normal bits to garbage.
  {
    auto &s = bd::gpu::state();
    u32 spec = s.pipelineState.specConstants &
               ~(bd::gpu::kSpecConstantR11G11B10Normal | bd::gpu::kSpecConstantDhen3nNormal);
    if (decl && decl->hasR11G11B10Normal)
      spec |= bd::gpu::kSpecConstantR11G11B10Normal;
    if (decl && decl->hasDhen3nNormal)
      spec |= bd::gpu::kSpecConstantDhen3nNormal;
    bd::gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState,
                                       s.pipelineState.specConstants, spec);
  }

  bd::gpu::Video::SetVertexDeclaration(decl);
}

void D3DDevice_SetTexture_hook(u32 /*device*/, u32 sampler, u32 texture_guest) {
#if defined(SVR_NATIVE_RENDERER)
  bd::gpu::SvrZone zone(bd::gpu::SvrZoneId::kSetTexture);
  auto *tex = bd::gpu::SvrResolveTexture(texture_guest);
#else
  auto *tex = bd::gpu::ResolveGuestTexture(texture_guest);
#endif
  const bool unresolved = (!tex && texture_guest);
#if defined(SVR_NATIVE_RENDERER)
  if (unresolved) {
    // Each texture that falls back to the white debug texture, once (fetch constant dwords).
    static std::mutex s_mutex;
    static std::unordered_set<u32> s_seen;
    std::lock_guard lock(s_mutex);
    if (s_seen.size() < 256 && s_seen.insert(texture_guest).second) {
      const auto *t = bd::mem::at<const bd::gpu::D3DTexture>(texture_guest);
      BD_WARN("[svr] texture 0x{:08X} slot {} unresolved: fetch {:08X} {:08X} {:08X} {:08X} "
              "{:08X} {:08X}",
              texture_guest, sampler, t ? u32(t->Format.dword[0]) : 0u,
              t ? u32(t->Format.dword[1]) : 0u, t ? u32(t->Format.dword[2]) : 0u,
              t ? u32(t->Format.dword[3]) : 0u, t ? u32(t->Format.dword[4]) : 0u,
              t ? u32(t->Format.dword[5]) : 0u);
    }
  }
#endif
  if (unresolved) {
    // Unresolved (outside the supported native format set, or a mirror the device refused to
    // create): the opaque white 4x4 debug texture, so the quad shows as its vertex colour.
    tex = bd::gpu::GetOrCreateDebugTexture();
  }
  bd::gpu::Video::SetTexture(sampler, tex);
}

#if !defined(SVR_NATIVE_RENDERER)
// SvR binds geometry at draw time from the device instead (src/native/svr_geometry.cpp): the
// struct-keyed registry below never notices a refilled buffer or a reused header, and keeps stream
// 0 on the BeginVertices upload until the next SetStreamSource.
void D3DDevice_SetStreamSource_hook(u32 /*device*/, u32 stream,
                                    u32 buffer_guest, u32 offset, u32 stride) {
  auto *buf =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestBuffer>(buffer_guest);
  // Physical VBs wrap an engine-owned D3DVertexBuffer struct never
  // HostResourceHeap::Alloc'd, so FromGuest misses. Struct VA map covers VBs
  // registered eagerly (bdSceneGraphRegisterVBHook), and the lazy bootstrap
  // reads the struct's Xenos fetch constant fields for buffers from unhooked
  // paths.
  if (!buf && buffer_guest) {
    buf =
        ResolveGuestBufferVa(buffer_guest, bd::gpu::ResourceType::VertexBuffer);
  }
  if (buf && buf->hasBuffer()) {
    auto ref = buf->bufferRef(offset);
    const u32 bound_size = offset < buf->dataSize ? buf->dataSize - offset : 0;
    bd::gpu::Video::SetVertexStream(stream, ref, bound_size, stride);
  } else {
    bd::gpu::Video::SetVertexStream(stream, plume::RenderBufferReference{}, 0,
                                    0);
  }
}

void D3DDevice_SetIndices_hook(u32 /*device*/, u32 indices_guest) {
  auto *ib =
      bd::gpu::HostResourceHeap::FromGuest<bd::gpu::GuestBuffer>(indices_guest);
  // Physical IBs wrap an engine-owned struct FromGuest misses (same as
  // SetStreamSource). Without the struct VA bridge + lazy bootstrap, every
  // scene mesh's IB binds null, every index reads 0, scene goes black.
  if (!ib && indices_guest) {
    ib =
        ResolveGuestBufferVa(indices_guest, bd::gpu::ResourceType::IndexBuffer);
  }
  bd::gpu::Video::SetIndices(ib);
}
#endif

// SetSamplerState is deliberately NOT hooked: its recompiled body writes the
// sampler state table at device+0x1BC, which FlushRenderState reads directly.
//
// The constant setters below stay raw: they touch no argument, only run the
// original and then mark dirty to gate the CBV upload, and their arities
// differ.
#define REBLUE_CONSTANT_DIRTY_HOOK(fn, mark)                                   \
  REX_EXTERN(__imp__##fn);                                                     \
  REX_HOOK_RAW(fn) {                                                           \
    __imp__##fn(ctx, base);                                                    \
    mark;                                                                      \
  }

// SvR 2008: constant setter hooks are not mapped yet; FlushRenderState uploads every draw.

#undef REBLUE_CONSTANT_DIRTY_HOOK

// bdSetRenderState. BD's chokepoint for every D3DRS write: the per-D3DRS guest
// setters have no direct xrefs, so all state reaches them through this vtable
// dispatch, keyed by the render state's byte offset (its index * 4).
constexpr u32 kRsAlphaTestEnable = 24 * 4;
constexpr u32 kRsAlphaRef = 25 * 4;
constexpr float kAlphaRefScale = 1.0f / 256.0f; // ALPHAREF is 0..255

} // namespace

#if defined(SVR_NATIVE_RENDERER)
namespace {
bd::gpu::GuestTexture *g_mirror_saved_textures[kUfcMirrorTextures];
}

void UfcMirrorSave() {
  auto &s = bd::gpu::state();
  std::lock_guard lock(s.mutex);
  for (u32 i = 0; i < kUfcMirrorTextures; ++i)
    g_mirror_saved_textures[i] = s.textures[i];
}

void UfcMirrorApply(uint32_t vs, uint32_t ps, uint32_t decl, const uint32_t *textures) {
  D3DDevice_SetVertexShader_hook(0, vs);
  D3DDevice_SetPixelShader_hook(0, ps);
  D3DDevice_SetVertexDeclaration_hook(0, decl);
  for (u32 i = 0; i < kUfcMirrorTextures; ++i)
    if (textures[i])
      D3DDevice_SetTexture_hook(0, i, textures[i]);
}

void UfcMirrorRestore(uint32_t vs, uint32_t ps, uint32_t decl) {
  D3DDevice_SetVertexShader_hook(0, vs);
  D3DDevice_SetPixelShader_hook(0, ps);
  D3DDevice_SetVertexDeclaration_hook(0, decl);
  auto &s = bd::gpu::state();
  std::lock_guard lock(s.mutex);
  for (u32 i = 0; i < kUfcMirrorTextures; ++i) {
    if (s.textures[i] != g_mirror_saved_textures[i]) {
      s.textures[i] = g_mirror_saved_textures[i];
      bd::gpu::MarkTextureSlotDirty(i);
    }
  }
}
#endif

REXCVAR_DEFINE_BOOL(ufc3_knuckle_trace, false, "ufc3",
                    "Debug (GitHub #5): log the knuckle zone move per mesh, and guest SetTexture calls with a "
                    "texture pointer that is no guest object (command record + parameter block; binds nothing)");

namespace {
inline u32 TraceLoad32(u8 *base, u32 addr) {
  u32 v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
// A D3D texture header lives in the guest heaps (0x40000000 up), 4-byte aligned.
inline bool BadTexturePointer(u32 t) { return t && (t < 0x40000000u || (t & 3)); }
void TraceBadTexture(PPCContext &ctx, u8 *base) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 40) return;
  const u32 rec = ctx.r29.u32;
  REXLOG_INFO("[knuckle] bad SetTexture: sampler {} texture {:08X} lr {:08X} r1 {:08X} r26 {:08X} r27 {:08X} r28 {:08X} "
              "r29 {:08X} r30 {:08X} r31 {:08X}",
              ctx.r4.u32, ctx.r5.u32, static_cast<u32>(ctx.lr), ctx.r1.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32, rec,
              ctx.r30.u32, ctx.r31.u32);
  const u32 lr = static_cast<u32>(ctx.lr);
  if (lr >= 0x8278B588u && lr < 0x8278C300u && rec >= 0x40000000u)  // the state-block walk: r29 = the record
    for (int row = -16; row < 4; ++row) {
      const u32 a = rec + row * 16;
      REXLOG_INFO("[knuckle]   rec {:08X}: {:08X} {:08X} {:08X} {:08X}{}", a, TraceLoad32(base, a), TraceLoad32(base, a + 4),
                  TraceLoad32(base, a + 8), TraceLoad32(base, a + 12), row == 0 ? "  <-" : "");
    }
  // The current parameter block (r26, 16 bytes per slot; the texture is slot * 16), and its record header before it.
  const u32 blk = ctx.r26.u32;
  if (logged.load() <= 2 && blk >= 0x40000000u)
    for (int slot = -1; slot < 40; ++slot) {
      const u32 a = blk + slot * 16;
      REXLOG_INFO("[knuckle]   blk {:08X} slot {:2}: {:08X} {:08X} {:08X} {:08X}", a, slot, TraceLoad32(base, a),
                  TraceLoad32(base, a + 4), TraceLoad32(base, a + 8), TraceLoad32(base, a + 12));
    }
}
}  // namespace

#if defined(SVR_NATIVE_RENDERER)
// SvR: the game's setters run first and keep the device state; the renderer observes.
SVR_HOOK_AFTER(D3DDevice_SetRenderTarget, D3DDevice_SetRenderTarget_hook);
SVR_HOOK_AFTER(D3DDevice_SetDepthStencilSurface, D3DDevice_SetDepthStencilSurface_hook);
SVR_HOOK_AFTER(D3DDevice_SetScissorRect, D3DDevice_SetScissorRect_hook);
SVR_HOOK_AFTER(D3DDevice_SetVertexShader, D3DDevice_SetVertexShader_hook);
SVR_HOOK_AFTER(D3DDevice_SetPixelShader, D3DDevice_SetPixelShader_hook);
SVR_HOOK_AFTER(D3DDevice_SetVertexDeclaration, D3DDevice_SetVertexDeclaration_hook);
REX_EXTERN(__imp__D3DDevice_SetTexture);
extern "C" REX_FUNC(D3DDevice_SetTexture) {
  if (REXCVAR_GET(ufc3_knuckle_trace) && BadTexturePointer(ctx.r5.u32)) {
    TraceBadTexture(ctx, base);
    ctx.r5.u64 = 0;
  }
  SVR_HOOK_EMULATED(D3DDevice_SetTexture)
  PPCContext svr_args;
  ::svr::CopyArgs(svr_args, ctx);
  {
    bd::gpu::SvrZone svr_zone(bd::gpu::SvrZoneId::kGuestD3D);
    __imp__D3DDevice_SetTexture(ctx, base);
  }
  bd::gpu::SvrZone svr_zone(bd::gpu::SvrZoneId::kHookState);
  rex::ppc::HostToGuestFunction<D3DDevice_SetTexture_hook>(svr_args, base);
}
#else
REX_HOOK(D3DDevice_SetViewport, D3DDevice_SetViewport_hook);
REX_HOOK(D3DDevice_SetRenderTarget, D3DDevice_SetRenderTarget_hook);
REX_HOOK(D3DDevice_SetDepthStencilSurface,
         D3DDevice_SetDepthStencilSurface_hook);
REX_HOOK(D3DDevice_SetScissorRect, D3DDevice_SetScissorRect_hook);
REX_HOOK(D3DDevice_SetVertexShader, D3DDevice_SetVertexShader_hook);
REX_HOOK(D3DDevice_SetPixelShader, D3DDevice_SetPixelShader_hook);
REX_HOOK(D3DDevice_SetVertexDeclaration, D3DDevice_SetVertexDeclaration_hook);
REX_HOOK(D3DDevice_SetTexture, D3DDevice_SetTexture_hook);
REX_HOOK(D3DDevice_SetStreamSource, D3DDevice_SetStreamSource_hook);
REX_HOOK(D3DDevice_SetIndices, D3DDevice_SetIndices_hook);
#endif
// SvR 2008: alpha test is read from the device's RB_COLORCONTROL / RB_ALPHA_REF register
// values at draw time (ReadDeviceRenderState), so no setter hooks are needed.
