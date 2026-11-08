// The game's recorded command buffers (NR-8b-2i, 2k): arena screens and rim wall, two a frame.
// The game records them at venue load, each on its own D3D device, through the same draw calls the
// native hooks see, and they never change afterwards (ufc-nr-nr8b21-cbdump-native). So native keeps a
// copy of the recording device at each recorded draw, and at RunCommandBuffer draws from that copy,
// with the constants the buffer inherits (masks at cb +24 and +32) taken from the live device.
// ufc3_cb_probe logs each buffer once, PM4 included; ufc3_skip_command_buffers drops them, which on
// the emulated renderer shows what they are. Emulated with default cvars: the game's functions run
// unchanged.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "ufc_cb_mirror.h"
#include "ufc_d3d.h"
#include "ufc_native.h"

REXCVAR_DEFINE_BOOL(ufc3_cb_probe, false, "ufc3",
                    "Dev: log each recorded command buffer the first time RunCommandBuffer runs it");
REXCVAR_DEFINE_BOOL(ufc3_skip_command_buffers, false, "ufc3",
                    "Dev bisect: don't run the game's recorded command buffers, to see what they draw "
                    "on the emulated renderer");
REXCVAR_DECLARE(int32_t, ufc3_native_off);

namespace {

uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

struct Scan {
  uint32_t draws = 0, predicated = 0;
  uint32_t ops[128] = {};
  std::string first_draws;
};

// Same walk as the census (src/dev/ufc_d3d_census.cpp), which isn't built with the native hooks.
void ScanVa(uint8_t* base, uint32_t va, uint32_t dwords, int depth, Scan& out) {
  if (!va || dwords > 0x100000 || depth > 4)
    return;
  for (uint32_t i = 0; i < dwords;) {
    const uint32_t h = Load32(base, va + 4 * i);
    const uint32_t type = h >> 30;
    if (type == 0) {
      i += 2 + ((h >> 16) & 0x3FFF);
    } else if (type == 1) {
      i += 3;
    } else if (type == 2) {
      i += 1;
    } else {
      const uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      ++out.ops[op];
      if (op == 0x22 || op == 0x36) {
        ++out.draws;
        if (h & 1)
          ++out.predicated;
        if (out.draws <= 16 && i + 2 < dwords)
          out.first_draws += fmt::format(" {:08X}", Load32(base, va + 4 * (i + 2)));
      } else if ((op == 0x3F || op == 0x37) && i + 2 < dwords) {  // INDIRECT_BUFFER(_PFD)
        const uint32_t addr = Load32(base, va + 4 * (i + 1));
        ScanVa(base, addr >= 0x80000000u ? addr : 0xA0000000u + (addr & 0x1FFFFFFFu),
               Load32(base, va + 4 * (i + 2)) & 0xFFFFF, depth + 1, out);
      }
      i += 1 + count;
    }
  }
}

void LogFirstRun(uint8_t* base, uint32_t lr, uint32_t dev, uint32_t cb) {
  static std::mutex lock;
  static std::unordered_set<uint32_t> seen;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() >= 256 || !seen.insert(cb).second)
      return;
  }
  Scan scan;
  uint32_t node = Load32(base, cb + ufc3::d3d::kCbSegments);
  uint32_t segments = 0;
  for (int nodes = 0; node >= 0x10000 && nodes < 256; ++nodes) {
    const uint32_t n = Load32(base, node + 4);
    for (uint32_t e = 0; e < n && e < 4096; ++e, ++segments) {
      const uint32_t entry = node + 8 + 8 * e;
      const uint32_t addr = Load32(base, entry + 4);
      ScanVa(base, addr >= 0x80000000u ? addr : 0xA0000000u + (addr & 0x1FFFFFFFu),
             Load32(base, entry) & 0xFFFFFF, 0, scan);
    }
    node = Load32(base, node);
  }
  std::string ops;
  for (int op = 0; op < 128; ++op)
    if (scan.ops[op])
      ops += fmt::format(" {:02X}x{}", op, scan.ops[op]);
  REXLOG_INFO("[cb] {:08X} first run: lr {:08X} flags {:08X} rt {:08X} ds {:08X} vs {:08X} ps {:08X}: {} segments, "
              "{} draws ({} predicated), ops{} | draw initiators{}",
              cb, lr, Load32(base, cb + ufc3::d3d::kCbFlags), Load32(base, dev + ufc3::d3d::kRenderTarget0),
              Load32(base, dev + ufc3::d3d::kDepthStencil), Load32(base, dev + ufc3::d3d::kVertexShader),
              Load32(base, dev + ufc3::d3d::kPixelShader), segments, scan.draws, scan.predicated, ops,
              scan.first_draws);
}

