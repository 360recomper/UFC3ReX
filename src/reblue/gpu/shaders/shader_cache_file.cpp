/**
 * @file    gpu/shaders/shader_cache_file.cpp
 * @brief   Reading and writing shader_cache.bin / shader_cache_linked.bin.
 *          No engine headers (ufc3_prelink links it too).
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/shaders/shader_cache_file.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#include <xxhash.h>
#include <zstd.h>

namespace bd::gpu::shader_cache_file {
namespace {

constexpr char kCacheMagic[8] = {'U', 'F', 'C', 'S', 'H', 'C', '0', '1'};
constexpr char kLinkedMagic[8] = {'U', 'F', 'C', 'S', 'H', 'L', '0', '1'};

bool ReadAll(const std::filesystem::path &path, std::vector<uint8_t> &out,
             std::string &error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "can't open " + path.string();
    return false;
  }
  out.resize(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(out.data()), out.size())) {
    error = "can't read " + path.string();
    return false;
  }
  return true;
}

// Header + entries + compressed blob, all inside the file, offsets inside the
// decompressed blob.
template <typename Header, typename Entry>
bool CheckLayout(const std::vector<uint8_t> &file, const Header &h,
                 const char (&magic)[8], std::string &error) {
  if (std::memcmp(h.magic, magic, 8) != 0 || h.version != 1) {
    error = "wrong magic or version";
    return false;
  }
  const uint64_t need = sizeof(Header) + uint64_t(h.entryCount) * sizeof(Entry) +
                        h.dxilCompressedSize;
  if (need != file.size()) {
    error = "size mismatch (" + std::to_string(file.size()) + " bytes, header says " +
            std::to_string(need) + ")";
    return false;
  }
  const auto *e = reinterpret_cast<const Entry *>(file.data() + sizeof(Header));
  for (uint32_t i = 0; i < h.entryCount; ++i) {
    if (uint64_t(e[i].dxilOffset) + e[i].dxilSize > h.dxilSize) {
      error = "entry " + std::to_string(i) + " points past the DXIL";
      return false;
    }
  }
  return true;
}

} // namespace

bool Load(const std::filesystem::path &path, Cache &out, std::string &error) {
  std::vector<uint8_t> file;
  if (!ReadAll(path, file, error))
    return false;
  ShaderCacheFileHeader h{};
  if (file.size() < sizeof(h)) {
    error = "file too small";
    return false;
  }
  std::memcpy(&h, file.data(), sizeof(h));
  if (!CheckLayout<ShaderCacheFileHeader, ShaderCacheFileEntry>(file, h, kCacheMagic, error))
    return false;

  const auto *e = reinterpret_cast<const ShaderCacheFileEntry *>(file.data() + sizeof(h));
  out.entries.clear();
  out.entries.reserve(h.entryCount);
  for (uint32_t i = 0; i < h.entryCount; ++i)
    out.entries.push_back({e[i].hash, e[i].dxilOffset, e[i].dxilSize,
                           e[i].specConstantsMask, nullptr, e[i].flags});
  if (!std::is_sorted(out.entries.begin(), out.entries.end(),
                      [](const auto &a, const auto &b) { return a.hash < b.hash; })) {
    error = "entries not sorted";
    return false;
  }

  const uint8_t *blob = file.data() + sizeof(h) + size_t(h.entryCount) * sizeof(*e);
  out.dxil.resize(h.dxilSize);
  const size_t n = ZSTD_decompress(out.dxil.data(), out.dxil.size(), blob, h.dxilCompressedSize);
  if (ZSTD_isError(n) || n != h.dxilSize) {
    error = "DXIL decompression failed";
    return false;
  }
  out.fileHash = XXH3_64bits(file.data(), file.size());
  return true;
}

bool LoadLinked(const std::filesystem::path &path, uint64_t sourceHash,
                LinkedCache &out, std::string &error) {
  std::vector<uint8_t> file;
  if (!ReadAll(path, file, error))
    return false;
  LinkedCacheFileHeader h{};
  if (file.size() < sizeof(h)) {
    error = "file too small";
    return false;
  }
  std::memcpy(&h, file.data(), sizeof(h));
  if (!CheckLayout<LinkedCacheFileHeader, LinkedCacheFileEntry>(file, h, kLinkedMagic, error))
    return false;
  if (h.sourceHash != sourceHash) {
    error = "linked from another shader_cache.bin (rebuild it with ufc3_prelink)";
    return false;
  }

  const auto *e = reinterpret_cast<const LinkedCacheFileEntry *>(file.data() + sizeof(h));
  out.entries.assign(e, e + h.entryCount);
  const uint8_t *blob = file.data() + sizeof(h) + size_t(h.entryCount) * sizeof(*e);
  out.dxil.resize(h.dxilSize);
  mz_ulong len = static_cast<mz_ulong>(h.dxilSize);
  if (h.dxilSize &&
      (mz_uncompress(out.dxil.data(), &len, blob, static_cast<mz_ulong>(h.dxilCompressedSize)) !=
           MZ_OK ||
       len != h.dxilSize)) {
    error = "DXIL decompression failed";
    return false;
  }
  return true;
}

bool WriteLinked(const std::filesystem::path &path, uint64_t sourceHash,
                 const std::vector<LinkedCacheFileEntry> &entries,
                 const std::vector<uint8_t> &dxil, std::string &error) {
  mz_ulong compLen = mz_compressBound(static_cast<mz_ulong>(dxil.size()));
  std::vector<uint8_t> comp(compLen);
  if (!dxil.empty()) {
    if (mz_compress2(comp.data(), &compLen, dxil.data(), static_cast<mz_ulong>(dxil.size()),
                     MZ_UBER_COMPRESSION) != MZ_OK) {
      error = "deflate failed";
      return false;
    }
  } else {
    compLen = 0;
  }
  LinkedCacheFileHeader h{};
  std::memcpy(h.magic, kLinkedMagic, 8);
  h.version = 1;
  h.entryCount = static_cast<uint32_t>(entries.size());
  h.sourceHash = sourceHash;
  h.dxilCompressedSize = compLen;
  h.dxilSize = dxil.size();

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char *>(&h), sizeof(h));
  f.write(reinterpret_cast<const char *>(entries.data()),
          std::streamsize(entries.size() * sizeof(LinkedCacheFileEntry)));
  f.write(reinterpret_cast<const char *>(comp.data()), std::streamsize(compLen));
  if (!f) {
    error = "can't write " + path.string();
    return false;
  }
  return true;
}

ShaderCacheEntry *Find(Cache &cache, uint64_t hash) {
  auto it = std::lower_bound(cache.entries.begin(), cache.entries.end(), hash,
                             [](const ShaderCacheEntry &lhs, uint64_t rhs) { return lhs.hash < rhs; });
  return (it != cache.entries.end() && it->hash == hash) ? &*it : nullptr;
}

const LinkedCacheFileEntry *FindLinked(const LinkedCache &cache, uint64_t hash,
                                       uint32_t specConstants) {
  auto it = std::lower_bound(
      cache.entries.begin(), cache.entries.end(), std::pair{hash, specConstants},
      [](const LinkedCacheFileEntry &lhs, const std::pair<uint64_t, uint32_t> &rhs) {
        if (lhs.hash != rhs.first)
          return lhs.hash < rhs.first;
        return lhs.specConstants < rhs.second;
      });
  return (it != cache.entries.end() && it->hash == hash && it->specConstants == specConstants)
             ? &*it
             : nullptr;
}

} // namespace bd::gpu::shader_cache_file
