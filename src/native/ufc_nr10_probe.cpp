// NR-10 probe (ufc3_native_nr10_log, log only): what the game's DXT compressor reads and writes.
// CAF and career skins are composited on the GPU, resolved, then compressed to DXT5 by the effect
// yGPUCompression: one rectangle draw per mip level in sub_82793B10, whose pixel shader writes the
// blocks to memory (memexport) between BeginExport and EndExport (FINDINGS-nr10-memexport.md,
// par/10.md). The native renderer has no memexport, so this only logs: each compress call with its
// source and destination, how much of the source the resolves seen so far cover, what the guest
// bytes hold, and whether the draw reached the host. It also names every texture that is mirrored
// from memory a resolve target owns (stale on native: resolves never reach guest memory).
// The compressor and export hooks log on the emulated renderer too; resolve and bind lines are
// native only. With the cvar off every hook runs only the game's function.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>  // WIN32_LEAN_AND_MEAN, NOMINMAX: set by the build
#endif

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"
#include "ufc_d3d.h"
#include "ufc_memexport_cpu.h"
#include "ufc_native.h"
#include "ufc_nr10_probe.h"
#include "ufc_resources.h"

REXCVAR_DEFINE_BOOL(ufc3_native_nr10_log, false, "ufc3",
                    "Dev: log the game's DXT compressor (CAF and career skins): each compress call, "
                    "the resolves that feed it and the textures read from resolved memory ([nr10])");
REXCVAR_DEFINE_INT32(ufc3_native_nr10_dump, 0, "ufc3",
                     "Dev, with ufc3_native_nr10_log: save the source and destination bytes of up to this "
                     "many compress calls (small levels, a few per size) to nr10_dump beside the log file");

namespace bd::gpu {
void SvrDrawCounts(u64& drawn, u64& dropped);  // ufc_resources.cpp
}

namespace {

namespace xe = rex::graphics::xenos;
using bd::gpu::NativeMirrorSource;

constexpr uint32_t kMaxLines = 6000;        // all [nr10] lines of a run
constexpr uint32_t kMaxResolveLines = 1500;
constexpr uint32_t kMaxBindLines = 400;
constexpr uint32_t kMaxRectsPerTarget = 256;
constexpr uint32_t kCell = 8;               // coverage grid, guest pixels
constexpr double kRecheckSeconds = 1.5;     // a compress call's bytes are hashed again this much later
constexpr uint32_t kDumpMaxPixels = 512 * 512;  // source levels above this are not dumped
constexpr uint32_t kDumpPerSize = 3;        // dumps per distinct source size

uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

// Outside a hook body (no membase at hand).
uint32_t LoadVa(uint32_t va) {
  return bd::mem::load<u32>(va);
}

uint32_t ThreadId() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

xe::xe_gpu_texture_fetch_t FetchOf(const NativeMirrorSource& src) {
  xe::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, src.fetch, sizeof(src.fetch));
  return fetch;
}

std::string FetchText(const xe::xe_gpu_texture_fetch_t& f) {
  return fmt::format("{}x{} fmt {} levels {} base {:08X} mip {:08X}{} endian {} swizzle {:03X} numfmt {}",
                     f.size_2d.width + 1, f.size_2d.height + 1, uint32_t(f.format), f.mip_max_level + 1,
                     uint32_t(f.base_address) << 12, uint32_t(f.mip_address) << 12, f.tiled ? " tiled" : " linear",
                     uint32_t(f.endianness), uint32_t(f.swizzle), uint32_t(f.num_format));
}

// A copy of guest memory that can't fault: the range may be freed by the time the watcher reads
// it, and on the emulated renderer a resolve destination can be read-protected (readback_resolve
// ondemand). False = some of it was not readable.
bool CopyGuest(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
  out.resize(size);
  if (!data || !size)
    return false;
#if defined(_WIN32)
  SIZE_T done = 0;
  return ReadProcessMemory(GetCurrentProcess(), data, out.data(), size, &done) && done == size;
#else
  std::memcpy(out.data(), data, size);
  return true;
#endif
}

struct Bytes {
  size_t size = 0;
  bool readable = false;
  double nonzero = 0.0;  // percent of bytes
  uint64_t hash = 0;
  std::string Text() const {
    return readable ? fmt::format("{} bytes, {:.1f}% non-zero, hash {:016X}", size, nonzero, hash)
                    : fmt::format("{} bytes, NOT READABLE", size);
  }
};