uint32_t ToVa(uint32_t addr) {
  return addr >= 0x80000000u ? addr : 0xA0000000u + (addr & 0x1FFFFFFFu);
}

void LogWords(uint8_t* base, const char* tag, uint32_t cb, uint32_t va, uint32_t dwords, uint32_t cap) {
  for (uint32_t i = 0; i < dwords && i < cap; i += 16) {
    std::string line;
    for (uint32_t k = i; k < i + 16 && k < dwords && k < cap; ++k)
      line += fmt::format(" {:08X}", Load32(base, va + 4 * k));
    REXLOG_INFO("[cbdump] {:08X} {} +{:X}:{}", cb, tag, i, line);
  }
}

uint64_t HashWords(uint8_t* base, uint32_t va, uint32_t dwords, uint64_t h) {
  for (uint32_t i = 0; i < dwords; ++i)
    h = (h ^ Load32(base, va + 4 * i)) * 0x100000001B3ull;
  return h;
}

// One buffer's PM4, packet by packet, with what LOAD_ALU_CONSTANT and IM_LOAD point at. log = false
// only hashes it, to see whether the game edits a recorded buffer between runs.
uint64_t WalkPm4(uint8_t* base, uint32_t cb, uint32_t va, uint32_t dwords, bool log, uint64_t h) {
  if (!va || dwords > 0x100000)
    return h;
  h = HashWords(base, va, dwords, h);
  for (uint32_t i = 0; i < dwords;) {
    const uint32_t p = Load32(base, va + 4 * i);
    const uint32_t type = p >> 30;
    if (type == 0) {
      const uint32_t n = ((p >> 16) & 0x3FFF) + 1;
      if (log) {
        REXLOG_INFO("[cbdump] {:08X} @{:X} reg {:04X} x{}", cb, i, p & 0x7FFF, n);
        LogWords(base, "regs", cb, va + 4 * (i + 1), n, 64);
      }
      i += 1 + n;
    } else if (type == 1) {
      i += 3;
    } else if (type == 2) {
      i += 1;
    } else {
      const uint32_t op = (p >> 8) & 0x7F, n = ((p >> 16) & 0x3FFF) + 1;
      if (log) {
        REXLOG_INFO("[cbdump] {:08X} @{:X} op {:02X} x{} hdr {:08X}", cb, i, op, n, p);
        LogWords(base, "body", cb, va + 4 * (i + 1), n, op == 0x2B ? 8 : 32);
      }
      if (op == 0x2F && n >= 3) {  // LOAD_ALU_CONSTANT {addr, offset | type << 16, size}
        const uint32_t from = ToVa(Load32(base, va + 4 * (i + 1)) & 0x3FFFFFFF);
        const uint32_t size = Load32(base, va + 4 * (i + 3)) & 0xFFF;
        h = HashWords(base, from, size, h);
        if (log)
          LogWords(base, "alu", cb, from, size, 1024);
      } else if (op == 0x27 && n >= 2 && log) {  // IM_LOAD {addr | stage, start << 16 | size}
        LogWords(base, "ucode", cb, ToVa(Load32(base, va + 4 * (i + 1)) & ~3u),
                 Load32(base, va + 4 * (i + 2)) & 0xFFFF, 8);
      }
      i += 1 + n;
    }
  }
  return h;
}

