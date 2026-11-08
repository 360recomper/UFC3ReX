# Building from source

Windows 10/11 x64. You need `default.xex` and the game files from your own copy of the game.

Tools: Visual Studio 2022 Build Tools (C++ workload + Windows SDK), LLVM/Clang, CMake, Ninja, Git,
Python 3, .NET 8 SDK (launcher only). Edit `env.bat` so it points at them.

## 1. SDK (ReXGlue nightly-20261002 + our patch)

```bat
git clone https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
git checkout nightly-20261002-bd833a2a
git submodule update --init --recursive
git apply ..\UFC3ReX\sdk-patch\rexglue-sdk-nightly-20261002-ufc3-fixes.patch
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_FIDELITYFX_CAS_ONLY=ON
cmake --build --preset win-amd64-release --target install
cmake --build --preset win-amd64-relwithdebinfo --target install
```

The patch adds the mods overlay fix, the F4 quick settings entries, Vulkan upload batching (fixes the
black screen while a fighter is rocked on Vulkan), the XMA final-frame fix (ring intros no longer stall),
cvar/config fixes, the scaled-texture and readback fixes, scripted input for self-tests, UTF-8 paths,
the null guest GPU backend the native renderer runs on, and some diagnostics.

Set `UFC3_SDK` to the SDK's install folder (`out\install\win-amd64`), or put it at `..\sdk-install`.

## 2. Native renderer third-party sources

The native renderer (on by default in the releases) needs plume and reblue-XenosRecomp at pinned commits,
plus our patches in `patches\` (pins in CREDITS.md). Fetch them once:

```bat
python -I tools\nr_fetch_third_party.py
```

## 3. Game

Extract your ISO into `assets\`:

```bat
python tools\xiso_extract.py "UFC Undisputed 3 (USA).iso" assets
```

Then `build.bat native` (RelWithDebInfo with the native renderer, the configuration the releases ship;
`build.bat nonative` leaves it out, and plain `build.bat` keeps the last choice). `tools\build_release.bat`
is the Release preset. Both run `rexglue codegen` first; the generated code goes to `generated\` and is
never committed. A native build also produces `ufc3_shader_cache.exe`, `XenosRecomp.exe` and `ufc3_prelink.exe`
next to `ufc3.exe`: the launcher runs them on the player's own disc files to build the shader cache.
No game shaders are in this repository.

## 4. Launcher and release zips

```bat
dotnet publish launcher -c Release -o launcher\publish
powershell -File tools\release.ps1 -GameBuild out\build\win-amd64-relwithdebinfo -Version v1.0 -AllowRdDlls
```

This creates `dist\<date>\UFC3-Recomp-v1.0.zip` (launcher + game exe, no game files) and
`UFC3-DLSS5-Addon-v1.0.zip`, the same layout as the GitHub release. Native builds also pack the shader cache
tools and `config\native\pipelines_seed.bin` (shader content hashes only, no game data).

## Layout

`src/` app code and game hooks (each opt-in feature is a cvar), `src/native` + `src/reblue` the native renderer,
`cmake/` its build, `patches/` its plume/XenosRecomp patches, `third_party/` small vendored libraries, `config/functions.toml` codegen seeds,
`ufc3_manifest.toml` codegen manifest, `launcher/` the launcher, `dlss5/` DLSS 5 add-on scripts and templates,
`sdk-patch/` our SDK changes, `tools/` ISO extract, DLC install and release scripts, `tools/native` the shader cache tools, `licenses/` third-party licences.
