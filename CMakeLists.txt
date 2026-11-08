# ufc3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ufc3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(UFC3_SOURCES
    src/main.cpp
    src/gpu_preference.cpp
)

if(WIN32)
    add_executable(ufc3 WIN32 ${UFC3_SOURCES})
else()
    add_executable(ufc3 ${UFC3_SOURCES})
endif()

rexglue_setup_target(ufc3 GPU_PLUGINS xenos)

# Native renderer (cmake/native_renderer.cmake): compiled in, picked at run time with
# ufc3_renderer=native; the default stays emulated. build.bat native / nonative sets it.
option(UFC3_NATIVE_RENDERER "Compile in the native (plume D3D12) renderer" OFF)
if(UFC3_NATIVE_RENDERER)
    include(cmake/native_renderer.cmake)
    ufc3_add_native_renderer(ufc3)
endif()
