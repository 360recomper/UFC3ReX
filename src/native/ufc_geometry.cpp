// SvR 2008: vertex and index buffers for the native renderer's draws (see svr_geometry.h).
//
// Geometry is read at draw time from the device state the game's draw is about to consume, which
// is what the GPU fetches:
//   stream N: vertex fetch constant 95 - N (device + 0x778 - 8 * N: GPU address of the stream's
//     vertex 0, SetStreamSource offset included, | 3; size | endian), the D3DVertexBuffer at
//     device + 0x30AC + 4 * N and the stride / 4 at device + 0x30F0 + N;
//   indices: the D3DIndexBuffer at device + 0x3094 (SetIndices only stores the pointer).
// Each guest range gets a host copy holding the bytes the GPU sees. The CPU rewrites buffers
// whenever it likes (Lock/Unlock between draws, new data loaded into reused memory), so a copy is
// compared with guest memory (content hash) the first time it's used in a frame and after every
// Unlock over it. New content gets a new host buffer (draws already recorded keep the old one);
// content that keeps changing streams through the per-frame upload ring instead.

#include "ufc_geometry.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <xxhash.h>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/physical_buffers.h"
#include "gpu/resources.h"
#include "ufc_hook.h"
#include "ufc_resources.h"

REXCVAR_DEFINE_BOOL(ufc3_native_geometry_every_draw, false, "ufc3",
                    "Native renderer dev aid: compare vertex and index buffers with guest memory at "
                    "every draw, for buffers the CPU rewrites between draws without an Unlock");
REXCVAR_DEFINE_BOOL(ufc3_native_check_geometry, false, "ufc3",
                    "Native renderer dev aid (NR-P4): hash each buffer the trusted path would not "
                    "look at, and log what it would have missed; also logs every buffer that "
                    "changes without an Unlock. Corrects what it finds, so judge the picture from a "
                    "run without it");
REXCVAR_DEFINE_BOOL(ufc3_native_geom_trust, true, "ufc3",
                    "Native renderer speed patch (NR-P4, on since NR-B3's A/B): trusted geometry "
                    "checks, fewer hashes of buffers that have been stable for many frames; false = "
                    "the old path (spot checks at every use)");
REXCVAR_DECLARE(int32_t, ufc3_native_off);

