// ufc3 - Ultimate difficulty in Career and Title Defense mode are selectable from the start (no setting).
//
// The Career difficulty list (script widget "DifficultyListSTR", class vtable
// 0x82131380, items Beginner / Experienced / Advanced / Expert / Ultimate) is
// set up by sub_82B10680. It turns list item 4 (Ultimate) on only when the
// byte at [sys+0x1909C] (system data [0x83577434], saved in SYSTEM.DAT) is
// non-zero. That byte is a bit mask the career flow (sub_822DD3B0, called
// from the career state machine sub_822DFA68) sets once a career is finished,
// one bit per value of a halfword in the career record (probably the
// difficulty the career was played on).
//
// The hook lets the game run its own setup, then turns item 4 on as the real
// unlock would. The save is not written, so nothing changes in SYSTEM.DAT.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/logging.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_82B10680);  // career difficulty list: enable Ultimate if unlocked
REX_EXTERN(sub_82969588);         // list widget -> item i (nullptr if absent)

extern "C" REX_FUNC(sub_82B10680) {
  uint32_t list = ctx.r3.u32;
  PPCContext c = ctx;
  __imp__sub_82B10680(ctx, base);
  c.r3.u64 = list;
  c.r4.u64 = 4;
  sub_82969588(c, base);
  uint32_t item = c.r3.u32;
  if (item) {
    // Same field the game writes: (item - 4) + 16 = 1 (enabled).
    uint32_t one = _byteswap_ulong(1u);
    std::memcpy(base + item + 12, &one, 4);
  }
  uint32_t sys;
  std::memcpy(&sys, base + 0x83577434, 4);
  sys = _byteswap_ulong(sys);
  REXLOG_INFO("[ultimate] career difficulty list: save unlock mask {:02X}, Ultimate item {:08X} on",
              sys ? base[sys + 0x1909C] : 0, item);
}

// Title Defense (Game Modes list) from the start, same rule: nothing written to the save.
//
// The main menu enables Game Modes item 2 (Title Defense) with the word at [sys+0x118D4] (sub_829391B0 passes it
// as the enable flag to sub_82938338), and its StartTitleDefenseDialog binding (sub_829376E8) opens dialog 2012
// (the "locked" notice) instead of starting the mode when the word is 0. Both read it only through the getter
// sub_82974888, so the getter answers "unlocked" (1) when the save says 0.
REX_EXTERN(__imp__sub_82974888);  // save: Title Defense unlocked word

extern "C" REX_FUNC(sub_82974888) {
  __imp__sub_82974888(ctx, base);
  if (ctx.r3.u32 == 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("[ultimate] Title Defense: save word 0 -> unlocked (lr {:08X})", uint32_t(ctx.lr));
    }
    ctx.r3.u64 = 1;
  }
}
