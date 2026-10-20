// ufc3 - opt-in cheats: unlock everything, free unlock-shop purchases.
//
// Unlock state lives in the system data ([0x83577434], saved in SYSTEM.DAT)
// in one 4908-byte block at sys+0x10458:
//   +0x000  fighter bits, 1 bit per fighter ID (IDs < 1024)
//   +(cat+9)*32   owned/usable bits of unlock-shop category cat (256 items)
//   +(cat+19)*32  unlocked bits of the same category
//   +4896   unlock points balance (capped at 99,999,999), +4904 total earned
// The categories come from static_x.mpc sys/shopitem: taunts, shirts, PRIDE
// entrances, UFC fighter calls, unlockable fighters, Ultimate Fight movies,
// nicknames and Topps cards. The item manager is [0x834501CC].
//
// The hooks only change what the checks answer; nothing is written to the
// save, so turning the option off restores the real progress. The game's own
// award path (it asks "already unlocked?" before setting a bit) still sees the
// real state, so items earned while the cheat is on are saved as usual.
//
//   sub_82ED1790(decoded fighter, mask)  fighter selectable? Locked fighters
//       (Chuck Liddell, Mark Coleman, Dan Severn, Heath Herring and their
//       alternate versions) have unlock flag bit 0 in plpm and need their bit.
//   sub_82C558F8(mgr, cat, item)  item unlocked?
//   sub_82C55A30(mgr, cat, item)  item owned/usable (CAF editor lists)?
//   sub_82C56C88(mgr, cat)        number of owned items in a category
//   sub_82C556F0(r3, points)      spend unlock points

#pragma once

#include <atomic>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_unlock_all, false, "ufc3",
                    "Treat every fighter and unlock-shop item (taunts, clothes, entrances, "
                    "movies, nicknames, Topps cards) as unlocked; the save is not changed");
REXCVAR_DEFINE_BOOL(ufc3_free_unlock_points, false, "ufc3",
                    "Buying unlock-shop items does not spend unlock points");
REXCVAR_DEFINE_BOOL(ufc3_trace_unlocks, false, "ufc3",
                    "Log unlock checks with their callers and results");

REX_EXTERN(__imp__sub_82ED1790);  // fighter unlock check
REX_EXTERN(__imp__sub_82C558F8);  // item unlocked?
REX_EXTERN(__imp__sub_82C55A30);  // item owned?
REX_EXTERN(__imp__sub_82C56C88);  // owned items in a category
REX_EXTERN(__imp__sub_82C556F0);  // spend unlock points
REX_EXTERN(sub_82C54890);         // item ID -> index in its category (-1 if absent)
REX_EXTERN(sub_82C54850);         // item count of a category

namespace ufc3_unlock {

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

// Callers that award an item: they test "already unlocked?" and set the bit
// if not, so they must see the real answer.
inline bool IsAwardCaller(uint32_t lr) {
  static constexpr uint32_t kRanges[][2] = {
      {0x82C59F30, 0x82C59FB8}, {0x82C5A1A0, 0x82C5A268}, {0x82C5A2D0, 0x82C5A3D8},
      {0x82C5A3D8, 0x82C5A578}, {0x82C5A950, 0x82C5AB40},
  };
  for (auto& r : kRanges) {
    if (lr >= r[0] && lr < r[1]) return true;
  }
  return false;
}

// Logs the first few results per caller/argument so the log stays small.
inline void Trace(const char* what, uint32_t lr, uint32_t a, uint32_t b, uint32_t real,
                  uint32_t result) {
  if (!REXCVAR_GET(ufc3_trace_unlocks)) return;
  static std::atomic<uint32_t> lines{0};
  if (lines.fetch_add(1) >= 4000) return;
  REXLOG_INFO("[unlock] {} from {:08X} ({}, {}) real={} -> {}", what, lr, a, b, real, result);
}

// Index of `item` in category `cat`, or -1 (same lookup the checks use).
inline int32_t ItemIndex(PPCContext& ctx, uint8_t* base, uint32_t mgr, uint32_t cat,
                         uint32_t item) {
  PPCContext c = ctx;
  c.r3.u64 = mgr;
  c.r4.u64 = cat;
  c.r5.u64 = item;
  sub_82C54890(c, base);
  return c.r3.s32;
}

// Shared body of the "unlocked?" and "owned?" hooks.
inline void ItemCheck(PPCContext& ctx, uint8_t* base, PPCFunc* real, const char* what) {
  uint32_t lr = uint32_t(ctx.lr), mgr = ctx.r3.u32, cat = ctx.r4.u32, item = ctx.r5.u32;
  PPCContext saved = ctx;
  real(ctx, base);
  uint32_t result = ctx.r3.u32;
  if (!result && REXCVAR_GET(ufc3_unlock_all) && !IsAwardCaller(lr)) {
    int32_t index = ItemIndex(saved, base, mgr, cat, item);
    if (index >= 0 && index < 256) ctx.r3.u64 = 1;
  }
  Trace(what, lr, cat, item, result, ctx.r3.u32);
}

}  // namespace ufc3_unlock

extern "C" REX_FUNC(sub_82ED1790) {
  using namespace ufc3_unlock;
  uint32_t lr = uint32_t(ctx.lr), rec = ctx.r3.u32, mask = ctx.r4.u32;
  __imp__sub_82ED1790(ctx, base);
  uint32_t real = ctx.r3.u32;
  if (!real && rec && REXCVAR_GET(ufc3_unlock_all)) {
    // Keep every rule except the save's unlock bit: the record must be in use,
    // have a real weight class (< 7) and not be a never-selectable type.
    uint32_t flags = Load32(base, rec + 8);
    bool in_use = Load32(base, rec + 4) != 0;
    bool weight_ok = (Load32(base, rec + 196) & 0x1F00000) < 0x700000;
    bool type_ok = !(flags & 0xE0) || (flags & 0x20);
    if (in_use && weight_ok && type_ok) ctx.r3.u64 = 1;
  }
  if (REXCVAR_GET(ufc3_trace_unlocks) && rec) {
    Trace("fighter", lr, Load32(base, rec), mask, real, ctx.r3.u32);
  }
}

extern "C" REX_FUNC(sub_82C558F8) {
  ufc3_unlock::ItemCheck(ctx, base, __imp__sub_82C558F8, "item-unlocked");
}

extern "C" REX_FUNC(sub_82C55A30) {
  ufc3_unlock::ItemCheck(ctx, base, __imp__sub_82C55A30, "item-owned");
}

extern "C" REX_FUNC(sub_82C56C88) {
  uint32_t mgr = ctx.r3.u32, cat = ctx.r4.u32;
  PPCContext saved = ctx;
  __imp__sub_82C56C88(ctx, base);
  if (REXCVAR_GET(ufc3_unlock_all)) {
    saved.r3.u64 = mgr;
    saved.r4.u64 = cat;
    sub_82C54850(saved, base);
    if (saved.r3.s32 > ctx.r3.s32) ctx.r3.u64 = saved.r3.u64;
  }
}

extern "C" REX_FUNC(sub_82C556F0) {
  if (REXCVAR_GET(ufc3_free_unlock_points)) {
    if (REXCVAR_GET(ufc3_trace_unlocks)) {
      REXLOG_INFO("[unlock] kept {} unlock points (from {:08X})", ctx.r4.s32, uint32_t(ctx.lr));
    }
    return;
  }
  __imp__sub_82C556F0(ctx, base);
}