namespace bd::gpu {
namespace {

constexpr u32 kPhysicalMemoryBytes = 0x20000000u;
// No SvR buffer comes near this; a bigger size is a garbage header.
constexpr u32 kMaxBufferBytes = 64u << 20;
// Streams bound from the device. Slot 15 stays empty: re:Blue's stand-in inputs for semantics a
// declaration lacks read it (gpu/vertex_declaration.cpp). SvR uses streams 0-4.
constexpr u32 kStreamCount = 15;
// New content on this many checks in a row: the buffer is dynamic and goes through the upload ring.
constexpr u32 kDynamicAfterChanges = 2;
// Upload ring chunks are 16 MB; bigger dynamic buffers keep getting host buffers.
constexpr u32 kMaxRingBytes = 4u << 20;
// Copies no draw has used for this long are released (their guest memory is likely freed).
constexpr u64 kEvictAfterFrames = 600;

struct Mirror {
  u32 endian = 0;  // xenos::Endian the GPU applies reading this range
  bool index = false;
  bool has_content = false;
  u32 changes = 0;        // consecutive checks that found new content
  u64 hash = 0;
  u64 sample = 0;         // SampleHash of the content `hash` was taken from
  u32 stable = 0;         // full checks in a row that found the content unchanged
  u64 checked_frame = 0;  // frame the copy was last compared in; 0 = compare at the next use
  u64 used_frame = 0;
  std::unique_ptr<plume::RenderBuffer> buffer;  // static content
  plume::RenderBufferReference ring{};          // dynamic content, bindable while ring_epoch is current
  u64 ring_epoch = 0;
};

std::mutex g_mutex;
std::map<u64, Mirror> g_mirrors;  // (GPU address << 32) | dword-rounded size
u32 g_largest = 0;                // largest size in g_mirrors: bounds SvrInvalidateGeometry's search
u64 g_last_sweep = 0;

u64 Key(u32 address, u32 size) { return (u64(address) << 32) | size; }

// The path before NR-P4 (spot checks every use, no front table) unless ufc3_native_geom_trust is on;
// ufc3_native_off bit 32 wins over the cvar.
bool OldGeometryPath() {
  return !REXCVAR_GET(ufc3_native_geom_trust) || (REXCVAR_GET(ufc3_native_off) & 32) != 0;
}

// Direct-mapped table in front of g_mirrors: a draw looks up two or three mirrors and most of those
// find nothing to do, so a tree descent per lookup is a large part of what a draw pays here. std::map
// nodes don't move; SweepLocked clears the table when it erases. Key 0 can't occur: GpuMemory refuses
// address 0.
struct FrontEntry {
  u64 key = 0;
  Mirror *mirror = nullptr;
};
constexpr u32 kFrontBits = 12;
FrontEntry g_front[1u << kFrontBits];

Mirror &MirrorLocked(u32 address, u32 size, bool old_path) {
  const u64 key = Key(address, size);
  if (old_path)
    return g_mirrors[key];
  FrontEntry &entry = g_front[(key * 0x9E3779B97F4A7C15ull) >> (64u - kFrontBits)];
  if (entry.key != key) {
    entry.mirror = &g_mirrors[key];
    entry.key = key;
  }
  return *entry.mirror;
}

// SVR_GEOMETRY_PARANOID=1 or ufc3_native_geometry_every_draw: compare at every draw, to find buffers
// the CPU rewrites between draws without an Unlock.
bool CompareEveryDraw() {
  static const bool on = [] {
    const char *env = std::getenv("SVR_GEOMETRY_PARANOID");
    return REXCVAR_GET(ufc3_native_geometry_every_draw) || (env && *env && *env != '0');
  }();
  return on;
}

const u8 *GpuMemory(u32 address, u32 size) {
  if (!address || !size || size > kMaxBufferBytes || u64(address) + size > kPhysicalMemoryBytes)
    return nullptr;
  auto *memory = REX_KERNEL_MEMORY();
  return memory ? memory->TranslatePhysical<const u8 *>(address) : nullptr;
}

// Xenos byte-swaps every 32-bit word it fetches (vertex fetch) or DMAs (indices) by the endian mode
// before splitting it into components, so the host copy holds the swapped bytes. XDK vertex
// buffers and BeginVertices data are 8in32 (re:Blue's bswap32), INDEX16 8in16, INDEX32 8in32.
// 16-bit pairs then come out in the order the shaders' .yxwz swap (swapFloats) expects, whatever
// the mode, because Xenos takes component X from the high half of the swapped word.
void CopyAsGpuReads(void *dst, const u8 *src, u32 size, u32 endian) {
  switch (endian & 3u) {
  case 1:  // k8in16
    rex::memory::copy_and_swap_16_unaligned(dst, src, size / 2);
    break;
  case 2:  // k8in32
    rex::memory::copy_and_swap_32_unaligned(dst, src, size / 4);
    break;
  case 3:  // k16in32
    rex::memory::copy_and_swap_16_in_32_unaligned(dst, src, size / 4);
    break;
  default:  // kNone
    std::memcpy(dst, src, size);
    break;
  }
}

// A full-arena frame uses tens of MB of vertex data; hashing all of it every frame cost several ms
// on CPUs with smaller caches. Rewrites through Lock/Unlock are flagged by the Unlock hooks (a full
// check at the next use), so a buffer nobody unlocked gets a spot check in between full checks
// every kFullCheckFrames frames (staggered by address). The spot check catches content loaded
// wholesale into reused memory at once. A buffer that changed is checked in full every frame until
// two full checks in a row find it unchanged: another thread may still have been writing it when
// it was copied (crowd members went missing on slower CPUs when the spot check skipped the rest).
constexpr u64 kFullCheckFrames = 8;
constexpr u32 kStableBeforeSpotChecks = 2;
constexpr u32 kAlwaysFullBytes = 16 * 1024;
constexpr u32 kSamples = 16, kSampleBytes = 64;

// NR-P4: a buffer earns trust. After kTrustAfter full checks in a row found it unchanged, a first use
// in a frame touches no guest memory: a sample every kTrustSampleFrames frames, the whole buffer every
// kTrustFullFrames (staggered by address), plus a sample whenever it wasn't used in the frame before,
// which is when memory reused for new content can come back. An Unlock still forces a full check, and
// a change sends the buffer back to full checks until it earns trust again, so one rewritten at
// least every kTrustAfter frames never gets here, Unlock or no Unlock. At or under kTrustMinBytes the
// whole buffer costs no more than a sample and is hashed at every first use.
constexpr u32 kTrustAfter = 16;
constexpr u32 kTrustMinBytes = kSamples * kSampleBytes;
constexpr u64 kTrustSampleFrames = 8;
constexpr u64 kTrustFullFrames = 64;

u64 SampleHash(const u8 *src, u32 size) {
  if (size <= kSamples * kSampleBytes)
    return XXH3_64bits(src, size);
  u8 buf[kSamples * kSampleBytes];
  const u32 step = (size - kSampleBytes) / (kSamples - 1);
  for (u32 i = 0; i < kSamples; ++i)
    std::memcpy(buf + i * kSampleBytes, src + size_t(i) * step, kSampleBytes);
  return XXH3_64bits(buf, sizeof(buf));
}

// Draws already recorded keep a retired copy until its frame slot's fence.
void Retire(Mirror &m) {
  if (m.buffer)
    RetirePhysicalBuffer(std::move(m.buffer));
  m.ring = {};
}

std::unique_ptr<plume::RenderBuffer> CreateCopy(const u8 *src, u32 size, u32 endian, bool index) {
  auto *device = Video::HostDevice();
  if (!device)
    return nullptr;
  const plume::RenderHeapType heap = GeometryHeapType(device, GeometryClass::Static);
  auto buffer = CreateHostBuffer(device,
                                 index ? plume::RenderBufferDesc::IndexBuffer(size, heap)
                                       : plume::RenderBufferDesc::VertexBuffer(size, heap),
                                 index ? "svr-ib" : "svr-vb");
  if (!buffer)
    return nullptr;
  void *mapped = buffer->map();
  if (!mapped)
    return nullptr;
  CopyAsGpuReads(mapped, src, size, endian);
  buffer->unmap();
  return buffer;
}

bool CheckGeometry() {
  static const bool on = REXCVAR_GET(ufc3_native_check_geometry);
  return on;
}

enum class GeomChange : u32 {
  kTrustMiss,  // the trusted path would have drawn old content: the pass line, must stay at 0
  kNoUnlock,   // a due full check found new content with no Unlock since the last check
  kMidFrame,   // new content since this frame's check with no Unlock (the old path misses this too)
};

// ufc3_native_check_geometry: lines at the 1st, 2nd, 4th... time per buffer and kind. Under g_mutex.
void ReportGeometryChange(GeomChange kind, u32 address, u32 size, bool index, const Mirror &m,
                          u64 frame, bool sampled) {
  static constexpr const char *kKinds[] = {"trust-miss", "no-unlock", "mid-frame"};
  static std::map<std::pair<u64, u32>, u32> s_seen;
  const u32 n = ++s_seen[{Key(address, size), u32(kind)}];
  if (kind == GeomChange::kTrustMiss)
    SvrPerfCount(SvrCount::kGeomTrustMisses, 1);
  if ((n & (n - 1)) == 0)
    BD_WARN("geometry check: {} #{} {} 0x{:08X} size {} stable {} sampled {} ring {} changes {} "
            "frame {}",
            kKinds[u32(kind)], n, index ? "ib" : "vb", address, size, m.stable, sampled ? 1 : 0,
            m.buffer ? 0 : 1, m.changes, frame);
}

// The host copy of [address, address + size) (GPU address) as the GPU reads it.
bool ResolveLocked(u32 address, u32 size, u32 endian, bool index, u64 frame,
                   plume::RenderBufferReference &out) {
  size = (size + 3u) & ~3u;  // the GPU reads whole words (an odd INDEX16 count ends mid-word)
  const u8 *src = GpuMemory(address, size);
  if (!src)
    return false;
  const bool old_path = OldGeometryPath();
  const bool every_draw = CompareEveryDraw();
  const bool check = !old_path && CheckGeometry();
  SvrPerfCount(SvrCount::kGeomResolves, 1);
  Mirror &m = MirrorLocked(address, size, old_path);
  g_largest = std::max(g_largest, size);
  const bool used_last_frame = m.used_frame + 1 >= frame;
  m.used_frame = frame;
  const bool same_kind = m.has_content && m.endian == endian && m.index == index;
  const bool ring_current = m.ring.ref && m.ring_epoch == UploadEpoch();
  bool rewritten = false;  // check only: new content with this frame's check already done
  if (same_kind && m.checked_frame == frame && !every_draw) {
    rewritten = check && XXH3_64bits(src, size) != m.hash;
    if (rewritten)
      ReportGeometryChange(GeomChange::kMidFrame, address, size, index, m, frame, false);
    if (!rewritten) {
      if (m.buffer) {
        out = m.buffer.get();
        return true;
      }
      if (ring_current) {
        out = m.ring;
        return true;
      }
    }
  }
  const bool unlocked = m.checked_frame == 0;  // or first use, or a copy that failed
  u64 sample = 0;
  bool have_sample = false;
  if (old_path) {
    // Not unlocked since its last check (checked_frame != 0), not due a full check: spot check.
    sample = SampleHash(src, size);
    have_sample = true;
    if (same_kind && !unlocked && size > kAlwaysFullBytes && !every_draw &&
        m.stable >= kStableBeforeSpotChecks && (frame + (address >> 8)) % kFullCheckFrames != 0 &&
        sample == m.sample) {
      m.checked_frame = frame;
      if (m.buffer) {
        out = m.buffer.get();
        return true;
      }
      if (ring_current) {
        out = m.ring;
        return true;
      }
    }
  } else if (same_kind && !unlocked && !every_draw && !rewritten && size > kTrustMinBytes &&
             m.stable >= kTrustAfter && (m.buffer || ring_current)) {
    const u64 phase = frame + (address >> 8);
    if (phase % kTrustFullFrames != 0) {
      bool same = true;
      if (!used_last_frame || phase % kTrustSampleFrames == 0) {
        sample = SampleHash(src, size);
        have_sample = true;
        same = sample == m.sample;
        SvrPerfCount(SvrCount::kGeomSamples, 1);
      }
      if (same) {
        if (check && XXH3_64bits(src, size) != m.hash) {
          ReportGeometryChange(GeomChange::kTrustMiss, address, size, index, m, frame, have_sample);
        } else {
          SvrPerfCount(SvrCount::kGeomTrusted, 1);
          m.checked_frame = frame;
          if (m.buffer)
            out = m.buffer.get();
          else
            out = m.ring;
          return true;
        }
      }
    }
  }
  SvrZone hash_zone(SvrZoneId::kGeomHash);
  // A dynamic buffer the game just unlocked goes to the ring anyway: only whether it stopped changing
  // would come out of hashing it. If it did, the first check with no Unlock finds that and it settles.
  const bool assume_changed = !old_path && !every_draw && !check && same_kind && unlocked &&
                              m.changes >= kDynamicAfterChanges && size <= kMaxRingBytes;
  const u64 hash = assume_changed ? ~m.hash : XXH3_64bits(src, size);
  if (!assume_changed) {
    SvrPerfCount(SvrCount::kGeomHashBytes, size);
    if (size <= kTrustMinBytes)
      SvrPerfCount(SvrCount::kGeomHashSmallBytes, size);
    if (unlocked)
      SvrPerfCount(SvrCount::kGeomHashUnlockedBytes, size);
  }
  if (check && same_kind && !unlocked && !rewritten && hash != m.hash)
    ReportGeometryChange(GeomChange::kNoUnlock, address, size, index, m, frame, have_sample);
  if (old_path)
    m.sample = sample;
  else if (size > kTrustMinBytes && !assume_changed &&
           (!same_kind || hash != m.hash || (have_sample && sample != m.sample)))
    m.sample = have_sample ? sample : SampleHash(src, size);
  m.stable = (same_kind && hash == m.hash) ? m.stable + 1 : 0;
  if (same_kind && hash == m.hash) {
    m.checked_frame = frame;
    if (m.buffer) {
      out = m.buffer.get();
      return true;
    }
    if (ring_current) {
      out = m.ring;
      return true;
    }
    // Dynamic content that stopped changing (its ring copy is from an older frame): it settles
    // into a host buffer below.
  }
  const u32 changes = (same_kind && hash != m.hash) ? m.changes + 1 : 0;
  plume::RenderBufferReference fresh_ring{};
  std::unique_ptr<plume::RenderBuffer> fresh;
  if (changes >= kDynamicAfterChanges && size <= kMaxRingBytes) {
    const ConstantAllocation alloc = AllocateUploadBytes(size, 256);
    if (alloc.memory) {
      CopyAsGpuReads(alloc.memory, src, size, endian);
      SvrPerfCount(SvrCount::kGeomRingBytes, size);
      fresh_ring = alloc.ref;
    }
  }
  if (!fresh_ring.ref) {
    fresh = CreateCopy(src, size, endian, index);
    SvrPerfCount(SvrCount::kGeomNewBuffers, 1);
    SvrPerfCount(SvrCount::kGeomNewBytes, size);
    if (!fresh) {
      // No device or out of memory: keep serving the old copy and look again at the next use.
      m.checked_frame = 0;
      if (!m.buffer)
        return false;
      out = m.buffer.get();
      return true;
    }
  }
  Retire(m);
  m.endian = endian;
  m.index = index;
  m.hash = hash;
  m.changes = changes;
  m.checked_frame = frame;
  m.has_content = true;
  if (fresh_ring.ref) {
    m.ring = fresh_ring;
    m.ring_epoch = UploadEpoch();
    out = m.ring;
  } else {
    m.buffer = std::move(fresh);
    out = m.buffer.get();
  }
  return true;
}

void SweepLocked(u64 frame) {
  if (frame < g_last_sweep + 60)
    return;
  g_last_sweep = frame;
  u32 largest = 0;
  bool erased = false;
  for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
    if (frame > it->second.used_frame + kEvictAfterFrames) {
      Retire(it->second);
      it = g_mirrors.erase(it);
      erased = true;
    } else {
      largest = std::max(largest, u32(it->first));
      ++it;
    }
  }
  if (erased)
    std::fill(std::begin(g_front), std::end(g_front), FrontEntry{});
  if (!OldGeometryPath())
    g_largest = largest;
}

struct StreamBinding {
  plume::RenderBufferReference ref{};
  u32 size = 0;
  u32 stride = 0;
};

// SetStreamSource (sub_8225A648) keeps stream N in vertex fetch constant 95 - N: the last two
// dwords of fetch slot 31 for stream 0, i.e. device + 0x778 - 8 * N.
void StreamFetch(const D3DDevice &device, u32 stream, u32 &dword0, u32 &dword1) {
  const u32 constant = 95u - stream;
  const DeviceFetchConstant &slot = device.fetchConstants[constant / 3u];
  dword0 = u32(slot.dword[(constant % 3u) * 2u]);
  dword1 = u32(slot.dword[(constant % 3u) * 2u + 1u]);
}

bool ResolveStreamLocked(const D3DDevice &device, u32 stream, u64 frame, StreamBinding &out) {
  const u32 vb_va = u32(device.streamSource[stream]);
  if (!vb_va)
    return false;
  u32 dword0, dword1;
  StreamFetch(device, stream, dword0, dword1);
  if ((dword0 & kFetchTypeMask) != kFetchTypeVertex)
    return false;
  const u32 address = dword0 & 0x1FFFFFFCu;  // GPU address of the stream's vertex 0
  const u32 size = dword1 & 0x03FFFFFCu;     // bytes from there to the end of the buffer
  const u32 endian = dword1 & 3u;
  // One copy of the whole D3DVertexBuffer, shared by streams set at different offsets into it.
  u32 base = address;
  u32 base_size = size;
  if (const auto *vb = bd::mem::at<const D3DVertexBuffer>(vb_va)) {
    const u32 vb_base = SvrGpuAddress(u32(vb->FetchLo) & ~3u);
    const u32 vb_size = u32(vb->FetchHi) & 0x03FFFFFCu;
    if (address >= vb_base && u64(address) + size <= u64(vb_base) + vb_size) {
      base = vb_base;
      base_size = vb_size;
    }
  }
  plume::RenderBufferReference ref;
  if (!ResolveLocked(base, base_size, endian, /*index=*/false, frame, ref))
    return false;
  out.ref = plume::RenderBufferReference(ref.ref, ref.offset + (address - base));
  out.size = size;
  out.stride = u32(device.streamStrideDiv4[stream]) * 4u;
  return true;
}

struct IndexBinding {
  plume::RenderBufferReference ref{};
  u32 size = 0;
  plume::RenderFormat format = plume::RenderFormat::R16_UINT;
};

// SetIndices (sub_8225A768) only stores the D3DIndexBuffer at device + 0x3094; DrawIndexedVertices
// (sub_825A4340) reads Common (bit 31 = INDEX32, bits 30-29 = endian), Address (+0x18, through
// GPU_CONVERT_CPU_TO_GPU_ADDRESS) and Size (+0x1C, bytes) at draw time. UFC's Size can stop short of
// the last INDEX16 (count=18 size=34, count=68 size=134): the Xenos fetches whole 32-bit words, so
// what a draw may read is Size rounded up to a word.
u32 IndexBufferReadableBytes(const D3DIndexBuffer &ib) {
  return (u32(ib.FetchHi) + 3u) & ~3u;
}

bool ResolveIndicesLocked(const D3DDevice &device, u64 frame, IndexBinding &out) {
  const auto *ib = bd::mem::at<const D3DIndexBuffer>(u32(device.indexBuffer));
  if (!ib)
    return false;
  const u32 common = ib->resource.Common;
  const u32 size = IndexBufferReadableBytes(*ib);
  if (!ResolveLocked(SvrGpuAddress(ib->FetchLo), size, (common >> 29) & 3u, /*index=*/true, frame,
                     out.ref))
    return false;
  out.size = size;
  out.format = (common & 0x80000000u) ? plume::RenderFormat::R32_UINT
                                      : plume::RenderFormat::R16_UINT;
  return true;
}

// D3DVertexBuffer_Unlock (sub_82252818) / D3DIndexBuffer_Unlock (sub_82253178), after the game's
// own: the CPU has finished writing the buffer. SvR re-fills dynamic buffers this way between draws
// (sub_825F9A28: a 120-byte quad; sub_8257E530: positions every frame).
void VertexBufferUnlocked(u32 vb_va) {
  SvrZone zone(SvrZoneId::kUnlock);
  if (const auto *vb = bd::mem::at<const D3DVertexBuffer>(vb_va))
    SvrInvalidateGeometry(u32(vb->FetchLo) & ~3u, u32(vb->FetchHi) & 0x03FFFFFCu);
}

void IndexBufferUnlocked(u32 ib_va) {
  SvrZone zone(SvrZoneId::kUnlock);
  if (const auto *ib = bd::mem::at<const D3DIndexBuffer>(ib_va))
    SvrInvalidateGeometry(ib->FetchLo, ib->FetchHi);
}

}  // namespace

