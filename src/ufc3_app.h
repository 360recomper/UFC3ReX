// ufc3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

#include "ufc3_paths.h"
#include "mod_pack.h"
#include "native/ufc_native.h"
#include <atomic>
#include <thread>
#if defined(UFC3_NATIVE_HOOKS)
#include <atomic>
#include <cstdlib>
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
void SvrInstallCrashTrace(const char* trace_dir);  // src/native/crash_trace.cpp
uint64_t SvrSetTestCpuMask(uint64_t mask);          // src/native/display_size.cpp
bool SvrCpuName(char* name, uint32_t size, uint32_t& logical);  // src/native/display_size.cpp
namespace bd::gpu {
void SvrTestGpuClocks(unsigned int gpu_mhz, unsigned int mem_mhz);  // src/native/ufc_frame_diag.cpp
}
#endif

// merge7 (user 2026-11-08): native is the default; emulated stays as the fallback.
REXCVAR_DEFINE_STRING(ufc3_renderer, "native", "ufc3",
                      "Renderer: native (plume D3D12, default) or emulated (Xenos GPU emulation, "
                      "the fallback; read once at startup)");
// NR-8b-5 self-tests: the launcher sets the renderer only in ufc3.toml next to the exe, which a
// self-test copy doesn't have. This loads a second toml the same way (command line still wins).
REXCVAR_DEFINE_STRING(ufc3_test_config, "", "ufc3",
                      "Test only: extra .toml loaded like ufc3.toml before the renderer is chosen");
REXCVAR_DEFINE_BOOL(ufc3_native_test_device_fail, false, "ufc3",
                    "Test only: native acts as if the host device failed (exit code 3)");
REXCVAR_DEFINE_STRING(ufc3_test_cpu_mask, "", "ufc3",
                      "Test only (native): hex mask of logical CPUs the process may use, a "
                      "weaker-CPU stand-in (NR-14e); empty = all");
REXCVAR_DEFINE_INT32(ufc3_test_gpu_mhz, 0, "ufc3",
                     "Test only (native, NVIDIA, needs admin): lock the graphics clock to this "
                     "many MHz for the run, a weaker-GPU stand-in (NR-15); 0 = off");
REXCVAR_DEFINE_INT32(ufc3_test_gpu_mem_mhz, 0, "ufc3",
                     "Test only (native, NVIDIA, needs admin): lock the memory clock to this many "
                     "MHz for the run (NR-15); 0 = off");

// Vulkan is the default (v1.0.1): with hoisted shared-memory uploads it holds
// 60 and it doesn't black out while a fighter is rocked, which D3D12 does.
// Without the hoist, Vulkan ends the render pass for every small upload (this
// game streams vertex data a page per draw) and drops to ~47 FPS at scale 3.
REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "GPU",
                      "Graphics backend: vulkan or d3d12");
REXCVAR_DEFINE_BOOL(ufc3_vulkan_hoist, true, "ufc3",
                    "Vulkan: batch shared-memory uploads at the start of each submission "
                    "(sets vulkan_shared_memory_hoist_uploads; fixes the black screen while rocked)");
REXCVAR_DEFINE_BOOL(ufc3_blood_guest_pixel, true, "ufc3",
                    "Resolution scale: run the GPU blood simulation and mat stain passes at guest "
                    "pixel centers (sets guest_pixel_center_pixel_shaders; stops blood sliding "
                    "across the mat at resolution_scale 2+)");

// The blood droplets are simulated on the GPU (yVParticleMap: a few texels of a
// 16_16_16_16_FLOAT map per droplet, updated every frame) and painted
// into a mat-space stain map that yVStainEffect then filters with 3x3 taps.
// Both sample the previous map at the interpolated texcoord; with resolution
// scaling the host pixels of a guest texel interpolate it off the guest pixel
// center, some taps land a host texel into the next particle or stain texel, and
// the maps drift every frame. Ucode hashes of those passes' pixel shaders:
inline constexpr const char* kUfc3BloodMapPixelShaders =
    // yVParticleMap (particle state update, collision, spawn).
    "D51BE16D6FD8EA5D,325C1C523EDA1AD3,5A19DF07A479BF61,0FB730849209964D,"
    "CB89AA014AB83634,198330C60B7EA7ED,DA69A0CA56065670,"
    // yVStainEffect (stain map filters).
    "A386FAFC1B0204BD,9E3DE715730B6042";

