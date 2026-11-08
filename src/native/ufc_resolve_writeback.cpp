// NR-10b: see ufc_resolve_writeback.h. Design and proof runs: FINDINGS-native-renderer.md.

#include "ufc_resolve_writeback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xxhash.h>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/backend.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/resources.h"
#include "ufc_geometry.h"
#include "ufc_nr10_probe.h"
#include "ufc_resources.h"

REXCVAR_DECLARE(int32_t, ufc3_native_off);
REXCVAR_DEFINE_BOOL(ufc3_native_writeback_gone, false, "ufc3",
                    "Dev: also write a remembered resolve to guest memory when that memory has since "
                    "gone to another guest allocation (pages still unchanged only). The CAF editor's "
                    "face needs pixels to survive that (run nrb4c shows a face with it, wrong colours; "
                    "without it the face is black), but it also lands old pixels in new textures");
REXCVAR_DEFINE_INT32(ufc3_native_wb_dump, 0, "ufc3",
                     "Dev (NR-10g): dump this many resolve write-back jobs (level 0, 512 px or wider) to "
                     "nr10_dump beside the log file: wbNNNN_host.bin = the host readback rows, "
                     "wbNNNN_mem.bin = the guest pages as written, wbNNNN.txt = the sizes");

namespace bd::gpu {

namespace {

namespace tu = rex::graphics::texture_util;
namespace xe = rex::graphics::xenos;
namespace rg = rex::graphics;

constexpr u32 kPage = 4096;
constexpr u32 kPhysicalBytes = 0x20000000u;
constexpr int32_t kOffBit = 4096;
constexpr size_t kMaxRectsPerEntry = 64;  // past this a target's new rect replaces its bounding box
constexpr uint64_t kBufferKeepBytes = 64u * 1024 * 1024;

// Where one mip level (or cube face) of a resolve destination sits, read the way
// native_texture_mirror.cpp reads it: a mirror through the same header gets these bytes back.
struct Level {
  u32 phys = 0;
  u32 guest = 0;  // the same byte as the header addresses it
  u32 width = 0, height = 0;
  u32 pitch_blocks = 0, row_pitch_bytes = 0;
  u32 extent = 0;
  i32 off_x = 0, off_y = 0;
  u32 bpb = 0, bpb_log2 = 0;
  bool tiled = true;
  xe::TextureFormat format = xe::TextureFormat::k_8_8_8_8;
  xe::Endian endian = xe::Endian::kNone;
  u32 swizzle = 0;
};

// One resolved rectangle, guest pixels of the level; [first, end) = the physical bytes it can touch.
struct Rect {
  i32 x0, y0, x1, y1;
  u32 first, end;
  u64 seq;
  // What each page of [first, end) held when the resolve was remembered. The game frees and
  // re-allocates (or re-protects) texture memory between a resolve and its next use, which drops
  // the watch without a fault: a page still holding these bytes has had no newer writer, so the
  // resolve's pixels still belong there.
  std::vector<u64> page_hash;
  // The guest allocation around the first byte then (probe line).
  u32 alloc_base = 0, alloc_size = 0, alloc_protect = 0, alloc_state = 0;
};

struct Entry {
  GuestTexture *host = nullptr;
  u32 level = 0, face = 0;
  Level L;
  std::vector<Rect> rects;
};

// Guest formats a resolve can leave, and the size of the host texel (ConvertGuestFormat). k_24_8 is
// left out: no CPU reader is known, and its host texture is a converted float.
bool HostTexelBytes(xe::TextureFormat format, u32 guest_bpb, u32 &host_bytes) {
  switch (format) {
  case xe::TextureFormat::k_4_4_4_4:
  case xe::TextureFormat::k_5_6_5:
  case xe::TextureFormat::k_1_5_5_5:
    host_bytes = 4;  // the host target is RGBA8
    return true;
  case xe::TextureFormat::k_8:
  case xe::TextureFormat::k_8_A:
  case xe::TextureFormat::k_8_B:
  case xe::TextureFormat::k_8_8:
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
  case xe::TextureFormat::k_16_16:
  case xe::TextureFormat::k_16_FLOAT:
  case xe::TextureFormat::k_16_16_FLOAT:
  case xe::TextureFormat::k_16_16_16_16:
  case xe::TextureFormat::k_16_16_16_16_FLOAT:
  case xe::TextureFormat::k_32_FLOAT:
  case xe::TextureFormat::k_32_32_32_32_FLOAT:
    host_bytes = guest_bpb;
    return true;
  default:
    return false;
  }
}

bool MakeLevel(const xe::xe_gpu_texture_fetch_t &fetch, u32 level, u32 face, Level &L) {
  if (fetch.dimension != xe::DataDimension::k2DOrStacked &&
      fetch.dimension != xe::DataDimension::kCube)
    return false;
  const rg::FormatInfo *info = rg::FormatInfo::Get(fetch.format);
  if (!info || info->block_width != 1 || info->block_height != 1)
    return false;
  const bool cube = fetch.dimension == xe::DataDimension::kCube;
  const u32 width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
  const u32 mip_max = std::min<u32>(fetch.mip_max_level, rex::log2_floor(std::max(width, height)));
  if (level > mip_max || face >= (cube ? 6u : 1u) || (level && face))
    return false;
  L.width = std::max(width >> level, 1u);
  L.height = std::max(height >> level, 1u);
  L.bpb = info->bytes_per_block();
  L.bpb_log2 = rex::log2_floor(L.bpb);
  L.tiled = fetch.tiled != 0;
  L.format = fetch.format;
  L.endian = fetch.endianness;
  L.swizzle = fetch.swizzle;
  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      cube ? xe::DataDimension::kCube : xe::DataDimension::k2DOrStacked, fetch.pitch, width, height,
      cube ? 6u : 1u, L.tiled, fetch.format, fetch.packed_mips != 0u, /*has_base=*/true, mip_max);
  u32 px = 0, py = 0, pz = 0;
  if (level == 0) {
    const u32 aligned_w = rex::align(width, 32u), aligned_h = rex::align(height, 32u);
    L.pitch_blocks = aligned_w;
    L.row_pitch_bytes = layout.base.row_pitch_bytes;
    const u32 stride = L.tiled ? aligned_w * aligned_h * L.bpb : layout.base.array_slice_stride_bytes;
    L.extent = L.tiled ? aligned_w * aligned_h * L.bpb
                       : (cube ? layout.base.array_slice_data_extent_bytes
                               : layout.base.level_data_extent_bytes);
    L.guest = (u32(fetch.base_address) << 12) + face * stride;
    L.phys = SvrGpuAddress(u32(fetch.base_address) << 12) + face * stride;
    if (fetch.packed_mips)
      tu::GetPackedMipOffset(width, height, 1u, fetch.format, 0u, px, py, pz);
  } else {
    if (!fetch.mip_address || layout.packed_level == 0u)
      return false;
    const bool packed = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    const u32 storage = packed ? layout.packed_level : level;
    const tu::TextureGuestLayout::Level &sl = layout.mips[storage];
    L.pitch_blocks = sl.row_pitch_bytes / L.bpb;
    L.row_pitch_bytes = sl.row_pitch_bytes;
    L.extent = sl.level_data_extent_bytes;
    L.guest = (u32(fetch.mip_address) << 12) + layout.mip_offsets_bytes[storage];
    L.phys = SvrGpuAddress(u32(fetch.mip_address) << 12) + layout.mip_offsets_bytes[storage];
    if (packed)
      tu::GetPackedMipOffset(width, height, 1u, fetch.format, level, px, py, pz);
  }
  L.off_x = i32(px);
  L.off_y = i32(py);
  return true;
}

inline i32 TexelOffset(const Level &L, i32 x, i32 y) {
  if (L.tiled)
    return tu::GetTiledOffset2D(L.off_x + x, L.off_y + y, L.pitch_blocks, L.bpb_log2);
  return (L.off_y + y) * i32(L.row_pitch_bytes) + (L.off_x + x) * i32(L.bpb);
}

// The physical bytes a rectangle can touch: tiled texels of a rectangle aren't contiguous.
void RectBounds(const Level &L, Rect &r) {
  u32 lo, hi;
  if (L.tiled) {
    lo = tu::GetTiledAddressLowerBound2D(u32(L.off_x + r.x0), u32(L.off_y + r.y0), L.pitch_blocks,
                                         L.bpb_log2);
    hi = tu::GetTiledAddressUpperBound2D(u32(L.off_x + r.x1), u32(L.off_y + r.y1), L.pitch_blocks,
                                         L.bpb_log2);
  } else {
    lo = u32(TexelOffset(L, r.x0, r.y0));
    hi = u32(TexelOffset(L, r.x1 - 1, r.y1 - 1)) + L.bpb;
  }
  hi = std::min(hi, L.extent);
  r.first = std::min(L.phys + lo, kPhysicalBytes);
  r.end = u32(std::min<uint64_t>(uint64_t(L.phys) + hi, kPhysicalBytes));
}

// Host texel -> the texel in GPU byte order (before the endian swap). The host target holds the
// logical colour; a guest header's fetch swizzle says which stored component gives each result
// (ZYXW = 0x60A on k_8_8_8_8, so x = blue). Targets of one or two channels keep stored order on
// the host (ApplyResolveTargetSwizzle), so they copy as they are.
inline void ToStored(const Level &L, const u8 *h, u8 *out) {
  u8 c[4];
  switch (L.format) {
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
  case xe::TextureFormat::k_4_4_4_4:
  case xe::TextureFormat::k_5_6_5:
  case xe::TextureFormat::k_1_5_5_5:
    std::memcpy(c, h, 4);
    for (u32 j = 0; j < 4; ++j) {
      const u32 from = (L.swizzle >> (3 * j)) & 7;
      if (from < 4)
        c[from] = h[j];
    }
    break;
  default:
    std::memcpy(out, h, L.bpb);
    return;
  }
  u32 v = 0;
  switch (L.format) {
  case xe::TextureFormat::k_4_4_4_4:
    v = ((u32(c[0]) * 15 + 127) / 255) | (((u32(c[1]) * 15 + 127) / 255) << 4) |
        (((u32(c[2]) * 15 + 127) / 255) << 8) | (((u32(c[3]) * 15 + 127) / 255) << 12);
    break;
  case xe::TextureFormat::k_5_6_5:
    v = ((u32(c[0]) * 31 + 127) / 255) | (((u32(c[1]) * 63 + 127) / 255) << 5) |
        (((u32(c[2]) * 31 + 127) / 255) << 11);
    break;
  case xe::TextureFormat::k_1_5_5_5:
    v = ((u32(c[0]) * 31 + 127) / 255) | (((u32(c[1]) * 31 + 127) / 255) << 5) |
        (((u32(c[2]) * 31 + 127) / 255) << 10) | (c[3] >= 128 ? 0x8000u : 0u);
    break;
  default:
    std::memcpy(out, c, 4);
    return;
  }
  out[0] = u8(v);
  out[1] = u8(v >> 8);
}

// The inverse of ToStored: a texel in GPU byte order (after the endian swap) -> the host texel.
inline void FromStored(const Level &L, const u8 *stored, u8 *h) {
  u8 c[4];
  u32 v = 0;
  switch (L.format) {
  case xe::TextureFormat::k_8_8_8_8:
  case xe::TextureFormat::k_8_8_8_8_A:
    std::memcpy(c, stored, 4);
    break;
  case xe::TextureFormat::k_4_4_4_4:
    v = stored[0] | (u32(stored[1]) << 8);
    c[0] = u8((v & 15) * 17);
    c[1] = u8(((v >> 4) & 15) * 17);
    c[2] = u8(((v >> 8) & 15) * 17);
    c[3] = u8(((v >> 12) & 15) * 17);
    break;
  case xe::TextureFormat::k_5_6_5:
    v = stored[0] | (u32(stored[1]) << 8);
    c[0] = u8(((v & 31) * 255 + 15) / 31);
    c[1] = u8((((v >> 5) & 63) * 255 + 31) / 63);
    c[2] = u8((((v >> 11) & 31) * 255 + 15) / 31);
    c[3] = 255;
    break;
  case xe::TextureFormat::k_1_5_5_5:
    v = stored[0] | (u32(stored[1]) << 8);
    c[0] = u8(((v & 31) * 255 + 15) / 31);
    c[1] = u8((((v >> 5) & 31) * 255 + 15) / 31);
    c[2] = u8((((v >> 10) & 31) * 255 + 15) / 31);
    c[3] = (v & 0x8000u) ? 255 : 0;
    break;
  default:
    std::memcpy(h, stored, L.bpb);
    return;
  }
  for (u32 j = 0; j < 4; ++j) {
    const u32 from = (L.swizzle >> (3 * j)) & 7;
    h[j] = from < 4 ? c[from] : (from == 5 ? 255 : 0);
  }
}

inline void SwapEndian(xe::Endian endian, u8 *p, u32 n) {
  switch (endian) {
  case xe::Endian::k8in16:
    for (u32 i = 0; i + 2 <= n; i += 2)
      std::swap(p[i], p[i + 1]);
    break;
  case xe::Endian::k8in32:
    for (u32 i = 0; i + 4 <= n; i += 4) {
      std::swap(p[i], p[i + 3]);
      std::swap(p[i + 1], p[i + 2]);
    }
    break;
  case xe::Endian::k16in32:
    for (u32 i = 0; i + 4 <= n; i += 4) {
      std::swap(p[i], p[i + 2]);
      std::swap(p[i + 1], p[i + 3]);
    }
    break;
  default:
    break;
  }
}

// --- What is outstanding -----------------------------------------------------------------------

std::atomic<u64> g_pending_pages[(kPhysicalBytes / kPage) / 64];

void SetPending(u32 first, u32 end) {
  for (u32 page = first / kPage; page < (end + kPage - 1) / kPage; ++page)
    g_pending_pages[page / 64].fetch_or(u64(1) << (page & 63), std::memory_order_relaxed);
}

void ClearPending(u32 first, u32 end) {
  for (u32 page = first / kPage; page < (end + kPage - 1) / kPage; ++page)
    g_pending_pages[page / 64].fetch_and(~(u64(1) << (page & 63)), std::memory_order_relaxed);
}

// Leaf lock: nothing else is taken while it's held.
std::mutex g_list_mutex;
std::unordered_map<u64, Entry> g_entries;
u64 g_seq = 0;

rex::memory::Memory *Mem() { return REX_KERNEL_MEMORY(); }

void HashPages(Rect &r) {
  const u32 span_first = r.first & ~(kPage - 1);
  const u32 pages = (u32(rex::align<uint64_t>(r.end, kPage)) - span_first) / kPage;
  r.page_hash.resize(pages);
  const u8 *base = Mem()->TranslatePhysical<const u8 *>(span_first);
  for (u32 i = 0; i < pages; ++i)
    r.page_hash[i] = XXH3_64bits(base + size_t(i) * kPage, kPage);
}

// The guest heap's allocation around a byte of a level, for the probe.
void QueryAllocation(const Level &L, u32 physical, u32 &base, u32 &size, u32 &protect, u32 &state) {
  base = size = protect = state = 0;
  const u32 guest = L.guest + (physical - L.phys);
  rex::memory::HeapAllocationInfo info{};
  if (auto *heap = Mem()->LookupHeap(guest); heap && heap->QueryRegionInfo(guest, &info)) {
    base = info.allocation_base;
    size = info.allocation_size;
    protect = info.protect;
    state = info.state;
  }
}

// --- Serving ------------------------------------------------------------------------------------

struct Job {
  GuestTexture *host;
  u32 level, face;
  Level L;
  Rect r;
  plume::RenderBox box{};
  u32 host_w = 0, host_h = 0, host_bytes = 0, row_pitch = 0;
  u64 offset = 0;
};

// Order: g_serve_mutex, then state().mutex; then, with state().mutex released, the global lock.
std::mutex g_serve_mutex;
std::unique_ptr<plume::RenderBuffer> g_buffer;
uint64_t g_buffer_bytes = 0;
u32 g_serves = 0;

void CopyToGuestRun(u8 *dest, const u8 *source, u32 length) {
  u32 offset = 0;
  while (offset < length) {
    u8 *const run = dest + offset;
    rex::memory::PageAccess access;
    size_t region = 0;
    if (!rex::memory::QueryProtect(run, region, access))
      return;
    const u32 run_length = u32(std::min<u64>(
        region - (reinterpret_cast<uintptr_t>(run) & uintptr_t(rex::memory::page_size() - 1)),
        length - offset));
    if (access == rex::memory::PageAccess::kReadWrite ||
        access == rex::memory::PageAccess::kExecuteReadWrite) {
      std::memcpy(run, source + offset, run_length);
    } else {
      rex::memory::PageAccess old_access;
      if (rex::memory::Protect(run, run_length, rex::memory::PageAccess::kReadWrite, &old_access)) {
        std::memcpy(run, source + offset, run_length);
        rex::memory::Protect(run, run_length, old_access);
      }
    }
    offset += run_length;
  }
}

// Host pixels -> one guest texel at scale above 1: the box the scaled rect rounding gives that guest
// pixel. Eight-bit channels are averaged, others take the first texel.
const u8 *HostSample(const Job &j, const u8 *mapped, i32 hx0, i32 hx1, i32 hy0, i32 hy1, u8 *box) {
  const u8 *first = mapped + j.offset + size_t(hy0) * j.row_pitch + size_t(hx0) * j.host_bytes;
  const plume::RenderFormat f = j.host->format;
  const bool bytes = f == plume::RenderFormat::R8G8B8A8_UNORM || f == plume::RenderFormat::R8_UNORM ||
                     f == plume::RenderFormat::R8G8_UNORM;
  if ((hx1 - hx0 == 1 && hy1 - hy0 == 1) || !bytes)
    return first;
  u32 sum[4] = {};
  const u32 count = u32(hx1 - hx0) * u32(hy1 - hy0);
  for (i32 y = hy0; y < hy1; ++y) {
    const u8 *row = mapped + j.offset + size_t(y) * j.row_pitch + size_t(hx0) * j.host_bytes;
    for (i32 x = hx0; x < hx1; ++x, row += j.host_bytes)
      for (u32 c = 0; c < j.host_bytes; ++c)
        sum[c] += row[c];
  }
  for (u32 c = 0; c < j.host_bytes; ++c)
    box[c] = u8((sum[c] + count / 2) / count);
  return box;
}

// NR-B3a: the pages of all jobs of one Serve, ascending. A job's span is consecutive pages, all of
// them in here, so it is consecutive slots from SlotOf(span_first).
struct SharedPages {
  std::vector<u32> page;
  std::vector<u8> temp;  // page.size() * kPage
  std::vector<u8> live;  // read-watched when paged in
  size_t SlotOf(u32 physical) const {
    return size_t(std::lower_bound(page.begin(), page.end(), physical) - page.begin());
  }
};

// Into guest memory, only on pages still watched: a page that lost its watch was given to something
// newer (a guest free or reallocation, a file read, an earlier CPU access) and keeps its bytes. A
// watched page has no guest access, so nothing can write it between the copy and the write.
// shared: the pages were read in by WriteJobsShared, which also writes them out.
// serve_texels: guest texels of all jobs of the Serve (NR-14b: a 5-rect career serve of rects under
// 128 K texels each ran on one core, 10-11 ms).
size_t WriteJob(const Job &j, const u8 *mapped_buffer, std::vector<u8> &temp, std::vector<u8> &rows,
                double times_ms[4], size_t serve_texels, SharedPages *shared = nullptr) {
  auto *memory = Mem();
  const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
  const auto w0 = std::chrono::steady_clock::now();
  // NR-14: a 4K career composite is 1.5 M guest texels from 56 MB of host rows (copy 19 ms, texels
  // 58 ms on one core): past 128 K texels in the Serve, each job of 64 K+ is split by rows over a
  // few threads (one per 32 K texels). Distinct texels of one level have distinct bytes, and jobs
  // still run one after another, so the result is the same bytes in any order.
  const size_t guest_texels = size_t(j.r.x1 - j.r.x0) * size_t(j.r.y1 - j.r.y0);
  const int32_t off = REXCVAR_GET(ufc3_native_off);
  const size_t workers =
      !(off & 2097152) && std::max(serve_texels, guest_texels) >= (128u << 10) && guest_texels >= (64u << 10)
          ? std::clamp<size_t>(std::min<size_t>(std::thread::hardware_concurrency() / 2,
                                                guest_texels >> 15),
                               1, 8)
          : 1;
  const auto split = [workers](i32 begin, i32 end, auto &&body) {
    if (workers <= 1) {
      body(begin, end);
      return;
    }
    const i32 per = (end - begin + i32(workers) - 1) / i32(workers);
    std::vector<std::thread> pool;
    for (size_t w = 1; w < workers; ++w) {
      const i32 b = std::min(end, begin + i32(w) * per), e = std::min(end, b + per);
      if (b < e)
        pool.emplace_back([&body, b, e] { body(b, e); });
    }
    body(begin, std::min(end, begin + per));
    for (std::thread &t : pool)
      t.join();
  };
  // NR-14b: the readback heap is cached CPU memory, so the texels are read straight from the mapped
  // rows (each band reads only its own). The copy (~10 ms of a 4K composite, plus zero-filling a
  // fresh 56 MB vector each Serve) stays behind ufc3_native_off bit 4194304.
  const u8 *mapped = mapped_buffer;
  if (off & 4194304) {
    rows.resize(size_t(j.row_pitch) * j.host_h);
    split(0, i32(j.host_h), [&](i32 y0, i32 y1) {
      std::memcpy(rows.data() + size_t(y0) * j.row_pitch,
                  mapped_buffer + j.offset + size_t(y0) * j.row_pitch, size_t(y1 - y0) * j.row_pitch);
    });
    mapped = rows.data() - j.offset;
  }
  const auto wr = std::chrono::steady_clock::now();
  const Level &L = j.L;
  const u32 span_first = j.r.first & ~(kPage - 1);
  const u32 span_end = u32(rex::align<uint64_t>(j.r.end, kPage));
  const u32 pages = (span_end - span_first) / kPage;
  u8 *page_bytes;
  const u8 *live;
  std::vector<u8> own_live;
  if (shared) {
    const size_t slot = shared->SlotOf(span_first);
    page_bytes = shared->temp.data() + slot * kPage;
    live = shared->live.data() + slot;
  } else {
    temp.resize(size_t(pages) * kPage);
    own_live.assign(pages, 0);
    for (u32 i = 0; i < pages; ++i) {
      const u32 page = span_first + i * kPage;
      auto global = rex::thread::global_critical_region::AcquireDirect();
      if (memory->IsPhysicalMemoryReadWatched(page)) {
        std::memcpy(temp.data() + size_t(i) * kPage, memory->TranslatePhysical<const u8 *>(page), kPage);
        own_live[i] = 1;
      }
    }
    page_bytes = temp.data();
    live = own_live.data();
  }
  const auto w1 = std::chrono::steady_clock::now();
  const i32 host_x0 = j.box.left, host_y0 = j.box.top;
  std::vector<i32> column_edges(size_t(j.r.x1 - j.r.x0) + 1);
  for (i32 gx = j.r.x0; gx <= j.r.x1; ++gx)
    column_edges[size_t(gx - j.r.x0)] = SvrScalePx(gx, j.host) - host_x0;
  split(j.r.y0, j.r.y1, [&](i32 band_y0, i32 band_y1) {
    for (i32 gy = band_y0; gy < band_y1; ++gy) {
      const i32 hy0 = std::clamp(SvrScalePx(gy, j.host) - host_y0, 0, i32(j.host_h) - 1);
      const i32 hy1 = std::clamp(SvrScalePx(gy + 1, j.host) - host_y0, hy0 + 1, i32(j.host_h));
      for (i32 gx = j.r.x0; gx < j.r.x1; ++gx) {
        const i32 hx0 = std::clamp(column_edges[size_t(gx - j.r.x0)], 0, i32(j.host_w) - 1);
        const i32 hx1 = std::clamp(column_edges[size_t(gx - j.r.x0) + 1], hx0 + 1, i32(j.host_w));
        const i32 so = TexelOffset(L, gx, gy);
        if (so < 0 || u32(so) + L.bpb > L.extent)
          continue;
        const u32 at = L.phys + u32(so);
        if (at < span_first || at + L.bpb > span_end || !live[(at - span_first) / kPage])
          continue;
        u8 box[16], texel[16];
        ToStored(L, HostSample(j, mapped, hx0, hx1, hy0, hy1, box), texel);
        SwapEndian(L.endian, texel, L.bpb);
        std::memcpy(page_bytes + (at - span_first), texel, L.bpb);
      }
    }
  });
  const auto w2 = std::chrono::steady_clock::now();
  // NR-10g dump: what the GPU rendered (host rows) and what goes into memory for it (temp), to tell
  // a wrong render from a wrong conversion offline (tools/nr10g_wb_dump.py).
  {
    static std::atomic<u32> s_dumped{0};
    const u32 want = u32(std::max(0, REXCVAR_GET(ufc3_native_wb_dump)));
    if (s_dumped.load(std::memory_order_relaxed) < want && j.level == 0 && j.host_w >= 512) {
      const u32 n = s_dumped.fetch_add(1, std::memory_order_relaxed);
      if (n < want) {
        const std::string stem = fmt::format("{}\\wb{:04}", UfcNr10DumpDir(), n);
        const auto put = [](const std::string &path, const void *data, size_t size) {
          if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(data, 1, size, f);
            std::fclose(f);
          }
        };
        put(stem + "_host.bin", mapped + j.offset, size_t(j.row_pitch) * j.host_h);
        put(stem + "_mem.bin", page_bytes, size_t(pages) * kPage);
        const std::string about = fmt::format(
            "host_w {} host_h {} row_pitch {} host_bytes {} host_format {} box {},{}-{},{}\n"
            "level_w {} level_h {} bpb {} format {} endian {} swizzle {:X} tiled {} phys {:08X} extent "
            "{} pitch_blocks {}\n"
            "rect {},{}-{},{} first {:08X} end {:08X} span_first {:08X} pages {} level {} seq {}\n",
            j.host_w, j.host_h, j.row_pitch, j.host_bytes, int(j.host->format), j.box.left, j.box.top,
            j.box.right, j.box.bottom, L.width, L.height, L.bpb, int(L.format), int(L.endian),
            L.swizzle, L.tiled ? 1 : 0, L.phys, L.extent, L.pitch_blocks, j.r.x0, j.r.y0, j.r.x1,
            j.r.y1, j.r.first, j.r.end, span_first, pages, j.level, j.r.seq);
        put(stem + ".txt", about.data(), about.size());
        BD_INFO("[nr10] wb dump #{} -> {}: host {}x{} level {}x{} rect {},{}-{},{} at {:08X}", n, stem,
                j.host_w, j.host_h, L.width, L.height, j.r.x0, j.r.y0, j.r.x1, j.r.y1, L.phys);
      }
    }
  }
  times_ms[1] += ms(wr - w0);
  times_ms[2] += ms(w2 - w1);
  if (shared)
    return 0;
  // Back in runs under one hold of the global lock: the host protection query in CopyToGuestRun
  // walks the whole 512 MB view, 0.5 ms a call, which was 486 of 505 ms page by page.
  size_t written = 0;
  {
    auto global = rex::thread::global_critical_region::AcquireDirect();
    u32 run_first = UINT32_MAX;
    const auto flush = [&](u32 end_page) {
      if (run_first == UINT32_MAX)
        return;
      const u32 bytes = (end_page - run_first) * kPage;
      CopyToGuestRun(memory->TranslatePhysical<u8 *>(span_first + run_first * kPage),
                     page_bytes + size_t(run_first) * kPage, bytes);
      written += bytes;
      run_first = UINT32_MAX;
    };
    for (u32 i = 0; i < pages; ++i) {
      if (live[i] && memory->IsPhysicalMemoryReadWatched(span_first + i * kPage)) {
        if (run_first == UINT32_MAX)
          run_first = i;
      } else {
        flush(i);
      }
    }
    flush(pages);
  }
  const auto w3 = std::chrono::steady_clock::now();
  times_ms[0] += ms(w1 - wr);
  times_ms[3] += ms(w3 - w2);
  return written;
}