void SvrDrawLogStream0(u32 device_guest) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device)
    return;
  u32 dword0, dword1;
  StreamFetch(*device, 0, dword0, dword1);
  const u32 stride = u32(device->streamStrideDiv4[0]) * 4u;
  std::string words;
  // Guest memory, big-endian words shown as the game wrote them.
  if (const u8 *src = GpuMemory(dword0 & 0x1FFFFFFCu, 4u * stride)) {
    for (u32 k = 0; k < 4u * stride / 4u; ++k) {
      u32 word;
      std::memcpy(&word, src + k * 4u, 4);
      words += fmt::format(" {:08X}", rex::byte_swap(word));
    }
  }
  BD_INFO("[drawlog]   stream0 vb 0x{:08X} fetch {:08X} {:08X} stride {} memory{}",
          u32(device->streamSource[0]), dword0, dword1, stride, words);
}

void SvrBindDrawGeometry(u32 device_guest, bool indexed) {
  SvrZone zone(SvrZoneId::kBindGeometry);
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device)
    return;
  const u64 frame = SvrFrameIndex();
  const GuestVertexDeclaration *decl = SvrLookupVertexDeclaration(u32(device->vertexDeclaration));
  StreamBinding streams[kStreamCount];
  bool used[kStreamCount] = {};
  IndexBinding indices;
  bool have_indices = false;
  u32 missing = 0;
  {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      SvrPerfCount(SvrCount::kLockMisses, 1);
      lock.lock();
    }
    SweepLocked(frame);
    for (u32 n = 0; n < kStreamCount; ++n) {
      used[n] = decl ? decl->vertexStreams[n] : u32(device->streamSource[n]) != 0u;
      if (used[n] && !ResolveStreamLocked(*device, n, frame, streams[n]))
        missing |= 1u << n;
    }
    if (indexed)
      have_indices = ResolveIndicesLocked(*device, frame, indices);
  }
  if (missing || (indexed && !have_indices)) {
    static std::atomic<u32> s_warned{0};
    const u32 k = s_warned.fetch_add(1, std::memory_order_relaxed);
    if (k < 16 || (k & (k - 1)) == 0)
      BD_WARN("SvR geometry #{}: unresolved streams 0x{:X}{} (index buffer 0x{:08X})", k, missing,
              indexed && !have_indices ? ", no index buffer" : "", u32(device->indexBuffer));
  }
  // Outside g_mutex: the Video setters take state().mutex.
  for (u32 n = 0; n < kStreamCount; ++n) {
    if (used[n])
      Video::SetVertexStream(n, streams[n].ref, streams[n].size, streams[n].stride);
  }
  if (indexed)
    Video::SetIndexBufferRef(indices.ref, have_indices ? indices.size : 0u, indices.format);
}

