// ufc3 - walkout skip trace (--ufc3_skip_trace).
//
// Some added fighters' walkouts can't be skipped once they start; stock
// fighters' can. To find what a skip press runs, this logs:
//   - pad button edges (sub_823C41F8 = XamInputGetState(user, state)),
//   - the cutscene script lists the game loads (sub_822C0D48: obj+12 count,
//     obj+24 entries of 136 bytes {scene, index}, file e<scene>_<index>.pms;
//     after loading, obj+1092 holds 8-byte entries whose +4 object gets the
//     +124 flag for scene 380/2380),
//   - per 100 ms window, the counted functions (skip_trace_calls.h) that were
//     silent for the previous second and are called now ("start").

#pragma once

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_skip_trace, false, "ufc3",
                    "Log pad button edges, cutscene script lists and functions that start being called (100 ms windows)");

#include "skip_trace_calls.h"

REX_EXTERN(__imp__sub_823C41F8);
REX_EXTERN(__imp__sub_822C0D48);
REX_EXTERN(__imp__sub_82BD3040);
REX_EXTERN(__imp__sub_82DEDA78);

namespace ufc3_skip_trace {

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}
inline uint16_t Load16(uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, 2);
  return _byteswap_ushort(v);
}

constexpr int kWindowMs = 100;
constexpr int kQuietWindows = 10;  // silent this many windows before a call counts as a start

inline void Dumper() {
  static int64_t last[kCount];
  for (auto& v : last) v = -1000000;
  for (int64_t w = 0;; ++w) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kWindowMs));
    if (!REXCVAR_GET(ufc3_skip_trace)) continue;
    std::string line;
    int n = 0;
    for (int i = 0; i < kCount; ++i) {
      uint32_t c = g_calls[i].exchange(0, std::memory_order_relaxed);
      if (!c) continue;
      if (w - last[i] > kQuietWindows) {
        if (n < 80) line += fmt::format(" {:08X}:{}", kAddr[i], c);
        ++n;
      }
      last[i] = w;
    }
    if (n) REXLOG_INFO("[skip] start w={} n={}{}", w, n, n > 80 ? fmt::format("{} ...", line) : line);
  }
}

inline void EnsureDumper() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(Dumper).detach(); });
}

// The arena-screen object (vtable 82154A70, busy fn sub_82DECC18): busy while
// +44 == 0, or +58 != 0 and its child (+96) is missing or child+32 == 0.
// Logged whenever its fields change (from the busy check and its update,
// sub_82DEDA78).
inline void LogVision(uint8_t* base, uint32_t obj) {
  static std::mutex m;
  static std::string last_vis;
  const uint32_t child = Load32(base, obj + 96);
  std::string vis = fmt::format(
      "+44={} +52={:08X} +56={} +58={} +68={:08X} +76={} +80={:08X} +84={:08X} +88={} +92={} child={:08X}",
      Load32(base, obj + 44), Load32(base, obj + 52), Load16(base, obj + 56), Load16(base, obj + 58),
      Load32(base, obj + 68), Load32(base, obj + 76), Load32(base, obj + 80), Load32(base, obj + 84),
      Load32(base, obj + 88), Load32(base, obj + 92), child);
  if (child) {
    const uint32_t c0 = Load32(base, child), c8 = Load32(base, child + 8);
    vis += fmt::format(" c+0={:08X}(vt {:08X}) c+4={:08X} c+8={:08X}(vt {:08X}) c+12={:08X} c+32={} c+36={:08X}", c0,
                       c0 ? Load32(base, c0) : 0, Load32(base, child + 4), c8, c8 ? Load32(base, c8) : 0,
                       Load32(base, child + 12), Load32(base, child + 32), Load32(base, child + 36));
    const char* s40 = reinterpret_cast<const char*>(base + child + 40);
    const char* s169 = reinterpret_cast<const char*>(base + child + 169);
    vis += fmt::format(" c+40='{}' c+169='{}'", std::string(s40, strnlen(s40, 128)),
                       std::string(s169, strnlen(s169, 64)));
  }
  std::lock_guard lock(m);
  if (vis != last_vis) {
    last_vis = vis;
    REXLOG_INFO("[skip] vision {:08X}: {}", obj, vis);
  }
}

inline uint16_t g_buttons[4];
inline uint8_t g_triggers[4][2];

}  // namespace ufc3_skip_trace