// NR-B3a: a bind write-back has 61-90 rects over a few shared pages, and every CopyToGuestRun costs
// a protection query (0.25-0.3 ms). All jobs convert into one copy of their pages, in seq order as
// before (a later job sees the earlier ones' texels, as the old page-in after each write gave it),
// and each run of consecutive pages goes back once: same bytes in guest memory.
size_t WriteJobsShared(const std::vector<Job> &jobs, const u8 *mapped, double times_ms[4],
                       size_t serve_texels) {
  auto *memory = Mem();
  const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
  const auto w0 = std::chrono::steady_clock::now();
  SharedPages sp;
  for (const Job &j : jobs)
    for (u32 p = j.r.first & ~(kPage - 1); p < j.r.end; p += kPage)
      sp.page.push_back(p);
  std::sort(sp.page.begin(), sp.page.end());
  sp.page.erase(std::unique(sp.page.begin(), sp.page.end()), sp.page.end());
  sp.temp.resize(sp.page.size() * kPage);
  sp.live.assign(sp.page.size(), 0);
  for (size_t i = 0; i < sp.page.size(); ++i) {
    auto global = rex::thread::global_critical_region::AcquireDirect();
    if (memory->IsPhysicalMemoryReadWatched(sp.page[i])) {
      std::memcpy(sp.temp.data() + i * kPage, memory->TranslatePhysical<const u8 *>(sp.page[i]), kPage);
      sp.live[i] = 1;
    }
  }
  const auto w1 = std::chrono::steady_clock::now();
  times_ms[0] += ms(w1 - w0);
  std::vector<u8> unused, rows;
  for (const Job &j : jobs)
    WriteJob(j, mapped, unused, rows, times_ms, serve_texels, &sp);
  const auto w2 = std::chrono::steady_clock::now();
  size_t written = 0;
  {
    auto global = rex::thread::global_critical_region::AcquireDirect();
    size_t run = SIZE_MAX;
    const auto flush = [&](size_t end) {
      if (run == SIZE_MAX)
        return;
      const u32 bytes = u32(end - run) * kPage;
      CopyToGuestRun(memory->TranslatePhysical<u8 *>(sp.page[run]), sp.temp.data() + run * kPage, bytes);
      written += bytes;
      run = SIZE_MAX;
    };
    for (size_t i = 0; i < sp.page.size(); ++i) {
      if (sp.live[i] && memory->IsPhysicalMemoryReadWatched(sp.page[i])) {
        if (run != SIZE_MAX && sp.page[i] != sp.page[i - 1] + kPage)
          flush(i);
        if (run == SIZE_MAX)
          run = i;
      } else {
        flush(i);
      }
    }
    flush(sp.page.size());
  }
  times_ms[3] += ms(std::chrono::steady_clock::now() - w2);
  return written;
}

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

