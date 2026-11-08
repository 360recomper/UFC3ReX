# Credits and third-party components

- **YucaGuapo**: project lead, testing, fixes, modding research
- **ReXGlue SDK** (Tom Clay and contributors): the static recompilation toolkit and runtime
- **Xenia** (Ben Vanik and the Xenia contributors): the emulator the ReXGlue runtime is derived from
- **AMD FidelityFX SDK** (Advanced Micro Devices, MIT): the CAS sharpening shader (licence: `licenses\AMD-FidelityFX-SDK-MIT.txt`)

This project is licensed under the GNU GPL v3.0 ([LICENSE](LICENSE)). The code below comes from
other projects and keeps its own licence. No game files and no game shaders are in this
repository or in any release: the native renderer's shader cache is built on the player's own PC
from their own disc files.

## Native renderer (default, `ufc3_renderer = native`)

The native renderer is a port of the one in the
[SmackDown vs. Raw 2010 recomp](https://github.com/nefariousjosiah/WWE-SVR2010-RECOMP)
by **nefariousjosiah** (GPL-3.0): its `src/native` (here renamed `src/native/ufc_*`),
`cmake/native_renderer.cmake`, every SvR change to the re:Blue renderer code in `src/reblue`, and
the patches in `patches/`. The UFC changes to those files are under the same licence.

| Component | How it is included | Licence |
|---|---|---|
| [re:Blue](https://github.com/zolaware/reblue) renderer core (zolaware, Blue Dragon recomp) | adapted in `src/reblue` (via SvR 2010) | BSD 3-Clause, `src/reblue/LICENSE.reblue` and each file's header |
| [plume](https://github.com/zolaware/plume) (rendering hardware interface) | fetched into `third_party/plume` at `e0c8871`, plus `patches/plume.patch` | MIT |
| [reblue-XenosRecomp](https://github.com/zolaware/reblue-XenosRecomp) (build tool, shader recompiler) | fetched into `third_party/XenosRecomp` at `339af41`, plus `patches/xenosrecomp.patch` | MIT |
| [zstd](https://github.com/facebook/zstd) (single-file decoder) | `third_party/reblue_thirdparty/zstd` | BSD, `third_party/reblue_thirdparty/zstd/LICENSE` |
| [miniz](https://github.com/richgel999/miniz) (Direct3D 12 linked-shader cache) | `third_party/reblue_thirdparty/miniz` | public domain (Unlicense), `third_party/reblue_thirdparty/miniz/LICENSE` |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) FSR 1 (EASU + RCAS) and the HLSL core headers it needs (the `ufc3_native_upscale` present pass) | copied unchanged into `third_party/fidelityfx/gpu` | MIT, `third_party/fidelityfx/LICENSE.txt` |
| re:Blue's shader prelink tool and runtime DXC linker (Direct3D 12) | `tools/native/prelink_shader_cache.cpp`, `src/reblue/gpu/shaders/dxc_link.*`, `shader_linker.*` | BSD 3-Clause (re:Blue) |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) 1.8.2407 (`dxcompiler.dll`, `dxil.dll`; build time, and next to `ufc3.exe` in native builds) | from XenosRecomp's `thirdparty/dxc-bin` | `dxcompiler.dll`: University of Illinois/NCSA, `third_party/licenses/DirectXShaderCompiler-LICENSE.txt`; `dxil.dll`: Microsoft's licence terms for it |

`third_party/plume` and `third_party/XenosRecomp` are not committed; `tools/nr_fetch_third_party.py`
fetches them at the commits above and applies the patches.

## Recompiler and runtime

| Component | How it is included | Licence |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (recompiler + runtime; portions from [Xenia](https://xenia.jp)) | built separately, plus `sdk-patch/rexglue-sdk-nightly-20261002-ufc3-fixes.patch` | BSD 3-Clause |

`rexruntime.dll` and `rexgpu-xenos.dll` are built from the ReXGlue SDK and contain, as the SDK
includes them: FFmpeg (libavcodec, libavutil; LGPL 2.1 or later, the game's XMA audio), libmspack
(LZX; LGPL 2.1), SDL 3 (zlib), Dear ImGui, fmt, spdlog, toml++ (MIT), xxHash (BSD 2-Clause),
utf8-cpp (Boost 1.0), disruptorplus, o1heap, aes_128, SIMDe, Vulkan Memory Allocator (MIT), glslang
(BSD 3-Clause and others), Vulkan/SPIR-V headers (Apache 2.0 / MIT) and the RenderDoc API header
(MIT). The LGPL libraries are statically linked into `rexruntime.dll` and not changed; you may
rebuild `rexruntime.dll` from the SDK source with your own versions and put it next to `ufc3.exe`.

## Game speed at low frame rates (`ufc3_adaptive_step`)

`src/adaptive_step.h` is adapted from `app/common/ufc_adaptive_rate.cpp` in
[UFC3MONS](https://github.com/monscriser/UFC3MONS) (Copyright (c) 2026 monscriser, UFC3MONS
contributors; MIT, full notice at the top of the file).

## Third-party licences (`licenses\`)
ReXGlue SDK (+ Xenia notice) `ReXGlue-SDK-LICENSE.txt`; AMD FidelityFX (MIT); Dear ImGui (MIT); SDL3 (zlib); FFmpeg (LGPL v2.1+, see `FFmpeg-LICENSE.md`);
fmt, spdlog, tomlplusplus, CLI11, inja, utfcpp, xxHash, simde, o1heap, aes_128, Catch2 (tests only), glslang, SPIRV-Headers/Tools,
Vulkan-Headers/Loader, VulkanMemoryAllocator, MoltenVK (macOS only), Tracy (dev builds only, not shipped).

UFC, UFC Undisputed 3 and all related names belong to their owners (Zuffa, THQ, Yuke's). This is an
unofficial fan project, not affiliated with or endorsed by them or by Microsoft.