// save_guard.h (crash-safe saves): defined there, included after this header in main.cpp.
namespace ufc3_save {
inline bool SaveInProgress();
inline bool WaitForSaves(int ms);
}  // namespace ufc3_save
REXCVAR_DECLARE(int32_t, ufc3_save_close_wait_ms);

class Ufc3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ufc3App>(new Ufc3App(ctx, "ufc3",
        PPCImageConfig));
  }

  // Closing the window (X, Alt+F4, the harness's WM_CLOSE) while the game is writing a save:
  // the SDK's close path stops the guest threads and hard-exits at once, which used to cut the
  // save off half written ("game data is damaged" next boot). Wait for the save to finish first.
  bool OnWindowCloseRequested() override {
    if (!ufc3_save::SaveInProgress()) return true;
    if (close_waiting_.exchange(true)) return false;
    const int wait_ms = REXCVAR_GET(ufc3_save_close_wait_ms);
    REXLOG_WARN("[save] window close requested during a save; closing when it is written (max {} ms)",
                wait_ms);
    std::thread([this, wait_ms] {
      const bool done = ufc3_save::WaitForSaves(wait_ms);
      REXLOG_WARN("[save] {}; closing now", done ? "save finished" : "save still running after the wait");
      app_context().CallInUIThread([this] {
        if (window()) window()->RequestClose();
      });
    }).detach();
    return false;
  }
  std::atomic<bool> close_waiting_{false};

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // SDK v0.10 no longer defaults the game mount; use the assets folder next
    // to the exe unless --game_data_root was given.
    if (paths.game_data_root.empty()) {
      auto assets_dir = rex::filesystem::GetExecutableFolder() / "assets";
      if (std::filesystem::is_directory(assets_dir)) {
        paths.game_data_root = assets_dir;
      }
    }
    // Fresh release folder (no ufc3.toml paths yet): saves in userdata\ and
    // the mods overlay in mods\, both next to the exe. The SDK's defaults are
    // the per-user app folder and ..\mods, which are not where the release
    // folder keeps them. Explicit cvars/toml paths still win.
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    if (rex::cvar::GetFlagByName("user_data_root").empty() &&
        !std::filesystem::is_directory(exe_dir / ".." / "saves", ec)) {
      bool cache_was_default = paths.cache_root == paths.user_data_root / "cache";
      paths.user_data_root = exe_dir / "userdata";
      if (cache_was_default) paths.cache_root = paths.user_data_root / "cache";
    }
    if (rex::cvar::GetFlagByName("mods_root").empty() &&
        std::filesystem::is_directory(exe_dir / "mods", ec)) {
      // Cvars are UTF-8: path::string() is the ANSI codepage, and the SDK's
      // to_path() threw utf8::invalid_utf8 on it (GitHub #3: a Turkish "Desktop" folder).
      rex::cvar::SetFlagByName("mods_root", rex::path_to_utf8(exe_dir / "mods"));
    }
    ufc3_paths::ResolvedGameDataRoot() = paths.game_data_root;
    ufc3_paths::ResolvedUserDataRoot() = paths.user_data_root;
  }

  // ufc3_renderer is read once, here; nothing switches renderers later (src/native/ufc_native.h).
  static void LatchRenderer() {
    const std::string& renderer = REXCVAR_GET(ufc3_renderer);
    g_ufc3_native = false;
    if (renderer == "native") {
#if defined(UFC3_NATIVE_HOOKS)
      // No shader cache beside the exe (ufc3_shader_cache, relative = beside the exe): native would
      // draw nothing, so a plain double-click before the launcher built the cache runs emulated.
      std::filesystem::path cache = rex::to_path(rex::cvar::GetFlagByName("ufc3_shader_cache"));
      if (cache.empty()) cache = "shader_cache.bin";
      if (cache.is_relative()) cache = rex::filesystem::GetExecutableFolder() / cache;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(cache, ec)) {
        REXLOG_WARN("native: no shader cache at {} (the launcher builds it, or run ufc3_shader_cache.exe), "
                    "using the emulated renderer", rex::path_to_utf8(cache));
        return;
      }
      g_ufc3_native = true;
      REXLOG_INFO("native: renderer on (experimental, plume D3D12)");
#elif defined(UFC3_NATIVE_RENDERER)
      // Census build (UFC3_NATIVE_HOOKS off): no native hooks compiled.
      REXLOG_WARN("native: not implemented, using the emulated renderer");
#else
      REXLOG_WARN("native: this build has no native renderer, using the emulated renderer");
#endif
    } else if (renderer != "emulated") {
      REXLOG_WARN("ufc3_renderer '{}' unknown, using the emulated renderer", renderer);
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    const std::string& test_config = REXCVAR_GET(ufc3_test_config);
    if (!test_config.empty()) {
      rex::cvar::LoadConfig(rex::to_path(test_config));
      REXLOG_INFO("test config {} loaded, ufc3_renderer = {}", test_config,
                  REXCVAR_GET(ufc3_renderer));
    }
    LatchRenderer();
    ufc3_pack::Latch();  // mod pack marker: stock or pack mode, before any gated cvar is read (src/mod_pack.h)
#if defined(UFC3_NATIVE_RENDERER)
    if (g_ufc3_native) {
      // Native renderer: the Xenos plugin's null backend keeps the game's D3D command stream
      // moving (fences, interrupts, vblank) without drawing; the hooks draw through plume.
      // NR-8: needs the SDK's null backend (SvR's rexglue-sdk.patch, graphics/null).
      if (!config.graphics) {
        config.graphics = rex::system::LoadGpuPlugin("xenos", "null");
        config.gpu_plugin = "xenos";
        REXLOG_INFO("native: guest GPU {}", config.graphics ? "null (sync only)" : "MISSING");
      }
      return;
    }
#endif
    // Xenos GPU emulation is a runtime-loaded plugin in v0.10; an empty
    // gpu_plugin cvar means no GPU at all.
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin("xenos", REXCVAR_GET(gpu_backend));
      if (!config.graphics) {
        config.gpu_plugin = "xenos";
      }
    }
    // Yuke's engine samples textures through fetch constants typed "invalid";
    // the 360 samples them anyway, the emulator default treats them as unbound,
    // which leaves the 3D fight scene black under the 2D overlay. The flag lives
    // in the GPU plugin, so set it after the plugin is loaded.
    if (!rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true")) {
      REXLOG_WARN("could not enable gpu_allow_invalid_fetch_constants");
    }
    if (REXCVAR_GET(ufc3_vulkan_hoist) &&
        !rex::cvar::SetFlagByName("vulkan_shared_memory_hoist_uploads", "true")) {
      REXLOG_WARN("could not enable vulkan_shared_memory_hoist_uploads");
    }
    if (REXCVAR_GET(ufc3_blood_guest_pixel) &&
        rex::cvar::GetFlagByName("guest_pixel_center_pixel_shaders").empty() &&
        !rex::cvar::SetFlagByName("guest_pixel_center_pixel_shaders", kUfc3BloodMapPixelShaders)) {
      REXLOG_WARN("could not set guest_pixel_center_pixel_shaders");
    }
  }

