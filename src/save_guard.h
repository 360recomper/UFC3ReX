// ufc3 - crash-safe saves (all-or-nothing per save, like the Xbox 360's own storage).
//
// Why: the SDK maps a saved-game package to a plain folder and writes the game's files in place.
// UFC 3 saves its system data (SYSTEM.DAT = settings, unlocks AND careers, CAFLIST.DAT, HLRQLIST.DAT,
// ...) by mounting UFC2012_SYSTEM_DATA (XamContentCreateEx OPEN_EXISTING) and re-creating each file
// (NtCreateFile disposition 5 = truncate to 0, then one NtWriteFile); created fighters and replay
// queues use CREATE_ALWAYS, which makes the SDK delete the whole package folder first. A real console
// commits a package only at XamContentClose, so a power cut never leaves half a save. Here, anything
// that stops the process between the truncate/delete and the end of the write (window closed or
// Alt+F4 during a save: the SDK's close path kills the guest threads and _Exit()s at once; a crash;
// Task Manager; a PC shutdown or power cut before Windows flushed the data) leaves a short, empty or
// zero-filled file. Every file carries a keyed SHA-256 digest (below); on the next boot the game finds
// it wrong and says "The game data appears to be damaged. Would you like to create a new save game?",
// and YES erases the career and the created fighters.
//
// What this does (no SDK change; wrappers around the guest's imports, as in import_trace.h):
// - First write in a mounted saved-game package (NtCreateFile/NtOpenFile with write intent or a
//   truncating disposition, or XamContentCreateEx CREATE_ALWAYS/TRUNCATE_EXISTING on an existing
//   package): copy the package (and its .header) to
//   <userdata>/<xuid>/<title>/ufc3-save-backup/<type>/<name>/ (via <name>.new, flushed, then swapped
//   in; never over a good backup with files that fail the game's check), and only then write the
//   marker <name>.saving. The game writes as before.
// - XamContentClose of that root = the save is complete: flush the files to disk, verify them with the
//   game's own digest, delete the marker. The backup stays as the previous good save (replay queues:
//   deleted, 12 MB each).
// - Boot (first content call) and every XamContentCreateEx of a package, before the game reads it:
//   a marker left behind (the save never finished), or files that would fail the game's check while
//   the backup passes -> the backup is copied back. The broken files are kept in
//   ufc3-save-backup/<type>/<name>.broken-<time>/.
// - Window close (ufc3_app.h OnWindowCloseRequested): while a save is in progress the close waits for
//   XamContentClose (up to ufc3_save_close_wait_ms), then closes.
// The backup folder is not under a content-type folder, so the game's content enumeration never lists it.
//
// Cvars: ufc3_save_guard (default on), ufc3_trace_saves (save opens/reads/writes/closes and the
// game's SHA-256 calls), ufc3_save_close_wait_ms; test only: ufc3_save_test_kill_after_writes,
// ufc3_save_test_write_delay_ms, ufc3_save_test_force_system_save (first_run_save.h).

#pragma once

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <initializer_list>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

#include "ufc3_paths.h"

REXCVAR_DEFINE_BOOL(ufc3_save_guard, true, "ufc3",
                    "Crash-safe saves: back up a save package before the game rewrites it and "
                    "restore it on the next boot if the save never finished");
REXCVAR_DEFINE_BOOL(ufc3_trace_saves, false, "ufc3",
                    "Log saved-game package opens, file writes and closes");
REXCVAR_DEFINE_INT32(ufc3_save_close_wait_ms, 10000, "ufc3",
                     "Closing the window during a save waits this long for the save to finish");
REXCVAR_DEFINE_INT32(ufc3_save_test_write_delay_ms, 0, "ufc3",
                     "TEST ONLY: sleep this long before each save-file write (a slow save, to test "
                     "that closing the window waits for it)");
REXCVAR_DEFINE_INT32(ufc3_save_test_kill_after_writes, -1, "ufc3",
                     "TEST ONLY: terminate the process in the first save of the run: 0 = right "
                     "after the first file is opened for writing (truncated), N = after the Nth "
                     "write call. -1 = off");

namespace ufc3_save {

namespace fs = std::filesystem;

constexpr uint32_t kSavedGameType = 1;
constexpr const char* kBackupDirName = "ufc3-save-backup";

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

inline uint16_t Load16(uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, 2);
  return _byteswap_ushort(v);
}

inline std::string GuestCString(uint8_t* base, uint32_t addr, size_t max = 64) {
  if (!addr) return {};
  const char* s = reinterpret_cast<const char*>(base + addr);
  return std::string(s, strnlen(s, max));
}

