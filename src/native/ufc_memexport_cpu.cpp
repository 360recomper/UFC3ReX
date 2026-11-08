// NR-10c: replaces one compress draw of the game's yGPUCompression effect (sub_82793B10) with a C++
// encoder, guest memory to guest memory. The native renderer has no memexport: the draw is dropped
// and every CAF and career skin stays as it was, zeros (black fighters) or stale bytes (confetti).
// Guest memory is the source of truth (NR-10a: sources mix CPU bytes and resolves), so NR-10b's
// write-back runs first.
//
// Only what the game does here is handled: memexport mode, fast DXT5, both rects set (226 of 226
// calls in the user's career run). Anything else logs and runs the game's body.
//
// The rects stay in level 0 units in mode 1: sub_82794378 halves this+448.. only when mode is 0.
// sub_82792C98 divides them by config +12..+24 (level 0 sizes) into the vertex buffer, so they scale
// with the level through the target's size. this+448.. and this+464.. hold y, x, w, h.

#include "ufc_memexport_cpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>
#include <rex/thread/mutex.h>

#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"
#include "ufc_dxt5_cpu.h"
#include "ufc_native.h"

REXCVAR_DEFINE_STRING(ufc3_native_memexport, "cpu", "ufc3",
                      "Native renderer, the game's DXT compressor (CAF and career skins): cpu = encode in "
                      "C++ in place of its draw; off = run the game's code, whose draw is dropped");

// par/10b.md: writes every pending resolve over this guest range to guest memory, waits for the GPU.
void UfcResolveToGuest(uint32_t guest_address, size_t size);

namespace bd::gpu {
void SvrMirrorInvalidate(u32 texture_va);  // ufc_resources.cpp
}

REX_EXTERN(sub_82792998);  // source level header at this+96
REX_EXTERN(sub_82792C18);  // export header at this+36

namespace {

namespace xe = rex::graphics::xenos;

constexpr uint32_t kDxt5 = 0x1A200154;
constexpr uint32_t kThisSrcRect = 448, kThisDstRect = 464;  // y, x, w, h

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return _byteswap_ulong(v);
}

// So a new use of the compressor shows in the log.
bool Refuse(const char* why, uint32_t a, uint32_t b) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_WARN("native: DXT compress left to the game's code: {} ({:X} {:X})", why, a, b);
  return false;
}

bool WholePowerOfTwo(uint32_t level0, uint32_t level) {
  return level && level0 >= level && level0 % level == 0 && ((level0 / level) & (level0 / level - 1)) == 0;
}

// Host pages of the range without write access, made writable for the encode (0014: a read-only
// page is written all the same, never skipped; the fight-end crash was a write to one).
struct WriteAccess {
  struct Run {
    uint8_t* at;
    size_t size;
    rex::memory::PageAccess old_access;
  };
  std::vector<Run> runs;
  bool ok = true;

  WriteAccess(uint8_t* data, size_t size) {
    const uintptr_t page_mask = uintptr_t(rex::memory::page_size() - 1);
    size_t offset = 0;
    while (offset < size) {
      uint8_t* at = data + offset;
      size_t region = 0;
      rex::memory::PageAccess access;
      if (!rex::memory::QueryProtect(at, region, access) || !region) {
        ok = false;
        return;
      }
      const size_t into_page = size_t(reinterpret_cast<uintptr_t>(at) & page_mask);
      const size_t run = std::min(region - into_page, size - offset);
      if (access != rex::memory::PageAccess::kReadWrite && access != rex::memory::PageAccess::kExecuteReadWrite) {
        auto lock = rex::thread::global_critical_region::AcquireDirect();
        rex::memory::PageAccess old_access;
        if (!rex::memory::Protect(at, run, rex::memory::PageAccess::kReadWrite, &old_access)) {
          ok = false;
          return;
        }
        runs.push_back({at, run, old_access});
      }
      offset += run;
    }
  }

  ~WriteAccess() {
    if (runs.empty())
      return;
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    for (const Run& r : runs)
      rex::memory::Protect(r.at, r.size, r.old_access);
  }
};