Bytes BytesOf(const uint8_t* data, size_t size, std::vector<uint8_t>* keep = nullptr) {
  Bytes out;
  out.size = size;
  std::vector<uint8_t> local;
  std::vector<uint8_t>& copy = keep ? *keep : local;
  out.readable = CopyGuest(data, size, copy);
  if (!out.readable)
    return out;
  size_t nonzero = 0;
  for (uint8_t byte : copy)
    nonzero += byte != 0;
  out.nonzero = 100.0 * double(nonzero) / double(size);
  out.hash = XXH3_64bits(copy.data(), size);
  return out;
}

struct Rect {
  uint32_t level, face;
  int32_t x0, y0, x1, y1;
  uint64_t first, last;
  uint32_t count;
};

// A resolve destination as last seen: its header and the guest memory its levels own.
struct Target {
  uint32_t dword1 = 0, dword2 = 0;
  uint32_t width = 0, height = 0, format = 0, levels = 0;
  const uint8_t* base = nullptr;
  size_t base_size = 0;
  const uint8_t* mips = nullptr;
  size_t mips_size = 0;
  std::vector<Rect> rects;
};

struct Bind {
  uint32_t dword1 = 0, dword2 = 0, generation = ~0u;
};

// A compress call looked at again kRecheckSeconds later: on the emulated renderer the GPU writes
// the destination (and, with readback_resolve=full, the source's resolved pixels) after the call
// returns. With `dump` both are saved, for checking an encoder byte for byte offline.
struct Recheck {
  uint32_t number;
  std::chrono::steady_clock::time_point due;
  const uint8_t* source;
  size_t source_size;
  uint64_t source_hash;
  const uint8_t* dest;
  size_t dest_size;
  uint64_t dest_hash;
  bool dump;
  std::string about;  // the call, for the dump's text file
};

std::mutex g_lock;
std::unordered_map<uint32_t, Target> g_targets;  // texture header VA -> resolve destination
std::unordered_map<uint32_t, Bind> g_binds;      // mirrored texture header VA -> last checked
std::unordered_set<uint64_t> g_bind_logged;      // texture VA, resolve target VA pairs already logged
std::vector<Recheck> g_rechecks;
uint32_t g_generation = 0;  // bumped when a resolve destination appears or changes
uint32_t g_lines = 0, g_resolve_lines = 0, g_bind_lines = 0, g_compress_count = 0;

// Caller holds g_lock.
bool Room() {
  if (g_lines == kMaxLines)
    REXLOG_INFO("[nr10] line cap reached, no more lines");
  return g_lines++ < kMaxLines;
}

// Which level of `t` starts at `at`, or -1. Mip levels are told apart by width.
int LevelAt(const Target& t, const uint8_t* at, uint32_t width) {
  if (!at)
    return -1;
  if (t.base && at == t.base)
    return 0;
  if (t.mips && at >= t.mips && at < t.mips + t.mips_size)
    for (uint32_t level = 1; level < t.levels; ++level)
      if (std::max(t.width >> level, 1u) == width)
        return int(level);
  if (t.base && at > t.base && at < t.base + t.base_size)
    return 0;  // inside the base level, not at its start
  return -1;
}

struct Coverage {
  double percent = 0.0;
  uint32_t rects = 0;
  uint64_t first = 0, last = 0;
  std::string list;
};

// How much of a width x height level the resolves into it cover (grid of kCell pixels).
Coverage CoverageOf(const Target& t, uint32_t level, uint32_t width, uint32_t height) {
  Coverage out;
  const uint32_t cw = (width + kCell - 1) / kCell, ch = (height + kCell - 1) / kCell;
  if (!cw || !ch)
    return out;
  std::vector<uint8_t> grid(size_t(cw) * ch, 0);
  for (const Rect& r : t.rects) {
    if (r.level != level)
      continue;
    ++out.rects;
    out.first = out.first ? std::min(out.first, r.first) : r.first;
    out.last = std::max(out.last, r.last);
    if (out.rects <= 6)
      out.list += fmt::format(" {},{}-{},{} x{} (frames {}-{})", r.x0, r.y0, r.x1, r.y1, r.count, r.first, r.last);
    for (uint32_t cy = 0; cy < ch; ++cy) {
      const int32_t y = int32_t(cy * kCell + kCell / 2);
      if (y < r.y0 || y >= r.y1)
        continue;
      for (uint32_t cx = 0; cx < cw; ++cx) {
        const int32_t x = int32_t(cx * kCell + kCell / 2);
        if (x >= r.x0 && x < r.x1)
          grid[size_t(cy) * cw + cx] = 1;
      }
    }
  }
  size_t covered = 0;
  for (uint8_t cell : grid)
    covered += cell;
  out.percent = 100.0 * double(covered) / double(grid.size());
  return out;
}

