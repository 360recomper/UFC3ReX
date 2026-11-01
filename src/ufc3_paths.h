// ufc3 - resolved data roots.
//
// Ufc3App::OnConfigurePaths fills paths.game_data_root / user_data_root with
// defaults (<exe>\assets, <exe>\userdata) that never go back into the
// game_data_root / user_data_root cvars, so code that needs the real folders
// asks here: the cvar first, then the resolved path.

#pragma once

#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>

// Path cvars hold UTF-8; build paths with rex::to_path, never path(std::string)
// (that reads the ANSI codepage and breaks non-ASCII folders, GitHub #3).
namespace ufc3_paths {

inline std::filesystem::path& ResolvedGameDataRoot() {
  static std::filesystem::path p;
  return p;
}

inline std::filesystem::path& ResolvedUserDataRoot() {
  static std::filesystem::path p;
  return p;
}

inline std::filesystem::path GameDataRoot() {
  std::string cvar = rex::cvar::GetFlagByName("game_data_root");
  return cvar.empty() ? ResolvedGameDataRoot() : rex::to_path(cvar);
}

inline std::filesystem::path UserDataRoot() {
  std::string cvar = rex::cvar::GetFlagByName("user_data_root");
  return cvar.empty() ? ResolvedUserDataRoot() : rex::to_path(cvar);
}

}  // namespace ufc3_paths
