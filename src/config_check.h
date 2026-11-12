// ufc3 - ufc3.toml health check (player report 2026-11-12: launcher settings ignored).
//
// The SDK loads ufc3.toml next to the exe with toml++. If the file does not parse, the SDK drops the
// WHOLE file and logs the error before logging is set up, so nothing shows: every launcher setting
// (fullscreen, resolution_scale, ufc3_fighter_music, ...) silently falls back to its default.
//
// How a release folder's file broke: the SDK's SaveConfig (every F4 quick-setting change) writes
// string cvars as "<value>" with no escaping, and the game set mods_root at runtime to the exe
// folder's mods path with Windows backslashes ("C:\Games\UFC3\mods"). "\G", "\U" etc. are invalid
// TOML escapes, so after the first F4 change the file never parsed again. The launcher's own
// line-based reader does not care, so it kept showing the player's settings as saved.
//
// This runs from a static initializer, before main() and so before the SDK's LoadConfig: when the
// file does not parse, the backslashes inside "..." values (Windows paths) become forward slashes,
// the original is kept as ufc3.toml.broken and the repaired file is written. Anything still broken
// is logged after logging starts (LogConfigCheck) and shown in a message box.

#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <thread>

namespace ufc3_config_check {

struct Result {
  std::filesystem::path file;
  bool present = false;
  bool ok = true;            // parses now (as found or after the repair)
  bool repaired = false;     // backslashes fixed and the file rewritten
  int fixed_lines = 0;
  std::string first_error;   // the error as found
  std::string still_error;   // error left after the repair (ok == false)
};

inline Result& State() {
  static Result r;
  return r;
}

// "" = parses; else "line N: description".
inline std::string ParseError(const std::string& text) {
#if TOML_EXCEPTIONS
  try {
    (void)toml::parse(text);
    return {};
  } catch (const toml::parse_error& e) {
    return "line " + std::to_string(e.source().begin.line) + ": " + std::string(e.description());
  }
#else
  auto r = toml::parse(text);
  if (r) return {};
  return "line " + std::to_string(r.error().source().begin.line) + ": " +
         std::string(r.error().description());
#endif
}

// key = "value with \ backslashes"  ->  key = "value with / backslashes". Only lines whose value
// is one basic string; a backslash right before the closing quote also becomes '/'. Valid escapes
// (\\ \" \n ...) are left alone only when the line already parses on its own.
inline bool FixLine(std::string& line) {
  size_t eq = line.find('=');
  if (eq == std::string::npos) return false;
  size_t key_start = line.find_first_not_of(" \t");
  if (key_start == std::string::npos || line[key_start] == '#' || line[key_start] == '[') return false;
  size_t open = line.find_first_not_of(" \t", eq + 1);
  if (open == std::string::npos || line[open] != '"') return false;
  size_t close = line.find_last_of('"');
  if (close <= open) return false;
  std::string value = line.substr(open + 1, close - open - 1);
  if (value.find('\\') == std::string::npos) return false;
  if (ParseError(line + "\n").empty()) return false;  // already valid (e.g. "C:\\x")
  // Collapse escaped "\\" first so a half-escaped path doesn't end up with "//".
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\') {
      out += '/';
      if (i + 1 < value.size() && value[i + 1] == '\\') ++i;
    } else {
      out += value[i];
    }
  }
  line = line.substr(0, open + 1) + out + line.substr(close);
  return true;
}

inline void Check() {
  Result& r = State();
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return;
  r.file = std::filesystem::path(buf).parent_path() / L"ufc3.toml";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(r.file, ec)) return;
  r.present = true;
  std::string text;
  {
    std::ifstream in(r.file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  r.first_error = ParseError(text);
  if (r.first_error.empty()) return;
  r.ok = false;

  // Repair line by line (keep the file's own line ends).
  std::vector<std::string> lines;
  std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  {
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
  }
  for (auto& line : lines) {
    if (FixLine(line)) ++r.fixed_lines;
  }
  if (r.fixed_lines == 0) {
    r.still_error = r.first_error;
    return;
  }
  std::string fixed;
  for (const auto& line : lines) fixed += line + eol;
  r.still_error = ParseError(fixed);
  if (!r.still_error.empty()) return;  // don't write a half-fixed file

  auto backup = r.file;
  backup += L".broken";
  std::filesystem::copy_file(r.file, backup, std::filesystem::copy_options::overwrite_existing, ec);
  std::ofstream out(r.file, std::ios::binary | std::ios::trunc);
  if (!out) {
    r.still_error = "could not write the repaired file";
    return;
  }
  out << fixed;
  out.close();
  r.ok = true;
  r.repaired = true;
}

// Runs before main(): the SDK reads ufc3.toml in ReXApp::SetupEnvironment, after static init.
inline const bool kChecked = (Check(), true);

// After logging starts (Ufc3App::OnPostInitLogging).
inline void LogConfigCheck() {
  const Result& r = State();
  const std::string file = rex::path_to_utf8(r.file);
  if (!r.present) {
    REXLOG_INFO("[config] no ufc3.toml next to the exe ({}): defaults", file);
    return;
  }
  if (r.repaired) {
    REXLOG_WARN("[config] {} did not parse ({}); repaired {} line(s) with Windows paths "
                "(backslashes -> /), original kept as ufc3.toml.broken",
                file, r.first_error, r.fixed_lines);
  }
  if (r.ok) {
    REXLOG_INFO("[config] {} loaded", file);
    return;
  }
  REXLOG_ERROR("[config] {} does not parse ({}): NONE of its settings are applied "
               "(launcher settings, fullscreen, resolution, music all at defaults)",
               file, r.still_error);
  // Visible: a non-blocking message box (the game keeps starting behind it).
  std::wstring msg =
      L"ufc3.toml has an error, so none of your settings (launcher Settings tab, F4 menu, "
      L"walkout music) are used this time. The game runs on default settings.\n\n" +
      std::filesystem::path(rex::to_path(r.still_error)).wstring() + L"\n\nFile: " +
      r.file.wstring() +
      L"\n\nFix that line, or delete ufc3.toml and set your options again in the launcher.";
  std::thread([msg] {
    MessageBoxW(nullptr, msg.c_str(), L"Undisputed Ultimate Edition - settings not loaded",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
  }).detach();
}

}  // namespace ufc3_config_check