// <folder of the log file>\nr10_dump, which a laptop job brings back with test.log.
std::string DumpDir() {
  std::string dir = rex::cvar::GetFlagByName("log_file");
  const size_t slash = dir.find_last_of("\\/");
  dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
  dir += "\\nr10_dump";
#if defined(_WIN32)
  CreateDirectoryA(dir.c_str(), nullptr);
#endif
  return dir;
}

void WriteBytes(const std::string& path, const void* data, size_t size) {
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(data, 1, size, f);
    std::fclose(f);
  }
}

// Every 200 ms: the compress calls whose time has come. Its own thread, because the emulated
// renderer has no per-frame call into this file.
void Watcher() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<Recheck> due;
    {
      std::lock_guard<std::mutex> guard(g_lock);
      const auto now = std::chrono::steady_clock::now();
      for (size_t i = 0; i < g_rechecks.size();) {
        if (g_rechecks[i].due > now) {
          ++i;
          continue;
        }
        due.push_back(std::move(g_rechecks[i]));
        g_rechecks.erase(g_rechecks.begin() + i);
      }
    }
    for (const Recheck& r : due) {
      std::vector<uint8_t> source_copy, dest_copy;
      const Bytes source = BytesOf(r.source, r.source_size, &source_copy);
      const Bytes dest = BytesOf(r.dest, r.dest_size, &dest_copy);
      const std::string text =
          fmt::format("{} s later: source {} ({}), destination {} ({})", kRecheckSeconds,
                      !source.readable ? "?" : source.hash == r.source_hash ? "unchanged" : "CHANGED", source.Text(),
                      !dest.readable ? "?" : dest.hash == r.dest_hash ? "unchanged" : "CHANGED", dest.Text());
      if (r.dump && source.readable && dest.readable) {
        static const std::string dir = DumpDir();
        const std::string stem = fmt::format("{}\\c{:04}", dir, r.number);
        WriteBytes(stem + "_src.bin", source_copy.data(), source_copy.size());
        WriteBytes(stem + "_dst.bin", dest_copy.data(), dest_copy.size());
        const std::string about = r.about + "\n" + text + "\n";
        WriteBytes(stem + ".txt", about.data(), about.size());
      }
      std::lock_guard<std::mutex> guard(g_lock);
      if (Room())
        REXLOG_INFO("[nr10] compress #{} {}{}", r.number, text, r.dump ? ", dumped" : "");
    }
  }
}

// Caller holds g_lock.
void AddRecheck(Recheck&& recheck) {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(Watcher).detach(); });
  g_rechecks.push_back(std::move(recheck));
}

// Inside sub_82793B10 on this thread: the compress call number, else 0.
thread_local uint32_t t_compress = 0;
thread_local std::string t_draw_text;
thread_local uint64_t t_drawn = 0, t_dropped = 0;

}  // namespace

bool UfcNr10On() {
  return REXCVAR_GET(ufc3_native_nr10_log);
}

std::string UfcNr10DumpDir() {
  static const std::string dir = DumpDir();
  return dir;
}

