// ufc3 - arena light colours as the game writes them, on either renderer (--ufc3_trace_flare, NR-8b-6).
//
// sub_82785B58 runs a render command that copies prebuilt vertices into BeginIndexedVertices:
// command + 16 = prim, base, vertex count, index count, format, stride; the vertices follow at + 48.
// The arena lights use stride 36: position, NORMAL0 (xy = the light's place on screen, 0..1),
// COLOR0, TEXCOORD0. A light draws several quads; its +-7.5 wide streak (x = 0xC0F00000) names it
// once per frame with its full colour. This runs before any renderer sees the draw, so a difference
// between the renderers here is the game's own data.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

REXCVAR_DEFINE_BOOL(ufc3_trace_flare, false, "ufc3",
                    "Log the arena lights' screen place and vertex colour twice a second, on both "
                    "renderers (NR-8b-6)");

REX_EXTERN(__imp__sub_82785B58);

namespace ufc3_flare_trace {

constexpr uint32_t kMaxLines = 12000;
constexpr uint32_t kSampleMs = 500;
constexpr uint32_t kWindowMs = 8;  // one frame's lights arrive well inside this

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline float LoadFloat(uint8_t* base, uint32_t addr) {
  const uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline void Sample(uint8_t* base, uint32_t command) {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static const Clock::time_point start = Clock::now();
  static Clock::time_point sample_start{};
  static std::vector<std::pair<uint32_t, uint32_t>> seen;
  static uint32_t sample = 0;
  static uint32_t lines = 0;

  const uint32_t header = command + 16;
  if (Load32(base, header + 8) != 4 || Load32(base, header + 20) != 36)
    return;
  const uint32_t v0 = header + 32;
  if (Load32(base, v0) != 0xC0F00000u)
    return;

  std::lock_guard lock(mutex);
  const Clock::time_point now = Clock::now();
  const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - sample_start).count();
  if (since >= kSampleMs) {
    sample_start = now;
    seen.clear();
    ++sample;
  } else if (since >= kWindowMs) {
    return;
  }
  const std::pair<uint32_t, uint32_t> place{Load32(base, v0 + 12), Load32(base, v0 + 16)};
  for (const auto& p : seen)
    if (p == place)
      return;
  seen.push_back(place);
  if (lines++ >= kMaxLines)
    return;
  const double t = std::chrono::duration<double>(now - start).count();
  REXLOG_INFO("[flare] s {} t {:.3f} light {} at {:.3f},{:.3f} colour {:08X} world {:08X} {:08X} {:08X}", sample,
              t, seen.size() - 1, LoadFloat(base, v0 + 12), LoadFloat(base, v0 + 16), Load32(base, v0 + 24),
              Load32(base, v0 + 4), Load32(base, v0 + 8), Load32(base, v0 + 20));
}

}  // namespace ufc3_flare_trace

extern "C" REX_FUNC(sub_82785B58) {
  if (REXCVAR_GET(ufc3_trace_flare))
    ufc3_flare_trace::Sample(base, ctx.r3.u32);
  __imp__sub_82785B58(ctx, base);
}
