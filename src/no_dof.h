// ufc3 - opt-in depth-of-field blur off (--ufc3_no_dof, also in the F4 menu).
//
// The depth-of-field composite shaders in shader/fxl.pac (8 variants: with or
// without luster, fish eye and YCbCr) lerp the sharp full-size scene with a
// blurred down-sized copy by depth. Each fetches the blurred copy through
// texture fetch constant 2 (g_smpDownSource); patching that fetch to constant 0
// (the sharp scene) turns the lerp into a no-op. Everything else in the pass
// (luster, fish eye, colour grading) still runs. tools/gen_dof_patch.py
// generates dof_patterns.h from the pac.

#pragma once

#include <rex/cvar.h>

#include "dof_patterns.h"
#include "shader_patch.h"

REXCVAR_DEFINE_BOOL(ufc3_no_dof, false, "ufc3",
                    "Turn the depth-of-field blur off (the blurred copy is swapped for the sharp scene)");

namespace ufc3_no_dof {

inline ufc3_shader_patch::Feature g_feature = [] {
  ufc3_shader_patch::Feature f;
  f.name = "no_dof";
  f.wanted = [] { return REXCVAR_GET(ufc3_no_dof); };
  for (auto& s : kShaders) f.targets.push_back({s.ucode, s.len, s.off, s.patch, 4});
  f.sites.resize(f.targets.size());
  return f;
}();
inline const bool g_registered = (ufc3_shader_patch::Features().push_back(&g_feature), true);

}  // namespace ufc3_no_dof
