// ufc3_shader_cache: builds the native renderer's shader cache on the player's machine (NR-7c).
//
//   ufc3_shader_cache <game data folder> <output folder> [--xenosrecomp <exe>] [--prelink <exe>]
//                     [--keep-temp]
//   ufc3_shader_cache --extract-only <game data folder> <shader folder>
//
// Game shaders can't ship, so the cache comes from the player's own disc files:
//   1. extract: every shader container in default.xex and DAT\shader\fxl.pac, plus the patched
//      bloom / depth-of-field variants (shader_set.cpp, a port of tools/nr_shader_extract.py);
//   2. translate: XenosRecomp.exe turns them into shader_cache.bin (DXIL);
//   3. link: ufc3_prelink.exe writes shader_cache_linked.bin (every spec-constant variant).
// It does what tools/nr_build_shader_cache.py does, without Python: the same disc files give the
// same two files, byte for byte.
//
// XenosRecomp.exe and ufc3_prelink.exe are looked for beside this exe (each needs dxcompiler.dll
// and dxil.dll in its own folder). Everything is built in <output folder>\shader_cache.tmp and
// moved into place at the end, so a failed or interrupted run leaves the old cache alone and the
// tool can simply be run again.
//
// Exit codes: 0 done, 1 bad arguments, 2 game files missing or not usable, 3 output folder
// problem, 4 XenosRecomp missing or failed, 5 ufc3_prelink missing or failed.

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "shader_set.h"

namespace fs = std::filesystem;
using namespace ufc3_shader_cache;

namespace {

// XenosRecomp's shader_common.h, embedded at build time (CMakeLists.txt): XenosRecomp reads it
// from a file, so it's written into the temp folder instead of shipping loose.
const unsigned char kShaderCommon[] = {
#include "shader_common_embed.inc"
};

constexpr char kTool[] = "ufc3_shader_cache";

enum ExitCode {
  kOk = 0,
  kUsage = 1,
  kGameFiles = 2,
  kOutput = 3,
  kTranslate = 4,
  kLink = 5,
};

struct Failure : std::runtime_error {
  Failure(int code, const std::string& what) : std::runtime_error(what), code(code) {}
  int code;
};

void Say(const std::string& line) {
  std::printf("%s\n", line.c_str());
  std::fflush(stdout);
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

std::string Format(const char* format, ...) {
  va_list args, measure;
  va_start(args, format);
  va_copy(measure, args);
  const int size = std::vsnprintf(nullptr, 0, format, measure);
  va_end(measure);
  std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
  if (size > 0) std::vsnprintf(text.data(), text.size() + 1, format, args);
  va_end(args);
  return text;
}

fs::path ExeDir() {
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(n);
  return fs::path(buffer).parent_path();
}

// The temp folder holds only plain files this tool (and the two child tools) wrote: in\*.bin and
// a few files beside it. Remove exactly that, never recurse, and refuse anything unexpected.
void RemoveWorkDir(const fs::path& work) {
  std::error_code ec;
  const auto status = fs::symlink_status(work, ec);
  if (!fs::exists(status)) return;
  if (status.type() != fs::file_type::directory)
    throw Failure(kOutput, PathText(work) + " is in the way and is not a plain folder");
  for (const fs::path& dir : {work / "in", work}) {
    if (fs::symlink_status(dir, ec).type() != fs::file_type::directory) continue;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->symlink_status(ec).type() == fs::file_type::regular) files.push_back(it->path());
    }
    for (const auto& f : files) fs::remove(f, ec);
  }
  fs::remove(work / "in", ec);
  fs::remove(work, ec);
  if (fs::exists(fs::symlink_status(work, ec)))
    throw Failure(kOutput, "cannot clear the old temp folder " + PathText(work));
}

struct ChildResult {
  DWORD exit_code = 0;
  std::string out, err;
};