uint64_t WalkSegments(uint8_t* base, uint32_t cb, bool log) {
  uint64_t h = 0xCBF29CE484222325ull;
  uint32_t node = Load32(base, cb + ufc3::d3d::kCbSegments);
  for (int nodes = 0; node >= 0x10000 && nodes < 256; ++nodes) {
    const uint32_t n = Load32(base, node + 4);
    for (uint32_t e = 0; e < n && e < 4096; ++e) {
      const uint32_t entry = node + 8 + 8 * e;
      h = WalkPm4(base, cb, ToVa(Load32(base, entry + 4)), Load32(base, entry) & 0xFFFFFF, log, h);
    }
    node = Load32(base, node);
  }
  return h;
}

void LogCbMasks(uint8_t* base, uint32_t cb) {
  std::string masks;
  for (uint32_t off = 0; off < 112; off += 4)
    masks += fmt::format(" {:08X}", Load32(base, cb + off));
  REXLOG_INFO("[cb] {:08X} object words 0..108:{}", cb, masks);
}

// What the device holds when the buffer runs: the state a buffer inherits comes from here.
void LogDeviceAtRun(uint8_t* base, uint32_t dev, uint32_t cb, uint32_t predicate) {
  namespace d = ufc3::d3d;
  std::string text = fmt::format("predicate {:08X} decl {:08X} ib {:08X} streams", predicate,
                                 Load32(base, dev + d::kVertexDeclaration),
                                 Load32(base, dev + d::kIndexBuffer));
  for (uint32_t s = 0; s < 4; ++s)
    text += fmt::format(" {:08X}", Load32(base, dev + d::kStreamSource + 4 * s));
  text += " textures";
  for (uint32_t s = 0; s < 16; ++s)
    if (const uint32_t t = Load32(base, dev + d::kTexture + 4 * s))
      text += fmt::format(" {}={:08X}", s, t);
  REXLOG_INFO("[cb] {:08X} device at run: {}", cb, text);
  for (uint32_t s = 0; s < 32; ++s) {
    const uint32_t f = dev + d::kFetchConstants + 24 * s;
    if (Load32(base, f) | Load32(base, f + 4) | Load32(base, f + 8))
      REXLOG_INFO("[cb] {:08X} fetch {}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", cb, s, Load32(base, f),
                  Load32(base, f + 4), Load32(base, f + 8), Load32(base, f + 12), Load32(base, f + 16),
                  Load32(base, f + 20));
  }
}

void ProbeRun(uint8_t* base, uint32_t lr, uint32_t dev, uint32_t cb, uint32_t predicate) {
  static std::mutex lock;
  static std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> runs;  // cb -> count, first hash
  uint32_t run = 0;
  uint64_t first = 0;
  {
    std::lock_guard<std::mutex> guard(lock);
    auto& r = runs[cb];
    run = ++r.first;
    first = r.second;
  }
  if (run == 1) {
    LogFirstRun(base, lr, dev, cb);
    LogCbMasks(base, cb);
    LogDeviceAtRun(base, dev, cb, predicate);
    const uint64_t h = WalkSegments(base, cb, true);
    std::lock_guard<std::mutex> guard(lock);
    runs[cb].second = h;
    REXLOG_INFO("[cb] {:08X} content hash {:016X}", cb, h);
  } else if (run == 600 || run == 3000) {
    const uint64_t h = WalkSegments(base, cb, false);
    REXLOG_INFO("[cb] {:08X} run {}: content {} ({:016X})", cb, run, h == first ? "unchanged" : "CHANGED", h);
    LogDeviceAtRun(base, dev, cb, predicate);
  }
}

uint8_t* g_base = nullptr;

