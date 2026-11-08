// ufc3 - bare-knuckle fights (FINDINGS-bare-knuckle.md, FINDINGS-bare-knuckle-ui.md).
//
// Every fighter's body already has bare hands under the gloves; sub_82B58FC0 is
// the only code that shows/hides the glove groups (glove, trans_tape for UFC;
// pride_glove, trans_pride_tap for PRIDE) through SetGroupVisible(this, handle,
// r5 = visible). The mid-asm hooks (config/functions.toml) sit right after each
// "li r5,1" and clear r5 when the hands setting says so:
//   bare = gloves, straps and tape hidden (an old "wraps" setting reads as bare)
//
// Those hooks alone don't reach the screen: the model keeps per-group flag arrays
// (slot 0 at +0x278 = main pass, read by the draw lists sub_8276C238/D300/E778;
// slot 1 at +0x27C = read by sub_8276B7D0; slots 2/3 = saved copies), and other
// gear code re-shows every group in slot 0 later (sub_82B59968 -> sub_82764380,
// 82B5D370, 82BFA420). So the group setters themselves are filtered for gear
// objects: sub_82763888 never shows a hidden hand group, and the two setters
// that write the arrays directly (sub_82763920, sub_82763A48) re-hide afterwards.
//
// In game: clicking the right stick (R3) toggles Gloves <-> Bare outside a
// match (start value = --ufc3_bare_knuckle). BACK was taken: it opens the Help menu on
// SELECT FIGHTER. R3 is ignored while the match-flow dispatcher sub_82CF1A18 runs.
// After a change the glove function runs again on the gear objects it saw last,
// so the 3D fighters on SELECT FIGHTER change at once.
//
// Crew models (referee and the like: 500-506) use the same gear class, but their _g IS the
// bare hand, in a group also named "glove" and with no other hand group
// (FINDINGS-bare-knuckle.md). They keep their hands: see FighterGear().

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "attire_pick.h"
#include "mod_pack.h"

REXCVAR_DEFINE_STRING(ufc3_bare_knuckle, "off", "ufc3",
                      "Fighters' hands at start: off (gloves) or bare (wraps = bare). "
                      "Right stick click toggles it in game, outside a match");
REXCVAR_DEFINE_BOOL(ufc3_bk_trace, false, "ufc3", "Log bare-knuckle glove calls and group flags ([bk] lines; debug)");

