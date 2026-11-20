cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime, qilin_run) is built for macOS 10.15,
# the oldest macOS a Vulkan 1.3 MoltenVK runs on; so it avoids std::format,
# which Apple's libc++ only has from 13.3 (recomp/runtime/strf.hpp). The
# developer tools (qilin, the tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(GBTGhoulies-QilinRecomp VERSION 0.40.1 LANGUAGES C CXX)

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

# ---- Sanitizers for the hand-written runtime (developer option) ----
# -DQILIN_SANITIZE=address,undefined (or thread) builds the runtime's own
# libraries and tests with those sanitizers and links every program with
# them. The generated code in qilin_run is left as it is: it's far too
# large to instrument, and its memory is the guest's, which it checks itself.
set(QILIN_SANITIZE "" CACHE STRING "Sanitizers for the runtime's own code (GCC/Clang), e.g. address,undefined")
add_library(qilin_sanitize INTERFACE)
if(QILIN_SANITIZE AND NOT MSVC)
    # (GCC's instrumented builds raise false array-bounds and overflow warnings.)
    target_compile_options(qilin_sanitize INTERFACE -fsanitize=${QILIN_SANITIZE} -fno-omit-frame-pointer -g -Wno-error)
    add_link_options(-fsanitize=${QILIN_SANITIZE})
endif()
target_link_libraries(qilin_warnings INTERFACE qilin_sanitize)

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
add_library(qilin_gfx STATIC recomp/runtime/gfx/renderer.cpp recomp/runtime/gfx/fsr/fsr_constants.cpp recomp/runtime/gfx/png.cpp recomp/runtime/gfx/vsh.cpp
    recomp/runtime/gfx/spirv.cpp recomp/runtime/gfx/nv2a_vs.cpp recomp/runtime/gfx/nv2a_ps.cpp recomp/runtime/gfx/texture.cpp)
target_include_directories(qilin_gfx PUBLIC recomp/runtime/gfx PRIVATE recomp/runtime)
target_link_libraries(qilin_gfx PUBLIC qilin_volk PRIVATE qilin_warnings)
add_executable(qilin_gfx_tests recomp/runtime/gfx/gfx_tests.cpp)
target_link_libraries(qilin_gfx_tests PRIVATE qilin_gfx qilin_warnings)
add_test(NAME qilin_gfx_tests COMMAND qilin_gfx_tests)
set_tests_properties(qilin_gfx_tests PROPERTIES SKIP_RETURN_CODE 77)

# ---- Audio engine: voices and the software mixer ----
add_library(qilin_audio STATIC recomp/runtime/audio/voice.cpp recomp/runtime/audio/reverb.cpp)
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
add_library(qilin_platform STATIC recomp/runtime/platform/window.cpp recomp/runtime/platform/input.cpp recomp/runtime/platform/settings.cpp
    recomp/runtime/platform/audio_out.cpp)
target_include_directories(qilin_platform PUBLIC recomp/runtime/platform)
target_link_libraries(qilin_platform PUBLIC SDL3::SDL3 PRIVATE qilin_warnings)

