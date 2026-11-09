/**
 * @file    gpu/device.cpp
 * @brief   Starting the host device: the plume interface, swap chain, pipeline
 *          layout, copy/resolve pipelines, and the device-lost watch.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>
#if defined(REBLUE_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"
#include "core/shutdown.h"
#include "gpu/bindless_allocator.h"
#include "gpu/constant_buffers.h"
#include "gpu/dred.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/host_heap.h"
#include "gpu/host_resource_heap.h"
#include "gpu/output.h"
#include "gpu/pipeline/pso_recorder.h"
#include "gpu/settings.h"
#include "gpu/surface_registry.h"
#include "gpu/texture_upload.h"
#include "platform/platform.h"
#include "ufc_frame_diag.h"

REXCVAR_DEFINE_STRING(ufc3_native_adapter, "", "ufc3",
                      "Native renderer: graphics adapter to use, by its exact name (empty or not "
                      "found = automatic: the high-performance GPU first)");
REXCVAR_DEFINE_INT32(ufc3_native_target_pool_mb, 0, "ufc3",
                     "Native renderer (NR-12d): render and depth targets are placed in heaps of "
                     "this many MB, with one spare heap made ahead on a background thread. 0 = by "
                     "the internal resolution (64 MB at 1x, 128 at 2x, 256 at 4K, 512 above). The "
                     "default pools take 64 MB heaps and give anything over 32 MB (every 4K "
                     "target) a heap of its own, made while the frame waits (5-40 ms each). "
                     "ufc3_native_off bit 262144 = the default pools");
REXCVAR_DEFINE_INT32(ufc3_native_target_pool_min, 0, "ufc3",
                     "Native renderer (NR-12d): target pool heaps made at start-up, besides the "
                     "spare");
REXCVAR_DEFINE_BOOL(ufc3_native_target_pool_spare, true, "ufc3",
                    "Native renderer (NR-12d): keep one empty target pool heap ready, made on a "
                    "background thread, so a scene change places its new targets instead of "
                    "waiting for a new heap");
REXCVAR_DEFINE_INT32(ufc3_native_texture_pool_mb, 64, "ufc3",
                     "Native renderer (NR-14d): textures built from the game's textures are placed "
                     "in heaps of this many MB (16-1024), with one spare heap made ahead on a "
                     "background thread like the target pool's. ufc3_native_off bit 16777216 = the "
                     "default pools");
REXCVAR_DEFINE_BOOL(ufc3_native_force_unaligned_bc_fallback, false, "ufc3",
                    "Native renderer (NR-WHITEUI): act as if the graphics card can't create DXT "
                    "textures whose size isn't a multiple of 4, so those are decoded to RGBA8 on "
                    "the CPU (what cards without D3D12 UnalignedBlockTexturesSupported get). Test "
                    "switch");
REXCVAR_DECLARE(int32_t, ufc3_native_off);

namespace plume {
#if defined(REBLUE_D3D12)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#else
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
} // namespace plume

namespace bd::gpu {

// src/native/ufc_resources.cpp: the internal resolution as a multiple of 720p.
float SvrAreaScale();

namespace {

float ChannelFromArgb(u32 argb, int shift) {
  return static_cast<float>((argb >> shift) & 0xFF) / 255.0f;
}

#if defined(REBLUE_D3D12)
// NR-12d: a new heap costs the frame 5-40 ms (the driver's allocation, longer for bigger heaps),
// placing a texture in a heap that exists costs well under 1 ms. The allocator keeps one empty
// heap after a free, so a block-size allocation released at once leaves a spare behind; this
// thread makes it whenever the pool has less than a heap free.
// The allocator makes a heap under the pool's lock, so the spare waits until creations have been
// quiet for a while: made in the middle of a scene change, the targets placed after it waited for
// the heap anyway (nr12d2: 9-13 ms placements, two career frames over 10 ms).
// NR-14d: the texture mirror pool keeps a spare the same way (a scene change's new textures waited
// 5-11 ms for a new 64 MB heap, nr14c-f1 `[nr12d] create bc-mirror ... new heap`).
struct PoolSpare {
  const char *name;
  std::mutex mutex;
  std::condition_variable cv;
  bool wanted = false;
  std::chrono::steady_clock::time_point last_request;
};
PoolSpare g_target_spare{"target"};
PoolSpare g_texture_spare{"texture"};
constexpr auto kSpareQuiet = std::chrono::milliseconds(250);

void PoolSpareThread(PoolSpare *spare, D3D12MA::Allocator *allocator, D3D12MA::Pool *pool,
                     u64 block_bytes) {
  bool budget_logged = false;
  for (;;) {
    {
      std::unique_lock lock(spare->mutex);
      spare->cv.wait(lock, [spare] { return spare->wanted; });
      while (std::chrono::steady_clock::now() - spare->last_request < kSpareQuiet)
        spare->cv.wait_until(lock, spare->last_request + kSpareQuiet);
      spare->wanted = false;
    }
    D3D12MA::Statistics stats{};
    pool->GetStatistics(&stats);
    if (stats.BlockBytes - stats.AllocationBytes >= block_bytes)
      continue;
    D3D12MA::Budget local{};
    allocator->GetBudget(&local, nullptr);
    if (local.UsageBytes + block_bytes > local.BudgetBytes) {
      if (!budget_logged)
        BD_INFO("[nr12d] no spare {} heap: {} of {} MB video memory in use", spare->name,
                local.UsageBytes >> 20, local.BudgetBytes >> 20);
      budget_logged = true;
      continue;
    }
    D3D12MA::ALLOCATION_DESC alloc_desc{};
    alloc_desc.CustomPool = pool;
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    info.SizeInBytes = block_bytes;
    info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12MA::Allocation *placeholder = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = allocator->AllocateMemory(&alloc_desc, &info, &placeholder);
    if (placeholder)
      placeholder->Release();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    pool->GetStatistics(&stats);
    BD_INFO("[nr12d] spare {} heap {} ({:.1f} ms): {} heaps, {} MB used of {} MB", spare->name,
            SUCCEEDED(hr) ? "made" : "FAILED", ms, stats.BlockCount,
            stats.AllocationBytes >> 20, stats.BlockBytes >> 20);
  }
}

void WantPoolSpare(PoolSpare &spare) {
  {
    std::lock_guard lock(spare.mutex);
    spare.wanted = true;
    spare.last_request = std::chrono::steady_clock::now();
  }
  spare.cv.notify_one();
}

// NR-14d: mirrors go to the texture pool when they fit a quarter of its heap; bigger ones keep the
// default pools (a custom pool can't give an allocation a heap of its own).
u64 TexturePoolBlockBytes() {
  return u64(std::clamp(REXCVAR_GET(ufc3_native_texture_pool_mb), 16, 1024)) << 20;
}

bool FitsTexturePool(const plume::RenderTextureDesc &desc) {
  const u32 block = plume::RenderFormatBlockWidth(desc.format);
  const u64 texel_bytes = plume::RenderFormatSize(desc.format);
  if (!block || !texel_bytes)
    return false;
  const u64 blocks_w = (u64(desc.width) + block - 1) / block;
  const u64 blocks_h = (u64(desc.height) + block - 1) / block;
  const u64 base = blocks_w * blocks_h * texel_bytes * std::max<u32>(desc.depth, 1) *
                   std::max<u32>(desc.arraySize, 1);
  return (base + base / 3) <= TexturePoolBlockBytes() / 4;  // mips at most a third more
}

// 64 MB at 1x up to 512 MB above 4K: big enough for the largest target at that scale (the 4K
// career composite is 72 MB), small enough that the spare costs little video memory at 1x.
u64 TargetPoolBlockBytes() {
  const int configured = REXCVAR_GET(ufc3_native_target_pool_mb);
  if (configured > 0)
    return u64(std::min(configured, 2048)) << 20;
  const float scale = SvrAreaScale();
  const u64 mb = scale <= 1.25f ? 64 : scale <= 2.0f ? 128 : scale <= 3.0f ? 256 : 512;
  return mb << 20;
}
#endif

} // namespace

plume::RenderColor ArgbToRenderColor(u32 argb) {
  return plume::RenderColor(ChannelFromArgb(argb, 16), ChannelFromArgb(argb, 8),
                            ChannelFromArgb(argb, 0),
                            ChannelFromArgb(argb, 24));
}

bool BuildFramebuffers(VideoState &s) {
  s.framebuffers.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.framebuffers.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    plume::RenderTexture *tex = s.swap_chain->getTexture(i);
    const plume::RenderTexture *color_attachments[1] = {tex};
    plume::RenderFramebufferDesc desc(color_attachments, 1);
    auto fb = s.device->createFramebuffer(desc);
    if (!fb) {
      BD_ERROR("Plume createFramebuffer failed for back buffer {}", i);
      s.framebuffers.clear(); // never leave a partial set, Present's empty()
                              // gate is all-or-nothing
      return false;
    }
    s.framebuffers.push_back(std::move(fb));
  }
  // NR-14e: the real output size (a 4K window on a smaller panel shows here).
  BD_INFO("swap chain {}x{}, {} buffers", s.swap_chain->getWidth(),
          s.swap_chain->getHeight(), count);
  return true;
}

// Rebuilt alongside BuildFramebuffers, because a resize can change the
// surface's negotiated image count. See render_semaphores for why this is not
// sized to kNumFrames.
bool BuildPresentSemaphores(VideoState &s) {
  s.render_semaphores.clear();
  const u32 count = s.swap_chain->getTextureCount();
  s.render_semaphores.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    auto sem = s.device->createCommandSemaphore();
    if (!sem) {
      BD_ERROR("Plume createCommandSemaphore failed for present semaphore {}",
               i);
      s.render_semaphores.clear();
      return false;
    }
    s.render_semaphores.push_back(std::move(sem));
  }
  return true;
}

plume::RenderSampleCounts Video::CvarMSAASampleCount() {
  auto &s = state();
  plume::RenderSampleCounts requested;
  switch (Settings::Get().MSAA()) {
  case 2:
    requested = plume::RenderSampleCount::COUNT_2;
    break;
  case 4:
    requested = plume::RenderSampleCount::COUNT_4;
    break;
  case 8:
    requested = plume::RenderSampleCount::COUNT_8;
    break;
  default:
    return plume::RenderSampleCount::COUNT_1;
  }
  // Power-of-two bitmask: a level is supported iff its bit is set in the mask.
  if ((s.supported_sample_mask & requested) == 0)
    return plume::RenderSampleCount::COUNT_1;
  return requested;
}

// Per-signature time throttle, so a sustained failure stays visible for its
// whole duration instead of going dark after a fixed count, and one noisy site
// cannot drown out another. The printed occurrence number carries the
// suppressed volume.
bool DiagShouldLog(u64 site, const GuestTexture *t, u32 *n_out) {
  *n_out = 0;
  if (!::bd::Settings::Get().Devmode())
    return false;
  const u64 key =
      (site << 56) |
      (t ? (u64(t->width) << 40) | (u64(t->height) << 24) | u64(t->format) : 0);
  using Clock = std::chrono::steady_clock;
  struct DiagState {
    u32 count = 0;
    Clock::time_point last_log{};
  };
  static std::mutex m;
  static std::unordered_map<u64, DiagState> states;
  std::lock_guard lock(m);
  DiagState &st = states[key];
  const u32 n = st.count++;
  *n_out = n;
  const auto now = Clock::now();
  if (n == 0 || now - st.last_log >= std::chrono::seconds(1)) {
    st.last_log = now;
    return true;
  }
  return false;
}

namespace {

// The dims BD believes its backbuffer has, which the implicit RT[0]
// placeholder and the default viewport must use rather than the raw swapchain
// dims: this size defines full-screen for the posteff chain seeding, and a
// swap-sized backbuffer on a non-16:9 output matches none of BD's fit-sized
// composite RTs.
void GuestBackBufferDims(u32 &w, u32 &h) {
  if (!Output::RenderSize(w, h)) {
    w = static_cast<u32>(kDesignCanvasWidth);
    h = static_cast<u32>(kDesignCanvasHeight);
  }
}

// (Re)create the back buffer placeholder's texture and view at w x h. Caller
// guarantees no in-flight GPU references (init, or the resize path's full-ring
// fence wait) and rebinds the SRV slot.
bool CreateBackBufferTexture(VideoState &s, GuestTexture *bb, u32 w, u32 h) {
  bb->framebuffers.clear();
  bb->width = w;
  bb->height = h;
  bb->format = plume::RenderFormat::B8G8R8A8_UNORM;
  bb->mipLevels = 1;
  bb->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  bb->layout = plume::RenderTextureLayout::UNKNOWN;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = w;
  desc.height = h;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = bb->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true; // avoid placed-resource uninit GBV debug fill
  bb->textureHolder = CreateHostTexture(s.device.get(), desc, "backbuffer-rt");
  bb->texture = bb->textureHolder.get();
  if (!bb->texture)
    return false;

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = bb->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  bb->textureView = bb->texture->createTextureView(view_desc);
  return true;
}

#if defined(REBLUE_D3D12)
std::string AdapterName(const WCHAR *name) {
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1)
    return {};
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, name, -1, out.data(), size, nullptr, nullptr);
  return out;
}

// NR-12c: which adapter plume picked, and which one the window's display hangs
// off. On a hybrid laptop the panel is wired to the integrated GPU, so frames
// from the dedicated one are copied across at every present (SPEC-native-fixes
// "Hybrid answered").
void LogAdapterAndDisplay(plume::RenderDevice *device, HWND hwnd) {
  auto *dev = static_cast<plume::D3D12Device *>(device);
  if (!dev || !dev->adapter || !dev->renderInterface ||
      !dev->renderInterface->dxgiFactory)
    return;
  DXGI_ADAPTER_DESC1 chosen = {};
  if (FAILED(dev->adapter->GetDesc1(&chosen)))
    return;
  bd::gpu::SvrPerfRenderAdapter(chosen.VendorId);
  const std::string wanted = REXCVAR_GET(ufc3_native_adapter);
  const std::string chosen_name = AdapterName(chosen.Description);
  BD_INFO("[nr12c] adapter: {} (vendor 0x{:04X}, device 0x{:04X}, {} MB "
          "dedicated, {})",
          chosen_name, chosen.VendorId, chosen.DeviceId,
          u64(chosen.DedicatedVideoMemory) >> 20,
          wanted.empty()            ? "automatic"
          : wanted == chosen_name   ? "named by ufc3_native_adapter"
                                    : "automatic: ufc3_native_adapter not found");
  if (!wanted.empty() && wanted != chosen_name)
    BD_WARN("[nr12c] ufc3_native_adapter \"{}\" is not a usable adapter here",
            wanted);

  const HMONITOR monitor =
      hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
  std::string display_name;
  bool found = false, same = false;
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT i = 0; !found && dev->renderInterface->dxgiFactory->EnumAdapters1(
                                 i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    IDXGIOutput *output = nullptr;
    for (UINT j = 0;
         !found && adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
      DXGI_OUTPUT_DESC output_desc = {};
      if (SUCCEEDED(output->GetDesc(&output_desc)) &&
          output_desc.Monitor == monitor) {
        found = true;
        same = desc.AdapterLuid.LowPart == chosen.AdapterLuid.LowPart &&
               desc.AdapterLuid.HighPart == chosen.AdapterLuid.HighPart;
        display_name = AdapterName(desc.Description);
      }
      output->Release();
    }
    adapter->Release();
  }
  if (!found)
    BD_INFO("[nr12c] display: no adapter lists the window's monitor");
  else if (same)
    BD_INFO("[nr12c] display: on the same adapter");
  else
    BD_INFO("[nr12c] display: on {} (hybrid graphics: frames are copied to it "
            "at present)",
            display_name);
}
#endif

// D3D12 has no single version number, so the highest feature level the device
// accepts stands in for one. plume probes shader model 6.0 alone, so its
// cached shaderModel says nothing.
std::string DescribeBackend(plume::RenderDevice *device) {
#if defined(REBLUE_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(device);
  static const D3D_FEATURE_LEVEL kLevels[] = {
      D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
  levels.NumFeatureLevels = static_cast<u32>(std::size(kLevels));
  levels.pFeatureLevelsRequested = kLevels;
  if (dev && dev->d3d &&
      dev->d3d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels,
                                    sizeof(levels)) >= 0) {
    const u32 fl = static_cast<u32>(levels.MaxSupportedFeatureLevel);
    return std::format("D3D12 {}_{}", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
  }
  return "D3D12";
#else
  auto *dev = static_cast<plume::VulkanDevice *>(device);
  if (!dev)
    return "Vulkan";
#if defined(__APPLE__)
  const u32 drv = dev->physicalDeviceProperties.driverVersion;
  const u32 mvk_major = drv / 10000, mvk_minor = (drv / 100) % 100,
            mvk_patch = drv % 100;
  if (mvk_major >= 1 && mvk_major <= 9)
    return std::format("MoltenVK {}.{}.{}", mvk_major, mvk_minor, mvk_patch);
#endif
  // The physical device's own version, not the 1.2 plume asks the instance for:
  // this is the number a driver bug report needs.
  const u32 v = dev->physicalDeviceProperties.apiVersion;
  return std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(v),
                     VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
#endif
}

plume::RenderFormat
PickDepthStencilFormat([[maybe_unused]] plume::RenderDevice *device) {
#if defined(REBLUE_D3D12)
  const auto vendor = device->getDescription().vendor;
  if ((vendor == plume::RenderDeviceVendor::NVIDIA ||
       vendor == plume::RenderDeviceVendor::INTEL) &&
      (device->getSampleCountsSupported(
           plume::RenderFormat::D24_UNORM_S8_UINT) &
       plume::RenderSampleCount::COUNT_1)) {
    return plume::RenderFormat::D24_UNORM_S8_UINT;
  }
#endif
  return plume::RenderFormat::D32_FLOAT_S8_UINT;
}

plume::RenderFormat PickSceneColorFormat(plume::RenderDevice *device) {
  if (Settings::Get().SceneColorR11G11B10() &&
      (device->getSampleCountsSupported(
           plume::RenderFormat::R11G11B10_FLOAT) &
       plume::RenderSampleCount::COUNT_1)) {
    return plume::RenderFormat::R11G11B10_FLOAT;
  }
  return plume::RenderFormat::R16G16B16A16_FLOAT;
}

} // namespace

VideoState &state() {
  static VideoState s;
  return s;
}

// Registered by the app (ReblueApp::OnPreLaunchModule), invoked from Present.
Video::OverlayDrawHook g_overlay_draw_hook;

bool Video::CreateHostDevice(rex::ui::Window *window) {
  if (!window) {
    BD_ERROR("Video::CreateHostDevice called with null window");
    return false;
  }
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (s.ready) {
    return true;
  }

  if (!s.device) { // pre-Runtime path: no guest memory required
    Output::Init(window);
    plume::RenderWindow render_window{};
    if (!bd::platform::GetNativeRenderWindow(window, render_window)) {
      return false;
    }

    for (u32 i = 0; i < std::size(s.input_slots); ++i) {
      s.input_slots[i].index = i;
    }

#if defined(REBLUE_D3D12)
    s.render_iface = plume::CreateD3D12Interface();
    if (!s.render_iface) {
      BD_ERROR("Plume CreateD3D12Interface failed");
      return false;
    }
    // After the interface, before the device: asking earlier can bind the
    // process-wide settings to the system runtime rather than the Agility
    // D3D12Core.dll that ends up owning the device.
    const bool dred_armed = EnableDred();
#else
    s.render_iface = plume::CreateVulkanInterface();
    if (!s.render_iface) {
      BD_ERROR("Plume CreateVulkanInterface failed");
      return false;
    }
#endif
    // UFC: ufc3_native_adapter names the graphics card (exact adapter name as Windows lists it);
    // empty = plume picks the high-performance GPU (NR-12c; most video memory before Windows 10
    // 1803). A name that isn't found falls back to the automatic pick.
    s.device = s.render_iface->createDevice(REXCVAR_GET(ufc3_native_adapter));
    if (!s.device) {
      BD_ERROR("Plume RenderInterface::createDevice failed");
      return false;
    }
#if defined(REBLUE_D3D12)
    LogAdapterAndDisplay(s.device.get(), render_window);
#endif
    s.backend_info = DescribeBackend(s.device.get());
#if defined(REBLUE_D3D12)
    {
      // NR-WHITEUI: without this, creating a DXT texture whose size isn't a multiple of 4 fails
      // (most menu bars and buttons), so the texture builders decode those to RGBA8 instead.
      D3D12_FEATURE_DATA_D3D12_OPTIONS8 opt8{};
      auto *d3d = static_cast<plume::D3D12Device *>(s.device.get())->d3d;
      const bool queried =
          d3d && SUCCEEDED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &opt8,
                                                    sizeof(opt8)));
      const bool cap = queried && opt8.UnalignedBlockTexturesSupported;
      const bool force = REXCVAR_GET(ufc3_native_force_unaligned_bc_fallback);
      SetUnalignedBlockTexturesSupported(cap && !force);
      BD_INFO("[nr-whiteui] unaligned BC textures: {}{}",
              queried ? (cap ? "supported" : "not supported") : "unknown (OPTIONS8 query failed)",
              force ? ", RGBA8 fallback forced by ufc3_native_force_unaligned_bc_fallback"
                    : (cap ? "" : ", unaligned DXT textures are decoded to RGBA8"));
    }
    if (!(REXCVAR_GET(ufc3_native_off) & 262144) &&
        REXCVAR_GET(ufc3_native_target_pool_mb) >= 0) {
      plume::RenderPoolDesc pool_desc;
      pool_desc.heapType = plume::RenderHeapType::DEFAULT;
      pool_desc.blockSize = TargetPoolBlockBytes();
      pool_desc.minBlockCount =
          u32(std::clamp(REXCVAR_GET(ufc3_native_target_pool_min), 0, 16));
      pool_desc.onlyRenderTargets = true;
      const auto t0 = std::chrono::steady_clock::now();
      auto pool = s.device->createPool(pool_desc);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      if (pool && static_cast<plume::D3D12Pool *>(pool.get())->d3d) {
        s.target_pool = pool.release();
        BD_INFO("[nr12d] target pool: {} MB heaps, {} made at start ({:.1f} ms), spare {}",
                pool_desc.blockSize >> 20, pool_desc.minBlockCount, ms,
                REXCVAR_GET(ufc3_native_target_pool_spare) ? "on" : "off");
        if (REXCVAR_GET(ufc3_native_target_pool_spare)) {
          std::thread(PoolSpareThread, &g_target_spare,
                      static_cast<plume::D3D12Device *>(s.device.get())->allocator,
                      static_cast<plume::D3D12Pool *>(s.target_pool)->d3d,
                      pool_desc.blockSize)
              .detach();
          WantPoolSpare(g_target_spare);
        }
      } else {
        BD_WARN("[nr12d] target pool not made; targets use the default pools");
      }
    }
    if (!(REXCVAR_GET(ufc3_native_off) & 16777216)) {
      plume::RenderPoolDesc pool_desc;
      pool_desc.heapType = plume::RenderHeapType::DEFAULT;
      pool_desc.blockSize = TexturePoolBlockBytes();
      pool_desc.onlyTextures = true;
      auto pool = s.device->createPool(pool_desc);
      if (pool && static_cast<plume::D3D12Pool *>(pool.get())->d3d) {
        s.texture_pool = pool.release();
        BD_INFO("[nr14d] texture pool: {} MB heaps, spare on", pool_desc.blockSize >> 20);
        std::thread(PoolSpareThread, &g_texture_spare,
                    static_cast<plume::D3D12Device *>(s.device.get())->allocator,
                    static_cast<plume::D3D12Pool *>(s.texture_pool)->d3d, pool_desc.blockSize)
            .detach();
        WantPoolSpare(g_texture_spare);
      } else {
        BD_WARN("[nr14d] texture pool not made; textures use the default pools");
      }
    }
#endif
    // bd_msaa is clamped to the color/depth intersection. Everything
    // shader-resolves, so no hardware resolve capability is needed.
    s.depth_stencil_format = PickDepthStencilFormat(s.device.get());
    s.scene_color_format = PickSceneColorFormat(s.device.get());
    const auto color_counts =
        s.device->getSampleCountsSupported(s.scene_color_format);
    const auto depth_counts =
        s.device->getSampleCountsSupported(s.depth_stencil_format);
    s.supported_sample_mask = color_counts & depth_counts;

    s.queue =
        s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    // Each list owns its allocator, so begin() resets independently. present()
    // waits the render semaphore, so the swapchain never scans out an
    // unfinished back buffer.
    for (u32 i = 0; i < kNumFrames; ++i) {
      s.command_lists[i] = s.queue->createCommandList();
      s.fences[i] = s.device->createCommandFence();
      s.acquire_semaphores[i] = s.device->createCommandSemaphore();
    }
    s.command_list = s.command_lists[0].get(); // recording alias -> slot 0
    [[maybe_unused]] const bool dred_breadcrumbs = PrepareDredCommandObjects();

    const bool upload_caps = s.device->getCapabilities().gpuUploadHeap;
    const bool upload_on = upload_caps && Settings::Get().GeometryGPUUpload();
    std::string caps = std::format(
        "GPU caps: {} on {} | scene {} {} | MSAA color=0x{:X} depth=0x{:X} "
        "usable=0x{:X} | geometry GPU_UPLOAD {}",
        s.backend_info, s.device->getDescription().name,
        s.scene_color_format == plume::RenderFormat::R11G11B10_FLOAT
            ? "R11G11B10"
            : "RGBA16F",
        s.depth_stencil_format == plume::RenderFormat::D24_UNORM_S8_UINT
            ? "D24S8"
            : "D32S8",
        color_counts, depth_counts, s.supported_sample_mask,
        upload_on      ? "on"
        : !upload_caps ? "unsupported"
                       : "off (bd_geometry_gpu_upload)");
#if defined(REBLUE_D3D12)
    const char *dred_state = !dred_armed        ? "off"
                             : dred_breadcrumbs ? "on"
                                                : "armed, no breadcrumb writes";
    caps += std::format(" | DRED {}", dred_state);
#endif
    BD_INFO("{}", caps);

    // kNumFrames + 1: a flip model swapchain needs one back buffer beyond the
    // frames in flight so acquiring N+1 never waits on scanout.
    plume::RenderSwapChainDesc desc(render_window,
                                    plume::RenderFormat::B8G8R8A8_UNORM,
                                    kNumFrames + 1, false, kNumFrames);
    s.swap_chain = s.queue->createSwapChain(desc);
#if !defined(REBLUE_D3D12)
    // plume's VulkanSwapChain defers VkSwapchain creation to resize(), so a
    // fresh swapchain is always empty until the first resize. The D3D12 backend
    // creates its swapchain in the constructor, so this stays Vulkan-only.
    if (s.swap_chain) {
      s.swap_chain->resize();
    }
#endif
    if (!s.swap_chain || s.swap_chain->isEmpty()) {
      BD_ERROR("Plume createSwapChain failed");
      return false;
    }
    if (!BuildFramebuffers(s)) {
      return false;
    }
    if (!BuildPresentSemaphores(s)) {
      return false;
    }
    if (!BuildPipelineLayout(s)) {
      return false;
    }
    if (!BuildCopyPipeline(s)) {
      return false;
    }
    // overlay drawer uploads textures through this
    if (!TryInit()) {
      BD_ERROR("TryInit failed, shader constants disabled");
    }
  }

  if (!rex::Runtime::instance()) {
    return true; // guest tail waits for the post-Runtime call
  }

  // Must precede any HostResourceHeap::Alloc.
  if (!bd::gpu::HostHeap::Get().Init()) {
    BD_ERROR("HostHeap init failed");
    return false;
  }

  // Implicit back buffer placeholder. BD draws post-bloom menu/UI without ever
  // calling SetRenderTarget (RT[0] is implicitly the back buffer on X360), so
  // BindDrawFramebuffer falls back to s.back_buffer_surface for such draws.
  {
    auto *bb = bd::gpu::HostResourceHeap::Alloc<bd::gpu::GuestTexture>(
        bd::gpu::ResourceType::RenderTarget);
    if (!bb) {
      BD_ERROR("HostResourceHeap::Alloc for back-buffer GuestTexture failed");
      return false;
    }
    u32 bb_w = 0, bb_h = 0;
    GuestBackBufferDims(bb_w, bb_h);
    if (!CreateBackBufferTexture(s, bb, bb_w, bb_h)) {
      BD_ERROR("Plume createTexture for back-buffer failed");
      return false;
    }

    // Inline SRV register (BindTextureSRV would re-take the held s.mutex) so
    // the per-Present blit can sample this back buffer.
    const u32 slot = AllocateSlot(s);
    if (slot == kInvalidDescriptorIndex) {
      BD_ERROR("Back-buffer SRV bind failed: bindless heap full");
      return false;
    }
    s.texture_descriptor_set->setTexture(
        slot, bb->texture, plume::RenderTextureLayout::SHADER_READ,
        bb->textureView.get());
    bb->descriptorIndex = slot;

    s.back_buffer_surface = bb;
  }
  // Default viewport = the BD-believed backbuffer, not the raw swapchain.
  s.viewport.width = static_cast<float>(s.back_buffer_surface->width);
  s.viewport.height = static_cast<float>(s.back_buffer_surface->height);
  s.dirtyStates.viewport = true;
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
  InitGPUProfiler(static_cast<plume::D3D12Device *>(s.device.get())->d3d,
                  static_cast<plume::D3D12CommandQueue *>(s.queue.get())->d3d);
#endif
  s.ready = true;
  BD_DEBUG("Plume backend ready: {}x{} ({} buffers, {} bindless slots)",
           static_cast<u32>(s.viewport.width),
           static_cast<u32>(s.viewport.height),
           static_cast<u32>(s.framebuffers.size()), kBindlessTextureCount);

  // Background-precompile the static PSO cache. Entries whose shaders/decls do
  // not exist yet are deferred until CreateShader/CreateVertexDeclaration.
  ReplayBootCache();
  return true;
}

void Video::SyncBackBufferSizeLocked() {
  auto &s = state();
  GuestTexture *bb = s.back_buffer_surface;
  if (!bb)
    return;
  u32 w = 0;
  u32 h = 0;
  GuestBackBufferDims(w, h);
  if (bb->width == w && bb->height == h)
    return;

  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }

  if (!CreateBackBufferTexture(s, bb, w, h)) {
    BD_ERROR("Back-buffer resize to {}x{} failed", w, h);
    return;
  }
  if (bb->descriptorIndex != kInvalidDescriptorIndex) {
    s.texture_descriptor_set->setTexture(
        bb->descriptorIndex, bb->texture,
        plume::RenderTextureLayout::SHADER_READ, bb->textureView.get());
  }
  s.viewport.width = static_cast<float>(w);
  s.viewport.height = static_cast<float>(h);
  s.dirtyStates.viewport = true;
  BD_INFO("[output-res] back buffer -> {}x{}", w, h);
}

void Video::ResizeTexture(GuestTexture *tex, u32 width, u32 height) {
  if (!tex || !tex->texture || (tex->width == width && tex->height == height))
    return;
  if (tex->viewDimension != plume::RenderTextureViewDimension::TEXTURE_2D)
    return;
  auto *device = HostDevice();
  if (!device)
    return;

  NotifyTextureDestroyed(tex);
  ParkTextureGPUObjects(tex);

  const bool is_depth = IsDepthFormat(tex->format);
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = tex->mipLevels ? tex->mipLevels : 1;
  desc.arraySize = 1;
  desc.format = tex->format;
  if (is_depth) {
    desc.flags = plume::RenderTextureFlag::DEPTH_TARGET;
  } else if (IsRenderTargetCapable(tex->format)) {
    desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  } else {
    desc.flags = plume::RenderTextureFlag::NONE;
  }
  desc.multisampling.sampleCount = tex->sampleCount;
  desc.committed = false;

  tex->textureHolder = CreateHostTexture(device, desc, "resized-texture");
  tex->texture = tex->textureHolder.get();
  MarkAllTextureSlotsDirty();
  if (!tex->texture) {
    BD_ERROR("Texture resize to {}x{} failed", width, height);
    return;
  }
  tex->width = width;
  tex->height = height;
  tex->layout = plume::RenderTextureLayout::UNKNOWN;
  tex->framebufferAttached = false;
  if (!is_depth)
    BindTextureSRV(tex);
}

void Video::BeginShutdown() {
  // No lock: a guest thread parked inside Present (its overlay hook marshals to
  // the UI thread, which is the thread running the shutdown) holds s.mutex, so
  // taking it here would deadlock stage 1 of the sequence.
  state().shutting_down.store(true, std::memory_order_release);
}

void Video::Shutdown(const std::function<void()> &ui_pump) {
  auto &s = state();

  // Bounded, not a plain lock: the render thread can be parked in Present's
  // overlay marshal holding s.mutex, waiting on the UI thread running this.
  // Skipping the drain beats a quit that hangs until the watchdog kills it.
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(250);
  std::unique_lock lock(s.mutex, std::defer_lock);
  while (!lock.try_lock()) {
    if (ui_pump)
      ui_pump();
    if (Clock::now() >= deadline) {
      BD_WARN("Shutdown: renderer busy, skipping GPU drain");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  s.ready = false;

  // Fences never signal on a removed device, and the swap chain is already
  // invalid, so flushing anything here would just hang until the watchdog.
  if (DeviceIsLost()) {
    BD_WARN("Shutdown: device lost, skipping GPU drain");
    return;
  }

  // Whatever the guest was recording is abandoned: ending the list keeps plume
  // consistent, and it is never submitted.
  if (s.command_list_open && s.command_list) {
    s.command_list->end();
    s.command_list_open = false;
  }

  // The drain proper. Same wait the resize path uses, over every slot with work
  // outstanding, so nothing is still executing when the process dies.
  if (s.queue) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (!s.command_list_submitted[i] || !s.fences[i])
        continue;
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }

  // Released while the device is still alive, so the surface goes back to the
  // compositor before the process dies. Everything else is left for process
  // teardown on purpose. See the Shutdown() contract.
  s.framebuffers.clear();
  s.render_semaphores.clear();
  for (u32 i = 0; i < kNumFrames; ++i)
    s.acquire_semaphores[i].reset();
  s.swap_chain.reset();
}

// Substituted for content textures reblue cannot resolve, so the draw renders
// with its vertex color instead of sampling an unbound slot. It is cleared to
// opaque WHITE (not a green marker): a failed mirror shows as a flat white quad
// (NR-WHITEUI: menus on GPUs without unaligned BC support).
namespace {
GuestTexture *g_debug_texture = nullptr;
} // namespace

GuestTexture *GetOrCreateDebugTexture() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (g_debug_texture)
    return g_debug_texture;
  if (!s.ready)
    return nullptr;
  BeginCommandList(s);
  if (!s.command_list_open)
    return nullptr;

  auto *t = new GuestTexture(ResourceType::RenderTarget);
  t->width = 4;
  t->height = 4;
  t->format = plume::RenderFormat::R8G8B8A8_UNORM;
  t->mipLevels = 1;
  t->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;

  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = t->width;
  desc.height = t->height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = t->format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.committed = true; // avoid placed-resource uninit GBV debug fill
  t->textureHolder = CreateHostTexture(s.device.get(), desc, "debug-fallback");
  if (!t->textureHolder) {
    BD_ERROR(
        "GetOrCreateDebugTexture: createTexture for fallback texture failed");
    delete t;
    return nullptr;
  }
  t->texture = t->textureHolder.get();

  plume::RenderTextureViewDesc view_desc;
  view_desc.format = t->format;
  view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  view_desc.mipLevels = 1;
  t->textureView = t->texture->createTextureView(view_desc);

  const u32 slot = AllocateSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    BD_ERROR("GetOrCreateDebugTexture: bindless heap full, fallback texture "
             "dropped");
    delete t;
    return nullptr;
  }
  s.texture_descriptor_set->setTexture(slot, t->texture,
                                       plume::RenderTextureLayout::SHADER_READ,
                                       t->textureView.get());
  t->descriptorIndex = slot;

  plume::RenderTextureBarrier to_rt(t->texture,
                                    plume::RenderTextureLayout::COLOR_WRITE);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &to_rt, 1);
  if (plume::RenderFramebuffer *fb = GetFramebuffer(s, t, nullptr)) {
    s.command_list->setFramebuffer(fb);
    s.command_list->clearColor(0, plume::RenderColor(1.0f, 1.0f, 1.0f, 1.0f));
    s.command_list->setFramebuffer(nullptr);
    // The draw framebuffer was just replaced: the next draw must bind it again (a texture made
    // mid-frame, e.g. an SvR resolve target first used between draws, otherwise leaves the list
    // with no framebuffer).
    s.draw_framebuffer_bound = false;
  }
  plume::RenderTextureBarrier to_read(t->texture,
                                      plume::RenderTextureLayout::SHADER_READ);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &to_read, 1);
  t->layout = plume::RenderTextureLayout::SHADER_READ;

  g_debug_texture = t;
  return g_debug_texture;
}

namespace {
// Quad list expansion index buffer capacity, matching Video::QuadlistMaxQuads.
constexpr u32 kQuadlistMaxQuads = 16384;
} // namespace

// Quad q's four vertices {4q..4q+3} become {4q+0,4q+1,4q+2, 4q+0,4q+2,4q+3}.
// Fixed capacity, built once: rebuilding mid-frame would free a buffer the
// open command list still references.
const plume::RenderIndexBufferView *Video::QuadlistExpansionIBView() {
  static std::unique_ptr<plume::RenderBuffer> s_buffer;
  static plume::RenderIndexBufferView s_view{};
  static std::once_flag s_once;
  std::call_once(s_once, []() {
    auto *device = HostDevice();
    if (!device)
      return;
    constexpr u32 kBytes = kQuadlistMaxQuads * 6 * sizeof(u32);
    auto desc = plume::RenderBufferDesc::IndexBuffer(
        kBytes, GeometryHeapType(device, GeometryClass::Static));
    s_buffer = CreateHostBuffer(device, desc, "quadlist-ib");
    if (!s_buffer)
      return;
    auto *mapped = static_cast<u32 *>(s_buffer->map());
    if (!mapped) {
      BD_ERROR("QuadlistExpansionIBView: map() null (resource null-backed?), "
               "quad-list draws will be unindexed");
      s_buffer.reset();
      return;
    }
    for (u32 q = 0; q < kQuadlistMaxQuads; ++q) {
      const u32 v = q * 4;
      u32 *o = mapped + q * 6;
      o[0] = v + 0;
      o[1] = v + 1;
      o[2] = v + 2;
      o[3] = v + 0;
      o[4] = v + 2;
      o[5] = v + 3;
    }
    s_buffer->unmap();
    s_view.buffer = s_buffer->at(0);
    s_view.size = kBytes;
    s_view.format = plume::RenderFormat::R32_UINT;
  });
  return s_buffer ? &s_view : nullptr;
}

u32 Video::QuadlistMaxQuads() { return kQuadlistMaxQuads; }

std::unique_ptr<plume::RenderBuffer>
CreateHostBuffer(plume::RenderDevice *device,
                 const plume::RenderBufferDesc &desc, const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostBuffer({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto buffer = device->createBuffer(desc);
  // plume returns a non-null wrapper even when the allocation failed, so probe
  // the backend handle. GPU_UPLOAD makes that reachable: DEVICE_LOCAL and
  // HOST_VISIBLE are requiredFlags, so unlike UPLOAD it has no fallback heap.
  if (!buffer
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12Buffer *>(buffer.get())->d3d == nullptr
#else
      || static_cast<plume::VulkanBuffer *>(buffer.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR(
        "CreateHostBuffer({}) failed: backend resource null (size={} bytes)",
        tag ? tag : "?", desc.size);
    // A whole-device failure (driver reset/TDR/sleep) nulls every create, so
    // turn that into an explicit device-lost dialog instead of a silent
    // cascade.
    CheckDeviceRemoved(tag ? tag : "buffer");
    return nullptr;
  }
  return buffer;
}

std::unique_ptr<plume::RenderTexture>
CreateHostTexture(plume::RenderDevice *device,
                  const plume::RenderTextureDesc &desc, const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostTexture({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  // NR-12d: single-sample render/depth targets go to the target pool (its heaps are only for
  // those, so other textures stay in the default pools).
  auto &s = state();
  const bool own_device = device == s.device.get() && !desc.committed &&
                          desc.multisampling.sampleCount == plume::RenderSampleCount::COUNT_1;
  const bool target = (desc.flags & (plume::RenderTextureFlag::RENDER_TARGET |
                                     plume::RenderTextureFlag::DEPTH_TARGET)) != 0;
  plume::RenderPool *pool = nullptr;
  if (own_device && target)
    pool = s.target_pool;
#if defined(REBLUE_D3D12)
  // NR-14d: mirrors of the game's textures (sampled only) go to the texture pool.
  else if (own_device && s.texture_pool && tag && std::strcmp(tag, "bc-mirror") == 0 &&
           !(desc.flags & plume::RenderTextureFlag::UNORDERED_ACCESS) && FitsTexturePool(desc))
    pool = s.texture_pool;
#endif
  const auto t0 = std::chrono::steady_clock::now();
  auto texture = pool ? pool->createTexture(desc) : device->createTexture(desc);
#if defined(REBLUE_D3D12)
  if (pool && pool == s.texture_pool &&
      (!texture || static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr)) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 8)
      BD_WARN("[nr14d] texture pool refused {}x{}x{} fmt {}; default pools", desc.width,
              desc.height, desc.arraySize, u32(desc.format));
    pool = nullptr;
    texture = device->createTexture(desc);
  }
  // A heap the allocator hasn't handed out before (a new block, or a texture committed alone) is
  // the driver allocation the frame waits on; slow creates and new heaps are logged.
  if (texture) {
    if (pool && pool == s.target_pool && REXCVAR_GET(ufc3_native_target_pool_spare))
      WantPoolSpare(g_target_spare);
    else if (pool)
      WantPoolSpare(g_texture_spare);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    auto *d3d_texture = static_cast<plume::D3D12Texture *>(texture.get());
    ID3D12Heap *heap =
        d3d_texture->allocation ? d3d_texture->allocation->GetHeap() : nullptr;
    static std::mutex heaps_mutex;
    static std::unordered_map<ID3D12Heap *, u32> heaps;
    bool new_heap = heap == nullptr;
    {
      std::lock_guard heaps_lock(heaps_mutex);
      if (heap)
        new_heap = heaps.emplace(heap, 0).second;
    }
    if (ms >= 0.2 || new_heap) {
      const u64 bytes =
          d3d_texture->allocation ? d3d_texture->allocation->GetSize() : 0;
      BD_INFO("[nr12d] create {} {}x{}x{} levels {} fmt {} {:.1f} MB {:.2f} ms {}",
              tag ? tag : "?", desc.width, desc.height,
              desc.dimension == plume::RenderTextureDimension::TEXTURE_3D
                  ? desc.depth
                  : desc.arraySize,
              desc.mipLevels, u32(desc.format), double(bytes) / 1048576.0, ms,
              heap == nullptr ? "committed"
              : new_heap      ? (pool ? "new pool heap" : "new heap")
                              : (pool ? "pool" : "placed"));
    }
  }
#endif
  if (!texture
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12Texture *>(texture.get())->d3d == nullptr
#else
      ||
      static_cast<plume::VulkanTexture *>(texture.get())->vk == VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR(
        "CreateHostTexture({}) failed: backend resource null ({}x{} fmt={})",
        tag ? tag : "?", desc.width, desc.height,
        static_cast<u32>(desc.format));
    CheckDeviceRemoved(tag ? tag : "texture");
    return nullptr;
  }
  return texture;
}

std::unique_ptr<plume::RenderPipeline>
CreateHostGraphicsPipeline(plume::RenderDevice *device,
                           const plume::RenderGraphicsPipelineDesc &desc,
                           const char *tag) {
  if (!device) {
    BD_ERROR("CreateHostGraphicsPipeline({}): no host device", tag ? tag : "?");
    return nullptr;
  }
  auto pipeline = device->createGraphicsPipeline(desc);
  // Both backends fail soft: a failed
  // vkCreateGraphicsPipelines/CreatePipelineState still hands back a live
  // wrapper holding a null pipeline, which only faults later at bind time.
  if (!pipeline
#if defined(REBLUE_D3D12)
      || static_cast<plume::D3D12GraphicsPipeline *>(pipeline.get())->d3d ==
             nullptr
#else
      || static_cast<plume::VulkanGraphicsPipeline *>(pipeline.get())->vk ==
             VK_NULL_HANDLE
#endif
  ) {
    BD_ERROR("CreateHostGraphicsPipeline({}) failed: backend pipeline null",
             tag ? tag : "?");
    CheckDeviceRemoved(tag ? tag : "pipeline");
    return nullptr;
  }
  return pipeline;
}

plume::RenderHeapType GeometryHeapType(plume::RenderDevice *device,
                                       GeometryClass cls) {
  // plume reports gpuUploadHeap as plain UMA on Vulkan, so this branch is
  // APU-only there. On D3D12 it also covers ReBAR discrete.
  const bool caps_ok =
      device != nullptr && device->getCapabilities().gpuUploadHeap;
  const bool gpu_upload_ok = caps_ok && Settings::Get().GeometryGPUUpload();
  return gpu_upload_ok && cls == GeometryClass::Static
             ? plume::RenderHeapType::GPU_UPLOAD
             : plume::RenderHeapType::UPLOAD;
}

void Video::SetOverlayDrawHook(OverlayDrawHook hook) {
  g_overlay_draw_hook = std::move(hook);
}

plume::RenderDevice *Video::HostDevice() { return state().device.get(); }

plume::RenderFormat Video::DepthStencilFormat() {
  return state().depth_stencil_format;
}

plume::RenderFormat Video::SceneColorFormat() {
  return state().scene_color_format;
}

Video::VideoMemory Video::MemoryUsage() {
  VideoMemory m;
  auto *device = state().device.get();
  if (!device)
    return m;
#if defined(REBLUE_D3D12)
  auto *dev = static_cast<plume::D3D12Device *>(device);
  IDXGIAdapter3 *adapter = nullptr;
  if (!dev->adapter ||
      dev->adapter->QueryInterface(IID_PPV_ARGS(&adapter)) < 0)
    return m;
  DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
  if (adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                    &info) >= 0) {
    m.used = info.CurrentUsage;
    m.budget = info.Budget;
  }
  adapter->Release();
#else
  auto *dev = static_cast<plume::VulkanDevice *>(device);
  if (!dev->allocator)
    return m;
  const VkPhysicalDeviceMemoryProperties *props = nullptr;
  vmaGetMemoryProperties(dev->allocator, &props);
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
  vmaGetHeapBudgets(dev->allocator, budgets);
  for (u32 i = 0; i < props->memoryHeapCount; ++i) {
    if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      m.used += budgets[i].usage;
      m.budget += budgets[i].budget;
    }
  }
#endif
  return m;
}

u32 Video::OutputWidth() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getWidth() : 0;
}

u32 Video::OutputHeight() {
  auto &s = state();
  return s.swap_chain ? s.swap_chain->getHeight() : 0;
}

std::string Video::GetDeviceName() {
  auto &s = state();
  return s.device ? s.device->getDescription().name : std::string("unknown");
}

const char *Video::BackendName() {
#if defined(REBLUE_D3D12)
  return "D3D12";
#else
  return "Vulkan";
#endif
}

const std::string &Video::GetBackendInfo() { return state().backend_info; }

plume::RenderPipelineLayout *Video::MainPipelineLayout() {
  return state().pipeline_layout.get();
}

u32 CurrentRenderPassId() {
  // g_currentRenderPassId (guest global, big-endian). Always-mapped XEX data,
  // but guard the translate anyway.
#if defined(SVR_NATIVE_RENDERER)
  return 0;  // Blue Dragon global; SvR has no equivalent (only labels logs)
#else
  constexpr u32 kCurrentRenderPassIdVa = 0x82777474;
  return bd::mem::load<u32>(kCurrentRenderPassIdVa);
#endif
}

u32 ComputeTexturePitch(const GuestTexture *tex) {
  // X360 texture pitch alignment is 0x100. RenderFormatSize is the per-texel
  // (uncompressed) byte size, and UNKNOWN -> 0, which the callers treat as
  // "skip this texture".
  constexpr u32 kPitchAlignment = 0x100u;
  const u32 texel = plume::RenderFormatSize(tex->format);
  return (tex->width * texel + kPitchAlignment - 1u) & ~(kPitchAlignment - 1u);
}

} // namespace bd::gpu