namespace ufc3_bk {

enum Hands : int { kGloves = 0, kBare = 1 };

inline std::atomic<int>& HandsRef() {
  static std::atomic<int> h{[] {
    if (!ufc3_pack::Active()) return kGloves;  // stock mode: gloves, R3 does nothing
    const std::string& v = REXCVAR_GET(ufc3_bare_knuckle);
    return v == "bare" || v == "wraps" ? kBare : kGloves;
  }()};
  return h;
}
inline int Hands() { return HandsRef().load(std::memory_order_relaxed); }

// Last time the match-flow dispatcher ran (it runs every frame of a match).
inline std::atomic<uint64_t> g_match_tick{0};
inline bool InMatch() { return GetTickCount64() - g_match_tick.load(std::memory_order_relaxed) < 1000; }

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

// Gear objects the glove function ran on, newest first, with their vtable at the
// time (a freed/reused object fails the check in Valid()).
struct Seen {
  uint32_t obj, vtable;
};
constexpr int kSeen = 8;
inline std::mutex g_mutex;
inline Seen g_seen[kSeen] = {};

inline void Remember(uint8_t* base, uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Seen s{obj, Load32(base, obj)};
  int i = 0;
  while (i < kSeen - 1 && g_seen[i].obj != obj) ++i;
  for (; i > 0; --i) g_seen[i] = g_seen[i - 1];
  g_seen[0] = s;
}

// Crew gear has only the "glove" group (its bare hand). Every fighter _g, disc and
// mods_integration, has trans_tape too and most have the PRIDE groups.
inline bool FighterGear(uint8_t* base, uint32_t obj) {
  for (uint32_t off = 0x71C; off <= 0x724; off += 4)
    if (static_cast<int32_t>(Load32(base, obj + off)) >= 0) return true;
  return false;
}

// Set while the glove function runs on crew gear (its hooks have no guest memory).
inline thread_local bool t_crew = false;

inline bool Valid(uint8_t* base, const Seen& s) {
  if (!s.obj || Load32(base, s.obj) != s.vtable) return false;
  uint32_t type = Load32(base, s.obj + 0x710);
  if (type != 0 && type != 2) return false;
  if (!FighterGear(base, s.obj)) return false;
  for (uint32_t off = 0x718; off <= 0x724; off += 4) {
    int32_t h = static_cast<int32_t>(Load32(base, s.obj + off));
    if (h < -1 || h > 4096) return false;
  }
  return true;
}

// Gear component class (fighter +0x6A4); its group handles sit at +0x718..+0x724.
constexpr uint32_t kGearVtable = 0x821344B8;

// Does the hands setting hide group `h` on gear object `obj`?
inline bool HiddenGroup(uint8_t* base, uint32_t obj, int32_t h) {
  int hands = Hands();
  if (hands == kGloves || h < 0 || Load32(base, obj) != kGearVtable || !FighterGear(base, obj)) return false;
  for (uint32_t off = 0x718; off <= 0x724; off += 4)
    if (h == static_cast<int32_t>(Load32(base, obj + off))) return true;
  return false;
}

inline void TraceForced(const char* who, uint32_t obj, int32_t h, int32_t slot) {
  static std::atomic<int> n{0};
  int k = ++n;
  if (REXCVAR_GET(ufc3_bk_trace) && (k <= 64 || k % 500 == 0))
    REXLOG_INFO("[bk] kept hidden #{} by {} obj {:08X} group {} slot {}", k, who, obj, h, slot);
}

}  // namespace ufc3_bk

REX_EXTERN(__imp__sub_82B58FC0);
REX_EXTERN(__imp__sub_82CF1A18);
REX_EXTERN(__imp__sub_82763888);
REX_EXTERN(__imp__sub_82763920);
REX_EXTERN(__imp__sub_82763A48);

// SetGroupVisible(this, handle, visible, slot; -1 = slots 0 and 1): a gear
// object's hidden hand groups stay hidden, whoever asks to show them.
extern "C" REX_FUNC(sub_82763888) {
  if (ctx.r5.u32 != 0 && ufc3_bk::HiddenGroup(base, ctx.r3.u32, static_cast<int32_t>(ctx.r4.u32))) {
    ufc3_bk::TraceForced("set", ctx.r3.u32, static_cast<int32_t>(ctx.r4.u32), static_cast<int32_t>(ctx.r6.u32));
    ctx.r5.u64 = 0;
  }
  __imp__sub_82763888(ctx, base);
}

namespace ufc3_bk {
// After a setter that writes the flag arrays itself: hide the hand groups again
// in the same slot(s).
inline void Rehide(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, const char* who) {
  if (Hands() == kGloves || Load32(base, obj) != kGearVtable) return;
  for (uint32_t off = 0x718; off <= 0x724; off += 4) {
    int32_t h = static_cast<int32_t>(Load32(base, obj + off));
    if (!HiddenGroup(base, obj, h)) continue;
    PPCContext c = ctx;
    c.r3.u64 = obj;
    c.r4.s64 = h;
    c.r5.u64 = 0;
    c.r6.s64 = static_cast<int32_t>(slot);
    __imp__sub_82763888(c, base);
  }
  TraceForced(who, obj, -1, static_cast<int32_t>(slot));
}
}  // namespace ufc3_bk

// Set groups by a byte mask (this, mask, visible, slot).
extern "C" REX_FUNC(sub_82763920) {
  uint32_t obj = ctx.r3.u32, slot = ctx.r6.u32;
  bool show = ctx.r5.u32 != 0;
  __imp__sub_82763920(ctx, base);
  if (show) ufc3_bk::Rehide(ctx, base, obj, slot, "mask");
}