// keep: rects this host texture resolved stay pending (NR-12a: a target's fill leaves them out).
void Serve(u32 first, u32 end, const char *why, const GuestTexture *keep = nullptr) {
  std::lock_guard serve(g_serve_mutex);
  const auto t0 = Clock::now();
  auto *memory = Mem();
  first &= ~(kPage - 1);
  end = u32(std::min<uint64_t>(rex::align<uint64_t>(end, kPage), kPhysicalBytes));
  std::vector<std::pair<u32, u32>> ranges{{first, end}};
  std::vector<Job> jobs;
  {
    std::lock_guard list(g_list_mutex);
    // Everything sharing a page with what's wanted: those pages come back as one, like emulated.
    for (bool grew = true; grew;) {
      grew = false;
      for (auto &[key, entry] : g_entries) {
        if (keep && entry.host == keep)
          continue;
        for (size_t i = 0; i < entry.rects.size();) {
          const Rect r = entry.rects[i];
          const u32 r_first = r.first & ~(kPage - 1);
          const u32 r_end = u32(rex::align<uint64_t>(r.end, kPage));
          const bool hit = std::any_of(ranges.begin(), ranges.end(), [&](const auto &range) {
            return r_first < range.second && r_end > range.first;
          });
          if (!hit) {
            ++i;
            continue;
          }
          jobs.push_back(Job{entry.host, entry.level, entry.face, entry.L, r});
          ranges.emplace_back(r_first, r_end);
          entry.rects.erase(entry.rects.begin() + i);
          grew = true;
        }
      }
    }
    for (auto it = g_entries.begin(); it != g_entries.end();)
      it = it->second.rects.empty() ? g_entries.erase(it) : std::next(it);
  }
  const size_t matched = jobs.size();
  // Pages that lost their watch without a fault (a guest free, reallocation or re-protection): hidden
  // again first, so nothing changes under the compare, then kept only if they still hold what they
  // held at the resolve. A page with other bytes went to something newer and is given back.
  size_t revived = 0, changed = 0, gone = 0;
  for (const Job &j : jobs) {
    const u32 span_first = j.r.first & ~(kPage - 1);
    const u32 span_end = u32(rex::align<uint64_t>(j.r.end, kPage));
    const u32 pages = (span_end - span_first) / kPage;
    if (j.r.page_hash.size() != pages)
      continue;
    std::vector<u8> was(pages, 0);
    u32 watched = 0;
    {
      auto global = rex::thread::global_critical_region::AcquireDirect();
      for (u32 i = 0; i < pages; ++i)
        watched += was[i] = memory->IsPhysicalMemoryReadWatched(span_first + i * kPage);
    }
    if (watched == pages)
      continue;
    // Memory that now belongs to another allocation held a texture that is gone: whatever lives
    // there now fills it itself, and old pixels landing in its untouched pages would only be wrong.
    u32 base_now, size_now, protect_now, state_now;
    QueryAllocation(j.L, j.r.first, base_now, size_now, protect_now, state_now);
    const bool same_allocation = base_now == j.r.alloc_base && size_now == j.r.alloc_size && size_now;
    if (!same_allocation && !REXCVAR_GET(ufc3_native_writeback_gone)) {
      ++gone;
      static std::atomic<u32> s_gone_logs{0};
      if (UfcNr10On() && s_gone_logs.fetch_add(1, std::memory_order_relaxed) < 24)
        BD_INFO("[nr10] watch lost {:08X}-{:08X} level {} rect {},{}-{},{}: allocation then {:08X}+{:X}, "
                "now {:08X}+{:X}: texture gone, not written",
                j.r.first, j.r.end, j.level, j.r.x0, j.r.y0, j.r.x1, j.r.y1, j.r.alloc_base,
                j.r.alloc_size, base_now, size_now);
      continue;
    }
    memory->EnablePhysicalMemoryReadWatch(span_first, span_end - span_first);
    const u8 *base = memory->TranslatePhysical<const u8 *>(span_first);
    u32 job_revived = 0, job_changed = 0, give_first = UINT32_MAX;
    const auto give_back = [&](u32 end_page) {
      if (give_first != UINT32_MAX)
        memory->DisablePhysicalMemoryReadWatch(span_first + give_first * kPage,
                                               (end_page - give_first) * kPage);
      give_first = UINT32_MAX;
    };
    for (u32 i = 0; i < pages; ++i) {
      bool keep = was[i] != 0;
      if (!keep) {
        bool hidden;
        {
          auto global = rex::thread::global_critical_region::AcquireDirect();
          hidden = memory->IsPhysicalMemoryReadWatched(span_first + i * kPage);
        }
        if (hidden) {
          keep = XXH3_64bits(base + size_t(i) * kPage, kPage) == j.r.page_hash[i];
          ++(keep ? job_revived : job_changed);
          if (!keep && give_first == UINT32_MAX)
            give_first = i;
        }
      }
      if (keep)
        give_back(i);
    }
    give_back(pages);
    revived += job_revived;
    changed += job_changed;
    static std::atomic<u32> s_logs{0};
    if (UfcNr10On() && s_logs.fetch_add(1, std::memory_order_relaxed) < 96) {
      BD_INFO("[nr10] watch lost {:08X}-{:08X} level {} rect {},{}-{},{}: {} of {} pages still watched, "
              "{} unchanged and hidden again, {} changed; allocation then {:08X}+{:X} protect {:X} "
              "state {:X}, now {:08X}+{:X} protect {:X} state {:X}",
              j.r.first, j.r.end, j.level, j.r.x0, j.r.y0, j.r.x1, j.r.y1, watched, pages,
              job_revived, job_changed, j.r.alloc_base, j.r.alloc_size, j.r.alloc_protect,
              j.r.alloc_state, base_now, size_now, protect_now, state_now);
    }
  }
  // Rects with no watched page left lost out to something newer: no need to touch the GPU for them.
  {
    auto global = rex::thread::global_critical_region::AcquireDirect();
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                              [&](const Job &j) {
                                for (u32 page = j.r.first & ~(kPage - 1); page < j.r.end; page += kPage)
                                  if (memory->IsPhysicalMemoryReadWatched(page))
                                    return false;
                                return true;
                              }),
               jobs.end());
  }
  std::sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) { return a.r.seq < b.r.seq; });

  size_t read_bytes = 0, written = 0;
  double write_ms[4] = {};  // pages in, rows, texels, pages out
  const auto t1 = Clock::now();
  const u8 *mapped = nullptr;
  u32 splits = 0;      // NR-14c: lists handed to the GPU earlier in this frame
  double open_ms = 0;  // recording the flush had to wait for (since the last split or open)
  if (!jobs.empty()) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const u32 cur = s.frame.load(std::memory_order_relaxed);
    if (s.ready && !s.shutting_down.load(std::memory_order_acquire)) {
      if (!s.command_list_open && s.command_list_submitted[cur]) {
        s.queue->waitForCommandFence(s.fences[cur].get());
        s.command_list_submitted[cur] = false;
      }
      BeginCommandList(s);
    }
    if (s.ready && s.command_list_open) {
      std::vector<GuestTexture *> hosts;
      uint64_t total = 0;
      for (Job &j : jobs) {
        MaterializeInboundLocked(s, j.host);
        if (std::find(hosts.begin(), hosts.end(), j.host) == hosts.end())
          hosts.push_back(j.host);
        const i32 level_w = i32(std::max(1u, j.host->width >> j.level));
        const i32 level_h = i32(std::max(1u, j.host->height >> j.level));
        const i32 x0 = std::clamp(SvrScalePx(j.r.x0, j.host), 0, level_w - 1);
        const i32 y0 = std::clamp(SvrScalePx(j.r.y0, j.host), 0, level_h - 1);
        const i32 x1 = std::clamp(SvrScalePx(j.r.x1, j.host), x0 + 1, level_w);
        const i32 y1 = std::clamp(SvrScalePx(j.r.y1, j.host), y0 + 1, level_h);
        j.box = plume::RenderBox(x0, y0, x1, y1);
        j.host_w = u32(x1 - x0);
        j.host_h = u32(y1 - y0);
        j.host_bytes = plume::RenderFormatSize(j.host->format);
        j.row_pitch = rex::align(j.host_w * j.host_bytes, 256u);
        j.offset = total;
        total += rex::align<uint64_t>(uint64_t(j.row_pitch) * j.host_h, 512u);
      }
      if (g_buffer_bytes < total || !g_buffer) {
        g_buffer = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(total),
                                    "resolve-writeback");
        g_buffer_bytes = g_buffer ? total : 0;
      }
      if (g_buffer) {
        std::vector<plume::RenderTextureBarrier> pre, post;
        for (GuestTexture *host : hosts) {
          pre.emplace_back(host->texture, plume::RenderTextureLayout::COPY_SOURCE);
          post.emplace_back(host->texture, plume::RenderTextureLayout::SHADER_READ);
        }
        s.draw_framebuffer_bound = false;
        s.command_list->barriers(plume::RenderBarrierStage::COPY, pre.data(), u32(pre.size()));
        for (const Job &j : jobs) {
          s.command_list->copyTextureRegion(
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  g_buffer.get(), j.host->format, j.host_w, j.host_h, 1, j.row_pitch / j.host_bytes,
                  j.offset),
              plume::RenderTextureCopyLocation::Subresource(j.host->texture, j.level, j.face), 0, 0, 0,
              &j.box);
          read_bytes += uint64_t(j.row_pitch) * j.host_h;
        }
        s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, post.data(), u32(post.size()));
        for (GuestTexture *host : hosts)
          host->layout = plume::RenderTextureLayout::SHADER_READ;
        splits = SvrListSplits();
        open_ms = SvrListOpenMs();
        if (SvrFlushAndWaitLocked(s))
          mapped = static_cast<const u8 *>(g_buffer->map());
      }
    }
  }
  const auto t2 = Clock::now();
  if (mapped) {
    size_t serve_texels = 0;
    for (const Job &j : jobs)
      serve_texels += size_t(j.r.x1 - j.r.x0) * size_t(j.r.y1 - j.r.y0);
    if (!(REXCVAR_GET(ufc3_native_off) & 1048576)) {
      written = WriteJobsShared(jobs, mapped, write_ms, serve_texels);
    } else {
      std::vector<u8> temp, rows;
      for (const Job &j : jobs)
        written += WriteJob(j, mapped, temp, rows, write_ms, serve_texels);
    }
    g_buffer->unmap();
  } else if (!jobs.empty()) {
    BD_WARN("native: resolve write-back of {} rects dropped: no GPU copy", jobs.size());
  }
  if (g_buffer && g_buffer_bytes > kBufferKeepBytes) {
    g_buffer.reset();  // no list refers to it: the copy above was waited for
    g_buffer_bytes = 0;
  }
  // Those pages are the guest's again. The mirrors over them are out of date.
  for (const auto &[range_first, range_end] : ranges) {
    ClearPending(range_first, range_end);
    memory->DisablePhysicalMemoryReadWatch(range_first, range_end - range_first);
    SvrMirrorsInvalidateRange(range_first, range_end - range_first);
  }
  // A resolve landing on these pages while this ran (a fault served for another thread): again.
  std::vector<std::pair<u32, u32>> again;
  {
    std::lock_guard list(g_list_mutex);
    for (const auto &[key, entry] : g_entries)
      for (const Rect &r : entry.rects)
        for (const auto &[range_first, range_end] : ranges)
          if (r.first < range_end && r.end > range_first) {
            again.emplace_back(r.first, r.end);
            break;
          }
  }
  for (const auto &[again_first, again_end] : again) {
    SetPending(again_first, again_end);
    memory->EnablePhysicalMemoryReadWatch(again_first & ~(kPage - 1),
                                          u32(rex::align<uint64_t>(again_end, kPage)) -
                                              (again_first & ~(kPage - 1)));
  }
  const u32 n = ++g_serves;
  if ((!jobs.empty() && n <= 64) || n % 64 == 0 || UfcNr10On())
    BD_INFO("[nr10] write-back #{} {}: {:08X}-{:08X}: {} rects ({} remembered, {} of textures gone, {} "
            "pages hidden again, {} changed), {} KB read back, {} KB written, gather {:.2f} ms, flush "
            "{:.2f} ms (list {:.2f} ms, {} splits), write {:.2f} ms (pages in {:.2f}, rows {:.2f}, texels "
            "{:.2f}, pages out {:.2f})",
            n, why, first, end, jobs.size(), matched, gone, revived, changed, read_bytes / 1024,
            written / 1024, Ms(t0, t1), Ms(t1, t2), open_ms, splits, Ms(t2, Clock::now()),
            write_ms[0], write_ms[1], write_ms[2], write_ms[3]);
}

