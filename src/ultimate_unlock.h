// ufc3 - Ultimate difficulty in Career is selectable from the start.
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