// X_OBJECT_ATTRIBUTES {root_directory, name_ptr -> X_ANSI_STRING {u16 len, u16 max, u32 ptr}}
inline std::string GuestObjectName(uint8_t* base, uint32_t attrs) {
  if (!attrs) return {};
  uint32_t name = Load32(base, attrs + 4);
  if (!name) return {};
  uint16_t len = Load16(base, name);
  uint32_t buf = Load32(base, name + 4);
  if (!buf || !len) return {};
  return std::string(reinterpret_cast<const char*>(base + buf), len);
}

inline std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(toupper(c)); });
  return s;
}

inline bool Tracing() { return REXCVAR_GET(ufc3_trace_saves); }

// A package: <userdata>\<xuid>\<title>\<type>\<name>\; its backup area is
// <userdata>\<xuid>\<title>\ufc3-save-backup\<type>\.
struct Package {
  fs::path dir;     // the package folder
  fs::path header;  // <title>\Headers\<type>\<name>.header
  fs::path bk;      // <title>\ufc3-save-backup\<type>
  std::string name;
  std::string root;  // guest root it is being saved under (upper case), "" = unknown
  bool valid() const { return !dir.empty(); }
  fs::path backup() const { return bk / name; }
  fs::path marker() const { return bk / (name + ".saving"); }
};

inline Package PackageFromDir(const fs::path& dir) {
  Package p;
  fs::path type_dir = dir.parent_path();
  if (Upper(type_dir.filename().string()) != "00000001") return p;  // saved games only
  fs::path title_dir = type_dir.parent_path();
  p.dir = dir;
  p.name = dir.filename().string();
  p.header = title_dir / "Headers" / type_dir.filename() / (p.name + ".header");
  p.bk = title_dir / kBackupDirName / type_dir.filename();
  return p;
}

struct State {
  std::mutex mutex;
  std::map<std::string, Package> active;     // key: package dir (upper), save in progress
  std::map<uint32_t, std::string> handles;   // guest file handle -> package key (write opens)
  std::map<uint32_t, std::string> reads;      // trace only: guest handle -> save file path (read opens)
  std::map<std::string, std::string> fresh;  // package key -> root: new package, nothing to back up
  std::once_flag recovered;
  std::atomic<int> active_count{0};
  std::atomic<int> writes{0};                // test kill counter
  std::atomic<bool> kill_armed{true};
};

inline State& S() {
  static State s;
  return s;
}

inline std::string Key(const fs::path& p) { return Upper(rex::path_to_utf8(p.lexically_normal())); }

// ---- host file helpers ----------------------------------------------------------------------

inline void FlushFile(const fs::path& file) {
  HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  FlushFileBuffers(h);
  CloseHandle(h);
}

inline void FlushTree(const fs::path& dir) {
  std::error_code ec;
  if (fs::is_regular_file(dir, ec)) {
    FlushFile(dir);
    return;
  }
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    if (it->is_regular_file(ec)) FlushFile(it->path());
  }
}

inline uint64_t TreeBytes(const fs::path& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  }
  return total;
}

// Copy src to dst (dst replaced), flushed. False on any error.
inline bool CopyTree(const fs::path& src, const fs::path& dst) {
  std::error_code ec;
  fs::remove_all(dst, ec);
  ec.clear();
  fs::create_directories(dst, ec);
  if (ec) return false;
  fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  FlushTree(dst);
  return true;
}

inline bool WriteMarker(const fs::path& marker, const std::string& text) {
  HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  return n == text.size();
}

inline std::string NowStamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond);
}

// ---- the game's own integrity check -----------------------------------------------------------
//
// What UFC 3 checks when it loads a save (traced from its XeCryptSha256 calls, reproduced offline on
// every file of the test save): each file starts with a 32-byte keyed digest,
//   most files (CAFLIST, HLR*LIST, CAROLIST, PNTLIST, STR, CAFGEAR/TEX/TML, SEQ, SEQPRM):
//     [0x00,0x20) = SHA256( SHA256([0x20, end)) || kKeyData )
//   SYSTEM.DAT (0x49020 bytes): [0x00,0x20) = SHA256( SHA256([0x20,0x40) + [0x80,0x48BD8) +
//     [0x48C18,end)) || kKeySystem )
// A file that fails is what makes the game say "The game data appears to be damaged. Would you like
// to create a new save game?" (a truncated SYSTEM.DAT and one flipped byte both give that prompt).
// CAFPRM.DAT uses some other layout: only checked for being there and not empty.
constexpr uint8_t kKeySystem[16] = {0x1D, 0xDB, 0x0A, 0xA9, 0xE9, 0xBA, 0x59, 0x4A,
                                    0x5E, 0xB7, 0xF8, 0x6E, 0x82, 0x7A, 0x15, 0x20};
constexpr uint8_t kKeyData[16] = {0x5D, 0x33, 0x40, 0x56, 0x43, 0xE0, 0x3A, 0xE7,
                                  0x1A, 0x5D, 0x8B, 0xE9, 0x91, 0x27, 0x40, 0xAD};

