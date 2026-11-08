// SvR 2008 host resources for the game's own D3D textures and surfaces (see svr_resources.h).

#include "ufc_resources.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dwmapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/output.h"
#include "gpu/resources.h"
#include "gpu/vertex_declaration.h"
#include "ufc_frame_diag.h"
#include "ufc_nr10_probe.h"
#include "ufc_resolve_writeback.h"
#include "../shader_patch_gen.h"

// src/native/crash_trace.cpp: hang watchdog, fed once per presented frame.
void SvrWatchdogHeartbeat();
// src/native/display_size.cpp.
bool SvrDisplaySize(uint32_t &w, uint32_t &h);
bool SvrOnSteamDeck();

REXCVAR_DEFINE_INT32(svr_render_scale, 0, "SvR",
                     "Internal resolution: 1 = 720p (original), 2 = 1440p, 3 = 4K (2160p), "
                     "4 = 2880p. 0 = auto: from the display (1440p on 1080p/1440p screens, 4K on "
                     "4K screens), 720p on Steam Deck and on built-in (laptop) graphics");
REXCVAR_DEFINE_DOUBLE(ufc3_native_scale, 0.0, "ufc3",
                      "Native renderer: internal resolution as a multiple of 1280x720, in steps of "
                      "0.25 up to 4 (1.5 = 1080p, 2 = 1440p, 3 = 4K). 0 = the game's resolution_scale");
REXCVAR_DEFINE_STRING(ufc3_native_keep1x, "1024x512,384x64,64x64", "ufc3",
                      "Native renderer: guest sizes (WxH, comma-separated) of surfaces and resolve "
                      "textures that stay 1x at any scale: the face liquid map (1024x512), the ambient "
                      "cube and its sheet (64x64, 384x64). A resolve between a 1x and a scaled target "
                      "copies the whole surface, so list both ends; a resolve target takes the scale "
                      "of the surface it is first resolved from. 16-bit float targets (the blood "
                      "particle state) always stay 1x; so does the mat stain map, see "
                      "ufc3_native_keep1x_stain");
REXCVAR_DEFINE_BOOL(ufc3_native_keep1x_stain, true, "ufc3",
                    "Native renderer: the mat blood stain map (256x256, its surface and its 565 "
                    "resolve texture) stays 1x at any scale. false = scaled: mat blood slides");
REXCVAR_DECLARE(int32_t, ufc3_native_off);
REXCVAR_DEFINE_STRING(ufc3_native_g4, "log", "ufc3",
                      "Native renderer, NR-10f (par/10.md G4): guest memory the CPU rewrites under a live "
                      "resolve target. log = count and name such binds ([nr10] G4 lines, one sample per "
                      "bound target per frame); fix = also re-fill the host target from that memory; off");