void UfcNr10NoteResolve(uint32_t dest_va, uint32_t level, uint32_t face, uint32_t flags, bool have_rect,
                        int32_t left, int32_t top, int32_t right, int32_t bottom, int32_t dst_x, int32_t dst_y,
                        uint32_t src_w, uint32_t src_h) {
  NativeMirrorSource src;
  const bool mirrorable = bd::gpu::GetNativeMirrorSource(dest_va, src);
  const xe::xe_gpu_texture_fetch_t fetch = FetchOf(src);
  const uint64_t frame = bd::gpu::SvrFrameIndex();
  if (!have_rect) {
    left = top = 0;
    right = int32_t(src_w);
    bottom = int32_t(src_h);
  }
  std::lock_guard<std::mutex> guard(g_lock);
  Target& t = g_targets[dest_va];
  if (t.dword1 != fetch.dword_1 || t.dword2 != fetch.dword_2 || !t.width) {
    const bool was = t.width != 0;
    t = {};
    t.dword1 = fetch.dword_1;
    t.dword2 = fetch.dword_2;
    t.width = fetch.size_2d.width + 1;
    t.height = fetch.size_2d.height + 1;
    t.format = uint32_t(fetch.format);
    t.levels = fetch.mip_max_level + 1;
    t.base = src.base;
    t.base_size = src.base_size;
    t.mips = src.mips;
    t.mips_size = src.mips_size;
    ++g_generation;
    if (was && Room())
      REXLOG_INFO("[nr10] resolve target {:08X} now describes another texture: {}", dest_va, FetchText(fetch));
  }
  const Rect rect{level, face, dst_x, dst_y, dst_x + (right - left), dst_y + (bottom - top), frame, frame, 1};
  for (Rect& r : t.rects) {
    if (r.level == level && r.face == face && r.x0 == rect.x0 && r.y0 == rect.y0 && r.x1 == rect.x1 &&
        r.y1 == rect.y1) {
      ++r.count;
      r.last = frame;
      return;
    }
  }
  if (t.rects.size() < kMaxRectsPerTarget)
    t.rects.push_back(rect);
  // The 1280x720 frame targets resolve every frame and never feed the compressor.
  if ((t.width == 1280 && t.height == 720) || g_resolve_lines >= kMaxResolveLines || !Room())
    return;
  ++g_resolve_lines;
  REXLOG_INFO("[nr10] resolve frame {} tid {} flags {:X} src {}x{} rect {},{}-{},{} -> {:08X} ({}{}) level {} face {} "
              "at {},{}",
              frame, ThreadId(), flags, src_w, src_h, left, top, right, bottom, dest_va, FetchText(fetch),
              mirrorable ? "" : ", no guest layout", level, face, dst_x, dst_y);
}

void UfcNr10NoteBind(uint32_t texture_va, const NativeMirrorSource& src) {
  const xe::xe_gpu_texture_fetch_t fetch = FetchOf(src);
  std::lock_guard<std::mutex> guard(g_lock);
  Bind& bind = g_binds[texture_va];
  if (bind.dword1 == fetch.dword_1 && bind.dword2 == fetch.dword_2 && bind.generation == g_generation)
    return;
  bind = {fetch.dword_1, fetch.dword_2, g_generation};
  const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
  for (const auto& [va, t] : g_targets) {
    if (va == texture_va)
      continue;
    const int level = LevelAt(t, src.base, width);
    if (level < 0 || !g_bind_logged.insert((uint64_t(texture_va) << 32) | va).second)
      continue;
    if (g_bind_lines >= kMaxBindLines || !Room())
      return;
    ++g_bind_lines;
    const Coverage cover = CoverageOf(t, uint32_t(level), width, height);
    const Bytes bytes = BytesOf(src.base, src.base_size);
    REXLOG_INFO("[nr10] bind frame {} tid {}{}: texture {:08X} ({}) is mirrored from guest memory that resolve target "
                "{:08X} ({}x{} fmt {} levels {}) owns, level {}{}: resolves cover {:.1f}% ({} rects, last frame {}), "
                "guest {}",
                bd::gpu::SvrFrameIndex(), ThreadId(), t_compress ? fmt::format(" in compress #{}", t_compress) : "",
                texture_va, FetchText(fetch), va, t.width, t.height, t.format, t.levels, level,
                src.base == t.base || level ? "" : " (inside, not at its start)", cover.percent, cover.rects,
                cover.last, bytes.Text());
  }
}

