/**
 * @file    gpu/frame_ring.cpp
 * @brief   The kNumFrames command list ring: opening a list on the recording
 *          slot, retiring resources against it, and advancing past its fence.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#if defined(SVR_NATIVE_RENDERER)
#include "ufc_command_stream.h"
#include "ufc_frame_diag.h"
#endif
#include "gpu/frame.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#if defined(REBLUE_D3D12)
#include <plume_d3d12.h>
#endif

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/physical_buffers.h"
#include "gpu/surface_registry.h"

namespace bd::gpu {

namespace {

// ufc3_native_perf: GPU time of each frame slot, from a timestamp at each end of each command list
// it submits (NR-14e: one pair per list, summed; a pair across NR-14c's early submits also timed
// the CPU gaps between them, 32 ms in 30 fps scenes). Lists past kGpuTimePairs go untimed.
// The pools are never freed: the process hard-exits.
constexpr u32 kGpuTimePairs = 32;
plume::RenderQueryPool *g_gpu_time_pool[kNumFrames] = {};
u32 g_gpu_time_pairs[kNumFrames] = {};  // pairs with both timestamps written
bool g_gpu_time_open[kNumFrames] = {};  // the open list has its start timestamp

void SvrGpuTimeBegin(VideoState &s, u32 slot) {
  if (!g_svr_perf_on.load(std::memory_order_relaxed) || g_gpu_time_open[slot] ||
      g_gpu_time_pairs[slot] >= kGpuTimePairs)
    return;
  if (!g_gpu_time_pool[slot]) {
    auto *device = Video::HostDevice();
    if (!device)
      return;
    g_gpu_time_pool[slot] = device->createQueryPool(2 * kGpuTimePairs).release();
  }
  s.command_list->writeTimestamp(g_gpu_time_pool[slot], 2 * g_gpu_time_pairs[slot]);
  g_gpu_time_open[slot] = true;
}

// After the slot's fence (every list of the frame has finished), or with nothing submitted.
void SvrGpuTimeRead(u32 slot, bool submitted) {
  if (submitted && g_gpu_time_pairs[slot]) {
    g_gpu_time_pool[slot]->queryResults();
    const u64 *ns = g_gpu_time_pool[slot]->getResults();
    u64 sum = 0;
    for (u32 i = 0; i < g_gpu_time_pairs[slot]; ++i)
      if (ns[2 * i + 1] > ns[2 * i])
        sum += ns[2 * i + 1] - ns[2 * i];
    SvrPerfGpuTime(sum);
  }
  g_gpu_time_pairs[slot] = 0;
  g_gpu_time_open[slot] = false;
}

// NR-14c: lists submitted mid-frame (SvrSplitListIfDueLocked), kept until the slot's fence has
// passed, then reused. Render thread, under s.mutex.
std::vector<std::unique_ptr<plume::RenderCommandList>> g_split_busy[kNumFrames];
std::vector<std::unique_ptr<plume::RenderCommandList>> g_split_spare[kNumFrames];
u32 g_split_count[kNumFrames] = {};
std::chrono::steady_clock::time_point g_list_opened;

// After a wait on the slot's fence: every list submitted for it before has finished.
void ReclaimSplitLists(u32 slot) {
  for (auto &list : g_split_busy[slot])
    g_split_spare[slot].push_back(std::move(list));
  g_split_busy[slot].clear();
}

}  // namespace

// Before every submit of the slot's open list.
void SvrGpuTimeEnd(u32 slot) {
  if (!g_gpu_time_open[slot])
    return;
  state().command_list->writeTimestamp(g_gpu_time_pool[slot], 2 * g_gpu_time_pairs[slot] + 1);
  ++g_gpu_time_pairs[slot];
  g_gpu_time_open[slot] = false;
}

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  if (!s.pipeline_layout)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
#if defined(SVR_NATIVE_RENDERER)
  s.command_list = SvrRecordingList(cur, s.command_lists[cur].get());
#else
  s.command_list = s.command_lists[cur].get();
#endif
  s.draw_framebuffer_bound = false;
  s.last_resolved_dst = nullptr;
  s.command_list->begin();
  g_list_opened = std::chrono::steady_clock::now();
  SvrGpuTimeBegin(s, cur);
  if (!s.null_texture_barriers_submitted) {
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
      barriers[i] = plume::RenderTextureBarrier(
          s.null_textures[i].get(), plume::RenderTextureLayout::SHADER_READ);
    }
    s.command_list->barriers(plume::RenderBarrierStage::NONE, barriers,
                             kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  s.command_list_open = true;
  s.dirtyStates.vertexStreamFirst = 0;
  s.dirtyStates.vertexStreamLast = 15;
  s.dirtyStates.indices = true;
  s.dirtyStates.pipelineState = true;
  s.dirtyStates.vertexShaderConstants = true;
  s.dirtyStates.pixelShaderConstants = true;
  s.current_pso = nullptr;
  InvalidateSharedBinding();
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
  SetGPUProfilerCommandList(
      static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
}

void Video::OpenCommandList() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  BeginCommandList(s);
}

void Video::OpenCommandListLocked() { BeginCommandList(state()); }

plume::RenderCommandList *Video::CommandList() {
  return state().command_list;
}

namespace {
std::mutex g_destroy_pending_mutex;
std::unordered_set<u32> g_destroy_pending;
} // namespace

void Video::QueueResourceDestroy(u32 guest_va, ResourceType type) {
  if (type == ResourceType::VertexShader || type == ResourceType::PixelShader ||
      type == ResourceType::VertexDeclaration) {
    return;
  }
  auto &s = state();
  {
    std::lock_guard lock(g_destroy_pending_mutex);
    if (!g_destroy_pending.insert(guest_va).second)
      return;
  }
  if (type == ResourceType::RenderTarget ||
      type == ResourceType::DepthStencil) {
    auto *surface = HostResourceHeap::FromGuest<GuestTexture>(guest_va);
    if (surface && surface->registered) {
      std::lock_guard lock(g_destroy_pending_mutex);
      g_destroy_pending.erase(guest_va);
      return;
    }
  }
  s.deferred_destroy[Video::RetireSlot("guest resource")].Queue(guest_va, type);
}

u32 Video::CurrentFrameSlot() {
  return state().frame.load(std::memory_order_relaxed);
}

u32 Video::RetireSlot(const char *what) {
  auto &s = state();
  const u32 slot = s.frame.load(std::memory_order_relaxed);
  if (static_cast<i32>(slot) ==
      s.reclaiming_slot.load(std::memory_order_relaxed)) {
    static std::atomic<u32> s_hits{0};
    const u32 n = s_hits.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 || (n & 0xFF) == 0) {
      BD_WARN("[retire-race] #{} {} parked into slot {} while it is being "
              "reclaimed, freed with no fence covering it",
              n, what, slot);
    }
  }
  return slot;
}

namespace {

constexpr size_t kTextureFreesPerDrain = 24;
constexpr size_t kTextureBacklogFlushAll = 512;

void DrainTextureBacklog(VideoState &s) {
  std::vector<std::unique_ptr<plume::RenderTexture>> batch;
  {
    std::lock_guard lock(s.mutex);
    const size_t pending = s.texture_free_backlog.size();
    if (!pending)
      return;
    if (pending > kTextureBacklogFlushAll) {
      static std::atomic<u32> s_logged{0};
      if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        BD_WARN("texture free backlog reached {}, flushing it in one drain",
                pending);
      }
    }
    const size_t take =
        pending > kTextureBacklogFlushAll
            ? pending
            : (pending < kTextureFreesPerDrain ? pending
                                               : kTextureFreesPerDrain);
    batch.reserve(take);
    for (size_t i = 0; i < take; ++i) {
      batch.push_back(std::move(s.texture_free_backlog[i]));
    }
    const auto first = s.texture_free_backlog.begin();
    s.texture_free_backlog.erase(
        first, first + static_cast<std::ptrdiff_t>(take));
  }
  BD_CPU_ZONE("DrainTextureBacklog");
  batch.clear();
}

} // namespace

void DrainSlot(VideoState &s, u32 slot) {
  {
    std::lock_guard lock(s.mutex);
    s.texture_view_graveyard[slot].clear();
    for (auto &tex : s.texture_graveyard[slot])
      s.texture_free_backlog.push_back(std::move(tex));
    s.texture_graveyard[slot].clear();
    s.buffer_graveyard[slot].clear();
    DrainDescriptorSlotsLocked(s, slot);
    s.reclaiming_slot.store(-1, std::memory_order_relaxed);
  }
  DrainTextureBacklog(s);
  SurfaceRegistry::Tick();
  auto dead = s.deferred_destroy[slot].Take();
  for (const auto &d : dead) {
    if (d.type == ResourceType::Texture ||
        d.type == ResourceType::VolumeTexture ||
        d.type == ResourceType::RenderTarget ||
        d.type == ResourceType::DepthStencil) {
      if (auto *t = HostResourceHeap::FromGuest<GuestTexture>(d.guest_va)) {
        t->pendingDestroy = true;
      }
    }
  }
  {
    BD_CPU_ZONE("DestroyResources");
    for (const auto &d : dead) {
      {
        std::lock_guard lock(g_destroy_pending_mutex);
        g_destroy_pending.erase(d.guest_va);
      }
      DestroyResourceNow(d.guest_va, d.type);
    }
  }
  {
    BD_CPU_ZONE("DrainNativeMirrors");
    DrainEvictedNativeTextures(slot);
  }
  {
    BD_CPU_ZONE("DrainPhysGraveyard");
    DrainBufferGraveyard(slot);
  }
}

void AdvanceAndWaitReused(VideoState &s) {
  const u32 slot = s.next_frame;
  s.reclaiming_slot.store(static_cast<i32>(slot), std::memory_order_relaxed);
  s.frame.store(slot, std::memory_order_relaxed);
  s.next_frame = (slot + 1) % kNumFrames;
#if defined(SVR_NATIVE_RENDERER)
  s.command_list = SvrRecordingList(slot, s.command_lists[slot].get());
#else
  s.command_list = s.command_lists[slot].get();
#endif
  const bool submitted = s.command_list_submitted[slot];
  if (submitted) {
    {
#if defined(SVR_NATIVE_RENDERER)
      SvrDiagTimer gpu_wait(SvrDiag::kGpuWait);
      SvrZone zone_fence(SvrZoneId::kFenceWait);
#endif
      s.queue->waitForCommandFence(s.fences[slot].get());
    }
    s.command_list_submitted[slot] = false;
#if defined(REXGLUE_ENABLE_PROFILING) && defined(REBLUE_D3D12)
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  }
  // The slot's last frame ended in a fenced submit after its split lists, and that fence was waited
  // for here or by an earlier full wait.
  SvrGpuTimeRead(slot, submitted);
  ReclaimSplitLists(slot);
  g_split_count[slot] = 0;
  ResetFrame(slot);
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SvrGpuTimeEnd(cur);
#if defined(SVR_NATIVE_RENDERER)
  // Threaded recording: waits for the worker to replay the rest, then ends the real list.
  SvrRecordingList(cur, s.command_lists[cur].get())->end();
#else
  s.command_lists[cur]->end();
#endif
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
}

bool SvrFlushAndWaitLocked(VideoState &s) {
  if (!s.command_list_open)
    return false;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  SubmitOpenListLocked(s);
  {
#if defined(SVR_NATIVE_RENDERER)
    SvrDiagTimer gpu_wait(SvrDiag::kGpuWait);
#endif
    s.queue->waitForCommandFence(s.fences[cur].get());
  }
  s.command_list_submitted[cur] = false;
  ReclaimSplitLists(cur);
  BeginCommandList(s);
  return s.command_list_open;
}

void SvrSplitListIfDueLocked(VideoState &s, i32 interval_us) {
  if (interval_us <= 0 || !s.command_list_open || s.occlusion_counting)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  if (g_split_count[cur] >= kMaxListSplits)
    return;
  // BeginCommandList's own conditions: the slot must reopen, or nothing would fence the split.
  if (s.shutting_down.load(std::memory_order_acquire) || !s.pipeline_layout)
    return;
  if (std::chrono::steady_clock::now() - g_list_opened < std::chrono::microseconds(interval_us))
    return;
  SvrGpuTimeEnd(cur);
#if defined(SVR_NATIVE_RENDERER)
  SvrZone zone_submit(SvrZoneId::kSubmit);
  SvrRecordingList(cur, s.command_lists[cur].get())->end();
#else
  s.command_lists[cur]->end();
#endif
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, nullptr);
  g_split_busy[cur].push_back(std::move(s.command_lists[cur]));
  if (!g_split_spare[cur].empty()) {
    s.command_lists[cur] = std::move(g_split_spare[cur].back());
    g_split_spare[cur].pop_back();
  } else {
    s.command_lists[cur] = s.queue->createCommandList();
  }
  ++g_split_count[cur];
  BeginCommandList(s);
}

u32 SvrListSplits() { return g_split_count[state().frame.load(std::memory_order_relaxed)]; }

double SvrListOpenMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_list_opened)
      .count();
}

} // namespace bd::gpu
