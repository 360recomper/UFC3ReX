// Pipelines compiled ahead of use, so they don't stall a frame.
//
// Without this every pipeline the game needs is compiled on the render thread the first time it
// is drawn: up to ~60 in a second when a new scene starts, which shows as frame drops (heavier on
// NVIDIA's driver, whose compiles are slower). Each pipeline compiled that way is appended to
// pipelines.bin next to the executable, keyed by content hashes of its shaders and vertex
// declaration, which stay the same between runs. On the next start the stored pipelines go into
// pso_recorder's replay list, which resolves each one as soon as its shaders and declaration
// exist (usually while the scene loads) and hands it to the background compiler here. Draws are
// never skipped: a pipeline the background compiler hasn't finished yet still compiles on the
// render thread as before, so nothing can go missing (SvR renders some textures only once).
//
// NR-9h: pipelines.bin starts empty on a new install, and a farm run's folder is new every time, so
// every farm run so far compiled everything on the render thread. pipelines_seed.bin is a read-only
// list that travels with the build, beside shader_cache.bin (tools/nr_pso_seed.py makes it from
// the rec= fields of test.log). ufc3_native_pso_seed=false ignores it (NR-B3 A/B: 543 render-thread misses a run -> 4).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include "core/logging.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_precache.h"

REXCVAR_DEFINE_BOOL(svr_pipeline_precache, true, "SvR",
                    "Compile pipelines the game used before in the background as their shaders "
                    "load (pipelines.bin), instead of during a frame");
REXCVAR_DEFINE_BOOL(ufc3_native_pso_seed, true, "ufc3",
                    "Native renderer speed patch (NR-9h, on since NR-B3's A/B): precompile the pipeline "
                    "seed list (pipelines_seed.bin beside the shader cache), and blank vertex strides in "
                    "the pipeline key so its records match; false = the old path");
REXCVAR_DEFINE_STRING(ufc3_pipeline_seed, "", "ufc3",
                      "Native renderer: read-only list of pipelines to precompile (empty = "
                      "pipelines_seed.bin beside ufc3_shader_cache)");
REXCVAR_DECLARE(std::string, ufc3_shader_cache);

namespace bd::gpu {

extern const unsigned char kEmbeddedPipelineSeed[];
extern const std::size_t kEmbeddedPipelineSeedSize;

namespace {

constexpr u32 kMagic = 0x50525653;  // "SVRP"
constexpr u32 kVersion = 2;  // 2: strides blanked in the key (NR-9h)

struct FileHeader {
  u32 magic = kMagic;
  u32 version = kVersion;
  u32 record_size = sizeof(PipelineState);
  u32 reserved = 0;
};

std::filesystem::path StorePath() {
  return rex::filesystem::GetExecutableFolder() / "pipelines.bin";
}

std::filesystem::path SeedPath() {
  const std::string chosen = REXCVAR_GET(ufc3_pipeline_seed);
  std::filesystem::path path = std::filesystem::u8path(chosen);
  if (chosen.empty()) {
    path = std::filesystem::u8path(REXCVAR_GET(ufc3_shader_cache));
    path.replace_filename("pipelines_seed.bin");
  }
  return path.is_relative() ? rex::filesystem::GetExecutableFolder() / path : path;
}

// The stored form: the pipeline with content hashes in its shader and declaration slots, and the
// device-chosen formats in their neutral form (SanitizePipelineState maps them back on load):
// depth-stencil D32S8 (D3D12 picks D24S8 on NVIDIA / Intel) and the scene's RGBA16F, so a store
// recorded on one PC or graphics API serves every other.
PipelineState Stored(const PipelineState &state, u64 vs, u64 ps, u64 decl) {
  PipelineState s = state;
  if (plume::RenderFormatIsStencil(s.depthStencilFormat))
    s.depthStencilFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
  if (s.renderTargetFormat == plume::RenderFormat::R11G11B10_FLOAT)
    s.renderTargetFormat = plume::RenderFormat::R16G16B16A16_FLOAT;
  s.vertexShader = reinterpret_cast<GuestShader *>(vs);
  s.pixelShader = reinterpret_cast<GuestShader *>(ps);
  s.vertexDeclaration = reinterpret_cast<GuestVertexDeclaration *>(decl);
  return s;
}

std::mutex g_store_mutex;
std::unordered_set<u64> g_stored;  // keys of stored records (hash of the stored form)

// --- background compiler ----------------------------------------------------------------------
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<PipelineState> g_queue;
std::unordered_set<u64> g_queued;  // live keys ever queued, so each pipeline is compiled once
std::once_flag g_workers_once;
std::atomic<u32> g_compiled{0};

void Worker() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"SvR pipeline precompile");
#endif
  for (;;) {
    PipelineState state;
    {
      std::unique_lock lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return !g_queue.empty(); });
      state = g_queue.front();
      g_queue.pop_front();
    }
    bool built = false;
    GetOrCreatePipeline(state, &built);
    if (built) {
      const u32 n = g_compiled.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n == 1 || n % 100 == 0)
        BD_INFO("pso: {} pipelines precompiled in the background", n);
    }
  }
}

void StartWorkers() {
  std::call_once(g_workers_once, [] {
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned count = std::clamp(hw / 4, 1u, 3u);
    for (unsigned i = 0; i < count; ++i)
      std::thread(Worker).detach();  // process exit is a hard kill, never joined
    BD_INFO("pso: {} background pipeline compile thread(s)", count);
  });
}