void UfcNr10NoteShortcut(uint32_t texture_va) {
  // Fetch dwords 1 (format, base) and 2 (size) of the D3D texture header (+0x1C).
  const uint32_t dword1 = LoadVa(texture_va + 0x1C + 4), dword2 = LoadVa(texture_va + 0x1C + 8);
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_binds.find(texture_va);
  if (it == g_binds.end() || (it->second.dword1 == dword1 && it->second.dword2 == dword2))
    return;
  if (g_bind_lines >= kMaxBindLines || !Room())
    return;
  ++g_bind_lines;
  REXLOG_INFO("[nr10] stale mirror frame {} tid {}{}: texture {:08X} changed its header within one frame (fetch "
              "{:08X} {:08X}, was {:08X} {:08X}) and keeps the mirror of the earlier one",
              bd::gpu::SvrFrameIndex(), ThreadId(), t_compress ? fmt::format(" in compress #{}", t_compress) : "",
              texture_va, dword1, dword2, it->second.dword1, it->second.dword2);
}

void UfcNr10NoteDraw(uint32_t dev, uint32_t prim, uint32_t count, bool after) {
  if (!t_compress)
    return;
  uint64_t drawn = 0, dropped = 0;
  bd::gpu::SvrDrawCounts(drawn, dropped);
  if (!after) {
    namespace d = ufc3::d3d;
    t_drawn = drawn;
    t_dropped = dropped;
    const auto shader_text = [&](uint32_t va) {
      const bd::gpu::GuestShader* shader = bd::gpu::SvrLookupShader(va);
      if (!shader)
        return fmt::format("{:08X} (no host object)", va);
      if (!shader->shaderCacheEntry)
        return fmt::format("{:08X} (not in the shader cache)", va);
      return fmt::format("{:08X} ({:016X})", va, uint64_t(shader->shaderCacheEntry->hash));
    };
    const auto& s = bd::gpu::state();
    const auto target_text = [](const bd::gpu::GuestTexture* t) {
      return t ? fmt::format("{}x{} host format {}", t->width, t->height, uint32_t(t->format)) : std::string("none");
    };
    const uint32_t rt0 = LoadVa(dev + d::kRenderTarget0), rt1 = LoadVa(dev + d::kRenderTarget0 + 4);
    t_draw_text += fmt::format(" | draw prim {} count {} vs {} ps {} rt0 {:08X} (format {:08X}) rt1 {:08X} ds {:08X}, "
                               "host target {} depth {}",
                               prim, count, shader_text(LoadVa(dev + d::kVertexShader)),
                               shader_text(LoadVa(dev + d::kPixelShader)), rt0,
                               rt0 ? LoadVa(rt0 + 0x28) : 0, rt1, LoadVa(dev + d::kDepthStencil),
                               target_text(s.render_target), target_text(s.depth_stencil));
    for (uint32_t slot : {5u, 15u})
      t_draw_text += fmt::format(" t{} {:08X}", slot, LoadVa(dev + d::kTexture + 4 * slot));
  } else {
    t_draw_text += drawn > t_drawn ? " -> reached the host" : dropped > t_dropped ? " -> DROPPED" : " -> not counted";
  }
}

// D3DDevice_BeginExport / EndExport (dev, Index, pResource, Flags). Also called by sub_82386650.
REX_EXTERN(__imp__sub_8236BBE0);
extern "C" REX_FUNC(sub_8236BBE0) {
  if (UfcNr10On()) {
    static std::unordered_map<uint32_t, uint32_t> by_caller;
    std::lock_guard<std::mutex> guard(g_lock);
    if (++by_caller[uint32_t(ctx.lr)] <= 24 && Room())
      REXLOG_INFO("[nr10] BeginExport tid {} lr {:08X} dev {:08X} index {} resource {:08X} flags {:X}{}", ThreadId(),
                  uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                  t_compress ? fmt::format(" in compress #{}", t_compress) : "");
  }
  __imp__sub_8236BBE0(ctx, base);
}

REX_EXTERN(__imp__sub_8236BCE8);
extern "C" REX_FUNC(sub_8236BCE8) {
  const uint32_t lr = uint32_t(ctx.lr), resource = ctx.r5.u32;
  __imp__sub_8236BCE8(ctx, base);
  if (UfcNr10On()) {
    static std::unordered_map<uint32_t, uint32_t> by_caller;
    std::lock_guard<std::mutex> guard(g_lock);
    if (++by_caller[lr] <= 24 && Room())
      REXLOG_INFO("[nr10] EndExport returned tid {} lr {:08X} resource {:08X}{}", ThreadId(), lr, resource,
                  t_compress ? fmt::format(" in compress #{}", t_compress) : "");
  }
}