// Set the groups of the meshes that match (this, ..., visible, slot).
extern "C" REX_FUNC(sub_82763A48) {
  uint32_t obj = ctx.r3.u32, slot = ctx.r6.u32;
  bool show = ctx.r5.u32 != 0;
  __imp__sub_82763A48(ctx, base);
  if (show) ufc3_bk::Rehide(ctx, base, obj, slot, "mesh");
}

// Match-flow phase dispatcher (FINDINGS-post-ko-window.md): marks "in a match".
extern "C" REX_FUNC(sub_82CF1A18) {
  ufc3_bk::g_match_tick.store(GetTickCount64(), std::memory_order_relaxed);
  __imp__sub_82CF1A18(ctx, base);
}

// The glove function itself: remember which gear object it ran on.
extern "C" REX_FUNC(sub_82B58FC0) {
  static std::atomic<int> calls{0};
  uint32_t obj = ctx.r3.u32;
  int n = ++calls;
  if (REXCVAR_GET(ufc3_bk_trace) && (n <= 64 || n % 500 == 0))
    REXLOG_INFO("[bk] glove fn #{} obj {:08X} vt {:08X} type {} pose {} handles {} {} {} {} hands {} match {}", n, obj,
                ufc3_bk::Load32(base, obj), ufc3_bk::Load32(base, obj + 0x710), ufc3_bk::Load32(base, obj + 0x714),
                static_cast<int32_t>(ufc3_bk::Load32(base, obj + 0x718)),
                static_cast<int32_t>(ufc3_bk::Load32(base, obj + 0x71C)),
                static_cast<int32_t>(ufc3_bk::Load32(base, obj + 0x720)),
                static_cast<int32_t>(ufc3_bk::Load32(base, obj + 0x724)), ufc3_bk::Hands(), ufc3_bk::InMatch());
  ufc3_bk::t_crew = !ufc3_bk::FighterGear(base, obj);
  if (!ufc3_bk::t_crew) ufc3_bk::Remember(base, obj);
  __imp__sub_82B58FC0(ctx, base);
  ufc3_bk::t_crew = false;
}

// 0x82B590EC (UFC glove) / 0x82B59188 (PRIDE glove): hide for bare.
void ufc3_bk_glove(PPCRegister& r5) {
  if (ufc3_bk::Hands() != ufc3_bk::kGloves && !ufc3_bk::t_crew) r5.s64 = 0;
}

// 0x82B59108 (UFC trans_tape) / 0x82B591A4 (trans_pride_tap): hide for bare.
void ufc3_bk_tape(PPCRegister& r5) {
  if (ufc3_bk::Hands() == ufc3_bk::kBare) r5.s64 = 0;
}

// XamInputGetState(user, flags, state): R3 toggles the hands setting (game
// thread, once per pad poll), then the glove function runs again on the live
// gear objects it saw last.
namespace ufc3_bk {
// Re-run the glove function on every remembered fighter object (R3, or a select preview that
// finished loading after it). Returns how many were redone.
inline int RedoHands(PPCContext& ctx, uint8_t* base) {
  int redone = 0;
  Seen seen[kSeen];
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memcpy(seen, g_seen, sizeof(seen));
  }
  for (const auto& s : seen) {
    if (REXCVAR_GET(ufc3_bk_trace) && s.obj && !Valid(base, s))
      REXLOG_INFO("[bk] skip {:08X} (vt {:08X} now {:08X}, type {})", s.obj, s.vtable, Load32(base, s.obj),
                  Load32(base, s.obj + 0x710));
    if (!Valid(base, s)) continue;
    PPCContext c = ctx;  // own registers; the callee's frame goes below the caller's r1
    c.r3.u64 = s.obj;
    __imp__sub_82B58FC0(c, base);
    ++redone;
  }
  return redone;
}
inline void OnPreviewReady(PPCContext& ctx, uint8_t* base, int side) {
  if (Hands() != kBare || InMatch()) return;
  int n = RedoHands(ctx, base);
  if (REXCVAR_GET(ufc3_bk_trace)) REXLOG_INFO("[bk] preview side {} loaded: {} fighters redone", side, n);
}
inline const bool kPreviewHook = (ufc3_attire::g_on_preview_ready = &OnPreviewReady, true);
}  // namespace ufc3_bk

