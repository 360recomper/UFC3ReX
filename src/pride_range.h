// ufc3 - PRIDE fighter ID range.
//
// The game treats PRIDE fighters as IDs 300-333 (plus the alternates 1015-1023)
// in fixed places. The fighter-list builder sub_82EF6038 writes its ID ranges
// to the stack at entry: UFC, UFC alternates, PRIDE {300, 334}, PRIDE alternates.
// A PRIDE twin at ID 334+ (build_roster_mod.py "pride_id") is never enumerated,
// so the PRIDE list lacks it. The mid-asm hooks (config/functions.toml) raise
// the PRIDE end to --ufc3_pride_id_end, and make the two "is PRIDE" label checks
// (sub_829A5378, sub_82A70350) treat 334..end-1 as PRIDE.

#pragma once

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_UINT32(ufc3_pride_id_end, 499, "ufc3",
                      "End (exclusive) of the PRIDE fighter ID range the game enumerates; the disc "
                      "uses 334. Needed for PRIDE twins at IDs 334-498");

namespace ufc3_pride {
inline uint32_t End() {
  uint32_t e = REXCVAR_GET(ufc3_pride_id_end);
  return e < 334 ? 334 : (e > 499 ? 499 : e);
}
}  // namespace ufc3_pride

// 0x82EF6118, after "li r20,334": the PRIDE range end.
void ufc3_pride_range_end(PPCRegister& r20) { r20.s64 = ufc3_pride::End(); }

// 0x829A53C4 / 0x82A70414, before "cmpwi r11,300": fold the new PRIDE IDs into
// the disc range so the check passes. r11 is dead afterwards in both.
void ufc3_pride_fold(PPCRegister& r11) {
  int32_t id = r11.s32;
  if (id >= 334 && id < static_cast<int32_t>(ufc3_pride::End())) r11.s64 = 300;
}
