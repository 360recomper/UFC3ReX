// ufc3 - mod pack marker: the public exe plays stock, the mod pack unlocks the extras (merge7, user 2026-11-07).
//
// <mods_root>/ufc3_modpack.marker, first line "UFC3-MODPACK-MARKER <version>", written by every pack build
// (tools/modpack_marker.py, called by build_modpack.py and rebuild_integration.py). Checked once at boot
// (Ufc3App::OnPreSetup, after the paths and any test config are in), before any gated cvar is read.
//
// Without the marker (no mods root, mods_enabled=false, or a mods folder without it) the extras are off,
// whatever ufc3.toml, the launcher or the command line say:
//   - Blood and Guts: every blood.h hook passes straight through (stock blood levels, chest, knuckles, wash,
//     replay restore, liquid state copy, hand meshes, blood tint), blood_light / blood_wet shader patches off,
//     cut and bleed rates 1 (stock), doctor stoppage stock, debug cut/doctor forcing off.
//   - Bare knuckle: hands stay gloved, R3 does nothing, no R3 legend hint.
//   - Attire picker: L3 does nothing, no L3 legend hint, attire_counts.txt ignored.
// Every fix (renderer, native, shader-patch stutter fix, crash fixes) stays on for everyone.
// The marker only decides whether the code runs; there is no cvar to override it.

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace ufc3_pack {

inline constexpr const char* kMarkerName = "ufc3_modpack.marker";
inline constexpr const char* kMagic = "UFC3-MODPACK-MARKER";

inline std::atomic<bool>& ActiveRef() {
  static std::atomic<bool> a{false};
  return a;
}

// Reads the marker once and logs which mode is active. Called from OnPreSetup; Active() calls it too in
// case a hook ever runs first.
inline bool Latch() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::string enabled = rex::cvar::GetFlagByName("mods_enabled");
    std::string mods = rex::cvar::GetFlagByName("mods_root");
    std::string why;
    bool active = false;
    if (enabled == "false") {
      why = "mods_enabled = false";
    } else if (mods.empty()) {
      why = "no mods folder";
    } else {
      std::filesystem::path p = rex::to_path(mods) / kMarkerName;
      std::ifstream f(p, std::ios::binary);
      std::string line;
      if (!f || !std::getline(f, line)) {
        why = "no " + std::string(kMarkerName) + " in " + mods;
      } else if (line.rfind(kMagic, 0) != 0) {
        why = std::string(kMarkerName) + " has the wrong contents";
      } else {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        why = line.substr(std::char_traits<char>::length(kMagic));
        while (!why.empty() && why.front() == ' ') why.erase(why.begin());
        active = true;
      }
    }
    ActiveRef().store(active, std::memory_order_relaxed);
    if (active)
      REXLOG_INFO("[pack] mod pack marker found (version {}): pack mode, Blood and Guts, bare knuckle and attire picker available",
                  why.empty() ? "?" : why);
    else
      REXLOG_INFO("[pack] stock mode ({}): Blood and Guts, bare knuckle and attire picker off", why);
  });
  return ActiveRef().load(std::memory_order_relaxed);
}

inline bool Active() {
  static std::atomic<bool> latched{false};
  if (!latched.load(std::memory_order_acquire)) {
    Latch();
    latched.store(true, std::memory_order_release);
  }
  return ActiveRef().load(std::memory_order_relaxed);
}

}  // namespace ufc3_pack
