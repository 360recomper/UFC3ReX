# Native renderer (from the SvR 2010 recomp by nefariousjosiah, GPL-3.0, adapted from re:Blue,
# BSD-3-Clause; see CREDITS.md and src/reblue/LICENSE.reblue).
#
# Draws through plume on Direct3D 12 by hooking the game's XDK D3D functions instead of emulating
# the Xenos GPU. Unlike SvR it's a run-time choice (ufc3_renderer, src/native/ufc_native.h), so one
# exe carries both renderers and the default stays emulated.
#
# NR-5 (phase 0) builds the pieces that don't depend on UFC addresses: plume, the small third-party
# libraries and the host shaders. The hook sources (src/reblue, src/native/ufc_*.cpp) are in the
# tree but not compiled yet: they need the SDK's null GPU backend (NR-8). UFC3_NATIVE_HOOKS turns
# them on then.
#
# Game shaders can't ship, so unlike SvR there's no generated shader_cache.cpp: the renderer reads
# shader_cache.bin + shader_cache_linked.bin beside the exe at run time (NR-7b,
# src/reblue/gpu/shaders/shader_cache.h). tools/nr_build_shader_cache.py builds both: XenosRecomp
# writes the first, ufc3_prelink (built here) the second. Players have no Python:
# ufc3_shader_cache (tools/native/shader_extract, built here) does the same on their machine.
#
# The renderer's sources go straight into the executable: its REX_HOOK overrides are strong
# definitions nothing else references, so a static library would drop them at link time.

enable_language(C)  # zstd and xxHash are C