namespace bd::gpu {

// Host-side creation from re:Blue's CreateTexture/CreateSurface hooks (gpu/hooks/resource.cpp).
GuestTexture *SvrCreateHostTexture(u32 width, u32 height, u32 depth, u32 levels, u32 usage,
                                   u32 format, u32 d3d_type);
GuestTexture *SvrCreateHostSurface(u32 width, u32 height, u32 format);

namespace {

std::mutex g_mutex;

// A host resource made for a guest surface or GPU-written texture, with the guest description it
// was made from. Road to WrestleMania unloads characters between scenes and reuses those guest
// objects for other textures (another wrestler's composited skin at another size): a host
// resource kept by address alone then showed the previous character's skin mixed into the new one.
struct HostTarget {
  GuestTexture *host = nullptr;
  u32 desc[2] = {};  // surface: size bits, format; texture: fetch dwords 1 (format, base), 2 (size)
  u32 fill_owed = 0;  // NR-12a: mip levels not yet filled from guest memory (bit per level)
  std::vector<UfcFillRect> covered;  // what resolves wrote into owed levels since (host pixels)
};
std::unordered_map<u32, HostTarget> g_surfaces;  // D3DSurface VA -> host render target
std::unordered_map<u32, HostTarget> g_targets;   // texture VA -> GPU-written host texture

// The texture's GPUTEXTURE_FETCH_CONSTANT (D3DTexture+0x1C), as host-order dwords.
bool ReadFetch(u32 texture_va, rex::graphics::xenos::xe_gpu_texture_fetch_t &fetch) {
  const auto *texture = bd::mem::at<const D3DTexture>(texture_va);
  if (!texture)
    return false;
  for (int i = 0; i < 6; ++i)
    (&fetch.dword_0)[i] = u32(texture->Format.dword[i]);
  return fetch.type == rex::graphics::xenos::FetchConstantType::kTexture;
}

// Sets aside a host resource whose guest object now describes something else. It is not destroyed:
// these host-made targets are linked into the renderer's resolve bookkeeping, which the
// D3DResource_Destroy path doesn't expect for them (freeing one crashed in
// DrainResolveLinksLocked). The struct is kept for good.
// NR-14d2: the CAF editor at 4K retires 10 targets per change (72 MB composites among them), 377
// in a run: video memory went over budget, new heaps took 150-200 ms and GPU waits 300+ ms. A
// retired target's GPU objects are now freed at frame end once nothing can read them
// (SweepRetiredLocked); ufc3_native_off bit 1073741824 = kept, as before.
constexpr int32_t kOffKeepRetired = 1073741824;
struct Retired {
  GuestTexture *host;
  bool target;  // a resolve target (g_targets); surfaces stay as they were
  bool freed;
};
std::vector<Retired> g_retired;
u32 g_retired_live = 0;
void RetireHost(GuestTexture *host, bool target = false) {
  if (!host)
    return;
  g_retired.push_back({host, target, false});
  ++g_retired_live;
  BD_INFO("SvR: {} stale host target(s) set aside, {} not freed", g_retired.size(), g_retired_live);
}

// Whether the renderer has `host` bound or linked anywhere a free would change what is drawn.
// Caller holds state().mutex.
bool HostInUseLocked(const VideoState &s, const GuestTexture *host) {
  // NotifyTextureDestroyed drops an inbound link and framebuffers itself; something resolved
  // out of this one would lose its pixels.
  if (!host->destinationTextures.empty())
    return true;
  if (s.render_target == host || s.depth_stencil == host || s.bound_fb_rt == host ||
      s.bound_fb_ds == host || s.last_drawn_rt == host || s.scene_depth == host ||
      s.last_resolved_dst == host || s.fullscreen_chain_head == host)
    return true;
  return std::find(std::begin(s.textures), std::end(s.textures), host) != std::end(s.textures);
}

// Frees the GPU objects of retired targets nothing needs any more (the parked textures go after
// the frame's fence, as a guest destroy's do). Caller holds g_mutex, not state().mutex.
void SweepRetiredLocked() {
  if (!g_retired_live || (REXCVAR_GET(ufc3_native_off) & kOffKeepRetired))
    return;
  u32 freed = 0;
  double mb = 0;
  for (Retired &r : g_retired) {
    if (r.freed || !r.target || !r.host->texture || UfcWritebackHolds(r.host))
      continue;
    {
      auto &s = state();
      std::lock_guard lock(s.mutex);
      if (HostInUseLocked(s, r.host))
        continue;
    }
    mb += double(r.host->width) * r.host->height * std::max(1u, r.host->depth) *
          plume::RenderFormatSize(r.host->format) * 1.34 / (1024.0 * 1024.0);
    r.host->pendingDestroy = true;
    Video::NotifyTextureDestroyed(r.host);
    ParkTextureGPUObjects(r.host);
    r.freed = true;
    --g_retired_live;
    ++freed;
  }
  if (freed)
    BD_INFO("[nr14d2] freed {} retired target(s), ~{:.0f} MB; {} retired not freed", freed, mb,
            g_retired_live);
}

// NR-12a: targets with a fill still owed, for the draw-time check (SvrFillSampledTargets).
std::unordered_map<GuestTexture *, u32> g_fill_pending;  // host -> texture VA
std::atomic<u32> g_fill_pending_count{0};

// Caller holds g_mutex.
void ForgetPendingFillLocked(GuestTexture *host) {
  if (host && g_fill_pending.erase(host)) {
    host->svrFillPending = false;
    g_fill_pending_count.store(u32(g_fill_pending.size()), std::memory_order_release);
  }
}

// Whether g_targets' entry for texture_va still matches the texture there; a stale one is retired
// and removed. Caller holds g_mutex.
bool TargetCurrent(u32 texture_va, std::unordered_map<u32, HostTarget>::iterator it) {
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch) ||
      (fetch.dword_1 == it->second.desc[0] && fetch.dword_2 == it->second.desc[1]))
    return true;
  BD_INFO("SvR target 0x{:08X}: now {}x{} fmt {} (was another texture) -> rebuilt", texture_va,
          fetch.size_2d.width + 1, fetch.size_2d.height + 1, u32(fetch.format));
  RetireHost(it->second.host, /*target=*/true);
  ForgetPendingFillLocked(it->second.host);
  g_targets.erase(it);
  return false;
}

}  // namespace

// Built-in graphics (Intel UHD / Iris Xe, the Radeon in Ryzen laptops) share the CPU's memory,
// power and heat: rendering above 720p there costs the steady 60.
bool OnBuiltInGraphics() {
  auto *device = Video::HostDevice();
  return device && (device->getDescription().type == plume::RenderDeviceType::INTEGRATED ||
                    device->getCapabilities().uma);
}

u32 SvrAutoRenderScale() {
  static std::atomic<u32> s_auto{0};
  if (const u32 scale = s_auto.load(std::memory_order_relaxed))
    return scale;
  if (!Video::HostDevice())
    return 1;  // not decided before the graphics device exists
  u32 scale = 1;
  if (OnBuiltInGraphics()) {
    BD_INFO("SvR native: built-in graphics, auto resolution is 720p");
  } else if (!SvrOnSteamDeck()) {
    // The larger of the window and the display, so a window on a big screen still renders
    // sharp (downscaled at present); 720p multiples rounded to nearest: 1080/1440 -> 2, 2160 -> 3.
    // The Deck keeps 1x even when docked: its GPU holds 60 fps there.
    u32 w = 0, h = 0, dw = 0, dh = 0;
    if (!Output::RenderSize(w, h))
      h = 0;
    if (SvrDisplaySize(dw, dh))
      h = std::max(h, dh);
    if (h)
      scale = std::clamp(u32((h + 360u) / 720u), 1u, 4u);
  }
  s_auto.store(scale, std::memory_order_relaxed);
  return scale;
}