std::atomic<uint64_t> g_calls{0}, g_blocks{0}, g_encode_us{0}, g_source_us{0};

}  // namespace

bool UfcCpuCompress(PPCContext& ctx, uint8_t* base, UfcCpuCompressReport* report) {
  using Clock = std::chrono::steady_clock;
  if (!g_ufc3_native)
    return false;
  static const bool on = std::string(REXCVAR_GET(ufc3_native_memexport)) != "off";
  if (!on)
    return false;

  const uint32_t self = ctx.r3.u32, src_desc = ctx.r5.u32, dst_desc = ctx.r6.u32, mode = ctx.r7.u32,
                 index = ctx.r8.u32;
  const uint32_t config = Load32(base, self + 4);
  const uint32_t dst_format = Load32(base, dst_desc + 16);
  if (mode != 1 || dst_format != kDxt5 || config < 0x10000 || Load32(base, config + 92))
    return Refuse("not memexport fast DXT5", mode, dst_format);
  if (!Load32(base, config + 76) || !Load32(base, config + 80))
    return Refuse("no rects", Load32(base, config + 76), Load32(base, config + 80));

  // This level, and the level 0 sizes the rects are relative to (see the top of the file).
  const uint32_t src_w = Load32(base, src_desc + 4), src_h = Load32(base, src_desc + 8);
  const uint32_t dst_w = Load32(base, dst_desc + 4), dst_h = Load32(base, dst_desc + 8);
  const uint32_t src0_w = Load32(base, config + 12), src0_h = Load32(base, config + 16);
  const uint32_t dst0_w = Load32(base, config + 20), dst0_h = Load32(base, config + 24);
  if (!WholePowerOfTwo(src0_w, src_w) || !WholePowerOfTwo(src0_h, src_h) || !WholePowerOfTwo(dst0_w, dst_w) ||
      !WholePowerOfTwo(dst0_h, dst_h) || dst_w < 32 || dst_h < 32)
    return Refuse("sizes", (src0_w << 16) | src_w, (dst0_w << 16) | dst_w);
  const uint32_t sy = Load32(base, self + kThisSrcRect), sx = Load32(base, self + kThisSrcRect + 4);
  const uint32_t sw = Load32(base, self + kThisSrcRect + 8), sh = Load32(base, self + kThisSrcRect + 12);
  const uint32_t dy = Load32(base, self + kThisDstRect), dx = Load32(base, self + kThisDstRect + 4);
  const uint32_t dw = Load32(base, self + kThisDstRect + 8), dh = Load32(base, self + kThisDstRect + 12);
  const uint32_t sdiv_x = src0_w / src_w, sdiv_y = src0_h / src_h, ddiv_x = dst0_w / dst_w, ddiv_y = dst0_h / dst_h;
  ufc3_dxt5::Rect rect;
  rect.src_x = sx / sdiv_x / 4;
  rect.src_y = sy / sdiv_y / 4;
  rect.dst_x = dx / ddiv_x / 4;
  rect.dst_y = dy / ddiv_y / 4;
  rect.width = sw / sdiv_x / 4;
  rect.height = sh / sdiv_y / 4;
  if (rect.width != dw / ddiv_x / 4 || rect.height != dh / ddiv_y / 4)
    return Refuse("source and destination rects differ", (sw << 16) | sh, (dw << 16) | dh);

  // From here on the level counts as handled. The game's own header builders: no device state, and
  // the probe reads the same headers. Same arguments as the body.
  ctx.r3.u64 = self;
  ctx.r4.u64 = src_desc;
  ctx.r5.u64 = dst_format;
  ctx.r6.u64 = mode;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 1;  // 16-byte blocks
  ctx.r9.u64 = self + 96;
  ctx.r10.u64 = index;
  sub_82792998(ctx, base);
  ctx.r3.u64 = self;
  ctx.r4.u64 = dst_desc;
  ctx.r5.u64 = self + 36;
  ctx.r6.u64 = index;
  sub_82792C18(ctx, base);

  bd::gpu::NativeMirrorSource alias, exported;
  if (!bd::gpu::GetNativeMirrorSource(self + 96, alias) || !bd::gpu::GetNativeMirrorSource(self + 36, exported) ||
      !alias.base || !exported.base) {
    Refuse("headers", self, 0);
    return true;  // the body would do no better: its draw is dropped
  }
  xe::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, alias.fetch, sizeof(alias.fetch));
  if (uint32_t(fetch.format) != uint32_t(xe::TextureFormat::k_8_8_8_8)) {
    Refuse("source format", uint32_t(fetch.format), 0);
    return true;
  }

  const auto t0 = Clock::now();
  UfcResolveToGuest(uint32_t(fetch.base_address) << 12, alias.base_size);
  const auto t1 = Clock::now();

  ufc3_dxt5::Source source;
  source.base = alias.base;
  source.size = alias.base_size;
  source.width = fetch.size_2d.width + 1;
  source.height = fetch.size_2d.height + 1;
  source.pitch = uint32_t(fetch.pitch) << 5;
  source.tiled = fetch.tiled != 0;
  source.endian = uint32_t(fetch.endianness);
  source.swizzle = uint32_t(fetch.swizzle);

  // The level's real extent as DXT5, whole 32x32-block tiles. The export header's own extent is
  // smaller on small levels (8 bytes a texel, half as many texels each way).
  ufc3_dxt5::Dest dest;
  dest.base = const_cast<uint8_t*>(exported.base);
  dest.width_blocks = dst_w / 4;
  dest.height_blocks = dst_h / 4;
  dest.size = size_t((dest.width_blocks + 31) & ~31u) * size_t((dest.height_blocks + 31) & ~31u) * 16;

  // A resolve still remembered over the destination (memory an old target owned) goes to guest
  // memory now: left pending, it would land on top of the blocks at the next bind or CPU access.
  xe::xe_gpu_texture_fetch_t export_fetch{};
  std::memcpy(&export_fetch, exported.fetch, sizeof(exported.fetch));
  UfcResolveToGuest(uint32_t(export_fetch.base_address) << 12, dest.size);

  ufc3_dxt5::Stats stats;
  {
    WriteAccess access(dest.base, dest.size);
    if (!access.ok) {
      Refuse("destination pages", uint32_t(fetch.base_address) << 12, uint32_t(dest.size));
      return true;
    }
    stats = ufc3_dxt5::EncodeRect(source, dest, rect);
  }
  const auto t2 = Clock::now();

  // The fighter's material binds the destination object (config +8): its next bind rebuilds the
  // mirror. SvrResolveTexture's own full hash would catch it within 8 frames anyway.
  bd::gpu::SvrMirrorInvalidate(Load32(base, config + 8));

  const auto us = [](auto d) { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count()); };
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_blocks.fetch_add(stats.blocks, std::memory_order_relaxed);
  g_encode_us.fetch_add(us(t2 - t1), std::memory_order_relaxed);
  g_source_us.fetch_add(us(t1 - t0), std::memory_order_relaxed);
  if (stats.out_of_range)
    REXLOG_WARN("native: DXT compress: {} blocks past the bytes, rect {},{} -> {},{} {}x{} source {}x{} dest {}x{}",
                stats.out_of_range, rect.src_x, rect.src_y, rect.dst_x, rect.dst_y, rect.width, rect.height, src_w,
                src_h, dst_w, dst_h);
  if (calls % 113 == 0)  // one career load
    REXLOG_INFO("native: CPU DXT5: {} calls, {} blocks, encode {:.1f} ms, resolve write-back {:.1f} ms (totals)", calls,
                g_blocks.load(), double(g_encode_us.load()) / 1000.0, double(g_source_us.load()) / 1000.0);
  if (report) {
    report->blocks = uint32_t(stats.blocks);
    report->encode_ms = double(us(t2 - t1)) / 1000.0;
    report->source_ms = double(us(t1 - t0)) / 1000.0;
  }
  return true;
}
