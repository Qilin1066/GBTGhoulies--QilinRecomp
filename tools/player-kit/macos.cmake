# The macOS download's CMake side (tools/player-kit/package-macos.sh lays out
# the app): included from CMakeLists.txt on Apple systems, as Toy Commander's
# (ToyCommander--QilinRecomp, the launcher's master copy) is.

# (The launcher's macOS part, apple_tools.cpp and ios_install.cpp, is added
# by tools/player-kit/launcher/launcher.cmake.)

# The macOS player kit (called from CMakeLists.txt's player kit, with
# kit_libraries, kit_dir and kit_archive set there): everything but the
# lifted code in one universal archive (arm64 and x86_64), the recompiler,
# the runtime's headers and build-flags.txt, which the launcher compiles and
# links the player's lifted code with, using Apple's clang from the Command
# Line Tools (/usr/bin/clang++, game.txt's compiler).
function(qilin_player_kit_macos)
    if(NOT "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES OR NOT "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        message(FATAL_ERROR "the macOS player kit is universal: configure it with -DCMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\"")
    endif()
    # Apple's libtool merges archives, universal ones too (Apple's ar has no MRI scripts).
    find_program(QILIN_APPLE_LIBTOOL NAMES libtool PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
    set(inputs)
    foreach(library IN LISTS kit_libraries)
        list(APPEND inputs "$<TARGET_FILE:${library}>")
    endforeach()
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${kit_dir}"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${kit_archive}"
        COMMAND "${QILIN_APPLE_LIBTOOL}" -static -no_warning_for_no_symbols -o "${kit_archive}" ${inputs}
        DEPENDS ${kit_libraries}
        COMMENT "Merging the player kit's libraries" VERBATIM)
    add_custom_target(ghoulies_kit ALL DEPENDS "${kit_archive}")
    # What SDL3 links on macOS (its CMakeLists.txt, sdl_link_dependency), and
    # iconv (SDL_iconv). The rest is the C++ library, which clang++ adds.
    set(kit_system_libraries
        -Wl,-framework,Cocoa -Wl,-framework,IOKit -Wl,-framework,ForceFeedback -Wl,-framework,Carbon
        -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox -Wl,-framework,AVFoundation -Wl,-framework,CoreMedia
        -Wl,-framework,CoreVideo -Wl,-framework,CoreBluetooth -Wl,-framework,CoreGraphics -Wl,-framework,Foundation
        -Wl,-framework,GameController -Wl,-framework,Metal -Wl,-framework,QuartzCore
        -Wl,-weak_framework,UniformTypeIdentifiers -Wl,-weak_framework,CoreHaptics -liconv)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # Read by the launcher (tools/player-kit/launcher/build.cpp): `name = value`
    # lines, $kit the kit's folder; `<name>.<arch>` follows `<name>` for the
    # Mac's own processor (host_architecture()). The game is compiled for the
    # player's Mac only: its processor's architecture and its CPU (Apple
    # clang's -mcpu=native on Apple silicon, -march=native on Intel), and for
    # the macOS it runs (clang's default, the SDK's).
    file(WRITE "${kit_dir}/build-flags.txt"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT), macOS: universal.\n"
        "cxxflags = ${QILIN_KIT_CXXFLAGS} -I$kit/include\n"
        "native.arm64 = -arch arm64 -mcpu=native\n"
        "native.x86_64 = -arch x86_64 -march=native\n"
        "ldflags = -O2\n"
        "ldflags.arm64 = -arch arm64\n"
        "ldflags.x86_64 = -arch x86_64\n"
        "libs = $kit/lib/libghoulies_kit.a ${kit_system_text}\n")
    # The kit's archive linked with stand-ins for the lifted code, for both
    # processors (CMAKE_OSX_ARCHITECTURES): it's complete.
    add_executable(ghoulies_kit_link_check ${CMAKE_SOURCE_DIR}/tools/player-kit/link_check.cpp)
    target_include_directories(ghoulies_kit_link_check PRIVATE ${CMAKE_SOURCE_DIR}/recomp/runtime)
    target_link_libraries(ghoulies_kit_link_check PRIVATE "${kit_archive}" ${kit_system_libraries})
    add_dependencies(ghoulies_kit_link_check ghoulies_kit)
endfunction()