namespace {

struct ScaleRatio {
  i32 num = 1;
  i32 den = 1;
  bool whole_only = false;
};

const ScaleRatio &Ratio() {
  static const ScaleRatio ratio = [] {
    ScaleRatio r;
    r.whole_only = (REXCVAR_GET(ufc3_native_off) & 2048) != 0;
    const double wanted = REXCVAR_GET(ufc3_native_scale);
    if (wanted > 0.0 && !r.whole_only) {
      r.num = std::clamp(i32(std::lround(wanted * 4.0)), 4, 16);
      r.den = 4;
    } else {
      // UFC: the game's own resolution_scale (the launcher's setting, shared with the emulated
      // renderer), not SvR's pick from the display: a 1x laptop test on a 1200p screen rendered at
      // 2x. svr_render_scale > 0 stays as a dev override.
      const int configured = REXCVAR_GET(svr_render_scale);
      if (configured > 0) {
        r.num = std::min(configured, 4);
      } else {
        const std::string game_scale = rex::cvar::GetFlagByName("resolution_scale");
        r.num = std::clamp(std::atoi(game_scale.c_str()), 1, 4);
      }
    }
    BD_INFO("native: internal resolution {}x{} ({}x)", 1280 * r.num / r.den, 720 * r.num / r.den,
            float(r.num) / float(r.den));
    return r;
  }();
  return ratio;
}

// Half up, also below zero (UFC resolves with a negative top-left in CAF).
i32 ScaleUp(i32 guest_px) {
  const ScaleRatio &r = Ratio();
  const i32 n = guest_px * r.num + r.den / 2;
  return n >= 0 ? n / r.den : -((-n + r.den - 1) / r.den);
}

bool Keep1xListed(u32 width, u32 height) {
  static const std::vector<std::pair<u32, u32>> list = [] {
    std::vector<std::pair<u32, u32>> sizes;
    const std::string text = REXCVAR_GET(ufc3_native_keep1x);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos)
        end = text.size();
      const std::string item = text.substr(at, end - at);
      const size_t x = item.find('x');
      if (x != std::string::npos)
        sizes.emplace_back(u32(std::strtoul(item.c_str(), nullptr, 10)),
                           u32(std::strtoul(item.c_str() + x + 1, nullptr, 10)));
      at = end + 1;
    }
    return sizes;
  }();
  return std::find(list.begin(), list.end(), std::make_pair(width, height)) != list.end();
}

// D3DFMT_A16B16G16R16F and k_16_16_16_16_FLOAT: yVParticleMap's state (64x24, its 32x32 companion).
// Feedback passes read their own last contents; scaled, they drift (FINDINGS-nr2 #6, SDK patch 0011).
constexpr u32 kSurface16F = 0x1A22AB60u;
constexpr u32 kFetch16F = 32u;

// yVStainEffect's mat stain map (FINDINGS-face-blood 1F3E3000): a 256x256 8888 RT (D3D surface format
// 0x1A200186) resolved into a 256x256 k_5_6_5 texture. Its 3x3 filter reads its own last contents, so
// scaled, the mat blood creeps like the particle state (NR-BL1). Size + format, so the other 256x256
// 8888 passes (0x18280186) and the 512x512 0x1A200186 surface stay scaled.
constexpr u32 kSurfaceStain = 0x1A200186u;
constexpr u32 kFetch565 = 4u;

bool Keep1xStain(u32 width, u32 height) {
  return width == 256 && height == 256 && REXCVAR_GET(ufc3_native_keep1x_stain);
}

// At 1x nothing is marked: every target is at guest size anyway, and resolves stay as they were.
bool Keep1xApplies() { return !Ratio().whole_only && Ratio().num != Ratio().den; }

bool Keep1xSurface(u32 width, u32 height, u32 format) {
  return Keep1xApplies() && (format == kSurface16F || Keep1xListed(width, height) ||
                             (format == kSurfaceStain && Keep1xStain(width, height)));
}

bool Keep1xTexture(u32 width, u32 height, u32 fetch_format) {
  return Keep1xApplies() && (fetch_format == kFetch16F || Keep1xListed(width, height) ||
                             (fetch_format == kFetch565 && Keep1xStain(width, height)));
}

}  // namespace

i32 SvrScalePx(i32 guest_px, const GuestTexture *target) {
  return (target && target->keep1x) ? guest_px : ScaleUp(guest_px);
}

float SvrScaleEdge(float guest_px, const GuestTexture *target) {
  if (target && target->keep1x)
    return guest_px;
  // A whole guest pixel lands on the host pixel the scissor and resolve rects use.
  const float whole = std::floor(guest_px);
  if (whole == guest_px)
    return float(ScaleUp(i32(whole)));
  return guest_px * float(Ratio().num) / float(Ratio().den);
}

float SvrAreaScale() {
  return Ratio().whole_only ? 1.0f : float(Ratio().num) / float(Ratio().den);
}

bool SvrWholeScaleOnly() { return Ratio().whole_only; }

GuestTexture *SvrResolveSurface(u32 surface_va) {
  if (!surface_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(surface_va))
    return host;
  std::lock_guard lock(g_mutex);
  const auto *surface = bd::mem::at<const D3DSurface>(surface_va);
  if (!surface)
    return nullptr;
  // Size packing as decoded by D3DSurface_GetDesc (see gpu/d3d.h).
  const u32 bits = surface->SizeBits;
  const u32 format = surface->Format;
  if (auto it = g_surfaces.find(surface_va); it != g_surfaces.end()) {
    if (it->second.desc[0] == bits && it->second.desc[1] == format)
      return it->second.host;
    RetireHost(it->second.host);
    g_surfaces.erase(it);
  }
  const u32 width = (bits >> 18) + 1;
  const u32 height = ((bits >> 3) & 0x7FFF) + 1;
  const bool keep1x = Keep1xSurface(width, height, format);
  const u32 host_width = keep1x ? width : u32(ScaleUp(i32(width)));
  const u32 host_height = keep1x ? height : u32(ScaleUp(i32(height)));
  SvrDiagTimer create(SvrDiag::kTargetCreate);
  GuestTexture *host = SvrCreateHostSurface(host_width, host_height, format);
  if (host)
    host->keep1x = keep1x;
  BD_INFO("SvR surface 0x{:08X}: {}x{} format 0x{:08X} -> host {}x{} {}", surface_va, width, height,
          format, host_width, host_height, host ? "ok" : "FAILED");
  g_surfaces.emplace(surface_va, HostTarget{host, {bits, format}});
  return host;
}