u32 SvrBindFanIndices(u32 vertex_count) {
  if (vertex_count < 3)
    return 0;
  const u32 index_count = (vertex_count - 2u) * 3u;
  const bool wide = vertex_count > 0xFFFFu;
  const u32 bytes = index_count * (wide ? 4u : 2u);
  const ConstantAllocation alloc = AllocateUploadBytes((bytes + 3u) & ~3u, 4);
  if (!alloc.memory)
    return 0;
  for (u32 t = 0; t < vertex_count - 2u; ++t) {
    const u32 tri[3] = {0u, t + 1u, t + 2u};
    for (u32 k = 0; k < 3; ++k) {
      if (wide) {
        std::memcpy(alloc.memory + size_t(t * 3u + k) * 4u, &tri[k], 4);
      } else {
        const u16 narrow = u16(tri[k]);
        std::memcpy(alloc.memory + size_t(t * 3u + k) * 2u, &narrow, 2);
      }
    }
  }
  Video::SetIndexBufferRef(alloc.ref, bytes,
                           wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  return index_count;
}

u32 SvrBindFanIndicesFromDevice(u32 device_guest, u32 start_index, u32 fan_count) {
  if (fan_count < 3)
    return 0;
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  const auto *ib = device ? bd::mem::at<const D3DIndexBuffer>(u32(device->indexBuffer)) : nullptr;
  if (!ib)
    return 0;
  const u32 common = ib->resource.Common;
  const bool wide = (common & 0x80000000u) != 0;
  const u32 width = wide ? 4u : 2u;
  const u32 endian = (common >> 29) & 3u;
  if (u64(start_index) + fan_count > u64(IndexBufferReadableBytes(*ib)) / width)
    return 0;
  const u8 *src = GpuMemory(SvrGpuAddress(ib->FetchLo) + start_index * width, fan_count * width);
  if (!src)
    return 0;
  // The fan's indices as the GPU reads them (CopyAsGpuReads needs whole 32-bit words).
  std::vector<u8> fan(((fan_count * width) + 3u) & ~3u);
  std::vector<u8> raw(fan.size());
  std::memcpy(raw.data(), src, fan_count * width);
  CopyAsGpuReads(fan.data(), raw.data(), u32(fan.size()), endian);
  const u32 index_count = (fan_count - 2u) * 3u;
  const u32 bytes = index_count * width;
  const ConstantAllocation alloc = AllocateUploadBytes((bytes + 3u) & ~3u, 4);
  if (!alloc.memory)
    return 0;
  for (u32 t = 0; t < fan_count - 2u; ++t) {
    const u32 pick[3] = {0u, t + 1u, t + 2u};
    for (u32 k = 0; k < 3; ++k)
      std::memcpy(alloc.memory + size_t(t * 3u + k) * width, fan.data() + size_t(pick[k]) * width,
                  width);
  }
  Video::SetIndexBufferRef(alloc.ref, bytes,
                           wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  return index_count;
}

bool SvrReadStream0Vertices(u32 device_guest, u32 first, u32 count, std::vector<u8> &out,
                            u32 &stride) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device || !count || !u32(device->streamSource[0]))
    return false;
  u32 dword0, dword1;
  StreamFetch(*device, 0, dword0, dword1);
  if ((dword0 & kFetchTypeMask) != kFetchTypeVertex)
    return false;
  const u32 address = dword0 & 0x1FFFFFFCu;
  const u32 size = dword1 & 0x03FFFFFCu;
  stride = u32(device->streamStrideDiv4[0]) * 4u;
  if (!stride || (u64(first) + count) * stride > size)
    return false;
  const u32 bytes = count * stride;
  const u8 *src = GpuMemory(address + first * stride, bytes);
  if (!src)
    return false;
  out.resize(bytes);
  CopyAsGpuReads(out.data(), src, bytes, dword1 & 3u);
  return true;
}