set(UFC3_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(UFC3_XENOS_DIR "${UFC3_NATIVE_ROOT}/third_party/XenosRecomp")
set(UFC3_NATIVE_GEN "${CMAKE_BINARY_DIR}/native_gen")
if(NOT EXISTS "${UFC3_NATIVE_ROOT}/third_party/plume/CMakeLists.txt" OR
   NOT EXISTS "${UFC3_XENOS_DIR}/thirdparty/dxc-bin/bin/x64/dxc.exe")
    message(FATAL_ERROR "UFC3_NATIVE_RENDERER: third_party/plume or XenosRecomp missing; "
                        "run python -I tools/nr_fetch_third_party.py")
endif()

option(UFC3_NATIVE_HOOKS "Compile the native D3D hooks (off = NR-6 census build)" ON)

# D3D12 only: it's our default backend and the laptop tester's only one. Native Vulkan is out
# until the user asks (plume can do it, SvR builds both).
set(PLUME_D3D12_ENABLED ON CACHE BOOL "" FORCE)
add_subdirectory("${UFC3_NATIVE_ROOT}/third_party/plume" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

# Small third-party pieces the renderer uses.
add_library(ufc3_native_thirdparty STATIC
    "${UFC3_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd/zstddeclib.c"
    "${UFC3_XENOS_DIR}/thirdparty/xxHash/xxhash.c"
    "${UFC3_XENOS_DIR}/thirdparty/smol-v/source/smolv.cpp"
    "${UFC3_NATIVE_ROOT}/third_party/reblue_thirdparty/miniz/miniz.cpp")
target_include_directories(ufc3_native_thirdparty PUBLIC
    "${UFC3_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd"
    "${UFC3_XENOS_DIR}/thirdparty/xxHash"
    "${UFC3_XENOS_DIR}/thirdparty/smol-v/source"
    "${UFC3_NATIVE_ROOT}/third_party/reblue_thirdparty/miniz")

# Host shaders (copy, ImGui overlay, MSAA resolve, gamma) compiled with DXC to DXIL headers.
set(UFC3_DXC_DIR "${UFC3_XENOS_DIR}/thirdparty/dxc-bin/bin/x64")
set(UFC3_DXC "${UFC3_DXC_DIR}/dxc.exe")
set(UFC3_HLSL_DIR "${UFC3_NATIVE_ROOT}/src/reblue/gpu/shaders/hlsl")
set(UFC3_HOST_SHADER_HEADERS "")
function(ufc3_host_shader STEM PROFILE)
    file(GLOB hlsl_includes "${UFC3_HLSL_DIR}/*.hlsli" "${UFC3_NATIVE_ROOT}/third_party/fidelityfx/gpu/*.h"
        "${UFC3_NATIVE_ROOT}/third_party/fidelityfx/gpu/fsr1/*.h")
    set(out "${UFC3_NATIVE_GEN}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxil.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${UFC3_DXC}" -T ${PROFILE} -HV 2021 -all-resources-bound -Wno-ignored-attributes
                -I "${UFC3_NATIVE_ROOT}" -Fh "${out}" "${UFC3_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_dxil
        DEPENDS "${UFC3_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
        COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, dxil)"
        VERBATIM)
    set(UFC3_HOST_SHADER_HEADERS ${UFC3_HOST_SHADER_HEADERS} "${out}" PARENT_SCOPE)
endfunction()
foreach(shader IN ITEMS copy_vs imgui_vs)
    ufc3_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS copy_color_ps copy_depth_ps gamma_correction_ps fsr_easu_ps fsr_rcas_ps pfx_occlusion_count_ps
        imgui_ps resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
        resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
    ufc3_host_shader(${shader} ps_6_0)
endforeach()
# Builds the host shaders with the game even before the hooks include them.
add_custom_target(ufc3_host_shaders DEPENDS ${UFC3_HOST_SHADER_HEADERS})

# DXC's import library for the run-time spec-constant linker (dxc_link.cpp).
add_library(ufc3_dxcompiler SHARED IMPORTED)
set_target_properties(ufc3_dxcompiler PROPERTIES
    IMPORTED_LOCATION "${UFC3_DXC_DIR}/dxcompiler.dll"
    IMPORTED_IMPLIB "${UFC3_XENOS_DIR}/thirdparty/dxc-bin/lib/x64/dxcompiler.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${UFC3_XENOS_DIR}/thirdparty/dxc-bin/inc")

# D3D12: shaders that read specialization constants are DXIL libraries. ufc3_prelink (re:Blue's
# reblue_prelink) DXC-links every variant of a shader_cache.bin into shader_cache_linked.bin, so
# the game never links one while drawing. Built with the game, run by nr_build_shader_cache.py.
add_executable(ufc3_prelink
    "${UFC3_NATIVE_ROOT}/tools/native/prelink_shader_cache.cpp"
    "${UFC3_NATIVE_ROOT}/src/reblue/gpu/shaders/dxc_link.cpp"
    "${UFC3_NATIVE_ROOT}/src/reblue/gpu/shaders/shader_cache_file.cpp")
target_include_directories(ufc3_prelink PRIVATE "${UFC3_NATIVE_ROOT}/src/reblue")
target_link_libraries(ufc3_prelink PRIVATE ufc3_native_thirdparty ufc3_dxcompiler)

# ufc3_shader_cache (NR-7c): the cache build for the player's machine, no Python. It extracts the
# shaders from the player's disc files, then runs XenosRecomp.exe and ufc3_prelink.exe from its
# own folder. All three land beside ufc3.exe.
add_subdirectory("${UFC3_NATIVE_ROOT}/tools/native/shader_extract" "${CMAKE_BINARY_DIR}/shader_extract")
set_target_properties(ufc3_shader_cache PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/xenosrecomp.cmake")

if(UFC3_NATIVE_HOOKS)
    file(GLOB_RECURSE UFC3_NATIVE_SOURCES CONFIGURE_DEPENDS
        "${UFC3_NATIVE_ROOT}/src/reblue/*.cpp" "${UFC3_NATIVE_ROOT}/src/reblue/*.h"
        "${UFC3_NATIVE_ROOT}/src/native/*.cpp" "${UFC3_NATIVE_ROOT}/src/native/*.h")
endif()

function(ufc3_add_native_renderer target)
    target_compile_definitions(${target} PRIVATE UFC3_NATIVE_RENDERER=1)
    target_include_directories(${target} PRIVATE
        "${UFC3_NATIVE_ROOT}/src/reblue"
        "${UFC3_NATIVE_ROOT}/src/native"
        "${UFC3_NATIVE_GEN}"
        "${UFC3_NATIVE_ROOT}/third_party/plume"
        "${UFC3_XENOS_DIR}/XenosRecomp")
    target_link_libraries(${target} PRIVATE plume ufc3_native_thirdparty)
    add_dependencies(${target} ufc3_host_shaders ufc3_prelink ufc3_shader_cache ufc3_xenosrecomp)
    if(NOT UFC3_NATIVE_HOOKS)
        # NR-6 D3D call census (--ufc3_d3d_census). It overrides the same D3D functions as the
        # native hooks, so it's only built without them; NR-8 moves its counters into the hooks.
        target_sources(${target} PRIVATE "${UFC3_NATIVE_ROOT}/src/dev/ufc_d3d_census.cpp")
    endif()
    if(UFC3_NATIVE_HOOKS)
        target_sources(${target} PRIVATE
            ${UFC3_NATIVE_SOURCES}
            ${UFC3_HOST_SHADER_HEADERS})
        # SVR_NATIVE_RENDERER selects SvR's observe-the-game's-D3D design in the re:Blue sources.
        target_compile_definitions(${target} PRIVATE REBLUE_D3D12=1 SVR_NATIVE_RENDERER=1 UFC3_NATIVE_HOOKS=1)
        target_link_libraries(${target} PRIVATE d3d12 dxgi dwmapi ufc3_dxcompiler rex::o1heap)
        # UFC's addresses for the D3D functions re:Blue's hooks name (D3DDevice_Clear -> sub_...),
        # then the run-time gate: on the emulated renderer every hook runs only the game's body.
        # ufc3_prelink compiles dxc_link.cpp and shader_cache_file.cpp too (no rexglue includes), so
        # they don't get the forced headers; neither names a D3D function.
        set(UFC3_HOOKED_SOURCES ${UFC3_NATIVE_SOURCES})
        list(REMOVE_ITEM UFC3_HOOKED_SOURCES
            "${UFC3_NATIVE_ROOT}/src/reblue/gpu/shaders/dxc_link.cpp"
            "${UFC3_NATIVE_ROOT}/src/reblue/gpu/shaders/shader_cache_file.cpp")
        set_source_files_properties(${UFC3_HOOKED_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-include;${UFC3_NATIVE_ROOT}/src/native/ufc_d3d_names.h;-include;${UFC3_NATIVE_ROOT}/src/native/ufc_hook_gate.h")
        # Linker map: names recompiled functions in crash stacks.
        target_link_options(${target} PRIVATE "-Wl,/MAP:${CMAKE_BINARY_DIR}/ufc3.map")
    endif()
    # dxcompiler.dll + dxil.dll beside the exe (ufc3_prelink, and the runtime DXC linker for any
    # variant the prelink missed).
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UFC3_DXC_DIR}/dxcompiler.dll" "${UFC3_DXC_DIR}/dxil.dll" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    # v1.1.1: the seed is also compiled into the exe (ufc3_seed_embedded.cpp, generated at configure time),
    # because launchers <= 1.1.0 update only exe/dll/txt/md files and skipped pipelines_seed.bin.
    # A pipelines_seed.bin beside the shader cache still wins (newer seeds without a rebuild).
    set(seed_bin "${UFC3_NATIVE_ROOT}/config/native/pipelines_seed.bin")
    set(seed_cpp "${UFC3_NATIVE_GEN}/ufc3_seed_embedded.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${seed_bin}")
    file(READ "${seed_bin}" seed_hex HEX)
    file(SIZE "${seed_bin}" seed_size)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," seed_bytes "${seed_hex}")
    file(WRITE "${seed_cpp}.tmp"
        "// Generated from config/native/pipelines_seed.bin by cmake/native_renderer.cmake. Do not edit.\n"
        "#include <cstddef>\nnamespace bd::gpu {\n"
        "extern const unsigned char kEmbeddedPipelineSeed[] = {${seed_bytes}};\n"
        "extern const std::size_t kEmbeddedPipelineSeedSize = ${seed_size};\n}\n")
    file(COPY_FILE "${seed_cpp}.tmp" "${seed_cpp}" ONLY_IF_DIFFERENT)
    target_sources(${target} PRIVATE "${seed_cpp}")
    # Pipeline seed (NR-9h, NR-15b): read from beside the shader cache, which defaults to beside the exe.
    # Made by tools/nr_pso_seed.py from test logs; keyed by shader content hashes, no game data in it.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UFC3_NATIVE_ROOT}/config/native/pipelines_seed.bin" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