GuestTexture *SvrResolveTarget(u32 texture_va, const GuestTexture *source, bool defer_fill) {
  if (!texture_va)
    return nullptr;
  if (auto *host = HostResourceHeap::FromGuest<GuestTexture>(texture_va))
    return host;
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va); it != g_targets.end() && TargetCurrent(texture_va, it))
    return it->second.host;
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  if (!ReadFetch(texture_va, fetch)) {
    g_targets.emplace(texture_va, HostTarget{});
    return nullptr;
  }
  const u32 width = fetch.size_2d.width + 1;
  const u32 height = fetch.size_2d.height + 1;
  const u32 levels = fetch.mip_max_level + 1;
  // ConvertGuestFormat maps a raw GPUTEXTUREFORMAT byte by its low 6 bits; the tag keeps a small
  // byte from matching one of re:Blue's D3DFormat values (k_8_8_8_8 = 6 = its kIndex32).
  bool keep1x = Keep1xTexture(width, height, u32(fetch.format));
  // NR-11b: a resolve target takes the scale of the surface it is first resolved from, not its
  // size (the career composite's 1024x512 skin, resolved region by region from a scaled 2048x1024
  // surface, matched the liquid map's size and every region was stretched from the whole surface).
  // 16-bit float state stays 1x either way. ufc3_native_off bit 16384: by size only, as in NR-11.
  if (source && Keep1xApplies() && u32(fetch.format) != kFetch16F &&
      !(REXCVAR_GET(ufc3_native_off) & 16384) && source->keep1x != keep1x) {
    BD_INFO("SvR target 0x{:08X}: {}x{} {} like its source {}x{}", texture_va, width, height,
            source->keep1x ? "kept 1x" : "scaled", source->width, source->height);
    keep1x = source->keep1x;
  }
  const u32 host_width = keep1x ? width : u32(ScaleUp(i32(width)));
  const u32 host_height = keep1x ? height : u32(ScaleUp(i32(height)));
  // A resolved cube (UFC's per-frame ambient light, slot 13) must be a real cube: tfetchCube reads
  // the slot through the TextureCube heap. size_2d holds the face size. The host cube has no render
  // target flag, so its faces are only written by region copies.
  const bool cube = fetch.dimension == rex::graphics::xenos::DataDimension::kCube;
  SvrDiagTimer create(SvrDiag::kTargetCreate);
  GuestTexture *host = SvrCreateHostTexture(host_width, host_height, cube ? 6 : 1, levels, 0,
                                            0xC0000000u | u32(fetch.format), cube ? 18 : 0);
  BD_INFO("SvR target 0x{:08X}: {}x{} fmt {} levels {}{} -> host {}x{} {}", texture_va, width, height,
          u32(fetch.format), levels, cube ? " cube" : "", host_width, host_height,
          host ? "ok" : "FAILED");
  u32 fill_owed = 0;
  if (host) {
    host->keep1x = keep1x;
    ApplyResolveTargetSwizzle(host, fetch, texture_va);
    // NR-10g: what the memory holds now, so a partial resolve into one level leaves the others as
    // the hardware would (the CAF composite's head pass reads its own levels back). g_mutex is
    // held, state().mutex isn't: the same order the bind path's write-back uses.
    // NR-12a: what a resolve writes makes that part of the fill dead (32 MB scaled up on the CPU
    // for a 1280x720 target at 4K, most of them resolved whole at once), so the fill waits for the
    // first draw or bind that samples the target and leaves out every rect resolved by then
    // (SvrNoteTargetResolved). ufc3_native_off bit 32768: at once, every level, as in NR-10g.
    if (!(REXCVAR_GET(ufc3_native_off) & 8192)) {
      if (defer_fill && !(REXCVAR_GET(ufc3_native_off) & 32768)) {
        fill_owed = levels >= 32 ? ~0u : (1u << levels) - 1;
        host->svrFillPending = true;
        g_fill_pending[host] = texture_va;
        g_fill_pending_count.store(u32(g_fill_pending.size()), std::memory_order_release);
      } else {
        UfcFillTargetFromGuest(texture_va, host);
      }
    }
  }
  g_targets.emplace(texture_va, HostTarget{host, {fetch.dword_1, fetch.dword_2}, fill_owed});
  return host;
}

namespace {
std::atomic<u32> g_fills_skipped{0};

// Caller holds g_mutex. Fills what is still owed, leaving out what resolves wrote since.
void FillOwedLocked(u32 texture_va, HostTarget &target) {
  if (!target.fill_owed || !target.host) {
    ForgetPendingFillLocked(target.host);
    return;
  }
  const u32 fill = target.fill_owed;
  target.fill_owed = 0;
  ForgetPendingFillLocked(target.host);
  SvrDiagTimer timer(SvrDiag::kTargetFill);  // the new-target cost NR-12 measured, now apart
  UfcFillTargetFromGuest(texture_va, target.host, fill, &target.covered);
  target.covered.clear();
  target.covered.shrink_to_fit();
}

// Caller holds g_mutex.
HostTarget *OwedTargetLocked(u32 texture_va) {
  auto it = g_targets.find(texture_va);
  return it != g_targets.end() && it->second.fill_owed ? &it->second : nullptr;
}
}  // namespace