// --- Faults -------------------------------------------------------------------------------------

// The fault handler runs on the thread that touched the page, which may be anywhere in guest code.
// The GPU work doesn't run there: it goes to this thread, like the emulated GPU thread.
std::mutex g_request_mutex;
std::condition_variable g_request_cv, g_served_cv;
std::vector<u32> g_requests;
u64 g_requested = 0, g_served = 0;
thread_local bool t_service_thread = false;

void ServiceThread() {
  t_service_thread = true;
  for (;;) {
    std::vector<u32> requests;
    u64 requested = 0;
    {
      std::unique_lock lock(g_request_mutex);
      g_request_cv.wait(lock, [] { return !g_requests.empty(); });
      requests.swap(g_requests);
      requested = g_requested;
    }
    for (u32 physical : requests)
      Serve(physical, physical + kPage, "cpu");
    {
      std::lock_guard lock(g_request_mutex);
      g_served = requested;
    }
    g_served_cv.notify_all();
  }
}

void GiveBack(u32 physical, const char *reason) {
  static std::atomic<u32> s_logs{0};
  if (s_logs.fetch_add(1, std::memory_order_relaxed) < 32)
    BD_WARN("[nr10] write-back skipped ({}): page {:08X} given back as it is", reason, physical);
  Mem()->DisablePhysicalMemoryReadWatch(physical & ~(kPage - 1), kPage);
}

