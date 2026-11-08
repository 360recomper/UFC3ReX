/**
 * @file    gpu/shaders/guest_shaders.cpp
 * @brief   Turning a guest shader into a host one: cache lookup (UFC 3: from
 *          shader_cache.bin, read at run time) and spec constant linking.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/device.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <plume_render_interface.h>
#include <rex/cvar.h>

REXCVAR_DECLARE(i32, ufc3_native_off);
#include <rex/hash.h>

#include "core/logging.h"
#include "gpu/backend.h"
#include "gpu/host_resource_heap.h"
#include "gpu/pipeline/pso_recorder.h"
#include "gpu/shaders/shader_cache_file.h"
#include "gpu/shaders/shader_linker.h"

REXCVAR_DEFINE_STRING(ufc3_shader_cache, "shader_cache.bin", "ufc3",
                      "Native renderer: the shader cache built from the game files (relative = "
                      "beside the exe). The prelinked variants are read from "
                      "<name>_linked.bin next to it.");

namespace bd::gpu {

namespace {
namespace scf = shader_cache_file;

// UFC 3: the cache is two files built on the player's machine (shader_cache.h),
// read once on the first shader create. A missing or bad file leaves the cache
// empty, so every shader logs a miss and its draws are dropped.
std::once_flag g_cache_once;
scf::Cache g_cache;
scf::LinkedCache g_linked_cache;

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

void LoadShaderCache() {
  std::call_once(g_cache_once, [] {
    std::filesystem::path path = std::filesystem::u8path(REXCVAR_GET(ufc3_shader_cache));
    if (path.is_relative())
      path = ExeDir() / path;
    std::string error;
    if (!scf::Load(path, g_cache, error)) {
      BD_ERROR("native: shader cache {}: {}", path.string(), error);
      g_cache = {};
      return;
    }
    std::filesystem::path linked = path;
    linked.replace_filename(path.stem().string() + "_linked" + path.extension().string());
    if (!scf::LoadLinked(linked, g_cache.fileHash, g_linked_cache, error)) {
      BD_WARN("native: prelinked shaders {}: {} (spec-constant shaders link at run time)",
              linked.string(), error);
      g_linked_cache = {};
    }
    BD_INFO("native: shader cache {} ({} shaders, {} prelinked variants, xxh3 {:016X})",
            path.string(), g_cache.entries.size(), g_linked_cache.entries.size(),
            g_cache.fileHash);
  });
}

ShaderCacheEntry *FindShaderCacheEntry(u64 hash) {
  LoadShaderCache();
  return scf::Find(g_cache, hash);
}

// Guards GuestShader::shader / linkedShaders lookups+inserts only. DXC link
// and createShader run unlocked so precache workers compile in parallel.
std::mutex g_shader_link_mutex;

// X360 exports BD's bloom bright mask into an LDR EDRAM tile, saturating it at
// 1.0 before the gaussian blur, but reblue's FP16 posteff chain keeps the raw
// mask (wc01 authors BLOOM BriMulti=50 -> exports ~59) and the blur spreads it
// into giant white blobs. These two masks are substituted by copies whose
// export is clamped to [0,1].
// SvR 2008: no shader substitutions yet (re:Blue swaps in clamped copies of Blue Dragon's
// bloom bright-pass shaders here, because FP16 render targets don't saturate like EDRAM).
bool BloomMaskClampBlob(u64, const void *&, size_t &) { return false; }

// Publishes the first shader built for gs, a concurrent builder's copy is
// dropped before any caller can observe it.
plume::RenderShader *PublishShader(GuestShader *gs,
                                   std::unique_ptr<plume::RenderShader> sh) {
  std::lock_guard lock(g_shader_link_mutex);
  if (!gs->shader)
    gs->shader = std::move(sh);
  return gs->shader.get();
}

// XenosRecomp's constant_table.h, the parts ReadConstantUse needs.
struct GuestConstantInfo {
  be_u32 name;
  be_u16 registerSet;
  be_u16 registerIndex;
  be_u16 registerCount;
  be_u16 reserved;
  be_u32 typeInfo;
  be_u32 defaultValue;
};
static_assert(sizeof(GuestConstantInfo) == 20);
constexpr u32 kRegisterSetFloat4 = 2;
constexpr u32 kConstantTableBase = 4;  // past the container's size word; constantInfo counts from here
constexpr u32 kConstantTableCountOffset = kConstantTableBase + 12;
constexpr u32 kConstantTableInfoOffset = kConstantTableBase + 16;

GuestConstantUse ReadConstantUse(const be_u32 *function) {
  GuestConstantUse use;
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  const u32 table_at = container->constantTableOffset;
  const u32 limit = container->virtualSize;
  if (!table_at || table_at + kConstantTableInfoOffset + 4 > limit)
    return use;
  const auto *bytes = reinterpret_cast<const u8 *>(function);
  const u32 count = *reinterpret_cast<const be_u32 *>(bytes + table_at + kConstantTableCountOffset);
  const u32 info_at = u32(*reinterpret_cast<const be_u32 *>(bytes + table_at + kConstantTableInfoOffset)) +
                      table_at + kConstantTableBase;
  if (count > limit / sizeof(GuestConstantInfo) ||
      info_at + count * u32(sizeof(GuestConstantInfo)) > limit)
    return use;
  const auto *infos = reinterpret_cast<const GuestConstantInfo *>(bytes + info_at);

  bool declared[256] = {};
  for (u32 i = 0; i < count; ++i) {
    if (u32(infos[i].registerSet) != kRegisterSetFloat4)
      continue;
    const u32 first = infos[i].registerIndex;
    u32 end = std::min(first + u32(infos[i].registerCount), 256u);
    // An array can be read with a0/aL past its declared count: XenosRecomp lets NAME(INDEX) run
    // to register 255, as the hardware's constant file does. UFC's skinning shaders read
    // g_fBruiseLv[a0] with indices past the table; left out, those registers held whatever the
    // upload ring had (select screen 2026-11-07: a dark torn patch on a fighter's cheek and a
    // streak on a shin, native only, gone with every register uploaded). ufc3_native_off bit
    // 134217728: declared counts only, as before.
    if (infos[i].registerCount > 1 && !(REXCVAR_GET(ufc3_native_off) & 134217728))
      end = 256u;
    for (u32 r = first; r < end; ++r)
      declared[r] = true;
  }

  // A gap of a few registers costs less than another compare and copy.
  for (u32 gap = 2;; gap *= 2) {
    use.rangeCount = 0;
    bool fits = true;
    for (u32 r = 0; r < 256 && fits; ++r) {
      if (!declared[r])
        continue;
      if (use.rangeCount) {
        auto &last = use.ranges[use.rangeCount - 1];
        if (r <= u32(last.first) + last.count + gap) {
          last.count = u16(r + 1 - last.first);
          continue;
        }
      }
      if (use.rangeCount == GuestConstantUse::kMaxRanges)
        fits = false;
      else
        use.ranges[use.rangeCount++] = {u16(r), 1};
    }
    if (fits)
      break;
  }
  for (u32 i = 0; i < use.rangeCount; ++i) {
    const auto &range = use.ranges[i];
    for (u32 r = range.first; r < u32(range.first) + range.count; ++r)
      use.mask[r / 64] |= u64(1) << (r % 64);
    use.registerEnd = u16(range.first + range.count);
  }
  use.known = true;
  return use;
}

} // namespace

GuestShader *CreateShader(const be_u32 *function, ResourceType type) {
  // The cache key hashes the whole container, matching how XenosRecomp keyed
  // the entries it generated.
  const auto *container = reinterpret_cast<const ShaderContainer *>(function);
  const u32 hash_len = container->virtualSize + container->physicalSize;
  const u64 hash = XXH3_64bits(function, hash_len);
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  auto *shader = HostResourceHeap::Alloc<GuestShader>(type);
  if (entry) {
    if (entry->guestShader == nullptr) {
      entry->guestShader = shader;
    }
    shader->shaderCacheEntry = entry;
    shader->constantUse = ReadConstantUse(function);
    if (!shader->constantUse.known)
      BD_WARN("native: shader {:016X} has no readable constant table, uploads all registers", hash);
  } else {
    BD_WARN("native: shader miss {:016X} (len {} type {})", hash, hash_len,
            static_cast<u32>(type));
  }
  // Let boot cache replay enqueue any pending PSO that was waiting on this
  // shader's microcode hash now that its host object exists.
  OnShaderCreated(hash);
  return shader;
}

plume::RenderShader *GetOrLinkShader(GuestShader *gs, u32 specConstants) {
  if (!gs)
    return nullptr;
  // hcg*ShaderCreateByHlsl path: shader populated directly, no cache entry.
  if (gs->shader)
    return gs->shader.get();
  const ShaderCacheEntry *entry = gs->shaderCacheEntry;
  if (!entry)
    return nullptr;
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;

  const void *clampBlob = nullptr;
  size_t clampSize = 0;
  if (BloomMaskClampBlob(entry->hash, clampBlob, clampSize)) {
    return PublishShader(gs, device->createShader(clampBlob, clampSize, "main",
                                                  kHostShaderFormat));
  }

  const u8 *cache = g_cache.dxil.data();

  const u32 masked = specConstants & entry->specConstantsMask;
  if (entry->specConstantsMask != 0) {
    std::lock_guard lock(g_shader_link_mutex);
    auto it = gs->linkedShaders.find(masked);
    if (it != gs->linkedShaders.end())
      return it->second.get();
  }

  // Miss: build outside the lock. DXC (per-call instances) and D3D12 shader
  // creation are free-threaded, so precache workers link in parallel and a
  // render thread miss waits only on its own build.
  if (entry->specConstantsMask == 0) {
    // The cache DXIL is already a complete vs/ps.
    return PublishShader(
        gs, device->createShader(cache + entry->dxilOffset, entry->dxilSize,
                                 "main", plume::RenderShaderFormat::DXIL));
  }

  // Spec constant shaders are DXIL libraries with an unresolved
  // g_SpecConstants() export. ufc3_prelink bakes every mask subset of every
  // cached shader into shader_cache_linked.bin, so the pre-linked lookup is
  // the norm. The runtime DXC link below only covers a missing or stale
  // linked file.
  std::unique_ptr<plume::RenderShader> sh;
  const LinkedCacheFileEntry *pre =
      scf::FindLinked(g_linked_cache, entry->hash, masked);
  if (pre) {
    sh = device->createShader(g_linked_cache.dxil.data() + pre->dxilOffset,
                              pre->dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
  } else {
    BD_WARN("GetOrLinkShader: no prelinked variant, runtime DXC link "
            "(hash=0x{:016X} mask=0x{:X})",
            entry->hash, masked);
    std::vector<u8> linked =
        LinkSpecConstant(cache + entry->dxilOffset, entry->dxilSize,
                         gs->type == ResourceType::PixelShader, masked);
    if (linked.empty())
      return nullptr;
    sh = device->createShader(linked.data(), linked.size(), "main",
                              plume::RenderShaderFormat::DXIL);
  }
  std::lock_guard lock(g_shader_link_mutex);
  auto [it, inserted] = gs->linkedShaders.try_emplace(masked, std::move(sh));
  return it->second.get();
}

GuestShader *FindGuestShaderByHash(u64 hash) {
  ShaderCacheEntry *entry = FindShaderCacheEntry(hash);
  if (!entry || !entry->guestShader)
    return nullptr;
  return entry->guestShader;
}

} // namespace bd::gpu