bool SvrReadWholeStream0(u32 device_guest, u32 max_bytes, std::vector<u8> &out, u32 &stride) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  if (!device)
    return false;
  u32 dword0, dword1;
  StreamFetch(*device, 0, dword0, dword1);
  const u32 size = dword1 & 0x03FFFFFCu;
  const u32 step = u32(device->streamStrideDiv4[0]) * 4u;
  if (!step || size < step || size > max_bytes)
    return false;
  return SvrReadStream0Vertices(device_guest, 0, size / step, out, stride);
}

bool SvrReadDeviceIndices(u32 device_guest, u32 start, u32 count, std::vector<u32> &out) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  const auto *ib = device ? bd::mem::at<const D3DIndexBuffer>(u32(device->indexBuffer)) : nullptr;
  if (!ib || !count)
    return false;
  const u32 common = ib->resource.Common;
  const u32 width = (common & 0x80000000u) ? 4u : 2u;
  if (u64(start) + count > u64(IndexBufferReadableBytes(*ib)) / width)
    return false;
  const u8 *src = GpuMemory(SvrGpuAddress(ib->FetchLo) + start * width, count * width);
  if (!src)
    return false;
  std::vector<u8> raw(((count * width) + 3u) & ~3u), host(raw.size());
  std::memcpy(raw.data(), src, count * width);
  CopyAsGpuReads(host.data(), raw.data(), u32(host.size()), (common >> 29) & 3u);
  out.resize(count);
  for (u32 i = 0; i < count; ++i) {
    if (width == 4u) {
      std::memcpy(&out[i], host.data() + size_t(i) * 4u, 4);
    } else {
      u16 narrow;
      std::memcpy(&narrow, host.data() + size_t(i) * 2u, 2);
      out[i] = narrow;
    }
  }
  return true;
}

