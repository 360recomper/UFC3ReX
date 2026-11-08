/**
 * @file    gpu/shaders/shader_cache.h
 * @brief   Compiled shader cache: the on-disk formats and the entry the
 *          renderer keeps per cached shader. The build-time ufc3_prelink
 *          compiles it without the rex SDK on its include path, hence
 *          <cstdint> rather than the rex aliases.
 *
 *          UFC 3: SvR linked XenosRecomp's generated shader_cache.cpp into the
 *          exe. The game's shaders can't ship, so UFC builds the cache on the
 *          player's machine from their own disc files instead and loads two
 *          files beside the exe at run time:
 *            shader_cache.bin         XenosRecomp (main.cpp writeBinaryCache)
 *            shader_cache_linked.bin  ufc3_prelink: the spec-constant variants
 *          Both little-endian, entries sorted for binary search.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::gpu {
struct GuestShader;
}

// shader_cache.bin: header, entries x entryCount, zstd(all DXIL blobs).
struct ShaderCacheFileHeader {
  char magic[8]; // "UFCSHC01"
  uint32_t version; // 1
  uint32_t entryCount;
  uint64_t dxilCompressedSize;
  uint64_t dxilSize;
};

struct ShaderCacheFileEntry {
  uint64_t hash; // XXH3 of the whole shader container
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t specConstantsMask;
  uint32_t flags; // kShaderFlag*
};

// The vertex shader fetches at indices it computes: it reads the streams as buffers (NR-8b-2g).
inline constexpr uint32_t kShaderFlagComputedFetch = 1;

// shader_cache_linked.bin: header, entries x entryCount, deflate(all DXIL
// blobs). sourceHash is the XXH3 of the shader_cache.bin it was linked from,
// so a stale file is ignored after the cache is rebuilt.
struct LinkedCacheFileHeader {
  char magic[8]; // "UFCSHL01"
  uint32_t version; // 1
  uint32_t entryCount;
  uint64_t sourceHash;
  uint64_t dxilCompressedSize;
  uint64_t dxilSize;
};

struct LinkedCacheFileEntry {
  uint64_t hash;          // == ShaderCacheEntry::hash
  uint32_t specConstants; // masked g_SpecConstants() value
  uint32_t dxilOffset;    // into the decompressed linked DXIL
  uint32_t dxilSize;
  uint32_t reserved;
};

static_assert(sizeof(ShaderCacheFileHeader) == 32);
static_assert(sizeof(ShaderCacheFileEntry) == 24);
static_assert(sizeof(LinkedCacheFileHeader) == 40);
static_assert(sizeof(LinkedCacheFileEntry) == 24);

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t specConstantsMask;
  bd::gpu::GuestShader *guestShader;
  uint32_t flags;
};