// XamInputGetState(user r3, state r4): XINPUT_STATE {u32 packet; u16 buttons;
// u8 left_trigger, right_trigger; ...}.
extern "C" REX_FUNC(sub_823C41F8) {
  namespace st = ufc3_skip_trace;
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_823C41F8(ctx, base);
  if (!REXCVAR_GET(ufc3_skip_trace) || user > 3 || !state || ctx.r3.u32 != 0) return;
  st::EnsureDumper();
  const uint16_t b = st::Load16(base, state + 4);
  const uint8_t lt = base[state + 6] > 64, rt = base[state + 7] > 64;
  if (b != st::g_buttons[user] || lt != st::g_triggers[user][0] || rt != st::g_triggers[user][1]) {
    REXLOG_INFO("[skip] pad {} buttons {:04X} -> {:04X} (down {:04X}) LT {} RT {}", user, st::g_buttons[user], b,
                b & ~st::g_buttons[user], lt, rt);
    st::g_buttons[user] = b;
    st::g_triggers[user][0] = lt;
    st::g_triggers[user][1] = rt;
  }
}

// sub_82BD3040(holder): the walkout skip handler (sub_822B8A98, via
// sub_82BD33D0) asks the up to 20 objects at holder+12 "busy?" through vtable
// slot 56 and refuses the skip while one says yes. Same loop, but it logs the
// list and which object answered, each time the answer changes.
extern "C" REX_FUNC(sub_82BD3040) {
  namespace st = ufc3_skip_trace;
  if (!REXCVAR_GET(ufc3_skip_trace)) {
    __imp__sub_82BD3040(ctx, base);
    return;
  }
  const uint32_t holder = ctx.r3.u32;
  const uint64_t lr = ctx.lr;
  int hit = -1;
  uint32_t hit_obj = 0, hit_fn = 0;
  for (int i = 0; i < 20; ++i) {
    const uint32_t obj = st::Load32(base, holder + 12 + 4 * i);
    if (!obj) continue;
    const uint32_t fn = st::Load32(base, st::Load32(base, obj) + 56);
    ctx.r3.u64 = obj;
    ctx.lr = 0x82BD3078;
    rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
    if (ctx.r3.u32 != 0) {
      hit = i;
      hit_obj = obj;
      hit_fn = fn;
      break;
    }
  }
  ctx.lr = lr;
  ctx.r3.u64 = hit >= 0 ? 1 : 0;
  static int last_hit = -2;
  static uint32_t last_fn = 0;
  for (int i = 0; i < 20; ++i) {
    const uint32_t obj = st::Load32(base, holder + 12 + 4 * i);
    if (obj && st::Load32(base, obj) == 0x82154A70) st::LogVision(base, obj);
  }
  if (hit != last_hit || hit_fn != last_fn) {
    last_hit = hit;
    last_fn = hit_fn;
    std::string list;
    for (int i = 0; i < 20; ++i) {
      const uint32_t obj = st::Load32(base, holder + 12 + 4 * i);
      if (obj) list += fmt::format(" [{}]{:08X}/vt{:08X}/f{:08X}", i, obj, st::Load32(base, obj),
                                   st::Load32(base, st::Load32(base, obj) + 56));
    }
    REXLOG_INFO("[skip] busy check holder={:08X} lr={:08X}: {} (obj {:08X} fn {:08X}) | list:{}", holder,
                static_cast<uint32_t>(lr), hit >= 0 ? fmt::format("BUSY at [{}]", hit) : std::string("free"),
                hit_obj, hit_fn, list);
  }
}

// Cutscene script list loader.
extern "C" REX_FUNC(sub_822C0D48) {
  namespace st = ufc3_skip_trace;
  const uint32_t obj = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  std::string list;
  int count = 0;
  if (REXCVAR_GET(ufc3_skip_trace) && obj) {
    st::EnsureDumper();
    count = static_cast<int>(st::Load32(base, obj + 12));
    const uint32_t arr = st::Load32(base, obj + 24);
    for (int i = 0; i < count && i < 64 && arr; ++i) {
      int32_t scene = static_cast<int32_t>(st::Load32(base, arr + 136 * i));
      int32_t index = static_cast<int32_t>(st::Load32(base, arr + 136 * i + 4));
      list += fmt::format(" e{}_{:04}", scene, index);
    }
  }
  __imp__sub_822C0D48(ctx, base);
  if (!REXCVAR_GET(ufc3_skip_trace) || !obj) return;
  std::string flags;
  const uint32_t ents = st::Load32(base, obj + 1092);
  for (int i = 0; i < count && i < 64 && ents; ++i) {
    const uint32_t o = st::Load32(base, ents + 8 * i + 4);
    flags += o ? fmt::format(" {}", st::Load32(base, o + 124)) : " -";
  }
  REXLOG_INFO("[skip] cutscene list obj={:08X} lr={:08X} n={}:{} | +124:{} | state +1224={}", obj, lr, count, list,
              flags, st::Load32(base, obj + 1224));
}

// The arena-screen object's per-frame update.
extern "C" REX_FUNC(sub_82DEDA78) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82DEDA78(ctx, base);
  if (REXCVAR_GET(ufc3_skip_trace) && obj) ufc3_skip_trace::LogVision(base, obj);
}
