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
cvar/config fixes and some audio diagnostics.

Set `UFC3_SDK` to the SDK's install folder (`out\install\win-amd64`), or put it at `..\sdk-install`.

## 2. Game

Extract your ISO into `assets\`:

```bat
python tools\xiso_extract.py "UFC Undisputed 3 (USA).iso" assets
```

Then `build.bat` (development build, RelWithDebInfo) or `tools\build_release.bat` (Release build for packaging).
Both run `rexglue codegen` first; the generated code goes to `generated\` and is never committed.

## 3. Launcher and release zips

```bat
dotnet publish launcher -c Release -o launcher\publish
powershell -File tools\release.ps1 -Version v1.0
```

This creates `dist\<date>\UFC3-Recomp-v1.0.zip` (launcher + game exe, no game files) and
`UFC3-DLSS5-Addon-v1.0.zip`, the same layout as the GitHub release.

## Layout

`src/` app code and game hooks (each opt-in feature is a cvar), `config/functions.toml` codegen seeds,
`ufc3_manifest.toml` codegen manifest, `launcher/` the launcher, `dlss5/` DLSS 5 add-on scripts and templates,
`sdk-patch/` our SDK changes, `tools/` ISO extract, DLC install and release scripts, `licenses/` third-party licences.