u32 SvrBindQuadIndicesFromDevice(u32 device_guest, u32 start_index, u32 index_count,
                                 i32 base_vertex, u64 vs_hash) {
  const u32 quads = index_count / 4u;
  std::vector<u32> guest;
  if (!quads || !SvrReadDeviceIndices(device_guest, start_index, quads * 4u, guest))
    return 0;
  const bool wide = *std::max_element(guest.begin(), guest.end()) > 0xFFFFu;
  const u32 width = wide ? 4u : 2u;
  const u32 out_count = quads * 6u;
  const u32 bytes = out_count * width;
  const ConstantAllocation alloc = AllocateUploadBytes((bytes + 3u) & ~3u, 4);
  if (!alloc.memory)
    return 0;
  static constexpr u32 kPick[6] = {0u, 1u, 2u, 0u, 2u, 3u};
  for (u32 q = 0; q < quads; ++q) {
    for (u32 k = 0; k < 6u; ++k) {
      const u32 index = guest[size_t(q) * 4u + kPick[k]];
      u8 *dst = alloc.memory + (size_t(q) * 6u + k) * width;
      if (wide) {
        std::memcpy(dst, &index, 4);
      } else {
        const u16 narrow = u16(index);
        std::memcpy(dst, &narrow, 2);
      }
    }
  }
  Video::SetIndexBufferRef(alloc.ref, bytes,
                           wide ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT);
  // One line per vertex shader: does this path change anything, and how the corners are stored.
  static std::mutex s_mutex;
  static std::set<u64> s_seen;
  bool first_time;
  {
    std::lock_guard lock(s_mutex);
    first_time = s_seen.insert(vs_hash).second;
  }
  if (first_time) {
    bool sequential = true;
    for (u32 i = 0; i < quads * 4u; ++i)
      sequential = sequential && guest[i] == guest[0] + i;
    std::string corners;
    for (u32 i = 0; i < 4u; ++i) {
      std::vector<u8> vertex;
      u32 stride = 0;
      float xyzw[4] = {};
      if (SvrReadStream0Vertices(device_guest, u32(i32(guest[i]) + base_vertex), 1u, vertex,
                                 stride) &&
          stride >= 16u)
        std::memcpy(xyzw, vertex.data(), 16);
      corners += fmt::format(" i{}=({:.2f},{:.2f},{:.2f},{:.2f})", guest[i], xyzw[0], xyzw[1],
                             xyzw[2], xyzw[3]);
    }
    BD_INFO("native: indexed quads: vs {:016X} start {} count {} sequential {} corners{}", vs_hash,
            start_index, index_count, sequential ? "yes" : "no", corners);
  }
  return out_count;
}