// Memory::PhysicalMemoryDataProvider, replacing the null GPU's unused one. Called with the global
// lock held once (an interrupt holds it twice, and can't wait for anything).
bool Provider(void *, u32 physical, std::unique_lock<std::recursive_mutex> global) {
  static std::atomic<u32> s_calls{0};
  if (const u32 n = s_calls.fetch_add(1, std::memory_order_relaxed); n < 48 || n % 256 == 0)
    BD_INFO("[nr10] fault #{} on page {:08X}, thread {}{}", n + 1, physical & ~(kPage - 1),
            rex::thread::current_thread_id(), t_service_thread ? " (service thread)" : "");
  if (rex::runtime::FunctionDispatcher::IsInInterrupt()) {
    GiveBack(physical, "interrupt");
    return true;
  }
  if (state().mutex.HeldByThisThread() || t_service_thread) {
    GiveBack(physical, "renderer locked by this thread");
    return true;
  }
  if (global.owns_lock())
    global.unlock();
  std::unique_lock lock(g_request_mutex);
  g_requests.push_back(physical & ~(kPage - 1));
  const u64 ticket = ++g_requested;
  g_request_cv.notify_one();
  if (!g_served_cv.wait_for(lock, std::chrono::seconds(5), [&] { return g_served >= ticket; })) {
    lock.unlock();
    GiveBack(physical, "waited 5 s");
  }
  return true;
}

void InstallOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    Mem()->SetPhysicalMemoryDataProvider(Provider, nullptr);
    std::thread(ServiceThread).detach();
  });
}

}  // namespace

bool UfcWritebackOn() { return !(REXCVAR_GET(ufc3_native_off) & kOffBit); }

u32 UfcPhysicalOf(const u8 *pointer) {
  return u32(pointer - Mem()->TranslatePhysical<const u8 *>(0));
}

void UfcWriteGuestPhysical(u32 physical, const void *data, size_t size) {
  CopyToGuestRun(Mem()->TranslatePhysical<u8 *>(physical), static_cast<const u8 *>(data), u32(size));
}

bool UfcWritebackHolds(const GuestTexture *host) {
  // Serve copies rects out of the list and reads their host after letting go of it.
  std::lock_guard serve(g_serve_mutex);
  std::vector<std::pair<u64, u64>> rects;  // entry key, rect seq
  std::vector<std::pair<u32, u32>> spans;
  {
    std::lock_guard list(g_list_mutex);
    for (const auto &[key, entry] : g_entries)
      if (entry.host == host)
        for (const Rect &r : entry.rects) {
          rects.emplace_back(key, r.seq);
          spans.emplace_back(r.first & ~(kPage - 1), r.end);
        }
  }
  if (rects.empty())
    return false;
  auto *memory = Mem();
  std::vector<u8> lost(rects.size(), 1);
  bool held = false;
  {
    auto global = rex::thread::global_critical_region::AcquireDirect();
    for (size_t i = 0; i < spans.size(); ++i)
      for (u32 page = spans[i].first; page < spans[i].second && lost[i]; page += kPage)
        if (memory->IsPhysicalMemoryReadWatched(page))
          lost[i] = 0;
  }
  std::lock_guard list(g_list_mutex);
  for (size_t i = 0; i < rects.size(); ++i) {
    auto it = g_entries.find(rects[i].first);
    if (it == g_entries.end())
      continue;
    auto &list_rects = it->second.rects;
    if (lost[i])
      list_rects.erase(std::remove_if(list_rects.begin(), list_rects.end(),
                                      [&](const Rect &r) { return r.seq == rects[i].second; }),
                       list_rects.end());
    if (list_rects.empty())
      g_entries.erase(it);
    else
      held = true;
  }
  return held;
}

bool UfcResolvePending(u32 physical, size_t size) {
  if (!size || physical >= kPhysicalBytes)
    return false;
  const u32 first = physical / kPage;
  const u32 last = u32(std::min<uint64_t>(uint64_t(physical) + size - 1, kPhysicalBytes - 1)) / kPage;
  for (u32 word = first / 64; word <= last / 64; ++word) {
    u64 bits = g_pending_pages[word].load(std::memory_order_relaxed);
    if (word == first / 64)
      bits &= ~u64(0) << (first & 63);
    if (word == last / 64 && (last & 63) != 63)
      bits &= (u64(1) << ((last & 63) + 1)) - 1;
    if (bits)
      return true;
  }
  return false;
}

void UfcResolveToGuestPhysical(u32 physical, size_t size, const char *why,
                               const GuestTexture *keep) {
  if (!UfcWritebackOn() || !UfcResolvePending(physical, size))
    return;
  // Serve takes state().mutex: a caller already holding it would wait for itself.
  if (state().mutex.HeldByThisThread()) {
    static std::atomic<u32> s_logs{0};
    if (s_logs.fetch_add(1, std::memory_order_relaxed) < 32)
      BD_WARN("[nr10] write-back skipped (renderer locked by this thread, {}): {:08X} +{}", why,
              physical, size);
    return;
  }
  Serve(physical, u32(std::min<uint64_t>(uint64_t(physical) + size, kPhysicalBytes)), why, keep);
}