void Enqueue(const PipelineState &state, bool front) {
  if (!REXCVAR_GET(svr_pipeline_precache))
    return;
  StartWorkers();
  {
    std::lock_guard lock(g_queue_mutex);
    if (!g_queued.insert(HashPipelineState(state)).second)
      return;
    if (front)
      g_queue.push_front(state);
    else
      g_queue.push_back(state);
  }
  g_queue_cv.notify_one();
}

}  // namespace

bool PrecacheEnabled() { return REXCVAR_GET(svr_pipeline_precache); }
void EnqueuePipeline(const PipelineState &state) { Enqueue(state, false); }
void EnqueuePipelinePriority(const PipelineState &state) { Enqueue(state, true); }

namespace {

// Adds the file's records not seen yet to out. False: the file is there but from another version.
// Caller holds g_store_mutex.
bool ReadRecords(const std::filesystem::path &path, std::vector<PipelineState> &out) {
  FILE *f = rex::filesystem::OpenFile(path, "rb");
  if (!f)
    return true;
  FileHeader header;
  const bool ok = std::fread(&header, sizeof(header), 1, f) == 1 && header.magic == kMagic &&
                  header.version == kVersion && header.record_size == sizeof(PipelineState);
  if (ok) {
    PipelineState s;
    while (std::fread(&s, sizeof(s), 1, f) == 1) {
      if (g_stored.insert(HashPipelineState(s)).second)
        out.push_back(s);
    }
  }
  std::fclose(f);
  return ok;
}

// The seed compiled into the exe (v1.1.1, cmake/native_renderer.cmake): launchers <= 1.1.0 update only
// exe/dll/txt/md files, so an auto-update never brought pipelines_seed.bin. Same format as the file.
bool ReadEmbeddedRecords(std::vector<PipelineState> &out) {
  if (kEmbeddedPipelineSeedSize < sizeof(FileHeader))
    return false;
  FileHeader header;
  std::memcpy(&header, kEmbeddedPipelineSeed, sizeof(header));
  if (header.magic != kMagic || header.version != kVersion || header.record_size != sizeof(PipelineState))
    return false;
  for (size_t at = sizeof(header); at + sizeof(PipelineState) <= kEmbeddedPipelineSeedSize;
       at += sizeof(PipelineState)) {
    PipelineState s;
    std::memcpy(&s, kEmbeddedPipelineSeed + at, sizeof(s));
    if (g_stored.insert(HashPipelineState(s)).second)
      out.push_back(s);
  }
  return true;
}

}  // namespace

// Stored pipelines for pso_recorder's replay list (ReplayBootCache), hashes in the pointer slots.
std::vector<PipelineState> SvrLoadPipelineStore() {
  std::vector<PipelineState> out;
  std::lock_guard lock(g_store_mutex);
  if (REXCVAR_GET(ufc3_native_pso_seed)) {
    // A seed file beside the shader cache wins (a newer seed without a new exe); else the embedded one.
    const auto seed = SeedPath();
    std::error_code ec;
    bool used = false;
    if (std::filesystem::exists(seed, ec)) {
      if (ReadRecords(seed, out))
        used = true;
      else
        BD_WARN("pso: {} is from another version, not used", seed.string());
    }
    if (used) {
      BD_INFO("pso: seed: file {} ({} pipelines)", seed.string(), out.size());
    } else if (ReadEmbeddedRecords(out)) {
      BD_INFO("pso: seed: embedded ({} pipelines, {} bytes)", out.size(), kEmbeddedPipelineSeedSize);
    } else {
      BD_WARN("pso: seed: none (embedded seed is from another version)");
    }
  }
  const size_t from_seed = out.size();
  if (!ReadRecords(StorePath(), out)) {
    std::error_code ec;
    std::filesystem::remove(StorePath(), ec);
    BD_INFO("pso: pipelines.bin is from another version; starting a new one");
  }
  // Logged at zero too: an empty list is why every farm run compiled on the render thread.
  BD_INFO("pso: {} pipelines from the seed list, {} from pipelines.bin; each compiles in the "
          "background once its shaders load",
          from_seed, out.size() - from_seed);
  return out;
}

// The record of a pipeline as the store keeps it, in hex, for the log line of a miss.
std::string SvrPipelineRecordHex(const PipelineState &state, u64 vs, u64 ps, u64 decl) {
  static const char kDigits[] = "0123456789ABCDEF";
  const PipelineState s = Stored(state, vs, ps, decl);
  const auto *bytes = reinterpret_cast<const unsigned char *>(&s);
  std::string hex;
  hex.reserve(sizeof(s) * 2);
  for (size_t i = 0; i < sizeof(s); ++i) {
    hex.push_back(kDigits[bytes[i] >> 4]);
    hex.push_back(kDigits[bytes[i] & 15]);
  }
  return hex;
}

// A pipeline just compiled on the render thread: store it for the next start. True if the store
// already had it, so its background compile hadn't finished.
bool SvrRecordPipeline(const PipelineState &state, u64 vs, u64 ps, u64 decl) {
  if (!vs || !REXCVAR_GET(svr_pipeline_precache))
    return false;
  const PipelineState s = Stored(state, vs, ps, decl);
  std::lock_guard lock(g_store_mutex);
  if (!g_stored.insert(HashPipelineState(s)).second)
    return true;
  const auto path = StorePath();
  std::error_code ec;
  const bool fresh = !std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0;
  FILE *f = rex::filesystem::OpenFile(path, fresh ? "wb" : "ab");
  if (!f)
    return false;
  if (fresh) {
    const FileHeader header;
    std::fwrite(&header, sizeof(header), 1, f);
  }
  std::fwrite(&s, sizeof(s), 1, f);
  std::fclose(f);
  return false;
}

}  // namespace bd::gpu