void SvrPrepareTargetResolve(u32 texture_va, bool exact) {
  if (!g_fill_pending_count.load(std::memory_order_acquire) || exact)
    return;
  std::lock_guard lock(g_mutex);
  if (HostTarget *target = OwedTargetLocked(texture_va))
    FillOwedLocked(texture_va, *target);
}

void SvrNoteTargetResolved(u32 texture_va, u32 level, i32 left, i32 top, i32 right, i32 bottom) {
  if (!g_fill_pending_count.load(std::memory_order_acquire) || level >= 32)
    return;
  std::lock_guard lock(g_mutex);
  HostTarget *target = OwedTargetLocked(texture_va);
  if (!target || !(target->fill_owed & (1u << level)) || left >= right || top >= bottom)
    return;
  const i32 lw = i32(std::max(1u, target->host->width >> level));
  const i32 lh = i32(std::max(1u, target->host->height >> level));
  if (left <= 0 && top <= 0 && right >= lw && bottom >= lh) {
    // The whole level: its fill is dead.
    target->fill_owed &= ~(1u << level);
    std::erase_if(target->covered, [&](const UfcFillRect &r) { return r.level == level; });
    const u32 n = g_fills_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 32 || n % 256 == 0)
      BD_INFO("[nr12a] target {:08X} level {}: whole level resolved, fill skipped (#{})", texture_va,
              level, n);
    if (!target->fill_owed)
      ForgetPendingFillLocked(target->host);
    return;
  }
  target->covered.push_back({level, left, top, right, bottom});
}

void SvrFillSampledTargets(const GuestTexture *const *slots, u32 count) {
  if (!g_fill_pending_count.load(std::memory_order_acquire))
    return;
  bool any = false;
  for (u32 i = 0; i < count && !any; ++i)
    any = slots[i] && slots[i]->svrFillPending;
  if (!any)
    return;
  std::lock_guard lock(g_mutex);
  for (u32 i = 0; i < count; ++i) {
    if (!slots[i] || !slots[i]->svrFillPending)
      continue;
    auto pending = g_fill_pending.find(const_cast<GuestTexture *>(slots[i]));
    if (pending == g_fill_pending.end())
      continue;
    const u32 texture_va = pending->second;
    if (HostTarget *target = OwedTargetLocked(texture_va); target && target->host == slots[i])
      FillOwedLocked(texture_va, *target);
    else
      ForgetPendingFillLocked(const_cast<GuestTexture *>(slots[i]));
  }
}

namespace {

std::shared_mutex g_object_mutex;
// Guest shader object -> host. The microcode address and patch generation let a lookup notice
// that the bloom/DOF patches (src/shader_patch.h) rewrote the microcode since.
struct ShaderObject {
  GuestShader *host;
  u32 function_va;
  bool pixel;
  u32 patch_generation;
};
std::unordered_map<u32, ShaderObject> g_shaders;
std::unordered_map<u64, GuestShader *> g_shaders_by_hash;
std::unordered_map<u32, GuestVertexDeclaration *> g_decls;  // guest declaration -> host
std::unordered_map<u64, GuestVertexDeclaration *> g_decls_by_hash;

// Caller holds g_object_mutex exclusively.
GuestShader *HostShaderFor(u32 function_va, bool pixel) {
  const auto *container = bd::mem::at<const ShaderContainer>(function_va);
  if (!container)
    return nullptr;
  // Keyed like CreateShader's cache lookup (the whole container), plus the stage.
  const u32 length = u32(container->virtualSize) + u32(container->physicalSize);
  const u64 hash = XXH3_64bits(container, length) ^ (pixel ? 0x9E3779B97F4A7C15ull : 0);
  GuestShader *&host = g_shaders_by_hash[hash];
  if (!host)
    host = CreateShader(reinterpret_cast<const be_u32 *>(container),
                        pixel ? ResourceType::PixelShader : ResourceType::VertexShader);
  return host;
}

}  // namespace

void SvrOnShaderCreated(u32 shader_va, u32 function_va, bool pixel) {
  if (!shader_va || !function_va)
    return;
  const u32 generation = ufc3_shader_patch::Generation();
  std::unique_lock lock(g_object_mutex);
  if (GuestShader *host = HostShaderFor(function_va, pixel))
    g_shaders[shader_va] = {host, function_va, pixel, generation};
}

GuestShader *SvrLookupShader(u32 shader_va) {
  if (!shader_va)
    return nullptr;
  const u32 generation = ufc3_shader_patch::Generation();
  {
    std::shared_lock lock(g_object_mutex);
    auto it = g_shaders.find(shader_va);
    if (it == g_shaders.end())
      return nullptr;
    if (it->second.patch_generation == generation)
      return it->second.host;
  }
  // A bloom/DOF toggle changed some microcode: re-key this shader. Both variants are in the
  // cache (tools/nr_shader_extract.py), and each gets its own host shader, so pipelines never mix.
  std::unique_lock lock(g_object_mutex);
  ShaderObject &obj = g_shaders[shader_va];
  if (obj.patch_generation != generation) {
    if (GuestShader *host = HostShaderFor(obj.function_va, obj.pixel))
      obj.host = host;
    obj.patch_generation = generation;
  }
  return obj.host;
}

