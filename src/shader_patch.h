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
        for (uint8_t* s = lo_p + a; s + (tg.len - a) <= hi_p;) {
          uint8_t* hit = static_cast<uint8_t*>(memchr(s, tg.orig[a], size_t(hi_p - s - (tg.len - a) + 1)));
          if (!hit) break;
          uint8_t* cand = hit - a;
          if (cand >= lo_p && std::memcmp(cand, tg.orig, tg.len) == 0) {
            auto& v = f.sites[t];
            if (std::find(v.begin(), v.end(), cand) == v.end()) v.push_back(cand);
          }
          s = hit + 1;
        }
      }
    }
    p = re;
  }
}

inline void Apply(Feature& f, bool on) {
  size_t n = 0;
  for (size_t t = 0; t < f.targets.size(); ++t) {
    const Target& tg = f.targets[t];
    for (uint8_t* site : f.sites[t]) {
      std::memcpy(site + tg.off, on ? tg.patch : tg.orig + tg.off, tg.patch_len);
      ++n;
    }
  }
  f.applied = on;
  REXLOG_INFO("[{}] shader patch {} at {} site(s)", f.name, on ? "applied" : "removed", n);
}

inline void Main(uint8_t* base) {
  using namespace std::chrono;
  auto start = steady_clock::now();
  for (;;) {
    for (Feature* f : Features()) {
      bool want = f->wanted();
      if (want && !f->Complete()) {
        bool had = false;
        // Virtual heap and physical-heap view; the pac is read into one of them.
        ScanRegion(base, 0x40000000, 0x80000000, *f);
        ScanRegion(base, 0xA0000000, 0xC0000000, *f);
        if (!had && f->Complete()) REXLOG_INFO("[{}] found all shader copies", f->name);
      }
      if (want != f->applied) {
        Apply(*f, want);
      } else if (want) {
        // Re-write quietly so copies found after the first apply get patched too.
        for (size_t t = 0; t < f->targets.size(); ++t) {
          for (uint8_t* site : f->sites[t])
            std::memcpy(site + f->targets[t].off, f->targets[t].patch, f->targets[t].patch_len);
        }
      }
    }
    bool young = steady_clock::now() - start < seconds(90);
    std::this_thread::sleep_for(young ? milliseconds(250) : seconds(2));
  }
}

inline void Tick(uint8_t* base) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread(Main, base).detach();
}

}  // namespace ufc3_shader_patch
