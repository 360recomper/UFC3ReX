/**
 * @file    tools/prelink_shader_cache.cpp
 * @brief   DXC pre-linker: writes shader_cache_linked.bin with a DXIL blob
 *          for every (shader, specConstantsMask subset) pair of a
 *          shader_cache.bin. UFC 3 runs it when the cache is built (the cache
 *          comes from the player's disc files, so not at compile time).
 *
 *          Standalone console tool: fprintf/stderr diagnostics by design
 *          (the engine's BD_* logging is not linked here).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gpu/shaders/dxc_link.h"
#include "gpu/shaders/shader_cache_file.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr,
                 "usage: ufc3_prelink <shader_cache.bin> <shader_cache_linked.bin>\n");
    return 1;
  }

  namespace scf = bd::gpu::shader_cache_file;
  scf::Cache cache;
  std::string error;
  if (!scf::Load(argv[1], cache, error)) {
    std::fprintf(stderr, "prelink: %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const std::vector<uint8_t>& dxil = cache.dxil;

  struct Job {
    const ShaderCacheEntry* entry;
    uint32_t masked;
  };
  std::vector<Job> jobs;
  for (const auto& e : cache.entries) {
    if (!e.specConstantsMask) continue;
    // Every subset of the mask, including 0 (a spec-constant library still
    // needs g_SpecConstants() resolved for the value-0 variant).
    uint32_t s = 0;
    do {
      jobs.push_back({&e, s});
      s = (s - e.specConstantsMask) & e.specConstantsMask;
    } while (s);
  }

  std::map<uint32_t, std::vector<uint8_t>> specLibs;
  for (const auto& j : jobs) specLibs.try_emplace(j.masked);
  for (auto& [value, blob] : specLibs) {
    blob = bd::gpu::CompileSpecConstantLib(value);
    if (blob.empty()) {
      std::fprintf(stderr, "prelink: spec lib compile failed (value=%u)\n",
                   value);
      return 1;
    }
  }

  struct Result {
    uint64_t hash;
    uint32_t masked;
    std::vector<uint8_t> dxil;
  };
  std::vector<Result> results(jobs.size());
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};

  auto worker = [&] {
    for (;;) {
      const size_t i = next.fetch_add(1);
      if (i >= jobs.size() || failed.load()) return;
      const Job& j = jobs[i];
      const uint8_t* lib = dxil.data() + j.entry->dxilOffset;
      const auto& spec = specLibs.find(j.masked)->second;
      // The cache stores no shader type, and a library's main only validates
      // against its own stage, so the failing profile identifies it.
      auto vs = bd::gpu::LinkSpecConstantLib(lib, j.entry->dxilSize, spec.data(),
                                             spec.size(), L"vs_6_0");
      auto ps = bd::gpu::LinkSpecConstantLib(lib, j.entry->dxilSize, spec.data(),
                                             spec.size(), L"ps_6_0");
      if (vs.empty() == ps.empty()) {
        std::fprintf(stderr,
                     "prelink: %s link hash=%016llX mask=0x%X (vs=%zu ps=%zu)\n",
                     vs.empty() ? "failed" : "ambiguous",
                     static_cast<unsigned long long>(j.entry->hash), j.masked,
                     vs.size(), ps.size());
        failed.store(true);
        return;
      }
      results[i] = {j.entry->hash, j.masked,
                    vs.empty() ? std::move(ps) : std::move(vs)};
    }
  };
  {
    unsigned hw = std::thread::hardware_concurrency();
    if (!hw) hw = 4;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < hw; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  if (failed.load()) return 1;

  std::sort(results.begin(), results.end(), [](const Result& a,
                                               const Result& b) {
    if (a.hash != b.hash) return a.hash < b.hash;
    return a.masked < b.masked;
  });

  std::vector<LinkedCacheFileEntry> entries;
  std::vector<uint8_t> blob;
  for (const auto& r : results) {
    entries.push_back({r.hash, r.masked, static_cast<uint32_t>(blob.size()),
                       static_cast<uint32_t>(r.dxil.size()), 0});
    blob.insert(blob.end(), r.dxil.begin(), r.dxil.end());
  }
  if (!scf::WriteLinked(argv[2], cache.fileHash, entries, blob, error)) {
    std::fprintf(stderr, "prelink: %s\n", error.c_str());
    return 1;
  }

  // Read it back the way the game does, so a bad file fails here, not in game.
  scf::LinkedCache check;
  if (!scf::LoadLinked(argv[2], cache.fileHash, check, error) ||
      check.entries.size() != entries.size() || check.dxil != blob) {
    std::fprintf(stderr, "prelink: read-back of %s failed: %s\n", argv[2],
                 error.c_str());
    return 1;
  }

  std::printf("prelink: %zu variant(s) of %zu spec-constant value(s), "
              "%zu bytes of DXIL\n",
              entries.size(), specLibs.size(), blob.size());
  return 0;
}