void SvrOnVertexDeclarationCreated(u32 decl_va, u32 elements_va) {
  if (!decl_va || !elements_va)
    return;
  auto *elements = bd::mem::at<GuestVertexElement>(elements_va);
  if (!elements)
    return;
  // Elements up to the D3DDECL_END terminator (stream 0xFF), padding zeroed for a stable key.
  std::vector<GuestVertexElement> normalized;
  for (const auto *e = elements; normalized.size() < 64; ++e) {
    normalized.push_back(*e);
    normalized.back().padding = 0;
    if (u16(e->stream) == 0xFF)
      break;
  }
  const u64 hash =
      XXH3_64bits(normalized.data(), normalized.size() * sizeof(GuestVertexElement));
  std::unique_lock lock(g_object_mutex);
  GuestVertexDeclaration *&host = g_decls_by_hash[hash];
  if (!host)
    host = CreateVertexDeclaration(normalized.data());
  g_decls[decl_va] = host;
}

GuestVertexDeclaration *SvrLookupVertexDeclaration(u32 decl_va) {
  if (!decl_va)
    return nullptr;
  std::shared_lock lock(g_object_mutex);
  auto it = g_decls.find(decl_va);
  return it != g_decls.end() ? it->second : nullptr;
}

namespace {

// A mirrored texture as last checked: the fetch constant and content it was built from.
struct MirrorCheck {
  u32 fetch[6] = {};
  u64 hash = 0;
  u64 sample = 0;
  u64 frame = 0;
  u32 stable = 0;  // full checks in a row that found the content unchanged
  GuestTexture *host = nullptr;
  u32 base_physical = 0, base_size = 0, mips_physical = 0, mips_size = 0;
};
std::unordered_map<u32, MirrorCheck> g_mirror_checks;  // D3D texture VA -> last check

// NR-10f, par/10.md G4: a header bound as its own resolve target returns the host texture without
// looking at guest memory, so bytes the CPU writes there after the target exists (a reload into the
// same object and memory) never reach the host. One spot sample per bound target per frame tells
// such a write from our own write-back (which queues the range below).
struct G4Check {
  u64 sample = 0;
  u64 frame = 0;
  bool has = false;
  bool written_back = false;  // our write-back changed the memory since the last sample
  u32 base_physical = 0, base_size = 0, mips_physical = 0, mips_size = 0;
};
std::unordered_map<u32, G4Check> g_g4_checks;
std::atomic<u32> g_g4_hits{0};

// Ranges whose bytes changed behind the checks, for the next bind. A queue, not g_mutex: the
// write-back runs while the render thread can hold g_mutex waiting for it.
std::mutex g_invalidate_mutex;
std::vector<std::pair<u32, u32>> g_invalidate;
std::atomic<bool> g_invalidate_any{false};

// Caller holds g_mutex. frame = 0 makes the next check rebuild without comparing.
void DrainMirrorInvalidationsLocked() {
  if (!g_invalidate_any.load(std::memory_order_acquire))
    return;
  std::vector<std::pair<u32, u32>> ranges;
  {
    std::lock_guard lock(g_invalidate_mutex);
    ranges.swap(g_invalidate);
    g_invalidate_any.store(false, std::memory_order_release);
  }
  for (auto &[va, check] : g_mirror_checks) {
    for (const auto &[first, size] : ranges) {
      if ((check.base_size && check.base_physical < first + size &&
           check.base_physical + check.base_size > first) ||
          (check.mips_size && check.mips_physical < first + size &&
           check.mips_physical + check.mips_size > first)) {
        check.frame = 0;
        check.stable = 0;
        break;
      }
    }
  }
  for (auto &[va, check] : g_g4_checks) {
    for (const auto &[first, size] : ranges) {
      if ((check.base_size && check.base_physical < first + size &&
           check.base_physical + check.base_size > first) ||
          (check.mips_size && check.mips_physical < first + size &&
           check.mips_physical + check.mips_size > first)) {
        check.written_back = true;
        break;
      }
    }
  }
}
std::atomic<u64> g_frame{1};

// Hashing every bound texture in full each frame was ~20 MB a frame in a full arena, several ms
// on CPUs with smaller caches. A texture is hashed in full once every kFullCheckFrames frames
// (staggered by address) and spot-checked in between. The game rewrites a texture wholesale
// (Bink frames, menus reloading into reused memory), which the spot check catches at once; a
// partial rewrite it misses shows at the next full check, at most kFullCheckFrames later.
// A texture that changed is hashed in full every frame until two full checks in a row find it
// unchanged: the game may still have been writing it when it was copied (a Bink frame half
// decoded on another thread), and the rest of that write can land where no sample looks. Spot
// checks missed exactly that on slower CPUs (green video, stale crowd).
constexpr u64 kFullCheckFrames = 8;
constexpr u32 kStableBeforeSpotChecks = 2;
constexpr size_t kAlwaysFullBytes = 16 * 1024;
constexpr size_t kSamples = 16, kSampleBytes = 64;

u64 HashSource(const NativeMirrorSource &src) {
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  if (src.base_size)
    hash = XXH3_64bits_withSeed(src.base, src.base_size, hash);
  if (src.mips_size)
    hash = XXH3_64bits_withSeed(src.mips, src.mips_size, hash);
  return hash;
}

u64 SampleRange(const u8 *data, size_t size, u64 seed) {
  if (size <= kSamples * kSampleBytes)
    return XXH3_64bits_withSeed(data, size, seed);
  u8 buf[kSamples * kSampleBytes];
  const size_t step = (size - kSampleBytes) / (kSamples - 1);
  for (size_t i = 0; i < kSamples; ++i)
    std::memcpy(buf + i * kSampleBytes, data + i * step, kSampleBytes);
  return XXH3_64bits_withSeed(buf, sizeof(buf), seed);
}

u64 SampleSource(const NativeMirrorSource &src) {
  u64 hash = XXH3_64bits(src.fetch, sizeof(src.fetch));
  if (src.base_size)
    hash = SampleRange(src.base, src.base_size, hash);
  if (src.mips_size)
    hash = SampleRange(src.mips, src.mips_size, hash);
  return hash;
}

// G4 (see G4Check). Caller holds g_mutex. Log mode counts and names the binds where the sampled
// guest bytes under a live target changed with no write-back of ours in between; fix mode also
// re-fills the host target from that memory (pending resolves written back first, inside the fill).
void G4CheckTargetLocked(u32 texture_va, GuestTexture *host, u64 frame) {
  static const int mode = [] {
    const std::string value = REXCVAR_GET(ufc3_native_g4);
    return value == "fix" ? 2 : value == "log" ? 1 : 0;
  }();
  if (!mode || !host)
    return;
  G4Check &check = g_g4_checks[texture_va];
  if (check.frame == frame)
    return;
  check.frame = frame;
  DrainMirrorInvalidationsLocked();
  NativeMirrorSource src;
  if (!GetNativeMirrorSource(texture_va, src)) {
    check.has = false;
    return;
  }
  const u32 base_physical = src.base ? UfcPhysicalOf(src.base) : 0;
  const u32 mips_physical = src.mips ? UfcPhysicalOf(src.mips) : 0;
  const bool same_memory = check.has && base_physical == check.base_physical &&
                           u32(src.base_size) == check.base_size &&
                           mips_physical == check.mips_physical && u32(src.mips_size) == check.mips_size;
  u64 sample = SampleSource(src);
  if (same_memory && !check.written_back && sample != check.sample) {
    const u32 n = g_g4_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    bool refilled = false;
    if (mode == 2)
      refilled = UfcFillTargetFromGuest(texture_va, host);
    if (n <= 64 || n % 256 == 0)
      BD_INFO("[nr10] G4 #{}: target 0x{:08X} {}x{} fmt {} base {:08X} +{}: guest bytes changed under "
              "the live host target without a write-back (CPU write){}",
              n, texture_va, host->width, host->height, src.fetch[1] & 0x3F, base_physical,
              src.base_size, refilled ? ", host re-filled from memory" : "");
    if (refilled)
      sample = SampleSource(src);  // what the host holds now
  }
  check.sample = sample;
  check.has = true;
  check.written_back = false;
  check.base_physical = base_physical;
  check.base_size = u32(src.base_size);
  check.mips_physical = mips_physical;
  check.mips_size = u32(src.mips_size);
}

}  // namespace