// Compressor start (vtable 0x820EBCA8 [1]): runs sub_82794378 now, or queues it when config +68 is set.
REX_EXTERN(__imp__sub_82794700);
extern "C" REX_FUNC(sub_82794700) {
  if (UfcNr10On()) {
    const uint32_t self = ctx.r3.u32, config = Load32(base, self + 4);
    std::string text;
    if (config >= 0x10000) {
      text = fmt::format("source object {:08X} destination object {:08X} deferred {} mode {} refined {}",
                         Load32(base, config + 4), Load32(base, config + 8), Load32(base, config + 68),
                         Load32(base, config + 88), Load32(base, config + 92));
      if (Load32(base, config + 76))
        text += fmt::format(" source rect {} {} {} {}", Load32(base, config + 32), Load32(base, config + 36),
                            Load32(base, config + 40), Load32(base, config + 44));
      if (Load32(base, config + 80))
        text += fmt::format(" destination rect {} {} {} {}", Load32(base, config + 48), Load32(base, config + 52),
                            Load32(base, config + 56), Load32(base, config + 60));
    }
    std::lock_guard<std::mutex> guard(g_lock);
    if (Room())
      REXLOG_INFO("[nr10] start frame {} tid {} lr {:08X} compressor {:08X}: {}", bd::gpu::SvrFrameIndex(), ThreadId(),
                  uint32_t(ctx.lr), self, text);
  }
  __imp__sub_82794700(ctx, base);
}