namespace {

// NR-12a: scratch textures for the GPU target fill, one per format, size and role (0 = the guest-size
// upload, 1 = a level-size stretch target), kept for the process (the same few sizes come back at
// every scene change). Past the cap a new size gets none and its fill takes the CPU scale.
constexpr u64 kFillScratchCap = 256ull << 20;
std::unordered_map<u64, GuestTexture *> g_fill_scratch;  // never freed: the device outlives them
u64 g_fill_scratch_bytes = 0;

// Caller holds state().mutex.
GuestTexture *FillScratchLocked(VideoState &s, plume::RenderFormat format, u32 width, u32 height,
                                u32 role) {
  if (!width || !height || width > 0xFFFF || height > 0xFFFF)
    return nullptr;
  const u64 key = u64(format) | (u64(role) << 8) | (u64(width) << 16) | (u64(height) << 40);
  if (auto it = g_fill_scratch.find(key); it != g_fill_scratch.end())
    return it->second;
  const u64 bytes = u64(width) * height * plume::RenderFormatSize(format);
  if (g_fill_scratch_bytes + bytes > kFillScratchCap)
    return nullptr;
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = false;
  auto holder = CreateHostTexture(s.device.get(), desc, "target-fill-scratch");
  if (!holder)
    return nullptr;
  auto *t = new GuestTexture();
  t->textureHolder = std::move(holder);
  t->texture = t->textureHolder.get();
  t->width = width;
  t->height = height;
  t->mipLevels = 1;
  t->format = format;
  t->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  g_fill_scratch.emplace(key, t);
  g_fill_scratch_bytes += bytes;
  BD_INFO("[nr12a] fill scratch {}x{} fmt {} role {} ({} KB, {} MB in all)", width, height,
          u32(format), role, bytes / 1024, g_fill_scratch_bytes >> 20);
  return t;
}

}  // namespace