HANDLE OpenInheritable(const fs::path& path, bool write) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  return CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, write ? CREATE_ALWAYS : OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Runs exe with plain-ASCII relative arguments from inside cwd. The child tools open files with
// narrow-character paths, so they never see the (possibly non-ASCII) full path of the folder.
ChildResult RunChild(const fs::path& exe, const wchar_t* arguments, const fs::path& cwd,
                     const std::string& log_stem, int failure_code) {
  const fs::path out_path = cwd / (log_stem + ".out.txt");
  const fs::path err_path = cwd / (log_stem + ".err.txt");
  const HANDLE out = OpenInheritable(out_path, true);
  const HANDLE err = OpenInheritable(err_path, true);
  const HANDLE in = OpenInheritable(L"NUL", false);
  if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    throw Failure(kOutput, "cannot write in " + PathText(cwd));
  }

  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out;
  si.hStdError = err;
  std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
  PROCESS_INFORMATION pi = {};
  const BOOL started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.c_str(),
                                      &si, &pi);
  const DWORD start_error = GetLastError();
  CloseHandle(out);
  CloseHandle(err);
  if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
  if (!started) {
    throw Failure(failure_code, Format("cannot start %s (Windows error %lu)",
                                       PathText(exe).c_str(), start_error));
  }

  // The child ends with this tool if the launcher or the player stops it.
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  ChildResult result;
  GetExitCodeProcess(pi.hProcess, &result.exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);

  result.out = ReadFileBytes(out_path);
  result.err = ReadFileBytes(err_path);
  return result;
}

std::string Tail(const std::string& text, size_t max = 2000) {
  return text.size() > max ? text.substr(text.size() - max) : text;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    pos = end + 1;
  }
  return lines;
}

void RequireTool(const fs::path& exe, int failure_code) {
  std::error_code ec;
  if (!fs::is_regular_file(exe, ec))
    throw Failure(failure_code, PathText(exe) + " not found (it ships beside " + kTool + ".exe)");
  for (const char* dll : {"dxcompiler.dll", "dxil.dll"}) {
    if (!fs::is_regular_file(exe.parent_path() / dll, ec)) {
      throw Failure(failure_code, std::string(dll) + " not found beside " + PathText(exe));
    }
  }
}

// shader_cache.bin as XenosRecomp writes it (format: third_party/XenosRecomp/XenosRecomp/main.cpp).
uint32_t CheckCacheFile(const std::string& file) {
  struct Header {
    char magic[8];
    uint32_t version;
    uint32_t entry_count;
    uint64_t dxil_compressed_size;
    uint64_t dxil_size;
  } header;
  constexpr size_t kEntrySize = 24;
  if (file.size() < sizeof(header)) throw Failure(kTranslate, "XenosRecomp wrote no shader cache");
  std::memcpy(&header, file.data(), sizeof(header));
  if (std::memcmp(header.magic, "UFCSHC01", 8) != 0 || header.version != 1 ||
      header.entry_count == 0 ||
      file.size() != sizeof(header) + size_t{header.entry_count} * kEntrySize +
                         header.dxil_compressed_size) {
    throw Failure(kTranslate, "XenosRecomp wrote a shader cache that does not check out");
  }
  return header.entry_count;
}

void MoveIntoPlace(const fs::path& from, const fs::path& to) {
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    throw Failure(kOutput, Format("cannot write %s (Windows error %lu)%s", PathText(to).c_str(),
                                  error, error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED
                                             ? "; is the game running or the folder read-only?"
                                             : ""));
  }
}

void PrintExtract(const ShaderSet& set) {
  std::string hits;
  for (const auto& [feature, count] : set.patch_hits)
    hits += (hits.empty() ? "" : ", ") + feature + " " + std::to_string(count);
  // Same two lines as tools/nr_shader_extract.py.
  Say(Format("%zu containers (fxl.pac %zu, default.xex %zu), %zu patched variants (%s)",
             set.containers, set.from_pac, set.from_xex, set.variants, hits.c_str()));
  Say("set hash " + set.set_hash);
  Say(Format("unpacked %zu zlib blobs, %llu bytes", set.zlib_blobs,
             static_cast<unsigned long long>(set.zlib_bytes)));
}

