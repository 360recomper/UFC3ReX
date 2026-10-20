// ufc3 - opt-in bloom off (--ufc3_no_bloom, also in the F4 menu).
//
// The bloom is a post effect in shader/fxl.pac (techniques T_HighPass /
// T_HighPass2 -> T_GaussianBlur5x5 -> T_Bloom/T_Star -> T_MergeTextures). The
// game picks techniques and sets g_f4HiPass without any name in the executable,
// so there is no guest function to hook. Instead the bright-pass pixel shader
// (the same 5-ALU microcode in both T_HighPass and T_HighPass2: tfetch, then
// luminance dot3 ... and a final `oC0 = r0 * r0'`) is patched in guest memory so
// its last instruction writes r0 + (-r0) = 0. The bright-pass target stays
// black, so blur, star and merge still run and every other effect (depth of
// field, tone mapping, replay filters) is untouched; the merge just adds nothing.

#pragma once

#include <rex/cvar.h>

#include "shader_patch.h"

REXCVAR_DEFINE_BOOL(ufc3_no_bloom, false, "ufc3",
                    "Turn the bloom/glow post effect off (blacks out its bright-pass shader)");

namespace ufc3_no_bloom {

// Last two ALU instructions of the bright-pass shader (big-endian dwords) and
// the replacement for the last one: ADDv oC0 = r0 + (-r0) instead of MULv.
inline constexpr uint8_t kOrig[] = {
    0x4c, 0x1e, 0x00, 0x00, 0x00, 0xfc, 0xb1, 0x6c, 0xe1, 0x01, 0x00, 0x00,  // ALU3
    0xc8, 0x8f, 0xc0, 0x00, 0x00, 0x15, 0x6c, 0x00, 0xe1, 0x00, 0x00, 0x00,  // ALU4
};
inline constexpr uint8_t kPatch[] = {
    0xc8, 0x8f, 0xc0, 0x00, 0x04, 0x6c, 0x6c, 0x00, 0xe0, 0x00, 0x00, 0x00,
};

inline ufc3_shader_patch::Feature g_feature = [] {
  ufc3_shader_patch::Feature f;
  f.name = "no_bloom";
  f.wanted = [] { return REXCVAR_GET(ufc3_no_bloom); };
  f.targets.push_back({kOrig, sizeof(kOrig), 12, kPatch, sizeof(kPatch)});
  f.sites.resize(1);
  return f;
}();
inline const bool g_registered = (ufc3_shader_patch::Features().push_back(&g_feature), true);

}  // namespace ufc3_no_bloom
