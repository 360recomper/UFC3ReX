// Native renderer switch (FINDINGS-native-renderer.md, NR-4 plan > "How the switch works").
//
// ufc3_renderer (emulated | native) is read once in Ufc3App::OnPreSetup and latched here. Every
// native hook starts with `if (!g_ufc3_native) return;`, so with the default (emulated) the game
// runs exactly as on main. Nothing switches renderers after startup.

#pragma once

inline bool g_ufc3_native = false;
