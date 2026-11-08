// ufc3 - steady fighter lighting across camera angles
// (--ufc3_steady_skin_light, default on; false = the original top-down blend). FINDINGS-skin-darken.md.
//
// Skin goes darker / more orange whenever the camera looks steeply down at the
// fighters (clinch, takedown and ground cameras). The game does it on purpose:
// sub_82EEB500 turns the camera's angle (camera +512) into a weight
//   w = clamp(clamp((pi - angle) * 2/pi, 0, 1) * 2.25, 0, 1)
// that is 0 on the side cameras and 1 looking down, and every fighter lighting
// setup blends by it:
// - sub_8273D0E0 sets g_f4ShadowParam (skin PS c170) = lerp(side preset at
//   [0x833C8108]+232, top-down preset at +212, w);
// - sub_8273D210 lerps the fighter light colours (light set +128 -> +796 by
//   w * set[992]); their sum is g_f4DifLghMax / g_f4DifLghMaxA (c183 / c185),
//   and the light direction in g_fm44ViewLight (c168) follows too.
// Top-down = less of the pale wrap/rim light and a warmer specular cube.
//
// With the option on the weight is always 0, so every camera gets the side
// (stand-up) look. Off = the original. Default off until a real-Xbox capture
// shows the stand-up tone on the ground too.

#pragma once

#include <rex/cvar.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(ufc3_steady_skin_light, true, "ufc3",
                    "Keep the fighters' stand-up (side camera) lighting on clinch/ground cameras "
                    "instead of the game's darker top-down blend");

// Share of the original top-down blend kept with the option on. At 0 thin chest blood on a fighter
// lying down reads pale pink (dmg4z5-heavy vs -nosteady), under the pale wrap/rim light.
REXCVAR_DEFINE_DOUBLE(ufc3_steady_skin_topdown, 0.0, "ufc3",
                      "With ufc3_steady_skin_light on: how much of the top-down blend to keep, 0..1");

REX_EXTERN(__imp__sub_82EEB500);  // camera-pitch weight for the fighter lighting (f1, 0 = side camera)

extern "C" REX_FUNC(sub_82EEB500) {
  __imp__sub_82EEB500(ctx, base);
  if (REXCVAR_GET(ufc3_steady_skin_light)) ctx.f1.f64 *= REXCVAR_GET(ufc3_steady_skin_topdown);
}
