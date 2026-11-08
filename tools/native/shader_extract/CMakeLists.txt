# ufc3_shader_cache: builds the native renderer's shader cache on the player's machine (NR-7c).
# No Python, no rexglue SDK: Windows crypto (bcrypt) for the xex and the file names, miniz for the
# zlib blobs, xxHash for the cache hash.
#
# With the game:  cmake/native_renderer.cmake adds this folder (build.bat native).
# On its own (after env.bat, from the repo root):
#   cmake -S tools/native/shader_extract -B out/nr7c -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#         -DCMAKE_CXX_COMPILER=clang++
#   cmake --build out/nr7c

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ufc3_shader_cache LANGUAGES CXX)
endif()

get_filename_component(UFC3_SC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(UFC3_SC_XENOS "${UFC3_SC_ROOT}/third_party/XenosRecomp")
set(UFC3_SC_MINIZ "${UFC3_SC_ROOT}/third_party/reblue_thirdparty/miniz")
set(UFC3_SC_COMMON "${UFC3_SC_XENOS}/XenosRecomp/shader_common.h")
if(NOT EXISTS "${UFC3_SC_COMMON}" OR NOT EXISTS "${UFC3_SC_MINIZ}/miniz.cpp")
    message(FATAL_ERROR "ufc3_shader_cache: third_party/XenosRecomp or reblue_thirdparty missing; "
                        "run python -I tools/nr_fetch_third_party.py")
endif()

# XenosRecomp reads shader_common.h from a file at run time. Embed it (as bytes, so the tool
# writes back exactly this file) instead of shipping it loose; a change re-runs the configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${UFC3_SC_COMMON}")
file(READ "${UFC3_SC_COMMON}" UFC3_SC_COMMON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," UFC3_SC_COMMON_BYTES "${UFC3_SC_COMMON_HEX}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)"
       "\\1\n" UFC3_SC_COMMON_BYTES "${UFC3_SC_COMMON_BYTES}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shader_common_embed.inc"
     CONTENT "${UFC3_SC_COMMON_BYTES}\n" @ONLY)

add_executable(ufc3_shader_cache
    main.cpp
    shader_set.cpp
    shader_set.h
    win_crypto.cpp
    win_crypto.h
    xex_image.cpp
    xex_image.h
    "${UFC3_SC_MINIZ}/miniz.cpp")
target_compile_features(ufc3_shader_cache PRIVATE cxx_std_20)
target_include_directories(ufc3_shader_cache PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"           # shader_common_embed.inc
    "${UFC3_SC_ROOT}/src"                   # bloom_patterns.h, dof_patterns.h
    "${UFC3_SC_MINIZ}"
    "${UFC3_SC_XENOS}/thirdparty/xxHash")
target_compile_definitions(ufc3_shader_cache PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_source_files_properties("${UFC3_SC_MINIZ}/miniz.cpp" PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(ufc3_shader_cache PRIVATE bcrypt)
# Static C runtime: the tool runs before anything else of the native renderer does.
set_target_properties(ufc3_shader_cache PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
