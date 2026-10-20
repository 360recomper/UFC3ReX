// ufc3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>

// D3D12 is the default: at high resolution_scale the Vulkan backend ends the
// render pass for every small shared-memory upload (this game streams vertex
// data a page per draw), which drops scale 3 to ~47 FPS; D3D12 holds 60.
REXCVAR_DEFINE_STRING(gpu_backend, "d3d12", "GPU",
                      "Graphics backend: vulkan or d3d12");

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
  }
};