bool UfcFillTargetFromGuest(u32 texture_va, GuestTexture *host, u32 levels,
                            const std::vector<UfcFillRect> *covered) {
  if (!host || !host->texture || !texture_va ||
      host->viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE ||
      IsDepthFormat(host->format))
    return false;
  const auto *texture = bd::mem::try_at<const D3DTexture>(texture_va);
  if (!texture)
    return false;
  xe::xe_gpu_texture_fetch_t fetch{};
  for (int i = 0; i < 6; ++i)
    (&fetch.dword_0)[i] = u32(texture->Format.dword[i]);
  if (fetch.type != xe::FetchConstantType::kTexture ||
      fetch.dimension != xe::DataDimension::k2DOrStacked)
    return false;

  // NR-12a: what a resolve already wrote is left out, so only the boxes no resolve covers are
  // converted and uploaded (bands between the rects' top/bottom edges, minus the rects spanning each).
  struct Box {
    u32 x0, y0, x1, y1;
    u32 row_pitch;
    u64 offset;
  };
  struct Plan {
    Level L;
    u32 level, host_w, host_h;
    std::vector<Box> boxes;
  };
  std::vector<Plan> plans;
  const u32 host_bytes = plume::RenderFormatSize(host->format);
  u64 total = 0, texels = 0;
  for (u32 level = 0; level < std::max(host->mipLevels, 1u); ++level) {
    if (level >= 32 || !(levels & (1u << level)))
      continue;
    Plan p{};
    p.level = level;
    u32 want = 0;
    if (!MakeLevel(fetch, level, 0, p.L))
      break;
    if (!HostTexelBytes(p.L.format, p.L.bpb, want) || want != host_bytes)
      return false;
    if (uint64_t(p.L.phys) + p.L.extent > kPhysicalBytes)
      break;
    p.host_w = std::max(1u, host->width >> level);
    p.host_h = std::max(1u, host->height >> level);
    std::vector<std::array<u32, 4>> rects;  // l, t, r, b clipped to the level
    if (covered) {
      for (const UfcFillRect &c : *covered) {
        if (c.level != level)
          continue;
        const u32 l = u32(std::clamp<i32>(c.left, 0, i32(p.host_w)));
        const u32 r = u32(std::clamp<i32>(c.right, 0, i32(p.host_w)));
        const u32 t = u32(std::clamp<i32>(c.top, 0, i32(p.host_h)));
        const u32 b = u32(std::clamp<i32>(c.bottom, 0, i32(p.host_h)));
        if (l < r && t < b)
          rects.push_back({l, t, r, b});
      }
    }
    std::vector<u32> edges{0, p.host_h};
    for (const auto &r : rects) {
      edges.push_back(r[1]);
      edges.push_back(r[3]);
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (size_t e = 0; e + 1 < edges.size(); ++e) {
      const u32 y0 = edges[e], y1 = edges[e + 1];
      std::vector<std::pair<u32, u32>> spans;  // covered x spans in this band
      for (const auto &r : rects)
        if (r[1] <= y0 && r[3] >= y1)
          spans.push_back({r[0], r[2]});
      std::sort(spans.begin(), spans.end());
      u32 x = 0;
      const auto add = [&](u32 x0, u32 x1) {
        if (x0 >= x1)
          return;
        Box b{x0, y0, x1, y1, rex::align((x1 - x0) * host_bytes, 256u), 0};
        // A box that continues the one above it (same columns) is merged into it.
        if (!p.boxes.empty() && p.boxes.back().x0 == x0 && p.boxes.back().x1 == x1 &&
            p.boxes.back().y1 == y0) {
          p.boxes.back().y1 = y1;
          return;
        }
        p.boxes.push_back(b);
      };
      for (const auto &[l, r] : spans) {
        add(x, l);
        x = std::max(x, r);
      }
      add(x, p.host_w);
    }
    for (Box &b : p.boxes) {
      b.offset = total;
      total += rex::align<uint64_t>(uint64_t(b.row_pitch) * (b.y1 - b.y0), 512u);
      texels += uint64_t(b.x1 - b.x0) * (b.y1 - b.y0);
    }
    if (!p.boxes.empty())
      plans.push_back(std::move(p));
  }
  if (plans.empty())
    return false;
  const auto t0 = Clock::now();
  // Resolves still owed to this memory land in it first (what a bind does before mirroring). NR-12a:
  // not the target's own: what they wrote is on the host already and left out of the fill (reading
  // them back was a GPU wait of up to 38 ms a fill at 4K).
  for (const Plan &p : plans)
    UfcResolveToGuestPhysical(p.L.phys, p.L.extent, "fill", covered ? host : nullptr);
  const auto t1 = Clock::now();

  // NR-12a: a scaled target is filled at guest size and stretched on the GPU (nearest, the texel
  // the CPU path below picks): at 3x a ninth of the texels converted and uploaded. ufc3_native_off
  // bit 65536: the CPU scale.
  const bool all_scaled = std::all_of(plans.begin(), plans.end(), [](const Plan &p) {
    return p.host_w != p.L.width || p.host_h != p.L.height;
  });
  if (all_scaled && IsRenderTargetCapable(host->format) &&
      host->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      !(REXCVAR_GET(ufc3_native_off) & 65536)) {
    // The guest texels under each box (inclusive), so covered tiles are neither converted nor read
    // (their pages hold the target's own resolves, read-watched until written back).
    struct Guest {
      u32 gx0, gy0, gx1, gy1;
      u32 row_pitch;
      u64 offset;
    };
    std::vector<std::vector<Guest>> guest(plans.size());
    u64 guest_total = 0, guest_texels = 0;
    std::vector<std::pair<const Guest *, u32>> tasks;  // one guest row of one box each
    for (size_t i = 0; i < plans.size(); ++i) {
      const Plan &p = plans[i];
      for (const Box &b : p.boxes) {
        Guest g{u32(uint64_t(b.x0) * p.L.width / p.host_w),
                u32(uint64_t(b.y0) * p.L.height / p.host_h),
                std::min(p.L.width - 1, u32(uint64_t(b.x1 - 1) * p.L.width / p.host_w)),
                std::min(p.L.height - 1, u32(uint64_t(b.y1 - 1) * p.L.height / p.host_h)), 0, 0};
        g.row_pitch = rex::align((g.gx1 - g.gx0 + 1) * host_bytes, 256u);
        g.offset = guest_total;
        guest_total += rex::align<uint64_t>(uint64_t(g.row_pitch) * (g.gy1 - g.gy0 + 1), 512u);
        guest_texels += uint64_t(g.gx1 - g.gx0 + 1) * (g.gy1 - g.gy0 + 1);
        guest[i].push_back(g);
      }
    }
    for (size_t i = 0; i < plans.size(); ++i)
      for (const Guest &g : guest[i])
        for (u32 gy = g.gy0; gy <= g.gy1; ++gy)
          tasks.push_back({&g, u32(i) | (gy << 8)});
    auto *memory = Mem();
    std::unique_ptr<u8[]> rows(new u8[size_t(guest_total)]);
    const auto convert = [&](size_t first, size_t end) {
      for (size_t t = first; t < end; ++t) {
        const Guest &g = *tasks[t].first;
        const Level &L = plans[tasks[t].second & 0xFF].L;
        const u32 gy = tasks[t].second >> 8;
        const u8 *src = memory->TranslatePhysical<const u8 *>(L.phys);
        u8 *out = rows.get() + g.offset + uint64_t(gy - g.gy0) * g.row_pitch;
        std::memset(out, 0, size_t(g.gx1 - g.gx0 + 1) * host_bytes);
        const auto one = [&](u32 gx, u8 *h) {
          const i32 so = TexelOffset(L, i32(gx), i32(gy));
          if (so < 0 || u32(so) + L.bpb > L.extent)
            return;
          u8 texel[16];
          std::memcpy(texel, src + so, L.bpb);
          SwapEndian(L.endian, texel, L.bpb);
          FromStored(L, texel, h);
        };
        // 32-bit texels: x & 3 lands only in bits 2-3 of a tiled offset (and a linear row is
        // contiguous), so each aligned group of 4 is 16 contiguous bytes: one address a group, and
        // the endian swap plus swizzle as one byte pick (pick 4 = 0, pick 5 = 255).
        const bool four = L.bpb == 4 && !(REXCVAR_GET(ufc3_native_off) & 131072) &&
                          (L.format == xe::TextureFormat::k_8_8_8_8 ||
                           L.format == xe::TextureFormat::k_8_8_8_8_A ||
                           L.format == xe::TextureFormat::k_16_16 ||
                           L.format == xe::TextureFormat::k_16_16_FLOAT ||
                           L.format == xe::TextureFormat::k_32_FLOAT);
        if (!four) {
          for (u32 gx = g.gx0; gx <= g.gx1; ++gx, out += host_bytes)
            one(gx, out);
          continue;
        }
        u8 pick[4];
        const bool swizzled = L.format == xe::TextureFormat::k_8_8_8_8 ||
                              L.format == xe::TextureFormat::k_8_8_8_8_A;
        for (u32 j = 0; j < 4; ++j) {
          const u32 from = swizzled ? (L.swizzle >> (3 * j)) & 7 : j;
          if (from >= 4) {
            pick[j] = u8(from == 5 ? 5 : 4);
            continue;
          }
          switch (L.endian) {
          case xe::Endian::k8in16: pick[j] = u8(from ^ 1); break;
          case xe::Endian::k8in32: pick[j] = u8(3 - from); break;
          case xe::Endian::k16in32: pick[j] = u8(from ^ 2); break;
          default: pick[j] = u8(from); break;
          }
        }
        for (u32 gx = g.gx0; gx <= g.gx1;) {
          const u32 run = std::min<u32>(4 - ((u32(L.off_x) + gx) & 3), g.gx1 - gx + 1);
          const i32 so = TexelOffset(L, i32(gx), i32(gy));
          if (so < 0 || u32(so) + run * 4 > L.extent) {
            for (u32 k = 0; k < run; ++k, out += 4)
              one(gx + k, out);
          } else {
            const u8 *p = src + so;
            for (u32 k = 0; k < run; ++k, p += 4, out += 4) {
              const u8 v[6] = {p[0], p[1], p[2], p[3], 0, 255};
              out[0] = v[pick[0]];
              out[1] = v[pick[1]];
              out[2] = v[pick[2]];
              out[3] = v[pick[3]];
            }
          }
          gx += run;
        }
      }
    };
    // The texel-at-a-time path took about 9 ns a texel on one core (1.3 ms for a 512x512 target's
    // uncovered part): past 128 K texels the rows are split over a few threads.
    const size_t workers =
        guest_texels >= (128u << 10)
            ? std::clamp<size_t>(std::min<size_t>(std::thread::hardware_concurrency() / 2,
                                                  size_t(guest_texels >> 17)),
                                 1, 8)
            : 1;
    if (workers > 1) {
      std::vector<std::thread> pool;
      const size_t per = (tasks.size() + workers - 1) / workers;
      for (size_t w = 1; w < workers; ++w)
        pool.emplace_back(convert, std::min(tasks.size(), w * per),
                          std::min(tasks.size(), (w + 1) * per));
      convert(0, std::min(tasks.size(), per));
      for (std::thread &t : pool)
        t.join();
    } else {
      convert(0, tasks.size());
    }
    const auto t2 = Clock::now();
    bool uploaded = false;
    size_t draws = 0;
    {
      auto &s = state();
      std::lock_guard lock(s.mutex);
      if (s.ready && !s.shutting_down.load(std::memory_order_acquire))
        uploaded = [&]() -> bool {
          BeginCommandList(s);
          if (!s.command_list_open)
            return false;
          // The draws below leave the game's pipeline and framebuffer unbound (also on a failure).
          s.current_pso = nullptr;
          s.dirtyStates.pipelineState = true;
          s.draw_framebuffer_bound = false;
          plume::RenderPipeline *pipeline = GetOrCreateResolvePipeline(s, host->format);
          if (!pipeline)
            return false;
          // Every scratch first, so a missing one leaves nothing recorded.
          std::vector<std::pair<GuestTexture *, GuestTexture *>> scratch;  // guest size, level size
          for (const Plan &p : plans) {
            GuestTexture *src = FillScratchLocked(s, host->format, p.L.width, p.L.height, 0);
            GuestTexture *mid =
                p.level ? FillScratchLocked(s, host->format, p.host_w, p.host_h, 1) : host;
            if (!src || !mid || BindTextureSRVLocked(s, src) == kInvalidDescriptorIndex)
              return false;
            scratch.push_back({src, mid});
          }
          auto buffer = CreateHostBuffer(s.device.get(),
                                         plume::RenderBufferDesc::UploadBuffer(guest_total),
                                         "target-fill");
          if (!buffer)
            return false;
          void *mapped = buffer->map();
          if (!mapped) {
            s.buffer_graveyard[Video::RetireSlot("target fill")].push_back(std::move(buffer));
            return false;
          }
          std::memcpy(mapped, rows.get(), size_t(guest_total));
          buffer->unmap();
          s.draw_framebuffer_bound = false;
          for (size_t i = 0; i < plans.size(); ++i) {
            const Plan &p = plans[i];
            auto [src, mid] = scratch[i];
            // The guest texels under the boxes -> the guest-size scratch.
            plume::RenderTextureBarrier up(src->texture, plume::RenderTextureLayout::COPY_DEST);
            s.command_list->barriers(plume::RenderBarrierStage::COPY, &up, 1);
            for (const Guest &g : guest[i])
              s.command_list->copyTextureRegion(
                  plume::RenderTextureCopyLocation::Subresource(src->texture, 0, 0),
                  plume::RenderTextureCopyLocation::PlacedFootprint(
                      buffer.get(), host->format, g.gx1 - g.gx0 + 1, g.gy1 - g.gy0 + 1, 1,
                      g.row_pitch / host_bytes, g.offset),
                  g.gx0, g.gy0, 0);
            // Stretched into level 0 of the target itself, or into a level-size scratch that is
            // then copied into the level (a framebuffer only takes mip 0).
            plume::RenderFramebuffer *fb = GetFramebuffer(s, mid, nullptr);
            if (!fb)
              return false;  // the copies so far only touched scratch textures
            plume::RenderTextureBarrier pre[2] = {
                plume::RenderTextureBarrier(src->texture, plume::RenderTextureLayout::SHADER_READ),
                plume::RenderTextureBarrier(mid->texture, plume::RenderTextureLayout::COLOR_WRITE),
            };
            s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, pre, std::size(pre));
            src->layout = plume::RenderTextureLayout::SHADER_READ;
            mid->layout = plume::RenderTextureLayout::COLOR_WRITE;
            s.command_list->setFramebuffer(fb);
            s.command_list->setPipeline(pipeline);
            s.command_list->setViewports(
                plume::RenderViewport(0.0f, 0.0f, float(p.host_w), float(p.host_h)));
            struct CopyPushConstants {
              u32 resourceDescriptorIndex;
              u32 resourceDescriptorIndex2;
              float param0;
              float param1;
            } push{src->descriptorIndex, (p.host_w & 0xFFFFu) | (p.host_h << 16), 1.0f, -2.0f};
            s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &push,
                                                     kCopyPushConstantByteOffset, sizeof(push));
            for (const Box &b : p.boxes) {
              s.command_list->setScissors(
                  plume::RenderRect(i32(b.x0), i32(b.y0), i32(b.x1), i32(b.y1)));
              s.command_list->drawInstanced(3, 1, 0, 0);
              ++draws;
            }
            s.command_list->setFramebuffer(nullptr);
            if (mid != host) {
              plume::RenderTextureBarrier cp[2] = {
                  plume::RenderTextureBarrier(mid->texture, plume::RenderTextureLayout::COPY_SOURCE),
                  plume::RenderTextureBarrier(host->texture, plume::RenderTextureLayout::COPY_DEST),
              };
              s.command_list->barriers(plume::RenderBarrierStage::COPY, cp, std::size(cp));
              mid->layout = plume::RenderTextureLayout::COPY_SOURCE;
              host->layout = plume::RenderTextureLayout::COPY_DEST;
              for (const Box &b : p.boxes) {
                const plume::RenderBox box(i32(b.x0), i32(b.y0), i32(b.x1), i32(b.y1));
                s.command_list->copyTextureRegion(
                    plume::RenderTextureCopyLocation::Subresource(host->texture, p.level, 0),
                    plume::RenderTextureCopyLocation::Subresource(mid->texture, 0, 0), b.x0, b.y0,
                    0, &box);
              }
            }
          }
          plume::RenderTextureBarrier post(host->texture, plume::RenderTextureLayout::SHADER_READ);
          s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
          host->layout = plume::RenderTextureLayout::SHADER_READ;
          s.current_pso = nullptr;
          s.dirtyStates.pipelineState = true;
          s.draw_framebuffer_bound = false;
          // The copies are in this command list: the buffer lives until its fence.
          s.buffer_graveyard[Video::RetireSlot("target fill")].push_back(std::move(buffer));
          return true;
        }();
    }
    if (uploaded) {
      static std::atomic<u32> s_logs{0};
      const u32 n = s_logs.fetch_add(1, std::memory_order_relaxed);
      if (n < 48 || n % 64 == 0 || UfcNr10On())
        BD_INFO("[nr12a] gpu fill #{} target {:08X} ({}x{} fmt {} levels {}): {} levels, {} boxes, "
                "{} KB at guest size ({} K texels); pending {:.2f} ms, texels {:.2f} ms, record "
                "{:.2f} ms",
                n + 1, texture_va, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                u32(fetch.format), fetch.mip_max_level + 1, plans.size(), draws, guest_total / 1024,
                guest_texels / 1024, Ms(t0, t1), Ms(t1, t2), Ms(t2, Clock::now()));
      return true;
    }
    // No scratch texture or pipeline: the CPU scale below.
  }

  auto *memory = Mem();
  // Only the box rows are written; the pitch padding is never copied.
  std::unique_ptr<u8[]> rows(new u8[size_t(total)]);
  std::vector<u8> lin;
  for (const Plan &p : plans) {
    const Level &L = p.L;
    const u8 *guest = memory->TranslatePhysical<const u8 *>(L.phys);
    for (const Box &b : p.boxes) {
      // The guest texels under the box in host texel order, a guest row at a time, then each host
      // row from it (nearest, for a scaled target; the same texel for every host pixel of a guest
      // one, as the resolve rounding puts them).
      const u32 gx0 = std::min(L.width - 1, u32(uint64_t(b.x0) * L.width / p.host_w));
      const u32 gx1 = std::min(L.width - 1, u32(uint64_t(b.x1 - 1) * L.width / p.host_w));
      lin.assign(size_t(gx1 - gx0 + 1) * host_bytes, 0);
      u8 *dst = rows.get() + b.offset;
      const size_t row_bytes = size_t(b.x1 - b.x0) * host_bytes;
      u32 last_gy = UINT32_MAX;
      for (u32 hy = b.y0; hy < b.y1; ++hy, dst += b.row_pitch) {
        const u32 gy = std::min(L.height - 1, u32(uint64_t(hy) * L.height / p.host_h));
        if (gy == last_gy) {
          std::memcpy(dst, dst - b.row_pitch, row_bytes);
          continue;
        }
        last_gy = gy;
        std::fill(lin.begin(), lin.end(), u8(0));
        u8 *out = lin.data();
        for (u32 gx = gx0; gx <= gx1; ++gx, out += host_bytes) {
          const i32 so = TexelOffset(L, i32(gx), i32(gy));
          if (so < 0 || u32(so) + L.bpb > L.extent)
            continue;
          u8 texel[16];
          std::memcpy(texel, guest + so, L.bpb);
          SwapEndian(L.endian, texel, L.bpb);
          FromStored(L, texel, out);
        }
        if (p.host_w == L.width) {
          std::memcpy(dst, lin.data(), row_bytes);
        } else {
          for (u32 hx = b.x0; hx < b.x1; ++hx) {
            const u32 gx = std::min(L.width - 1, u32(uint64_t(hx) * L.width / p.host_w));
            std::memcpy(dst + size_t(hx - b.x0) * host_bytes,
                        lin.data() + size_t(gx - gx0) * host_bytes, host_bytes);
          }
        }
      }
    }
  }
  const auto t2 = Clock::now();

  bool uploaded = false;
  size_t box_count = 0;
  {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.ready && !s.shutting_down.load(std::memory_order_acquire)) {
      BeginCommandList(s);
      if (s.command_list_open) {
        auto buffer = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::UploadBuffer(total),
                                       "target-fill");
        if (buffer) {
          if (void *mapped = buffer->map()) {
            std::memcpy(mapped, rows.get(), size_t(total));
            buffer->unmap();
            s.draw_framebuffer_bound = false;
            plume::RenderTextureBarrier pre(host->texture, plume::RenderTextureLayout::COPY_DEST);
            s.command_list->barriers(plume::RenderBarrierStage::COPY, &pre, 1);
            for (const Plan &p : plans) {
              for (const Box &b : p.boxes) {
                s.command_list->copyTextureRegion(
                    plume::RenderTextureCopyLocation::Subresource(host->texture, p.level, 0),
                    plume::RenderTextureCopyLocation::PlacedFootprint(
                        buffer.get(), host->format, b.x1 - b.x0, b.y1 - b.y0, 1,
                        b.row_pitch / host_bytes, b.offset),
                    b.x0, b.y0, 0);
                ++box_count;
              }
            }
            plume::RenderTextureBarrier post(host->texture, plume::RenderTextureLayout::SHADER_READ);
            s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &post, 1);
            host->layout = plume::RenderTextureLayout::SHADER_READ;
            uploaded = true;
          }
          // The copy is in this command list: the buffer lives until its fence.
          s.buffer_graveyard[Video::RetireSlot("target fill")].push_back(std::move(buffer));
        }
      }
    }
  }
  static std::atomic<u32> s_logs{0};
  const u32 n = s_logs.fetch_add(1, std::memory_order_relaxed);
  if (n < 48 || n % 64 == 0 || UfcNr10On())
    BD_INFO("[nr10] fill #{} target {:08X} ({}x{} fmt {} levels {} base {:08X} mip {:08X}) from guest "
            "memory: {} levels, {} boxes, {} KB ({} K host texels), {}; pending {:.2f} ms, texels "
            "{:.2f} ms, upload {:.2f} ms",
            n + 1, texture_va, fetch.size_2d.width + 1, fetch.size_2d.height + 1, u32(fetch.format),
            fetch.mip_max_level + 1, u32(fetch.base_address) << 12, u32(fetch.mip_address) << 12,
            plans.size(), box_count, total / 1024, texels / 1024,
            uploaded ? "uploaded" : "NOT uploaded", Ms(t0, t1), Ms(t1, t2), Ms(t2, Clock::now()));
  return uploaded;
}