int Usage() {
  std::fprintf(stderr,
               "usage: %s <game data folder> <output folder> [--xenosrecomp <exe>] "
               "[--prelink <exe>] [--keep-temp]\n"
               "       %s --extract-only <game data folder> <shader folder>\n"
               "The game data folder holds default.xex and DAT\\shader\\fxl.pac. Writes "
               "shader_cache.bin and\nshader_cache_linked.bin into the output folder.\n",
               kTool, kTool);
  return kUsage;
}

int Run(int argc, wchar_t** argv) {
  bool extract_only = false, keep_temp = false;
  fs::path xenosrecomp, prelink;
  std::vector<fs::path> positional;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg == L"--extract-only") {
      extract_only = true;
    } else if (arg == L"--keep-temp") {
      keep_temp = true;
    } else if ((arg == L"--xenosrecomp" || arg == L"--prelink") && i + 1 < argc) {
      (arg == L"--prelink" ? prelink : xenosrecomp) = fs::absolute(argv[++i]);
    } else if (arg.substr(0, 2) == L"--") {
      return Usage();
    } else {
      positional.emplace_back(argv[i]);
    }
  }
  if (positional.size() != 2) return Usage();
  std::error_code ec;
  const fs::path game_root = fs::absolute(positional[0]);
  const fs::path out_dir = fs::absolute(positional[1]);
  if (!fs::is_directory(game_root, ec))
    throw Failure(kGameFiles, "game data folder " + PathText(game_root) + " not found");

  const auto t_start = std::chrono::steady_clock::now();
  if (extract_only) {
    const ShaderSet set = ExtractShaderSet(game_root);
    try {
      WriteShaderSet(set, out_dir);
    } catch (const std::runtime_error& e) {
      throw Failure(kOutput, e.what());
    }
    PrintExtract(set);
    return kOk;
  }

  if (xenosrecomp.empty()) xenosrecomp = ExeDir() / "XenosRecomp.exe";
  if (prelink.empty()) prelink = ExeDir() / "ufc3_prelink.exe";
  RequireTool(xenosrecomp, kTranslate);
  RequireTool(prelink, kLink);

  fs::create_directories(out_dir, ec);
  if (!fs::is_directory(out_dir, ec))
    throw Failure(kOutput, "cannot create the output folder " + PathText(out_dir));
  // One build per output folder at a time (launcher and game could both start one).
  const fs::path lock_path = out_dir / "shader_cache.lock";
  const HANDLE lock = CreateFileW(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (lock == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    throw Failure(kOutput, error == ERROR_SHARING_VIOLATION
                               ? "another shader cache build is running in " + PathText(out_dir)
                               : Format("cannot write in %s (Windows error %lu)",
                                        PathText(out_dir).c_str(), error));
  }
  struct LockCloser {
    HANDLE h;
    ~LockCloser() { CloseHandle(h); }
  } lock_closer{lock};

  // 1. Extract.
  const ShaderSet set = ExtractShaderSet(game_root);
  PrintExtract(set);
  const fs::path work = out_dir / "shader_cache.tmp";
  RemoveWorkDir(work);
  try {
    WriteShaderSet(set, work / "in");
    WriteFileBytes(work / "shader_common.h",
                   {reinterpret_cast<const char*>(kShaderCommon), sizeof(kShaderCommon)});
  } catch (const std::runtime_error& e) {
    throw Failure(kOutput, e.what());
  }
  const double t_extract = Seconds(t_start);

  // 2. Translate to DXIL. XenosRecomp lists the shaders it can't translate on stderr and still
  // returns 0; the game logs those as misses if it ever asks for one.
  const auto t_translate_start = std::chrono::steady_clock::now();
  Say(Format("translating %zu shaders...", set.files.size()));
  const ChildResult xr = RunChild(xenosrecomp, L"in shader_cache.bin shader_common.h", work,
                                  "xenosrecomp", kTranslate);
  if (xr.exit_code != 0) {
    throw Failure(kTranslate,
                  Format("XenosRecomp failed (exit code 0x%lX); its output is in %s\n%s\n%s",
                         xr.exit_code, PathText(work).c_str(), Tail(xr.out).c_str(),
                         Tail(xr.err).c_str()));
  }
  std::string cache_file;
  try {
    cache_file = ReadFileBytes(work / "shader_cache.bin");
  } catch (const std::runtime_error&) {
    throw Failure(kTranslate, "XenosRecomp wrote no shader cache\n" + Tail(xr.err));
  }
  const uint32_t shader_count = CheckCacheFile(cache_file);
  const uint64_t cache_hash = XXH3_64bits(cache_file.data(), cache_file.size());
  for (const auto& line : Lines(xr.out)) Say(line);

  // The failure list, in nr_build_shader_cache.py's format: "<name> <reason> [<source>]".
  std::string failures;
  size_t failed = 0, failed_xex = 0;
  for (const auto& line : Lines(xr.err)) {
    const size_t s = line.find("source=");
    if (s == std::string::npos) continue;
    const size_t name_end = line.find(' ', s);
    if (name_end == std::string::npos || line.compare(name_end, 8, " reason=") != 0) continue;
    const std::string name = line.substr(s + 7, name_end - (s + 7));
    std::string source = "?";
    for (const auto& f : set.files) {
      if (f.name == name + ".bin") source = f.source;
    }
    failures += name + " " + line.substr(name_end + 8) + " [" + source + "]\r\n";
    ++failed;
    if (source.find("default.xex") != std::string::npos) ++failed_xex;
  }
  Say(Format("not translated %zu: default.xex %zu, fxl.pac %zu (list: shader_cache.bin.failures.txt)",
             failed, failed_xex, failed - failed_xex));
  const double t_translate = Seconds(t_translate_start);

  // 3. Pre-link the spec-constant variants.
  const auto t_link_start = std::chrono::steady_clock::now();
  Say("linking shader variants...");
  const ChildResult pl = RunChild(prelink, L"shader_cache.bin shader_cache_linked.bin", work,
                                  "prelink", kLink);
  if (pl.exit_code != 0) {
    throw Failure(kLink, Format("ufc3_prelink failed (exit code 0x%lX); its output is in %s\n%s\n%s",
                                pl.exit_code, PathText(work).c_str(), Tail(pl.out).c_str(),
                                Tail(pl.err).c_str()));
  }
  if (fs::file_size(work / "shader_cache_linked.bin", ec) == 0 || ec)
    throw Failure(kLink, "ufc3_prelink wrote no linked shader cache\n" + Tail(pl.err));
  for (const auto& line : Lines(pl.out)) Say(line);
  const double t_link = Seconds(t_link_start);

  // 4. Move into place. The linked file names the cache it was made from (xxh3), and the game
  // refuses a pair that doesn't match, so stopping between the moves does no harm.
  try {
    WriteFileBytes(work / "shader_cache.bin.failures.txt", failures);
  } catch (const std::runtime_error& e) {
    throw Failure(kOutput, e.what());
  }
  for (const char* name :
       {"shader_cache.bin.failures.txt", "shader_cache_linked.bin", "shader_cache.bin"}) {
    MoveIntoPlace(work / name, out_dir / name);
  }
  if (!keep_temp) RemoveWorkDir(work);

  Say(Format("shader cache ready: %u shaders, xxh3 %016llX, in %s", shader_count,
             static_cast<unsigned long long>(cache_hash), PathText(out_dir).c_str()));
  Say(Format("time: extract %.1f s, translate %.1f s, link %.1f s, total %.1f s", t_extract,
             t_translate, t_link, Seconds(t_start)));
  return kOk;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // A child tool with a missing DLL fails with an exit code instead of a message box.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  try {
    return Run(argc, argv);
  } catch (const Failure& e) {
    std::fprintf(stderr, "%s: error: %s\n", kTool, e.what());
    return e.code;
  } catch (const std::exception& e) {
    // Everything the extract step throws: missing, damaged or unsupported game files.
    std::fprintf(stderr, "%s: error: %s\n", kTool, e.what());
    return kGameFiles;
  }
}