void SvrNoteWordRoundedIndices(u32 device_guest, u32 primitive, u32 start, u32 count, u64 vs_hash,
                               u64 ps_hash) {
  const auto *device = bd::mem::at<const D3DDevice>(device_guest);
  const auto *ib = device ? bd::mem::at<const D3DIndexBuffer>(u32(device->indexBuffer)) : nullptr;
  if (!ib || !count)
    return;
  const u32 width = (u32(ib->resource.Common) & 0x80000000u) ? 4u : 2u;
  if ((u64(start) + count) * width <= u32(ib->FetchHi))
    return;
  // One line per distinct buffer + count: enough to name the objects, not a per-frame cost.
  static std::mutex s_mutex;
  static std::set<u64> s_seen;
  {
    std::lock_guard lock(s_mutex);
    if (s_seen.size() >= 64 || !s_seen.insert(Key(ib->FetchLo, count)).second)
      return;
  }
  std::vector<u32> indices;
  if (!SvrReadDeviceIndices(device_guest, start, count, indices))
    return;
  const u32 last = indices.back();
  const u32 others = count > 1 ? *std::max_element(indices.begin(), indices.end() - 1) : 0u;
  u32 dword0, dword1;
  StreamFetch(*device, 0, dword0, dword1);
  const u32 stride = u32(device->streamStrideDiv4[0]) * 4u;
  BD_INFO("native: index past Size: ib 0x{:08X} size {} p{} start {} count {} last {} max of others "
          "{} stream0 vertices {} vs {:016X} ps {:016X}",
          u32(ib->FetchLo), u32(ib->FetchHi), primitive, start, count, last, others,
          stride ? (dword1 & 0x03FFFFFCu) / stride : 0u, vs_hash, ps_hash);
}

void SvrInvalidateGeometry(u32 address, u32 size) {
  if (!address || !size)
    return;
  const u32 start = SvrGpuAddress(address);
  const u64 end = u64(start) + size;
  std::lock_guard lock(g_mutex);
  const u32 from = start > g_largest ? start - g_largest : 0u;
  u64 walked = 0;
  for (auto it = g_mirrors.lower_bound(Key(from, 0));
       it != g_mirrors.end() && (it->first >> 32) < end; ++it) {
    ++walked;
    if ((it->first >> 32) + u32(it->first) > start)
      it->second.checked_frame = 0;
  }
  SvrPerfCount(SvrCount::kUnlocks, 1);
  SvrPerfCount(SvrCount::kUnlockWalked, walked);
}

}  // namespace bd::gpu

SVR_HOOK_AFTER(D3DVertexBuffer_Unlock, bd::gpu::VertexBufferUnlocked);
SVR_HOOK_AFTER(D3DIndexBuffer_Unlock, bd::gpu::IndexBufferUnlocked);