struct Span {
  const uint8_t* p;
  size_t n;
};

inline bool Sha256(std::initializer_list<Span> parts, uint8_t out[32]) {
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0) != 0) return false;
  bool ok = true;
  for (auto& s : parts) {
    if (s.n && BCryptHashData(h, const_cast<PUCHAR>(s.p), ULONG(s.n), 0) != 0) ok = false;
  }
  if (BCryptFinishHash(h, out, 32, 0) != 0) ok = false;
  BCryptDestroyHash(h);
  return ok;
}

enum class Health { kUnknown, kGood, kBad };

inline bool ReadAll(const fs::path& file, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 30);
  if (ok) {
    out.resize(size_t(size.QuadPart));
    DWORD got = 0;
    size_t at = 0;
    while (ok && at < out.size()) {
      DWORD want = DWORD(std::min<size_t>(out.size() - at, 1u << 24));
      ok = ReadFile(h, out.data() + at, want, &got, nullptr) && got == want;
      at += got;
    }
  }
  CloseHandle(h);
  return ok;
}

// kGood = the digest matches (one of the two layouts), kBad = it does not, kUnknown = no digest
// layout known for this file (then only presence/emptiness can be judged by the caller).
inline Health CheckFile(const fs::path& file, bool* known_layout = nullptr) {
  std::vector<uint8_t> f;
  if (known_layout) *known_layout = false;
  if (!ReadAll(file, f)) return Health::kBad;
  uint8_t inner[32], outer[32];
  auto matches = [&](const uint8_t* key) {
    return Sha256({{inner, 32}, {key, 16}}, outer) && std::memcmp(outer, f.data(), 32) == 0;
  };
  if (f.size() > 0x20 && Sha256({{f.data() + 0x20, f.size() - 0x20}}, inner) && matches(kKeyData)) {
    if (known_layout) *known_layout = true;
    return Health::kGood;
  }
  if (f.size() == 0x49020 &&
      Sha256({{f.data() + 0x20, 0x20}, {f.data() + 0x80, 0x48BD8 - 0x80}, {f.data() + 0x48C18, 0x49020 - 0x48C18}},
             inner) &&
      matches(kKeySystem)) {
    if (known_layout) *known_layout = true;
    return Health::kGood;
  }
  return Health::kUnknown;
}

// Cache by (path, size, write time): a 13 MB created-fighter texture is hashed once per change.
inline Health CheckFileCached(const fs::path& file, bool* known_layout) {
  static std::mutex m;
  static std::map<std::string, std::tuple<uint64_t, int64_t, Health, bool>> cache;
  std::error_code ec;
  uint64_t size = fs::file_size(file, ec);
  if (ec) {
    if (known_layout) *known_layout = false;
    return Health::kBad;
  }
  int64_t t = fs::last_write_time(file, ec).time_since_epoch().count();
  std::string key = Key(file);
  {
    std::lock_guard lock(m);
    auto it = cache.find(key);
    if (it != cache.end() && std::get<0>(it->second) == size && std::get<1>(it->second) == t) {
      if (known_layout) *known_layout = std::get<3>(it->second);
      return std::get<2>(it->second);
    }
  }
  bool known = false;
  Health h = CheckFile(file, &known);
  std::lock_guard lock(m);
  cache[key] = {size, t, h, known};
  if (known_layout) *known_layout = known;
  return h;
}

// Files whose digest layout is known (seen verifying in the test save).
inline bool KnownLayoutName(std::string name) {
  name = Upper(name);
  static const char* kNames[] = {"SYSTEM.DAT",  "CAFLIST.DAT", "CAROLIST.DAT", "HLRCLIST.DAT",
                                 "HLRFLIST.DAT", "HLRHLIST.DAT", "HLRQLIST.DAT", "PNTLIST.DAT",
                                 "STR.DAT",     "CAFGEAR.DAT", "CAFTEX.DAT",   "CAFTML.DAT",
                                 "SEQ.DAT",     "SEQPRM.DAT"};
  for (auto* n : kNames)
    if (name == n) return true;
  return false;
}

// Would the game accept this package? Every file of `reference` (the backup, if there is one, else the
// package itself) must be there, non-empty, and pass the digest if its reference copy has a known
// layout. `why` gets the first failing file.
inline bool PackageLooksGood(const fs::path& dir, const fs::path& reference, std::string* why = nullptr) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (why) *why = "package folder missing";
    return false;
  }
  for (auto it = fs::directory_iterator(reference, ec); !ec && it != fs::end(it); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    fs::path cur = dir / it->path().filename();
    std::error_code ec2;
    if (!fs::is_regular_file(cur, ec2) || fs::file_size(cur, ec2) == 0) {
      if (why) *why = it->path().filename().string() + " missing or empty";
      return false;
    }
    bool ref_known = false;
    if (reference != dir) {
      ref_known = CheckFileCached(it->path(), nullptr) == Health::kGood;
    } else {
      ref_known = KnownLayoutName(cur.filename().string());
    }
    if (ref_known && CheckFileCached(cur, nullptr) != Health::kGood) {
      if (why) *why = cur.filename().string() + " fails the game's integrity check";
      return false;
    }
  }
  return true;
}

