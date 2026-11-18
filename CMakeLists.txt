cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime, qilin_run) is built for macOS 10.15,
# the oldest macOS a Vulkan 1.3 MoltenVK runs on; so it avoids std::format,
# which Apple's libc++ only has from 13.3 (recomp/runtime/strf.hpp). The
# developer tools (qilin, the tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(GBTGQilincomp VERSION 0.25.0 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges::to, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# ---- Zydis (vendored, MIT) ----
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_AVX512 OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_KNC OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/zydis EXCLUDE_FROM_ALL)

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# ---- Guest runtime (header-only) and the floating-point rules lifted code needs ----
# Lifted SSE code must be bit-exact IEEE single precision: no multiply-add
# contraction and no fast-math. guest.hpp also sets the pragmas Clang and MSVC
# honour; GCC only has the command-line switch, so link anything that compiles
# lifted code against this target.
add_library(qilin_runtime INTERFACE)
target_include_directories(qilin_runtime INTERFACE recomp/runtime)
if(MSVC)
    target_compile_options(qilin_runtime INTERFACE /fp:precise)
else()
    target_compile_options(qilin_runtime INTERFACE -ffp-contract=off -fno-fast-math)
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The XBE loader: runtime code too ----
add_library(qilin_xbe STATIC recomp/src/xbe.cpp)
target_include_directories(qilin_xbe PUBLIC recomp/src recomp/runtime)
target_link_libraries(qilin_xbe PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/xbe_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/vrange.cpp
    recomp/src/lifter.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_xbe Zydis qilin_runtime PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Tests (no game files needed) ----
enable_testing()
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)

# x87 transcendentals: fast paths against slow paths on each CI platform's compiler and CPU.
add_executable(qilin_x87_consistency tools/check/x87/consistency.cpp)
target_link_libraries(qilin_x87_consistency PRIVATE qilin_runtime qilin_warnings)
add_test(NAME qilin_x87_consistency COMMAND qilin_x87_consistency 200000)

# ---- Renderer: Vulkan 1.3, loaded at run time through volk (no SDK or link-time Vulkan needed) ----
add_library(qilin_volk STATIC third_party/volk/volk.c)
target_include_directories(qilin_volk PUBLIC third_party/volk third_party/vulkan-headers/include)
target_link_libraries(qilin_volk PUBLIC ${CMAKE_DL_LIBS})
add_library(qilin_gfx STATIC recomp/runtime/gfx/renderer.cpp recomp/runtime/gfx/png.cpp recomp/runtime/gfx/vsh.cpp
    recomp/runtime/gfx/spirv.cpp recomp/runtime/gfx/nv2a_vs.cpp recomp/runtime/gfx/nv2a_ps.cpp recomp/runtime/gfx/texture.cpp)
target_include_directories(qilin_gfx PUBLIC recomp/runtime/gfx PRIVATE recomp/runtime)
target_link_libraries(qilin_gfx PUBLIC qilin_volk PRIVATE qilin_warnings)
add_executable(qilin_gfx_tests recomp/runtime/gfx/gfx_tests.cpp)
target_link_libraries(qilin_gfx_tests PRIVATE qilin_gfx qilin_warnings)
add_test(NAME qilin_gfx_tests COMMAND qilin_gfx_tests)
set_tests_properties(qilin_gfx_tests PROPERTIES SKIP_RETURN_CODE 77)

# ---- Audio engine: voices and the software mixer ----
add_library(qilin_audio STATIC recomp/runtime/audio/voice.cpp)
target_include_directories(qilin_audio PUBLIC recomp/runtime/audio)
target_link_libraries(qilin_audio PRIVATE qilin_warnings)
add_executable(qilin_audio_tests recomp/runtime/audio/audio_tests.cpp)
target_link_libraries(qilin_audio_tests PRIVATE qilin_audio qilin_warnings)
add_test(NAME qilin_audio_tests COMMAND qilin_audio_tests)

# ---- Platform: the window, input and audio output, through SDL3 ----
# A system SDL3 (3.2 or later) is used if CMake finds one; otherwise a pinned
# release is fetched and built static, with only what the runtime uses.
option(QILIN_SYSTEM_SDL "Use an installed SDL3 if there is one" ON)
if(QILIN_SYSTEM_SDL)
    find_package(SDL3 3.2 CONFIG QUIET COMPONENTS SDL3)
endif()
if(NOT TARGET SDL3::SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)   # the runtime draws with its own Vulkan renderer
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
endif()
add_library(qilin_platform STATIC recomp/runtime/platform/window.cpp recomp/runtime/platform/input.cpp
    recomp/runtime/platform/audio_out.cpp)
target_include_directories(qilin_platform PUBLIC recomp/runtime/platform)
target_link_libraries(qilin_platform PUBLIC SDL3::SDL3 PRIVATE qilin_warnings)

# ---- Runtime host side: native replacements (HLE) for code that isn't lifted ----
add_library(qilin_host STATIC recomp/runtime/host/host.cpp recomp/runtime/host/xapi.cpp recomp/runtime/host/kernel.cpp recomp/runtime/host/kernel_crypto.cpp
                              recomp/runtime/host/kernel_io.cpp recomp/runtime/host/xinput.cpp
                              recomp/runtime/host/call_guest.cpp recomp/runtime/host/d3d.cpp
                              recomp/runtime/host/dsound.cpp recomp/runtime/host/guest_ram.cpp)
target_include_directories(qilin_host PUBLIC recomp/runtime/host)
target_link_libraries(qilin_host PUBLIC qilin_runtime qilin_gfx qilin_audio qilin_platform PRIVATE qilin_warnings)
add_executable(qilin_host_tests recomp/runtime/host/host_tests.cpp)
target_link_libraries(qilin_host_tests PRIVATE qilin_host qilin_warnings)
add_test(NAME qilin_host_tests COMMAND qilin_host_tests)

# qilin_run: the game, from `qilin lift` output (needs your own default.xbe):
#   qilin lift default.xbe --out gen && cmake -B build -DQILIN_GEN_DIR=$PWD/gen
set(QILIN_GEN_DIR "" CACHE PATH "qilin lift output directory to build qilin_run from")
if(QILIN_GEN_DIR)
    file(GLOB QILIN_GEN_SOURCES CONFIGURE_DEPENDS "${QILIN_GEN_DIR}/*.cpp")
    add_executable(qilin_run recomp/runtime/host/run.cpp recomp/runtime/dispatch.cpp ${QILIN_GEN_SOURCES})
    target_include_directories(qilin_run PRIVATE "${QILIN_GEN_DIR}")
    target_compile_definitions(qilin_run PRIVATE QILIN_CHECKED_MEMORY)  # bring-up: bad guest accesses stop with an address
    target_link_libraries(qilin_run PRIVATE qilin_host qilin_xbe)
endif()