namespace {
std::atomic<u64> g_draws{0}, g_draws_dropped{0};
}  // namespace

void SvrNoteDraw(bool drawn) {
  (drawn ? g_draws : g_draws_dropped).fetch_add(1, std::memory_order_relaxed);
}

// The counters behind SvrOnFrame's log line (reset there every 600 frames), for the NR-10 probe.
void SvrDrawCounts(u64 &drawn, u64 &dropped) {
  drawn = g_draws.load(std::memory_order_relaxed);
  dropped = g_draws_dropped.load(std::memory_order_relaxed);
}

void SvrMirrorsInvalidateRange(u32 physical, u32 size) {
  std::lock_guard lock(g_invalidate_mutex);
  g_invalidate.emplace_back(physical, size);
  g_invalidate_any.store(true, std::memory_order_release);
}

void SvrDrawLogTick(u64 frame);  // ufc_draw_log.cpp

void SvrOnFrame() {
  SvrDiagEndFrame();
  SvrWatchdogHeartbeat();
  const u64 frame = g_frame.fetch_add(1, std::memory_order_relaxed);
  SvrDrawLogTick(frame + 1);
  {
    std::lock_guard lock(g_mutex);
    SweepRetiredLocked();
  }
  // Present rate and draw counts in the log every 600 frames (pacing check; the draw count is
  // what the NR-6 census's "direct" column showed for the same scene on the emulated renderer).
  static auto last = std::chrono::steady_clock::now();
  if (frame == 1)
    BD_INFO("native: first frame presented");
  if (frame % 600 == 0) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last).count();
    last = now;
    BD_INFO("native: frame {}: {:.1f} presents/s, {:.1f} draws/frame, {:.1f} dropped/frame", frame,
            600.0 / seconds, double(g_draws.exchange(0, std::memory_order_relaxed)) / 600.0,
            double(g_draws_dropped.exchange(0, std::memory_order_relaxed)) / 600.0);
