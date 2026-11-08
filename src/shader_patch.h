// ufc3 - patch pixel-shader microcode in guest memory, switchable at run time.
//
// The game keeps the post-effect shaders from shader/fxl.pac in memory as
// big-endian Xenos microcode and the GPU backend translates a shader the first
// time it is drawn, so a patched copy has to be in place before that. A
// background thread scans the guest virtual and physical heaps for each
// feature's microcode (it is loaded at boot, well before the first fight),
// remembers every site it finds, and writes the patched or the original bytes
// as the feature's cvar changes (e.g. from the F4 menu).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>

#include <rex/logging.h>

#include "shader_patch_gen.h"

namespace ufc3_shader_patch {

struct Target {
  const uint8_t* orig;   // microcode to look for
  size_t len;
  size_t off;            // where the patch goes inside it
  const uint8_t* patch;  // replacement bytes
  size_t patch_len;
};

struct Feature {
  const char* name;
  bool (*wanted)();
  std::vector<Target> targets;
  std::vector<std::vector<uint8_t*>> sites;  // per target
  bool applied = false;

  bool Complete() const {
    for (auto& s : sites) {
      if (s.empty()) return false;
    }
    return !sites.empty();
  }
};

inline std::vector<Feature*>& Features() {
  static std::vector<Feature*> f;
  return f;
}

// One target over one committed run of memory. The game frees guest memory while this thread reads
// it (a load buffer at the top of the physical heap, decommitted right after the first frame): a
// region that VirtualQuery reported committed can be gone by the time memchr reaches that page. The
// read then faults with nothing to serve it (not a read-watched page), and that took the game down
// at boot on the laptop three times out of three once the native renderer's NR-10g fill moved the
// first frame by 20 ms. SEH skips the rest of such a region; the next pass finds the sites again.
// No locals with destructors in here: __try doesn't allow them.
inline bool ScanChunk(uint8_t* lo_p, uint8_t* hi_p, const Target& tg, size_t a,
                      std::vector<uint8_t*>& v) {
  __try {
    for (uint8_t* s = lo_p + a; s + (tg.len - a) <= hi_p;) {
      uint8_t* hit = static_cast<uint8_t*>(memchr(s, tg.orig[a], size_t(hi_p - s - (tg.len - a) + 1)));
      if (!hit) break;
      uint8_t* cand = hit - a;
      if (cand >= lo_p && std::memcmp(cand, tg.orig, tg.len) == 0) {
        if (std::find(v.begin(), v.end(), cand) == v.end()) v.push_back(cand);
      }
      s = hit + 1;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

inline void ScanRegion(uint8_t* base, uint32_t lo, uint32_t hi, Feature& f) {
  uint8_t* p = base + lo;
  uint8_t* end = base + hi;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
    uint8_t* rb = static_cast<uint8_t*>(mbi.BaseAddress);
    uint8_t* re = rb + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) {
      uint8_t* lo_p = std::max(rb, p);
      uint8_t* hi_p = std::min(re, end);
      for (size_t t = 0; t < f.targets.size(); ++t) {
        const Target& tg = f.targets[t];
        // Search for a byte that is rare in memory (not 00/FF/01/80), then compare.
        size_t a = 0;
        while (a + 1 < tg.len && (tg.orig[a] == 0x00 || tg.orig[a] == 0xFF ||
                                  tg.orig[a] == 0x01 || tg.orig[a] == 0x80))
          ++a;
        if (!ScanChunk(lo_p, hi_p, tg, a, f.sites[t])) {
          static std::atomic<int> s_faults{0};
          if (s_faults.fetch_add(1) < 8)
            REXLOG_WARN("[{}] scan: memory {}-{} went away while it was read, region skipped", f.name,
                        static_cast<const void*>(lo_p), static_cast<const void*>(hi_p));
          break;
        }
      }
    }
    p = re;
  }
}

// A remembered site can be a load buffer the game has freed and reused since the scan: writing there
// corrupts live data (dmg4z11 crashed in every run). Only ever write over the shader itself, and
// forget a site that no longer holds it.
inline bool Readable(const uint8_t* p, size_t len) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
  return p + len <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

inline bool HoldsShader(const uint8_t* site, const Target& tg) {
  if (!Readable(site, tg.len)) return false;
  size_t tail = tg.off + tg.patch_len;
  if (std::memcmp(site, tg.orig, tg.off) != 0 || std::memcmp(site + tail, tg.orig + tail, tg.len - tail) != 0) return false;
  return std::memcmp(site + tg.off, tg.orig + tg.off, tg.patch_len) == 0 ||
         std::memcmp(site + tg.off, tg.patch, tg.patch_len) == 0;
}

inline size_t WriteSites(Feature& f, bool on) {
  size_t n = 0;
  for (size_t t = 0; t < f.targets.size(); ++t) {
    const Target& tg = f.targets[t];
    auto& v = f.sites[t];
    for (auto it = v.begin(); it != v.end();) {
      if (!HoldsShader(*it, tg)) {
        REXLOG_INFO("[{}] site {} no longer holds the shader, dropped", f.name, static_cast<const void*>(*it));
        it = v.erase(it);
        continue;
      }
      const uint8_t* want = on ? tg.patch : tg.orig + tg.off;
      if (std::memcmp(*it + tg.off, want, tg.patch_len) != 0) std::memcpy(*it + tg.off, want, tg.patch_len);
      ++n;
      ++it;
    }
  }
  return n;
}

inline void Apply(Feature& f, bool on) {
  size_t n = WriteSites(f, on);
  f.applied = on;
  BumpGeneration();
  REXLOG_INFO("[{}] shader patch {} at {} site(s)", f.name, on ? "applied" : "removed", n);
}

inline void Main(uint8_t* base) {
  using namespace std::chrono;
  auto start = steady_clock::now();
  for (;;) {
    // The re-check every 2 s hitched the game: a 30-50 ms frame every 120 frames in fights (chi3x-z26-perf-clean vs
    // -nopatch). The shaders load at boot, so scan and rewrite only in the first 90 s; after that only act on a toggle.
    bool young = steady_clock::now() - start < seconds(90);
    for (Feature* f : Features()) {
      bool want = f->wanted();
      size_t sites_before = 0;
      for (auto& v : f->sites) sites_before += v.size();
      if (young && want && !f->Complete()) {
        bool had = false;
        // Virtual heap and physical-heap view; the pac is read into one of them.
        ScanRegion(base, 0x40000000, 0x80000000, *f);
        ScanRegion(base, 0xA0000000, 0xC0000000, *f);
        if (!had && f->Complete()) REXLOG_INFO("[{}] found all shader copies", f->name);
      }
      if (want != f->applied) {
        Apply(*f, want);
      } else if (young && want) {
        // Quietly: copies found after the first apply get patched too. A changed site count means
        // newly patched microcode: the native renderer re-keys its shaders (shader_patch_gen.h).
        if (WriteSites(*f, true) != sites_before) BumpGeneration();
      }
    }
    std::this_thread::sleep_for(young ? milliseconds(250) : seconds(2));
  }
}

inline void Tick(uint8_t* base) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread(Main, base).detach();
}

}  // namespace ufc3_shader_patch