// ---- recovery -------------------------------------------------------------------------------

// Put the backup (the save before the last one started) back over the package.
inline void RestoreFromBackup(const Package& p, const std::string& reason) {
  std::error_code ec;
  if (!fs::is_directory(p.backup(), ec)) {
    REXLOG_ERROR("[save] {}: {}, but there is no backup at {}; leaving it as it is", p.name, reason,
                 rex::path_to_utf8(p.backup()));
    fs::remove(p.marker(), ec);
    return;
  }
  uint64_t broken = fs::exists(p.dir, ec) ? TreeBytes(p.dir) : 0;
  REXLOG_WARN("[save] {}: {}; restoring the previous save ({} bytes, the broken one has {} bytes)",
              p.name, reason, TreeBytes(p.backup()), broken);
  // Keep the broken files for diagnosis, out of the game's way.
  fs::path keep = p.bk / (p.name + ".broken-" + NowStamp());
  fs::path restore = p.bk / (p.name + ".restore");
  if (!CopyTree(p.backup(), restore)) {
    REXLOG_ERROR("[save] {}: could not copy the backup; leaving the package as it is", p.name);
    return;
  }
  if (fs::exists(p.dir, ec)) {
    fs::rename(p.dir, keep, ec);
    if (ec) {
      // Rename failed (folder in use?): replace the files in place instead.
      ec.clear();
      fs::copy(p.dir, keep, fs::copy_options::recursive, ec);
      ec.clear();
      fs::remove_all(p.dir, ec);
      ec.clear();
    }
  }
  fs::create_directories(p.dir.parent_path(), ec);
  ec.clear();
  fs::rename(restore, p.dir, ec);
  if (ec) {
    ec.clear();
    fs::copy(restore, p.dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    std::error_code ec2;
    fs::remove_all(restore, ec2);
  }
  if (ec) {
    REXLOG_ERROR("[save] {}: restore failed ({}); marker kept, will retry next boot", p.name,
                 ec.message());
    return;
  }
  FlushTree(p.dir);
  fs::path bk_header = p.bk / (p.name + ".header");
  if (fs::is_regular_file(bk_header, ec)) {
    fs::create_directories(p.header.parent_path(), ec);
    ec.clear();
    fs::copy_file(bk_header, p.header, fs::copy_options::overwrite_existing, ec);
    FlushFile(p.header);
  }
  fs::remove(p.marker(), ec);
  REXLOG_WARN("[save] {}: previous save restored; the broken files are in {}", p.name,
              rex::path_to_utf8(keep));
}

// Before the game reads a package: a marker = the save that started after the backup was taken never
// finished; a package that would fail the game's own integrity check while the backup passes = the
// game would say "damaged" and offer to wipe it. Either way the backup goes back first.
inline void RestorePackage(const Package& p) {
  if (!REXCVAR_GET(ufc3_save_guard) || !p.valid()) return;
  std::error_code ec;
  if (fs::exists(p.marker(), ec)) {
    RestoreFromBackup(p, "the last save did not finish (game closed or crashed while saving)");
    return;
  }
  if (!fs::is_directory(p.backup(), ec) || !fs::is_directory(p.dir, ec)) return;
  std::string why;
  if (PackageLooksGood(p.dir, p.backup(), &why)) return;
  std::string bwhy;
  if (!PackageLooksGood(p.backup(), p.backup(), &bwhy)) {
    REXLOG_ERROR("[save] {}: {} and the backup is no better ({}); leaving it to the game", p.name, why,
                 bwhy);
    return;
  }
  RestoreFromBackup(p, "the game would reject it (" + why + ")");
}

// Every <userdata>\<xuid>\<title>\ufc3-save-backup\<type>\<name>.saving.
inline void RecoverAll() {
  if (!REXCVAR_GET(ufc3_save_guard)) return;
  fs::path root = ufc3_paths::UserDataRoot();
  std::error_code ec;
  int found = 0;
  for (auto& prof : fs::directory_iterator(root, ec)) {
    if (!prof.is_directory(ec)) continue;
    std::error_code ec2;
    for (auto& title : fs::directory_iterator(prof.path(), ec2)) {
      fs::path bkroot = title.path() / kBackupDirName;
      std::error_code ec3;
      if (!fs::is_directory(bkroot, ec3)) continue;
      for (auto& type : fs::directory_iterator(bkroot, ec3)) {
        std::error_code ec4;
        for (auto& f : fs::directory_iterator(type.path(), ec4)) {
          if (f.path().extension() != ".saving") continue;
          ++found;
          RestorePackage(PackageFromDir(title.path() / type.path().filename() / f.path().stem()));
        }
      }
    }
  }
  REXLOG_INFO("[save] crash-safe saves on (userdata {}), {} unfinished save(s) found at boot",
              rex::path_to_utf8(root), found);
}

inline void RecoverOnce() {
  std::call_once(S().recovered, [] { RecoverAll(); });
}

// ---- transactions ---------------------------------------------------------------------------

// Highlight-reel replay queues (UFC2012_HLR_QUE_nnn): saved after every round, 12 MB each.
inline bool IsReplayPackage(const std::string& name) {
  return Upper(name).rfind("UFC2012_HLR_QUE", 0) == 0;
}

// Called before the game's first change to a package. Backup first, marker last.
inline void Begin(const Package& p, const char* why) {
  if (!REXCVAR_GET(ufc3_save_guard) || !p.valid()) return;
  auto& s = S();
  // Begins run one at a time; the state lock is only held for the map lookups, never across the copy,
  // because every NtClose/NtWriteFile of the game checks the handle map.
  static std::mutex begin_mutex;
  std::lock_guard begin_lock(begin_mutex);
  std::string key = Key(p.dir);
  {
    std::lock_guard lock(s.mutex);
    if (s.active.count(key) || s.fresh.count(key)) return;
  }
  std::error_code ec;
  const auto t0 = std::chrono::steady_clock::now();
  if (fs::exists(p.marker(), ec)) {
    // An older unfinished save that could not be restored: its backup is still the good one.
    REXLOG_WARN("[save] {}: marker already set; keeping the existing backup", p.name);
  } else if (fs::is_directory(p.dir, ec) && !fs::is_empty(p.dir, ec) &&
             fs::is_directory(p.backup(), ec) && !PackageLooksGood(p.dir, p.backup())) {
    // The package is already broken (e.g. left by an older build): never let it replace a good backup.
    REXLOG_WARN("[save] {}: current files fail the game's check; keeping the existing backup", p.name);
    if (!WriteMarker(p.marker(), rex::path_to_utf8(p.dir))) return;
  } else if (fs::is_directory(p.dir, ec) && !fs::is_empty(p.dir, ec)) {
    fs::create_directories(p.bk, ec);
    ec.clear();
    fs::path fresh = p.bk / (p.name + ".new");
    if (!CopyTree(p.dir, fresh)) {
      REXLOG_ERROR("[save] {}: backup copy failed; this save is not protected", p.name);
      return;
    }
    fs::path bk_header = p.bk / (p.name + ".header");
    if (fs::is_regular_file(p.header, ec)) {
      fs::copy_file(p.header, p.bk / (p.name + ".header.new"),
                    fs::copy_options::overwrite_existing, ec);
      FlushFile(p.bk / (p.name + ".header.new"));
    }
    ec.clear();
    fs::remove_all(p.backup(), ec);
    ec.clear();
    fs::rename(fresh, p.backup(), ec);
    if (ec) {
      REXLOG_ERROR("[save] {}: could not swap in the backup ({}); this save is not protected",
                   p.name, ec.message());
      return;
    }
    if (fs::exists(p.bk / (p.name + ".header.new"), ec)) {
      fs::rename(p.bk / (p.name + ".header.new"), bk_header, ec);
    }
    if (!WriteMarker(p.marker(), rex::path_to_utf8(p.dir))) {
      REXLOG_ERROR("[save] {}: could not write the save marker; this save is not protected",
                   p.name);
      return;
    }
  } else {
    // A brand new (or empty) package: nothing to fall back to until it has been closed once.
    std::lock_guard lock(s.mutex);
    s.fresh[key] = p.root;
    if (Tracing()) REXLOG_INFO("[save] {}: new package, no backup needed", p.name);
    return;
  }
  {
    std::lock_guard lock(s.mutex);
    s.active[key] = p;
    s.active_count = int(s.active.size());
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("[save] {}: save started ({}); previous save backed up in {:.1f} ms", p.name, why, ms);
}

inline void Commit(const std::string& key, const char* why) {
  auto& s = S();
  Package p;
  {
    std::lock_guard lock(s.mutex);
    auto it = s.active.find(key);
    if (it == s.active.end()) return;
    p = it->second;
    s.active.erase(it);
    for (auto h = s.handles.begin(); h != s.handles.end();) {
      if (h->second == key) h = s.handles.erase(h); else ++h;
    }
    s.active_count = int(s.active.size());
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::error_code ec;
  FlushTree(p.dir);
  if (fs::is_regular_file(p.header, ec)) FlushFile(p.header);
  std::string bad;
  bool good = PackageLooksGood(p.dir, p.dir, &bad);
  fs::remove(p.marker(), ec);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (good && IsReplayPackage(p.name)) {
    // Replays (12 MB each, up to 50): the backup only had to cover the save itself.
    fs::remove_all(p.backup(), ec);
    fs::remove(p.bk / (p.name + ".header"), ec);
  }
  if (good) {
    REXLOG_INFO("[save] {}: save complete ({}), flushed and verified in {:.1f} ms", p.name, why, ms);
  } else {
    // A fully written save the game itself will reject: the next boot restores the backup.
    REXLOG_ERROR("[save] {}: the save just written fails the game's own check ({}); the previous save "
                 "stays in {}", p.name, bad, rex::path_to_utf8(p.backup()));
  }
}

inline void CommitRoot(const std::string& root, const char* why) {
  std::string r = Upper(root);
  std::vector<std::string> keys;
  {
    std::lock_guard lock(S().mutex);
    for (auto& [k, p] : S().active)
      if (p.root.empty() || p.root == r) keys.push_back(k);
    for (auto it = S().fresh.begin(); it != S().fresh.end();) {
      if (it->second.empty() || it->second == r) it = S().fresh.erase(it); else ++it;
    }
  }
  for (auto& k : keys) Commit(k, why);
}

// Package mounted under a guest root ("UFC2012" in "UFC2012:\SYSTEM.DAT"), if it is a saved game.
inline Package PackageForGuestPath(const std::string& path) {
  auto colon = path.find(':');
  if (colon == std::string::npos || colon == 0) return {};
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->kernel_state() || !rt->kernel_state()->content_manager()) return {};
  fs::path dir =
      rt->kernel_state()->content_manager()->GetOpenPackagePath(path.substr(0, colon));
  if (dir.empty()) return {};
  Package p = PackageFromDir(dir);
  p.root = Upper(path.substr(0, colon));
  return p;
}

inline fs::path PackageDirFor(const rex::system::xam::XCONTENT_DATA& data) {
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->kernel_state()) return {};
  auto* ks = rt->kernel_state();
  uint64_t xuid = ks->user_profile() ? ks->user_profile()->xuid() : 0;
  return ufc3_paths::UserDataRoot() / fmt::format("{:016X}", xuid) /
         fmt::format("{:08X}", ks->title_id()) /
         fmt::format("{:08X}", static_cast<uint32_t>(static_cast<rex::system::XContentType>(data.content_type))) / rex::to_path(data.file_name());
}

// ---- close-wait API (ufc3_app.h) ------------------------------------------------------------

inline bool SaveInProgress() { return S().active_count.load() > 0; }

inline bool WaitForSaves(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (SaveInProgress()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

inline void TestKill(const char* where) {
  REXLOG_WARN("[save] TEST: terminating the process {} (ufc3_save_test_kill_after_writes)", where);
  rex::FlushLogging();
  TerminateProcess(GetCurrentProcess(), 0xDEAD);
}

// ---- import wrappers ------------------------------------------------------------------------

using ImportFn = void (*)(PPCContext&, uint8_t*);

inline ImportFn Resolve(const char* name) {
  static HMODULE runtime = [] {
    HMODULE m = GetModuleHandleA("rexruntimerd.dll");
    return m ? m : GetModuleHandleA("rexruntime.dll");
  }();
  return reinterpret_cast<ImportFn>(GetProcAddress(runtime, name));
}

constexpr uint32_t kWriteAccess = 0x2 | 0x4 | 0x10000 | 0x40000000 | 0x10000000;  // write/append/delete/generic

// disposition: 0 supersede, 1 open, 2 create, 3 open-if, 4 overwrite, 5 overwrite-if
inline void OnFileOpen(PPCContext& ctx, uint8_t* base, ImportFn fn, const char* api,
                       uint32_t access, uint32_t disposition) {
  uint32_t handle_out = ctx.r3.u32;
  std::string path = GuestObjectName(base, ctx.r5.u32);
  Package p = PackageForGuestPath(path);
  bool writes = p.valid() && ((access & kWriteAccess) || disposition == 0 || disposition == 2 ||
                              disposition == 4 || disposition == 5);
  if (writes) Begin(p, path.c_str());
  fn(ctx, base);
  if (!p.valid()) return;
  uint32_t status = ctx.r3.u32;
  uint32_t handle = handle_out ? Load32(base, handle_out) : 0;
  if (Tracing()) {
    REXLOG_INFO("[save] {}('{}', access={:08X}, disp={}) -> {:08X} handle={:08X}{}", api, path,
                access, disposition, status, handle, writes ? " WRITE" : "");
  }
  if (!writes && int32_t(status) >= 0 && Tracing()) {
    std::lock_guard lock(S().mutex);
    S().reads[handle] = path;
  }
  if (writes && int32_t(status) >= 0) {
    {
      std::lock_guard lock(S().mutex);
      S().handles[handle] = Key(p.dir);
    }
    if (REXCVAR_GET(ufc3_save_test_kill_after_writes) == 0 && S().kill_armed.exchange(false)) {
      TestKill(fmt::format("after opening {} for writing", path).c_str());
    }
  }
}

inline bool IsSaveHandle(uint32_t handle) {
  std::lock_guard lock(S().mutex);
  return S().handles.count(handle) != 0;
}

inline void OnWrite(PPCContext& ctx, uint8_t* base, ImportFn fn, const char* api) {
  uint32_t handle = ctx.r3.u32;
  uint32_t length = ctx.r9.u32;  // NtWriteFile(h, ev, apc, apc_ctx, iosb, buf, len, offset*)
  uint32_t offset_ptr = ctx.r10.u32;
  uint32_t buffer = ctx.r8.u32, lr = static_cast<uint32_t>(ctx.lr);
  bool save = IsSaveHandle(handle);
  if (save && REXCVAR_GET(ufc3_save_test_write_delay_ms) > 0) {
    REXLOG_WARN("[save] TEST: delaying a save write by {} ms", REXCVAR_GET(ufc3_save_test_write_delay_ms));
    Sleep(DWORD(REXCVAR_GET(ufc3_save_test_write_delay_ms)));
  }
  fn(ctx, base);
  if (!save) return;
  int n = ++S().writes;
  if (Tracing()) {
    uint64_t off = 0;
    if (offset_ptr) off = (uint64_t(Load32(base, offset_ptr)) << 32) | Load32(base, offset_ptr + 4);
    REXLOG_INFO("[save] {}(handle={:08X}, buf={:08X}, len={:X}, offset={:X}) lr={:08X} #{} -> {:08X}",
                api, handle, buffer, length, off, lr, n, ctx.r3.u32);
  }
  int kill = REXCVAR_GET(ufc3_save_test_kill_after_writes);
  if (kill > 0 && n >= kill && S().kill_armed.exchange(false)) {
    TestKill(fmt::format("after save write #{}", n).c_str());
  }
}

}  // namespace ufc3_save

#define UFC3_SAVE_IMPORT(name)                          \
  static ufc3_save::ImportFn ufc3_save_fn_##name() {    \
    static ufc3_save::ImportFn fn = ufc3_save::Resolve("__imp__" #name); \
    return fn;                                          \
  }                                                     \
  extern "C" REX_FUNC(__imp__##name)

UFC3_SAVE_IMPORT(NtCreateFile) {
  ufc3_save::OnFileOpen(ctx, base, ufc3_save_fn_NtCreateFile(), "NtCreateFile", ctx.r4.u32,
                        ctx.r10.u32);
}

UFC3_SAVE_IMPORT(NtOpenFile) {
  ufc3_save::OnFileOpen(ctx, base, ufc3_save_fn_NtOpenFile(), "NtOpenFile", ctx.r4.u32, 1);
}

UFC3_SAVE_IMPORT(NtWriteFile) {
  ufc3_save::OnWrite(ctx, base, ufc3_save_fn_NtWriteFile(), "NtWriteFile");
}

UFC3_SAVE_IMPORT(NtWriteFileGather) {
  ufc3_save::OnWrite(ctx, base, ufc3_save_fn_NtWriteFileGather(), "NtWriteFileGather");
}

// Trace only: which guest buffer and caller read a save file (to find the game's load checks).
UFC3_SAVE_IMPORT(NtReadFile) {
  uint32_t handle = ctx.r3.u32, buffer = ctx.r8.u32, length = ctx.r9.u32;
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  ufc3_save_fn_NtReadFile()(ctx, base);
  if (!ufc3_save::Tracing()) return;
  std::string key;
  {
    std::lock_guard lock(ufc3_save::S().mutex);
    auto it = ufc3_save::S().reads.find(handle);
    if (it == ufc3_save::S().reads.end()) return;
    key = it->second;
  }
  REXLOG_INFO("[save] NtReadFile('{}', handle={:08X}, buf={:08X}, len={:X}) lr={:08X} -> {:08X}", key,
              handle, buffer, length, lr, ctx.r3.u32);
}

// Trace only (ufc3_trace_saves): the game's SHA-256 use, to see what its save digest covers.
UFC3_SAVE_IMPORT(XeCryptSha256Update) {
  uint32_t st = ctx.r3.u32, data = ctx.r4.u32, len = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);
  ufc3_save_fn_XeCryptSha256Update()(ctx, base);
  static std::atomic<int> n{0};
  if (ufc3_save::Tracing() && n.fetch_add(1) < 400) {
    std::string head;
    for (uint32_t i = 0; i < std::min<uint32_t>(len, 16); ++i) head += fmt::format("{:02X}", base[data + i]);
    REXLOG_INFO("[save] Sha256Update(state={:08X}, data={:08X}, len={:X}) head={} lr={:08X}", st, data, len,
                head, lr);
  }
}

UFC3_SAVE_IMPORT(XeCryptSha256Final) {
  uint32_t st = ctx.r3.u32, out = ctx.r4.u32, len = ctx.r5.u32, lr = static_cast<uint32_t>(ctx.lr);
  ufc3_save_fn_XeCryptSha256Final()(ctx, base);
  static std::atomic<int> n{0};
  if (ufc3_save::Tracing() && n.fetch_add(1) < 200) {
    REXLOG_INFO("[save] Sha256Final(state={:08X}, out={:08X}, len={:X}) = {:08X}{:08X}.. lr={:08X}", st, out,
                len, ufc3_save::Load32(base, out), ufc3_save::Load32(base, out + 4), lr);
  }
}

UFC3_SAVE_IMPORT(NtClose) {
  uint32_t handle = ctx.r3.u32;
  bool save = ufc3_save::IsSaveHandle(handle);
  ufc3_save_fn_NtClose()(ctx, base);
  if (ufc3_save::Tracing()) {
    std::lock_guard lock(ufc3_save::S().mutex);
    ufc3_save::S().reads.erase(handle);
  }
  if (!save) return;
  {
    std::lock_guard lock(ufc3_save::S().mutex);
    ufc3_save::S().handles.erase(handle);
  }
  if (ufc3_save::Tracing()) REXLOG_INFO("[save] NtClose(handle={:08X})", handle);
}

// XamContentCreateEx(user, root, content_data, flags, disposition*, license*, cache, size, ovl)
UFC3_SAVE_IMPORT(XamContentCreateEx) {
  using namespace ufc3_save;
  RecoverOnce();
  std::string root = GuestCString(base, ctx.r4.u32);
  uint32_t flags = ctx.r6.u32;
  rex::system::xam::XCONTENT_DATA data;
  std::memcpy(&data, base + ctx.r5.u32, sizeof(data));
  Package p;
  bool began_here = false;
  if (static_cast<uint32_t>(static_cast<rex::system::XContentType>(data.content_type)) == kSavedGameType) {
    p = PackageFromDir(PackageDirFor(data));
    p.root = Upper(root);
    if (p.valid()) {
      bool active;
      {
        std::lock_guard lock(S().mutex);
        active = S().active.count(Key(p.dir)) != 0;
      }
      uint32_t mode = flags & 0xF;
      // Not for CREATE_ALWAYS / TRUNCATE_EXISTING: the package is about to be replaced anyway.
      if (!active && mode != 2 && mode != 5) RestorePackage(p);
      // CREATE_ALWAYS / TRUNCATE_EXISTING: the SDK deletes the package folder before the game
      // writes the new one.
      if (mode == 2 || mode == 5) {
        // The SDK closes an old mount under the same root first: a save left open there is done.
        CommitRoot(root, "root re-created");
        std::error_code ec;
        if (!fs::is_directory(p.dir, ec)) {
          std::lock_guard lock(S().mutex);
          S().fresh[Key(p.dir)] = p.root;  // brand new package
        } else {
          Begin(p, mode == 2 ? "CREATE_ALWAYS" : "TRUNCATE_EXISTING");
          std::lock_guard lock(S().mutex);
          began_here = S().active.count(Key(p.dir)) != 0;
        }
      }
    }
  }
  ufc3_save_fn_XamContentCreateEx()(ctx, base);
  // A synchronous CREATE_ALWAYS that failed (e.g. the old folder could not be deleted completely
  // because another program held a file): the game will not write it, put the old save back.
  uint32_t result = ctx.r3.u32;
  if (began_here && result != 0 && result != 0x3E5 /* ERROR_IO_PENDING */) {
    REXLOG_WARN("[save] {}: XamContentCreateEx failed ({:08X}) after the old package was removed",
                p.name, result);
    {
      std::lock_guard lock(S().mutex);
      S().active.erase(Key(p.dir));
      S().active_count = int(S().active.size());
    }
    RestorePackage(p);
  }
  if (Tracing() && p.valid()) {
    REXLOG_INFO("[save] XamContentCreateEx(root='{}', file='{}', flags={:X}) -> {:08X} ({})", root,
                data.file_name(), flags, ctx.r3.u32, rex::path_to_utf8(p.dir));
  }
}

UFC3_SAVE_IMPORT(XamContentCreateEnumerator) {
  ufc3_save::RecoverOnce();
  ufc3_save_fn_XamContentCreateEnumerator()(ctx, base);
}

// XamContentClose(root, overlapped)
UFC3_SAVE_IMPORT(XamContentClose) {
  std::string root = ufc3_save::GuestCString(base, ctx.r3.u32);
  ufc3_save_fn_XamContentClose()(ctx, base);
  if (ufc3_save::Tracing()) REXLOG_INFO("[save] XamContentClose('{}') -> {:08X}", root, ctx.r3.u32);
  ufc3_save::CommitRoot(root, "XamContentClose");
}
