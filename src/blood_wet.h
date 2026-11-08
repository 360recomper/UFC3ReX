// ufc3 - blood on the fighters' skin keeps a wet specular (--ufc3_blood_wet).
//
// The skin pixel shaders (body 897408EB, face 7B7BD1AC) weight the base specular lobes by
// lerp(diffuse alpha, wet, (1 - mask) * sweat), so full blood is left with the skin's alpha, 0.11-0.23 on average.
// The patch gives that weight a floor that rises with the mask, to 0.6 at full mask (FLOOR_CONST 143 = 1.0 blew out white on chest and hands, strikers-z45-wet).
// At mask 0 the floor is +0 and skin is unchanged. Gloved blood changes too: shaders can't tell the mode.
// See FINDINGS-blood-wet.md and tools/gen_blood_wet_patch.py.
//
// The patch and its context lie before what blood_light changes, so the two work in any combination.
// A feature that starts off is never scanned for, so set this at launch: after the first 90 s
// shader_patch.h has stopped scanning, and turning it on would find no site.

#pragma once

#include <rex/cvar.h>

#include "blood_wet_patterns.h"
#include "mod_pack.h"
#include "shader_patch.h"

REXCVAR_DEFINE_BOOL(ufc3_blood_wet, false, "ufc3",
                    "Blood on skin keeps a wet specular (skin is unchanged, gloved blood changes too; set at launch)");

namespace ufc3_blood_wet {

inline ufc3_shader_patch::Feature g_feature = [] {
  ufc3_shader_patch::Feature f;
  f.name = "blood_wet";
  f.wanted = [] { return REXCVAR_GET(ufc3_blood_wet) && ufc3_pack::Active(); };  // pack only
  f.targets.push_back({kBodyOrig, sizeof(kBodyOrig), kBodyOff, kBodyPatch, sizeof(kBodyPatch)});
  f.targets.push_back({kFaceOrig, sizeof(kFaceOrig), kFaceOff, kFacePatch, sizeof(kFacePatch)});
  f.sites.resize(2);
  return f;
}();
inline const bool g_registered = (ufc3_shader_patch::Features().push_back(&g_feature), true);

}  // namespace ufc3_blood_wet