void UfcWritebackNoteResolve(u32 dest_va, GuestTexture *host, const GuestTexture *surface, u32 level,
                             u32 face, bool have_rect, i32 left, i32 top, i32 right, i32 bottom,
                             i32 dst_x, i32 dst_y) {
  if (!UfcWritebackOn() || !host || !surface)
    return;
  const auto *texture = bd::mem::try_at<const D3DTexture>(dest_va);
  if (!texture)
    return;
  xe::xe_gpu_texture_fetch_t fetch{};
  for (int i = 0; i < 6; ++i)
    (&fetch.dword_0)[i] = u32(texture->Format.dword[i]);
  if (fetch.type != xe::FetchConstantType::kTexture)
    return;
  InstallOnce();

  const u64 key_parts[3] = {(u64(dest_va) << 32) | fetch.dword_1,
                            (u64(fetch.dword_2) << 32) | (level << 8) | face,
                            u64(reinterpret_cast<uintptr_t>(host))};
  const u64 key = XXH3_64bits(key_parts, sizeof(key_parts));
  Rect rect{};
  bool known = false;
  {
    std::lock_guard list(g_list_mutex);
    auto it = g_entries.find(key);
    if (it == g_entries.end()) {
      Entry entry;
      entry.host = host;
      entry.level = level;
      entry.face = face;
      u32 host_bytes = 0;
      if (!MakeLevel(fetch, level, face, entry.L) ||
          !HostTexelBytes(entry.L.format, entry.L.bpb, host_bytes) ||
          plume::RenderFormatSize(host->format) != host_bytes) {
        static std::atomic<u32> s_logs{0};
        if (s_logs.fetch_add(1, std::memory_order_relaxed) < 16)
          BD_INFO("[nr10] resolve to {:08X} (format {}, level {}, face {}) stays host-only", dest_va,
                  u32(fetch.format), level, face);
        return;
      }
      it = g_entries.emplace(key, std::move(entry)).first;
    }
    Entry &entry = it->second;
    // The rows the host copy gets (Video::ResolveRegionToTexture), in guest pixels.
    const float scale = surface->keep1x ? 1.0f : SvrAreaScale();
    const i32 surface_w = i32(float(surface->width) / scale + 0.5f);
    const i32 surface_h = i32(float(surface->height) / scale + 0.5f);
    if (!have_rect) {
      left = top = 0;
      right = surface_w;
      bottom = surface_h;
    }
    if (left < 0) {
      dst_x -= left;
      left = 0;
    }
    if (top < 0) {
      dst_y -= top;
      top = 0;
    }
    if (dst_x < 0) {
      left -= dst_x;
      dst_x = 0;
    }
    if (dst_y < 0) {
      top -= dst_y;
      dst_y = 0;
    }
    right = std::min<i32>({right, surface_w, left + i32(entry.L.width) - dst_x});
    bottom = std::min<i32>({bottom, surface_h, top + i32(entry.L.height) - dst_y});
    if (right <= left || bottom <= top)
      return;
    rect = {dst_x, dst_y, dst_x + (right - left), dst_y + (bottom - top), 0, 0, ++g_seq};
    for (Rect &r : entry.rects) {
      if (r.x0 == rect.x0 && r.y0 == rect.y0 && r.x1 == rect.x1 && r.y1 == rect.y1) {
        r.seq = rect.seq;  // a per-frame target ends here
        rect.first = r.first;
        rect.end = r.end;
        known = true;
        break;
      }
    }
    if (!known) {
      if (entry.rects.size() >= kMaxRectsPerEntry) {
        for (const Rect &r : entry.rects) {
          rect.x0 = std::min(rect.x0, r.x0);
          rect.y0 = std::min(rect.y0, r.y0);
          rect.x1 = std::max(rect.x1, r.x1);
          rect.y1 = std::max(rect.y1, r.y1);
        }
        entry.rects.clear();
      }
      RectBounds(entry.L, rect);
      if (rect.end <= rect.first)
        return;
      HashPages(rect);
      QueryAllocation(entry.L, rect.first, rect.alloc_base, rect.alloc_size, rect.alloc_protect,
                      rect.alloc_state);
      entry.rects.push_back(rect);
    }
  }
  auto *memory = Mem();
  if (known) {
    {
      auto global = rex::thread::global_critical_region::AcquireDirect();
      if (memory->IsPhysicalMemoryReadWatched(rect.first & ~(kPage - 1)))
        return;  // already hidden, nothing to do
    }
    // The watch went since the last resolve into this rect: the bytes under it may be new.
    std::lock_guard list(g_list_mutex);
    if (auto it = g_entries.find(key); it != g_entries.end())
      for (Rect &r : it->second.rects)
        if (r.x0 == rect.x0 && r.y0 == rect.y0 && r.x1 == rect.x1 && r.y1 == rect.y1) {
          HashPages(r);
          QueryAllocation(it->second.L, r.first, r.alloc_base, r.alloc_size, r.alloc_protect,
                          r.alloc_state);
        }
  }
  SetPending(rect.first, rect.end);
  const bool any = memory->EnablePhysicalMemoryReadWatch(
      rect.first & ~(kPage - 1),
      u32(rex::align<uint64_t>(rect.end, kPage)) - (rect.first & ~(kPage - 1)));
  // Probe: what each new watch got. `again` = a rect seen before whose first page had lost its watch.
  static std::atomic<u32> s_logs{0};
  if (UfcNr10On() && s_logs.fetch_add(1, std::memory_order_relaxed) < 160) {
    u32 watched = 0, pages = 0;
    {
      auto global = rex::thread::global_critical_region::AcquireDirect();
      for (u32 page = rect.first & ~(kPage - 1); page < rect.end; page += kPage, ++pages)
        watched += memory->IsPhysicalMemoryReadWatched(page);
    }
    BD_INFO("[nr10] watch frame {} {:08X} level {} face {} rect {},{}-{},{} bytes {:08X}-{:08X}: "
            "any {}, {} of {} pages watched{}",
            SvrFrameIndex(), dest_va, level, face, rect.x0, rect.y0, rect.x1, rect.y1, rect.first,
            rect.end, any, watched, pages, known ? " (again)" : "");
  }
}

}  // namespace bd::gpu

void UfcResolveToGuest(uint32_t guest_address, size_t size) {
  bd::gpu::UfcResolveToGuestPhysical(bd::gpu::SvrGpuAddress(guest_address), size, "encoder");
}
