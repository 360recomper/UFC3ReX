// ufc3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

#include "ufc3_paths.h"

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

class Ufc3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ufc3App>(new Ufc3App(ctx, "ufc3",
        PPCImageConfig));
  }

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
      rex::cvar::SetFlagByName("mods_root", (exe_dir / "mods").string());
    }
    ufc3_paths::ResolvedGameDataRoot() = paths.game_data_root;
    ufc3_paths::ResolvedUserDataRoot() = paths.user_data_root;
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
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
};