#if defined(_WIN32)
    // What the PC was doing, for runs that crawl: on the test laptop the RTX 4060 (which presents
    // through the built-in Radeon) dropped to ~5 presents/s in some unattended runs and not others.
    {
      SYSTEM_POWER_STATUS power{};
      GetSystemPowerStatus(&power);
      LASTINPUTINFO input{sizeof(LASTINPUTINFO), 0};
      GetLastInputInfo(&input);
      DWORD foreground_pid = 0;
      GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
      // DwmFlush blocks this thread until the compositor's next frame, a late frame every 600
      // frames: only worth it when the run is crawling, which is what this line is for.
      const bool crawling = seconds > 20.0;
      const auto flush_start = std::chrono::steady_clock::now();
      const HRESULT flush = crawling ? DwmFlush() : S_OK;
      const double flush_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - flush_start)
                                  .count();
      BD_INFO("native: system: power {}{}, {} s since input, foreground {}, desktop compositor "
              "frame {:.1f} ms{}, monitors {}",
              power.ACLineStatus == 1 ? "AC" : power.ACLineStatus == 0 ? "battery" : "unknown",
              (power.SystemStatusFlag & 1) ? " (battery saver)" : "",
              (GetTickCount() - input.dwTime) / 1000u,
              foreground_pid == GetCurrentProcessId() ? "this game" : "another window", flush_ms,
              FAILED(flush) ? " (DwmFlush failed)" : "", GetSystemMetrics(SM_CMONITORS));
    }
#endif
  }
}

u64 SvrFrameIndex() { return g_frame.load(std::memory_order_relaxed); }

// NR-10c: the CPU rewrote this texture's bytes. Its next SvrResolveTexture rebuilds the mirror
// (frame 0 skips both the same-frame shortcut and the unchanged-hash path).
void SvrMirrorInvalidate(u32 texture_va) {
  std::lock_guard lock(g_mutex);
  if (auto it = g_mirror_checks.find(texture_va); it != g_mirror_checks.end()) {
    it->second.frame = 0;
    it->second.stable = 0;
  }
}

// The game writes texture memory with the CPU whenever it likes (every Bink movie frame, menus
// reloading into reused memory), so a mirror is checked against its source once per frame it's
// used and rebuilt when the fetch constant or the bytes changed. A texture that can't be mirrored
// is remembered the same way, so it isn't retried (or logged) until it changes.
GuestTexture *SvrResolveTexture(u32 texture_va) {
  if (!texture_va)
    return nullptr;
  SvrDiagTimer diag(SvrDiag::kTextureCheck);
  const u64 frame = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  if (auto it = g_targets.find(texture_va);
      it != g_targets.end() && it->second.host && TargetCurrent(texture_va, it)) {
    // NR-12a: levels no resolve has written yet hold what the memory holds when first sampled.
    FillOwedLocked(texture_va, it->second);
    G4CheckTargetLocked(texture_va, it->second.host, frame);
    return it->second.host;
  }
  DrainMirrorInvalidationsLocked();
  MirrorCheck &check = g_mirror_checks[texture_va];
  const bool writeback = UfcWritebackOn();
  if (check.frame == frame) {
    // One check per frame, unless the header at this address now describes something else (the
    // compressor's source alias changes per mip level within one frame, par/10.md G3) or a resolve
    // landed on its memory since.
    bool same = true;
    if (writeback) {
      rex::graphics::xenos::xe_gpu_texture_fetch_t now{};
      same = !ReadFetch(texture_va, now) ||
             (now.dword_1 == check.fetch[1] && now.dword_2 == check.fetch[2] &&
              now.dword_3 == check.fetch[3] && now.dword_5 == check.fetch[5]);
      same = same && !UfcResolvePending(check.base_physical, check.base_size) &&
             !UfcResolvePending(check.mips_physical, check.mips_size);
    }
    if (same) {
      if (UfcNr10On())
        UfcNr10NoteShortcut(texture_va);
      return check.host;
    }
  }
  NativeMirrorSource src;
  if (!GetNativeMirrorSource(texture_va, src)) {
    check.frame = frame;
    return check.host = nullptr;
  }
  if (writeback) {
    // Memory a resolve owns: its pixels first. Own-header binds never get here (g_targets above),
    // which is what keeps ordinary per-frame targets out of this.
    check.base_physical = src.base ? UfcPhysicalOf(src.base) : 0;
    check.base_size = u32(src.base_size);
    check.mips_physical = src.mips ? UfcPhysicalOf(src.mips) : 0;
    check.mips_size = u32(src.mips_size);
    std::memcpy(check.fetch, src.fetch, sizeof(check.fetch));
    UfcResolveToGuestPhysical(check.base_physical, check.base_size, "bind");
    UfcResolveToGuestPhysical(check.mips_physical, check.mips_size, "bind");
    DrainMirrorInvalidationsLocked();
  }
  if (UfcNr10On())
    UfcNr10NoteBind(texture_va, src);
  const size_t bytes = src.base_size + src.mips_size;
  const u64 sample = SampleSource(src);
  const bool full_due = bytes <= kAlwaysFullBytes || check.stable < kStableBeforeSpotChecks ||
                        (frame + (texture_va >> 4)) % kFullCheckFrames == 0;
  if (check.frame != 0 && !full_due && sample == check.sample) {
    check.frame = frame;
    return check.host;
  }
  u64 hash;
  {
    SvrDiagTimer hashing(SvrDiag::kTextureHash);
    hash = HashSource(src);
  }
  SvrDiagAdd(SvrDiag::kHashBytes, bytes);
  check.sample = sample;
  if (check.frame != 0 && check.hash == hash) {
    check.frame = frame;
    ++check.stable;
    return check.host;
  }
  check.stable = 0;
  {
    SvrDiagTimer mirror(SvrDiag::kTextureMirror);
    check.host = GetOrCreateNativeMirror(texture_va, 0);
  }
  check.hash = hash;
  check.frame = frame;
  return check.host;
}

}  // namespace bd::gpu