// One recorded draw: its call, and the recording device up to the end of the state the draw reads
// (kRecordingCommandBuffer is past the copy, so the scratch device reads as not recording).
constexpr uint32_t kDeviceCopy = 0x3300;
static_assert(ufc3::d3d::kVertexShader < kDeviceCopy && kDeviceCopy <= ufc3::d3d::kRecordingCommandBuffer);

struct RecordedDraw {
  bool indexed;
  uint32_t prim, base_vertex, start, count;
  std::vector<uint8_t> device;
};

std::mutex g_recorded_lock;
std::unordered_map<uint32_t, std::vector<RecordedDraw>> g_recorded;
uint32_t g_scratch_device = 0;

// Bit 63 of the mask = registers 0..3, one bit per 4 registers (sub_82784A90).
void CopyInheritedConstants(uint8_t* to, const uint8_t* live, uint32_t offset, uint32_t hi, uint32_t lo) {
  const uint64_t mask = (uint64_t(hi) << 32) | lo;
  for (uint32_t group = 0; group < 64; ++group)
    if (mask & (1ull << (63 - group)))
      std::memcpy(to + offset + 64 * group, live + offset + 64 * group, 64);
}

void ReplayBuffer(uint8_t* base, uint32_t dev, uint32_t cb) {
  namespace d = ufc3::d3d;
  std::lock_guard<std::mutex> guard(g_recorded_lock);
  auto it = g_recorded.find(cb);
  if (it == g_recorded.end() || it->second.empty())
    return;
  if (!g_scratch_device) {
    g_scratch_device = REX_KERNEL_MEMORY()->SystemHeapAlloc(d::kDeviceSize);
    if (!g_scratch_device)
      return;
    std::memset(base + g_scratch_device, 0, d::kDeviceSize);
    REXLOG_INFO("native: command buffer replay on, scratch device {:08X}", g_scratch_device);
  }
  uint8_t* scratch = base + g_scratch_device;
  const uint8_t* live = base + dev;
  const uint32_t vs_hi = Load32(base, cb + 24), vs_lo = Load32(base, cb + 28);
  const uint32_t ps_hi = Load32(base, cb + 32), ps_lo = Load32(base, cb + 36);
  const uint32_t live_vs = Load32(base, dev + d::kVertexShader), live_ps = Load32(base, dev + d::kPixelShader);
  const uint32_t live_decl = Load32(base, dev + d::kVertexDeclaration);
  UfcMirrorSave();
  for (const RecordedDraw& draw : it->second) {
    std::memcpy(scratch, draw.device.data(), kDeviceCopy);
    CopyInheritedConstants(scratch, live, d::kVsFloatConstants, vs_hi, vs_lo);
    CopyInheritedConstants(scratch, live, d::kPsFloatConstants, ps_hi, ps_lo);
    uint32_t textures[kUfcMirrorTextures];
    for (uint32_t s = 0; s < kUfcMirrorTextures; ++s) {
      textures[s] = Load32(base, g_scratch_device + d::kTexture + 4 * s);
      // A slot the recording never set keeps what the live device has: its texture stays in the
      // mirror, so its sampler bits have to agree.
      if (!textures[s])
        std::memcpy(scratch + d::kFetchConstants + 24 * s, live + d::kFetchConstants + 24 * s, 24);
    }
    UfcMirrorApply(Load32(base, g_scratch_device + d::kVertexShader),
                   Load32(base, g_scratch_device + d::kPixelShader),
                   Load32(base, g_scratch_device + d::kVertexDeclaration), textures);
    UfcDrawRecorded(g_scratch_device, draw.indexed, draw.prim, draw.base_vertex, draw.start, draw.count);
  }
  UfcMirrorRestore(live_vs, live_ps, live_decl);
}

}  // namespace