// One texture: a compress draw per mip level.
REX_EXTERN(__imp__sub_82794378);
extern "C" REX_FUNC(sub_82794378) {
  if (!UfcNr10On()) {
    __imp__sub_82794378(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_82794378(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::lock_guard<std::mutex> guard(g_lock);
  if (Room())
    REXLOG_INFO("[nr10] texture done frame {} tid {} lr {:08X} compressor {:08X}: {} levels, {:.2f} ms",
                bd::gpu::SvrFrameIndex(), ThreadId(), lr, self, Load32(base, self + 432), ms);
}

// One compress draw: (this, dev, source level description, destination level description, mode,
// index). Descriptions: +4 width, +8 height, +16 D3DFORMAT, +60 address of the level's data.
REX_EXTERN(__imp__sub_82793B10);
extern "C" REX_FUNC(sub_82793B10) {
  if (!UfcNr10On()) {
    if (!UfcCpuCompress(ctx, base, nullptr))  // NR-10c: on native the C++ encoder replaces the draw
      __imp__sub_82793B10(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32, dev = ctx.r4.u32, src_desc = ctx.r5.u32, dst_desc = ctx.r6.u32;
  const uint32_t mode = ctx.r7.u32, index = ctx.r8.u32, lr = uint32_t(ctx.lr);
  const uint32_t config = Load32(base, self + 4);
  const uint32_t src_w = Load32(base, src_desc + 4), src_h = Load32(base, src_desc + 8);
  const std::string head = fmt::format(
      "tid {} lr {:08X} compressor {:08X} dev {:08X} mode {} index {} (config deferred {} mode {} refined {}) source "
      "{}x{} format {:08X} at {:08X} -> destination {}x{} format {:08X} at {:08X}",
      ThreadId(), lr, self, dev, mode, index, config >= 0x10000 ? Load32(base, config + 68) : ~0u,
      config >= 0x10000 ? Load32(base, config + 88) : ~0u, config >= 0x10000 ? Load32(base, config + 92) : ~0u, src_w,
      src_h, Load32(base, src_desc + 16), Load32(base, src_desc + 60), Load32(base, dst_desc + 4),
      Load32(base, dst_desc + 8), Load32(base, dst_desc + 16), Load32(base, dst_desc + 60));
  uint32_t number = 0;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    number = ++g_compress_count;
  }
  t_compress = number;
  t_draw_text.clear();
  const auto start = std::chrono::steady_clock::now();
  UfcCpuCompressReport cpu;
  if (UfcCpuCompress(ctx, base, &cpu))
    t_draw_text = fmt::format(" | CPU encoder: {} blocks, encode {:.2f} ms, resolve write-back {:.2f} ms; config sizes "
                              "{}x{} -> {}x{}",
                              cpu.blocks, cpu.encode_ms, cpu.source_ms, Load32(base, config + 12),
                              Load32(base, config + 16), Load32(base, config + 20), Load32(base, config + 24));
  else
    __imp__sub_82793B10(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  t_compress = 0;

  // The headers the call built: the source alias at +96 (one level of the composite, sampler 5)
  // and, in memexport mode, the export resource at +36 (the destination level as 8-byte texels).
  NativeMirrorSource alias, exported;
  const bool have_alias = bd::gpu::GetNativeMirrorSource(self + 96, alias);
  const bool have_export = bd::gpu::GetNativeMirrorSource(self + 36, exported);
  const Bytes source = have_alias ? BytesOf(alias.base, alias.base_size) : Bytes{};
  // The export header's extent: the DXT5 level (one byte a pixel) rounded up to whole 32x32-block
  // tiles, which a tiled level smaller than a tile, or not a multiple of one, spreads over.
  const size_t out_size = have_export ? exported.base_size : 0;
  const Bytes dest = have_export ? BytesOf(exported.base, out_size) : Bytes{};
  const uint64_t frame = bd::gpu::SvrFrameIndex();

  const std::string source_text =
      fmt::format("source header: {}; guest {}", have_alias ? FetchText(FetchOf(alias)) : std::string("unreadable"),
                  source.Text());
  const std::string dest_text =
      fmt::format("export header: {}; destination {}",
                  have_export ? FetchText(FetchOf(exported)) : std::string("unreadable"), dest.Text());

  std::lock_guard<std::mutex> guard(g_lock);
  if (have_alias && have_export && out_size) {
    // A few of each source size, small levels only: enough to check an encoder byte for byte.
    static std::unordered_map<uint64_t, uint32_t> dumped_by_size;
    static uint32_t dumped = 0;
    const bool dump = dumped < uint32_t(REXCVAR_GET(ufc3_native_nr10_dump)) && src_w * src_h <= kDumpMaxPixels &&
                      dumped_by_size[(uint64_t(src_w) << 32) | src_h]++ < kDumpPerSize;
    dumped += dump;
    std::string about = fmt::format("compress #{} frame {} {}\n{}\n{}\nsource fetch", number, frame, head,
                                    source_text, dest_text);
    for (uint32_t dword : alias.fetch)
      about += fmt::format(" {:08X}", dword);
    about += "\nexport fetch";
    for (uint32_t dword : exported.fetch)
      about += fmt::format(" {:08X}", dword);
    AddRecheck({number,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kRecheckSeconds * 1000)),
                alias.base, alias.base_size, source.hash, exported.base, out_size, dest.hash, dump,
                std::move(about)});
  }
  if (!Room())
    return;
  REXLOG_INFO("[nr10] compress #{} frame {} {}, {:.2f} ms{}", number, frame, head, ms,
              t_draw_text.empty() ? " | no draw seen by the renderer" : t_draw_text);
  if (!Room())
    return;
  REXLOG_INFO("[nr10] compress #{} {}", number, source_text);
  if (!Room())
    return;
  REXLOG_INFO("[nr10] compress #{} {}", number, dest_text);
  if (!have_alias)
    return;
  uint32_t owners = 0;
  for (const auto& [va, t] : g_targets) {
    const int level = LevelAt(t, alias.base, src_w);
    if (level < 0)
      continue;
    ++owners;
    const Coverage cover = CoverageOf(t, uint32_t(level), src_w, src_h);
    if (owners <= 4 && Room())
      REXLOG_INFO("[nr10] compress #{} source is level {} of resolve target {:08X} ({}x{} fmt {} levels {}){}: "
                  "resolves cover {:.1f}% of {}x{} ({} rects, frames {}-{}, now {}):{}",
                  number, level, va, t.width, t.height, t.format, t.levels,
                  alias.base == t.base || level ? "" : " (inside, not at its start)", cover.percent, src_w, src_h,
                  cover.rects, cover.first, cover.last, frame, cover.list);
  }
  if (!owners && Room())
    REXLOG_INFO("[nr10] compress #{} source is not memory of any resolve target seen so far ({} targets)", number,
                g_targets.size());
}