# ---- The settings menu: Dear ImGui (MIT, vendored as ApbArcade-QilinRecomp has it) over the window ----
add_library(qilin_imgui STATIC
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(qilin_imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(qilin_imgui PUBLIC qilin_volk SDL3::SDL3)
if(MSVC)
    target_compile_options(qilin_imgui PRIVATE /W0)
else()
    target_compile_options(qilin_imgui PRIVATE -w)
endif()
add_library(qilin_ui STATIC recomp/runtime/ui/menu.cpp)
target_include_directories(qilin_ui PUBLIC recomp/runtime/ui)
target_link_libraries(qilin_ui PUBLIC qilin_gfx qilin_platform qilin_imgui PRIVATE qilin_warnings)
target_compile_definitions(qilin_ui PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Runtime host side: native replacements (HLE) for code that isn't lifted ----
add_library(qilin_host STATIC recomp/runtime/host/host.cpp recomp/runtime/host/xapi.cpp recomp/runtime/host/game_fixes.cpp recomp/runtime/host/kernel.cpp recomp/runtime/host/kernel_crypto.cpp recomp/runtime/host/sched.cpp
                              recomp/runtime/host/kernel_io.cpp recomp/runtime/host/xinput.cpp
                              recomp/runtime/host/call_guest.cpp recomp/runtime/host/d3d.cpp
                              recomp/runtime/host/dsound.cpp recomp/runtime/host/guest_ram.cpp recomp/runtime/host/aspect.cpp)
target_include_directories(qilin_host PUBLIC recomp/runtime/host)
target_link_libraries(qilin_host PUBLIC qilin_runtime qilin_gfx qilin_audio qilin_platform qilin_ui PRIVATE qilin_warnings)
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
    if(MSVC)
        # The generated files are large (up to 2.5 MB, hundreds of functions
        # each): room for their sections (/bigobj); the conformance switches
        # the rest of the project builds with; and, under Visual Studio's own
        # generator, the files compiled in parallel (/MP), not one by one.
        target_compile_options(qilin_run PRIVATE /bigobj /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
        target_compile_definitions(qilin_run PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(CMAKE_GENERATOR MATCHES "Visual Studio")
            target_compile_options(qilin_run PRIVATE /MP)
        endif()
    endif()
endif()

# ---- The player kit: everything qilin_run needs but the code lifted from default.xbe ----
# For the Windows download (tools/player-kit), as ApbArcade-QilinRecomp and PunchOutWii-QilinRecomp
# recomps': the runtime's start-up and dispatch and every library they link
# (the host layer, renderer, audio, platform, XBE loader, SDL3, volk), merged
# into one archive, with the headers the lifted code includes, qilin, and the
# flags and libraries to build the rest with (kit.ninja). A player's own PC
# lifts their own default.xbe and compiles only that code, with the same
# compiler, against the kit. Needs a MinGW toolchain
# (tools/player-kit/mingw-cross.cmake) and SDL3 built statically (the
# fetched one: -DQILIN_SYSTEM_SDL=OFF).
option(QILIN_PLAYER_KIT "Build the player kit (tools/player-kit)" OFF)
if(QILIN_PLAYER_KIT)
    if(NOT TARGET SDL3-static)
        message(FATAL_ERROR "the player kit needs SDL3 built statically (-DQILIN_SYSTEM_SDL=OFF)")
    endif()
    add_library(ghoulies_kit_main OBJECT recomp/runtime/host/run.cpp recomp/runtime/dispatch.cpp)
    target_compile_definitions(ghoulies_kit_main PRIVATE QILIN_CHECKED_MEMORY)
    target_link_libraries(ghoulies_kit_main PRIVATE qilin_host qilin_xbe qilin_warnings)
    set(kit_libraries qilin_host qilin_ui qilin_imgui qilin_gfx qilin_audio qilin_platform qilin_xbe qilin_volk SDL3-static)
    set(kit_archive "${CMAKE_BINARY_DIR}/libghoulies_kit.a")
    set(mri "CREATE ${kit_archive}\n")
    foreach(library IN LISTS kit_libraries)
        string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
    endforeach()
    string(APPEND mri "ADDMOD $<JOIN:$<TARGET_OBJECTS:ghoulies_kit_main>,\nADDMOD >\nSAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/ghoulies_kit.mri" CONTENT "${mri}")
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/ghoulies_kit.mri" "-DOUT=${kit_archive}"
                -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
        DEPENDS ghoulies_kit_main ${kit_libraries} $<TARGET_OBJECTS:ghoulies_kit_main> "${CMAKE_BINARY_DIR}/ghoulies_kit.mri"
        COMMENT "Merging the player kit's libraries" VERBATIM)
    add_custom_target(ghoulies_kit ALL DEPENDS "${kit_archive}")
    # The system libraries SDL3 and the runtime need (MinGW names).
    set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
        -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32 -lbcrypt)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # What the lifted code is compiled and linked with: kit.ninja, included by
    # the build.ninja the player's script writes ($kit is the kit's folder).
    # The same floating-point rules as qilin_runtime (bit-exact SSE), and the
    # same checked guest memory as qilin_run. Lifted calls nest as deeply as
    # the guest's: a 16 MB main stack.
    file(WRITE "${CMAKE_BINARY_DIR}/kit.ninja"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
        "cxxflags = -O2 -DNDEBUG -DQILIN_CHECKED_MEMORY -std=c++23 -ffp-contract=off -fno-fast-math -w -I$kit/include\n"
        "ldflags = -static -O2 -Wl,--stack,16777216\n"
        "libs = $kit/lib/libghoulies_kit.a ${kit_system_text}\n")
    # Linked with stand-ins for the lifted code: the kit is complete.
    add_executable(ghoulies_kit_link_check tools/player-kit/link_check.cpp)
    target_include_directories(ghoulies_kit_link_check PRIVATE recomp/runtime)
    target_link_libraries(ghoulies_kit_link_check PRIVATE "${kit_archive}" ${kit_system_libraries})
    target_link_options(ghoulies_kit_link_check PRIVATE -static -Wl,--stack,16777216)
    add_dependencies(ghoulies_kit_link_check ghoulies_kit)

    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT player)
    install(FILES "${kit_archive}" DESTINATION kit/lib COMPONENT player)
    install(FILES "${CMAKE_BINARY_DIR}/kit.ninja" DESTINATION kit COMPONENT player)
    install(FILES recomp/runtime/guest.hpp recomp/runtime/x87_math.hpp DESTINATION kit/include COMPONENT player)
endif()