#if defined(UFC3_NATIVE_HOOKS)
  // Exit code when the native device can't be created: the launcher restarts the game with
  // --ufc3_renderer=emulated (nothing is written to ufc3.toml).
  static constexpr int kNativeDeviceFailedExitCode = 3;

  // Both return the SDK defaults on the emulated renderer. Native (from SvR's svr2010_app.h): the
  // plume device draws the ImGui overlays (FPS, settings) at present time.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (!g_ufc3_native)
      return nullptr;
    auto drawer = std::make_unique<bd::gpu::ImGuiOverlayDrawer>();
    overlay_drawer_ = drawer.get();
    return drawer;
  }

  void OnPreLaunchModule() override {
    if (!g_ufc3_native)
      return;
    // SvR's crash/hang tracer (stack dump on a fault, logs\stderr.txt, hang watchdog).
    // Traces go beside the game's log file (a farm run copies that folder back), else beside the exe.
    {
      std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
      const std::string log_file = rex::cvar::GetFlagByName("log_file");
      if (!log_file.empty()) {
        std::filesystem::path parent = std::filesystem::path(log_file).parent_path();
        if (!parent.empty()) dir = parent;
      }
      SvrInstallCrashTrace(dir.string().c_str());
      REXLOG_INFO("native: crash and hang traces go to {}", dir.string());
    }
    {
      char cpu[128] = {};
      uint32_t logical = 0;
      if (SvrCpuName(cpu, sizeof(cpu), logical))
        REXLOG_INFO("native: CPU {}, {} logical", cpu, logical);
    }
    if (const std::string& cpu_mask = REXCVAR_GET(ufc3_test_cpu_mask); !cpu_mask.empty()) {
      const uint64_t set = SvrSetTestCpuMask(std::strtoull(cpu_mask.c_str(), nullptr, 16));
      REXLOG_INFO("native: test CPU mask {} -> {:X}", cpu_mask, set);
    }
    bd::gpu::SvrTestGpuClocks(uint32_t(REXCVAR_GET(ufc3_test_gpu_mhz)),
                              uint32_t(REXCVAR_GET(ufc3_test_gpu_mem_mhz)));
    if (REXCVAR_GET(ufc3_native_test_device_fail) || !bd::gpu::Video::CreateHostDevice(window())) {
      REXLOG_ERROR("native: host device creation failed; restart with --ufc3_renderer=emulated");
      rex::FlushLogging();
      std::_Exit(kNativeDeviceFailedExitCode);
    }
    // Present runs on the guest thread and ImGui on the UI thread: the UI thread records the
    // overlay in the background and Present replays the latest recording; only the first frame,
    // a resize or a new font texture draws directly on the UI thread (SvR).
    bd::gpu::Video::SetOverlayDrawHook([this](plume::RenderCommandList* cmd,
                                              plume::RenderFramebuffer* fb, uint32_t w,
                                              uint32_t h) {
      auto* drawer = overlay_drawer_;
      if (!imgui_drawer() || !imgui_drawer()->HasDialogs()) {
        if (drawer)
          drawer->ClearCapture();
        return;
      }
      if (!drawer || drawer->NeedsDirectDraw() || !drawer->ReplayLatest(cmd, fb, w, h)) {
        app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
          bd::gpu::ReblueUIDrawContext ctx(w, h, cmd, fb);
          imgui_drawer()->Draw(ctx);
        });
      }
      if (drawer && !overlay_capture_pending_.exchange(true)) {
        app_context().CallInUIThread([this, w, h] {
          if (imgui_drawer() && overlay_drawer_)
            overlay_drawer_->Capture(
                w, h, [this](rex::ui::UIDrawContext& ctx) { imgui_drawer()->Draw(ctx); });
          overlay_capture_pending_.store(false);
        });
      }
    });
  }

  void OnWindowPixelSizeChanged(uint32_t w, uint32_t h) override {
    if (!g_ufc3_native) {
      rex::ReXApp::OnWindowPixelSizeChanged(w, h);
      return;
    }
    bd::gpu::Video::RequestResize();
  }

  void OnShutdown() override { overlay_drawer_ = nullptr; }

 private:
  bd::gpu::ImGuiOverlayDrawer* overlay_drawer_ = nullptr;  // owned by ReXApp's immediate drawer
  std::atomic<bool> overlay_capture_pending_{false};
#endif
};
