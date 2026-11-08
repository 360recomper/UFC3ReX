/**
 * @file    gpu/shaders/shader_cache_file.h
 * @brief   Reading and writing the two shader cache files (formats in
 *          shader_cache.h). Shared by the renderer and ufc3_prelink, so no
 *          engine headers: errors come back as text for the caller to log.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gpu/shaders/shader_cache.h"

namespace bd::gpu::shader_cache_file {

struct Cache {
  std::vector<ShaderCacheEntry> entries; // sorted by hash
  std::vector<uint8_t> dxil;             // decompressed
  uint64_t fileHash = 0;                 // XXH3 of the whole file
};

struct LinkedCache {
  std::vector<LinkedCacheFileEntry> entries; // sorted by (hash, specConstants)
  std::vector<uint8_t> dxil;                 // decompressed
};

bool Load(const std::filesystem::path &path, Cache &out, std::string &error);

// Fails (with a reason) when the file was linked from a different cache.
bool LoadLinked(const std::filesystem::path &path, uint64_t sourceHash,
                LinkedCache &out, std::string &error);

// entries must already be sorted; offsets point into dxil.
bool WriteLinked(const std::filesystem::path &path, uint64_t sourceHash,
                 const std::vector<LinkedCacheFileEntry> &entries,
                 const std::vector<uint8_t> &dxil, std::string &error);

ShaderCacheEntry *Find(Cache &cache, uint64_t hash);
const LinkedCacheFileEntry *FindLinked(const LinkedCache &cache, uint64_t hash,
                                       uint32_t specConstants);

} // namespace bd::gpu::shader_cache_file