bool UfcCbCaptureDraw(uint32_t dev, bool indexed, uint32_t prim, uint32_t base_vertex, uint32_t start,
                      uint32_t count) {
  uint8_t* base = g_base;
  if (!base)
    return false;
  const uint32_t cb = Load32(base, dev + ufc3::d3d::kRecordingCommandBuffer);
  if (!cb)
    return false;
  RecordedDraw draw{indexed, prim, base_vertex, start, count, {}};
  draw.device.assign(base + dev, base + dev + kDeviceCopy);
  if (REXCVAR_GET(ufc3_cb_probe)) {
    namespace d = ufc3::d3d;
    std::string text;
    for (uint32_t s = 0; s < 16; ++s)
      if (const uint32_t t = Load32(base, dev + d::kTexture + 4 * s))
        text += fmt::format(" t{}={:08X}", s, t);
    REXLOG_INFO("[cb] recorded draw prim {} count {} start {} base {} vs {:08X} ps {:08X} decl {:08X} ib {:08X} s0 "
                "{:08X} s1 {:08X} rt {:08X} ds {:08X}{}",
                prim, count, start, base_vertex, Load32(base, dev + d::kVertexShader),
                Load32(base, dev + d::kPixelShader), Load32(base, dev + d::kVertexDeclaration),
                Load32(base, dev + d::kIndexBuffer), Load32(base, dev + d::kStreamSource),
                Load32(base, dev + d::kStreamSource + 4), Load32(base, dev + d::kRenderTarget0),
                Load32(base, dev + d::kDepthStencil), text);
  }
  std::lock_guard<std::mutex> guard(g_recorded_lock);
  g_recorded[cb].push_back(std::move(draw));
  return true;
}

bool UfcCbRecordedDraw(uint32_t dev, const char* name, uint32_t prim, uint32_t count, uint32_t, int32_t, uint64_t,
                       uint64_t) {
  uint8_t* base = g_base;
  if (!base || !Load32(base, dev + ufc3::d3d::kRecordingCommandBuffer))
    return false;
  REXLOG_WARN("native: recorded {} (prim {} count {}) dropped: this draw path has no replay", name, prim, count);
  return true;
}

REX_EXTERN(__imp__D3DDevice_BeginCommandBuffer);
extern "C" REX_FUNC(D3DDevice_BeginCommandBuffer) {
  g_base = base;
  {
    std::lock_guard<std::mutex> guard(g_recorded_lock);
    g_recorded[ctx.r4.u32].clear();
  }
  if (REXCVAR_GET(ufc3_cb_probe))
    REXLOG_INFO("[cb] begin dev {:08X} cb {:08X} r5 {:08X} r6 {:08X} r7 {:08X} r8 {:08X} r9 {:08X} lr {:08X}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                static_cast<uint32_t>(ctx.lr));
  __imp__D3DDevice_BeginCommandBuffer(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_EndCommandBuffer);
extern "C" REX_FUNC(D3DDevice_EndCommandBuffer) {
  if (REXCVAR_GET(ufc3_cb_probe))
    REXLOG_INFO("[cb] end dev {:08X} lr {:08X}", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  __imp__D3DDevice_EndCommandBuffer(ctx, base);
}

REX_EXTERN(__imp__D3DDevice_RunCommandBuffer);
extern "C" REX_FUNC(D3DDevice_RunCommandBuffer) {
  const uint32_t dev = ctx.r3.u32, cb = ctx.r4.u32;
  const bool recording = Load32(base, dev + ufc3::d3d::kRecordingCommandBuffer) != 0;
  if (REXCVAR_GET(ufc3_cb_probe) && !recording && cb >= 0x10000)
    ProbeRun(base, static_cast<uint32_t>(ctx.lr), dev, cb, ctx.r5.u32);
  if (REXCVAR_GET(ufc3_skip_command_buffers))
    return;
  if (g_ufc3_native && !recording && g_base && !(REXCVAR_GET(ufc3_native_off) & 64))
    ReplayBuffer(base, dev, cb);
  __imp__D3DDevice_RunCommandBuffer(ctx, base);
}