// Other pad listeners (venues.h: the PRIDE venue list), set by their headers. They get the
// guest XINPUT_STATE (wButtons at +4, thumbs at +8..+15, big-endian) and may rewrite it.
namespace ufc3_bk {
inline void (*g_on_pad)(uint8_t* base, uint32_t user, uint32_t state) = nullptr;
}

extern "C" REX_FUNC(__imp__XamInputGetState) {
  static ufc3_trace::ImportFn fn = ufc3_trace::Resolve("__imp__XamInputGetState");
  static bool held[4] = {};
  uint32_t user = ctx.r3.u32 & 3, state = ctx.r5.u32;
  fn(ctx, base);
  if (ctx.r3.u32 != 0 || !state) return;
  // Trace: every 2 s, the group flag words of the two newest gear objects in the
  // slot arrays [obj + 632 + 4*slot][handle], slots 0-3 (sub_82763888 writes
  // slots 0 and 1 for r6 = -1; +648 on are floats, not arrays).
  static uint64_t last_dump = 0;
  if (REXCVAR_GET(ufc3_bk_trace) && user == 0 && GetTickCount64() - last_dump > 2000) {
    last_dump = GetTickCount64();
    ufc3_bk::Seen top[2];
    {
      std::lock_guard<std::mutex> lock(ufc3_bk::g_mutex);
      top[0] = ufc3_bk::g_seen[0];
      top[1] = ufc3_bk::g_seen[1];
    }
    for (const auto& s : top) {
      if (!ufc3_bk::Valid(base, s)) continue;
      std::string vis;
      for (uint32_t off = 0x718; off <= 0x724; off += 4) {
        int32_t h = static_cast<int32_t>(ufc3_bk::Load32(base, s.obj + off));
        vis += " ";
        for (uint32_t slot = 0; slot < 4; ++slot) {
          uint32_t arr = ufc3_bk::Load32(base, s.obj + 632 + slot * 4);
          if (slot) vis += "/";
          vis += (h < 0 || arr < 0xA0000000) ?std::string("-") : std::to_string(ufc3_bk::Load32(base, arr + h * 4));
        }
      }
      REXLOG_INFO("[bk] vis {:08X} type {} glove/pglove/tape/ptape (slots 0/1/2/3){} match {}", s.obj,
                  ufc3_bk::Load32(base, s.obj + 0x710), vis, ufc3_bk::InMatch());
    }
  }
  if (ufc3_bk::g_on_pad) ufc3_bk::g_on_pad(base, user, state);
  ufc3_attire::OnPad(user, ufc3_bk::Load16(base, state + 4));  // L3 = attire picker (attire_pick.h)
  if (!ufc3_pack::Active()) return;  // stock mode: no bare knuckle toggle
  bool r3 =(ufc3_bk::Load16(base, state + 4) & 0x0080) != 0;  // XINPUT_GAMEPAD_RIGHT_THUMB
  bool pressed = r3 && !held[user];
  held[user] = r3;
  if (!pressed || ufc3_bk::InMatch()) return;

  int next = ufc3_bk::Hands() == ufc3_bk::kGloves ? ufc3_bk::kBare : ufc3_bk::kGloves;
  ufc3_bk::HandsRef().store(next, std::memory_order_relaxed);
  static const char* kNames[] = {"gloves", "bare"};
  int redone = ufc3_bk::RedoHands(ctx, base);
  REXLOG_INFO("[bk] hands -> {} (pad {}, {} fighters redone)", kNames[next], user, redone);
}
