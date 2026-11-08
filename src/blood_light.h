// ufc3 - blood keeps its colour under the fighters' white light terms (--ufc3_blood_light_fix).
//
// The skin pixel shaders (body 897408EB, face 7B7BD1AC) screen two white terms over the colour: an
// overbright highlight, large under strong lights such as PRIDE's, and a rim light L, large on ground
// cameras with ufc3_steady_skin_light on. Blood only tints the colour, so it reads pale pink under
// them. The patch scales both by the green of the blood tint, which is 1 - mask (g_f4BSSBldClr green
// is 0): bare skin is untouched. See tools/gen_blood_light_patch.py for the patched instructions.

#pragma once

#include <rex/cvar.h>

#include "blood_light_patterns.h"
#include "mod_pack.h"
#include "shader_patch.h"

REXCVAR_DEFINE_BOOL(ufc3_blood_light_fix, true, "ufc3",
                    "Blood keeps its colour under the fighters' white light terms (skin is unchanged)");

namespace ufc3_blood_light {

inline ufc3_shader_patch::Feature g_feature = [] {
  ufc3_shader_patch::Feature f;
  f.name = "blood_light";
  f.wanted = [] { return REXCVAR_GET(ufc3_blood_light_fix) && ufc3_pack::Active(); };  // Blood and Guts look: pack only
  f.targets.push_back({kBodyOrig, sizeof(kBodyOrig), kBodyOff, kBodyPatch, sizeof(kBodyPatch)});
  f.targets.push_back({kFaceOrig, sizeof(kFaceOrig), kFaceOff, kFacePatch, sizeof(kFacePatch)});
  f.sites.resize(2);
  return f;
}();
inline const bool g_registered = (ufc3_shader_patch::Features().push_back(&g_feature), true);

}  // namespace ufc3_blood_light
