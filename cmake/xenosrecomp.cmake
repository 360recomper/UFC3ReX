# XenosRecomp.exe (UFC3_XENOS_DIR, patched by tools/nr_fetch_third_party.py), built beside the game.
# ufc3_shader_cache runs it on the player's machine and tools/nr_build_shader_cache.py runs it here.
#
# Its own configure and build, not add_subdirectory:
# - its thirdparty folder defines fmt, zstd and xxHash targets, which the game's SDK packages may also define;
# - it must not pick up the game's compile options or build type. It ships, and a plain Release exe got
#   flagged by Windows Defender (NR-3), so these are the flags tools/nr_build_shader_cache.py used, which
#   give a build Defender passes. Its thirdparty folder still adds -O3 -DNDEBUG after them, as it did there.
# The build step runs every time but ninja only rebuilds what changed.

include(ExternalProject)

set(UFC3_XENOS_ARGS
    -DCMAKE_BUILD_TYPE=
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_C_FLAGS=-O2"
    "-DCMAKE_CXX_FLAGS=-O2 -g -DREBLUE_RECOMP")
if(WIN32)
    # The game project doesn't enable RC, its thirdparty does. Same LLVM install as the compilers.
    get_filename_component(UFC3_XENOS_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(UFC3_XENOS_RC NAMES llvm-rc HINTS "${UFC3_XENOS_LLVM_BIN}" REQUIRED)
    list(APPEND UFC3_XENOS_ARGS "-DCMAKE_RC_COMPILER=${UFC3_XENOS_RC}")
endif()

ExternalProject_Add(ufc3_xenosrecomp
    SOURCE_DIR "${UFC3_XENOS_DIR}"
    PREFIX "${CMAKE_BINARY_DIR}/xenosrecomp-ep"
    BINARY_DIR "${CMAKE_BINARY_DIR}/xenosrecomp"
    CMAKE_ARGS ${UFC3_XENOS_ARGS}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target XenosRecomp
    BUILD_ALWAYS ON
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "<BINARY_DIR>/XenosRecomp/XenosRecomp.exe" "${CMAKE_BINARY_DIR}/XenosRecomp.exe"
    BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/XenosRecomp.exe")
