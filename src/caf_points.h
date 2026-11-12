// ufc3 - opt-in cheat: Create-A-Fighter attribute and skill levels cost no points.
//
// How CAF stat points work (FIGHTER ABILITIES -> STATS -> ATTRIBUTES / SKILLS, class with vtable 0x82117B40):
//   - Every attribute and skill has a LEVEL (shown as the number on its bar, 1..100). The editor object holds the
//     current level at +744, the cap at +748, the points pool at +752 (u16) and the 4 attribute levels at +756..759.
//   - The pool comes from the CAF parameter table P = [0x83574A28]: P+176 = 1350 for a new fighter outside career
//     (sub_82A13848; +1050 more with the old "boost" flag), and is saved in the fighter's record as 13 bits of
//     record+688 (sub_82A13710 loads it, sub_82A137E8 stores it). Career (mode 10) uses P+226/228/230 instead.
//     Skills are the same screen class with their own pool (sub_82A13B50: sub_827EE008, 4250 for a new
//     welterweight, +4550 with the boost flag) and 18 levels at +760..777.
//   - Each level up costs points from P+184's table, worked out by sub_82901D80(dir, level, costs, bounds):
//     levels 1-30 cost 1, 31-60 cost 5, 61-70 15, 71-80 25, 81-90 40, 91-95 60, 96-100 80. LEFT refunds the same.
//     sub_82A13F70 (attributes) / sub_82A14108 (skills) only raise a level while level < cap and pool >= cost.
//   - The cap is per weight class and stat (sub_827EDEC0: P+234.., 85-100), e.g. a welterweight's strength 95.
//     One stat from 1 to 100 costs 1679 points, so 1350 can't even max one; all 22 stats need ~30,000.
//
// With ufc3_caf_max_points the cost function answers 0 outside career mode, so every level up (and down) is free:
// POINTS NEEDED shows 0, the pool never changes, and each stat can go up to the game's own cap for that weight
// class. Nothing new is written: the levels stay inside the game's limits and the saved pool is the real one, so the
// fighter loads the same in the stock game. Career-mode stat screens keep their normal costs.

#pragma once

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_caf_max_points, false, "ufc3",
                    "Create-A-Fighter: raising attributes and skills costs no points, so every stat can go up to "
                    "the game's own cap for the weight class (not in career mode)");

REX_EXTERN(__imp__sub_82901D80);  // CAF stat level cost: (dir, level, costs, bounds) -> points

namespace ufc3_caf_points {

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

// Game mode byte (sub_82EE8990 = [0x83574A00]+13); 10 = career, where the same screen spends career points.
inline bool InCareer(uint8_t* base) {
  uint32_t mode_obj = Load32(base, 0x83574A00);
  return mode_obj && base[mode_obj + 13] == 10;
}

}  // namespace ufc3_caf_points

// Only the CAF stat editor calls this (sub_82A139C0 / sub_82A13D38 / sub_82A14108).
extern "C" REX_FUNC(sub_82901D80) {
  __imp__sub_82901D80(ctx, base);
  if (ctx.r3.s32 != 0 && REXCVAR_GET(ufc3_caf_max_points) && !ufc3_caf_points::InCareer(base)) {
    ctx.r3.u64 = 0;
  }
}
