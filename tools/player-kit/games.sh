# The game's details for the packaging scripts (sourced), as Toy Commander's
# games.sh (ToyCommander--QilinRecomp, the launcher's master copy) has them.
# Everything game-specific in a download comes from here: the README
# templates' @NAME@ places (fill) and kit/game.txt (write_game_txt), which
# is all the launcher knows about the game (tools/player-kit/launcher/ADOPTING.md).
game=ghoulies   # the data folder (qilin/ghoulies, the runtime's own)
REPO="Qilin1066/GBTGhoulies--QilinRecomp"
TITLE="Grabbed by the Ghoulies"; PACKAGE="GBTGhoulies--QilinRecomp"; EXE="Ghoulies.exe"; BOOT="default.xbe"
BUILD_TITLE="Grabbed by the Ghoulies (USA)"; RELEASE="the USA release, MS-083"; PRODUCT="MS-083"
STUDIO="Rare / Microsoft Game Studios, Xbox, 2003"
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" \
        -e "s|@REPO@|$REPO|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the data it builds from (an extracted disc, a folder; `build` is
# recomp/src/identify.hpp's supported build, which `qilin identify --build`
# checks), how it is recompiled (one step: qilin lift of the folder's
# default.xbe), what a compile needs (the biggest lifted file, 2.5 MB, took
# about 340 MB with zig's Clang at -O2), how the game starts (the disc's
# folder and its default.xbe, as qilin_run takes them), and where releases
# are published (tags vX.Y.Z).
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=folder\ninput_noun=disc folder\n'
        printf 'input_hint=Choose the folder you extracted your disc into (with extract-xiso or xdvdfs, for example): the one holding default.xbe and the bundles, demand and xwavebank folders.\n'
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} lift {input}/default.xbe --out {gen}\n'
        printf 'compile_memory_mb=800\nbuild_space_mb=2048\ncache_mb=2048\n'
        printf 'run_args=--game-dir {input} {input}/default.xbe\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
# Copies the licences every download shares into <LICENSES folder>, by
# component: this project's (GPL-3.0); SDL3 (fetched by CMake into <build>);
# and the vendored third_party/ code the kit's library and the launcher are
# built from (third_party/README.md): Dear ImGui, Vulkan-Headers, volk,
# Zydis with Zycore (qilin), FidelityFX FSR 1 and SMAA (the renderer's
# passes), nxdk's kernel export list (CC0-1.0) and XbSymbolDatabase (the
# XDK function names the runtime carries).
#   common_licenses <LICENSES folder> <build folder> <repository root>
common_licenses() {
    lic=$1 build=$2 root=$3
    mkdir -p "$lic/$PACKAGE" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk" "$lic/Zydis" \
             "$lic/FidelityFX FSR" "$lic/SMAA" "$lic/nxdk" "$lic/XbSymbolDatabase"
    cp "$root/LICENSE" "$lic/$PACKAGE/LICENSE.txt"
    sdl=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_SDL3:PATH=//p' "$build/CMakeCache.txt")
    [ -n "$sdl" ] || sdl="$build/_deps/sdl3-src"
    cp "$sdl/LICENSE.txt" "$lic/SDL3/"
    cp "$root/third_party/imgui/LICENSE.txt" "$lic/Dear ImGui/"
    cp -R "$root/third_party/vulkan-headers/LICENSE.md" "$root/third_party/vulkan-headers/LICENSES" "$lic/Vulkan-Headers/"
    cp "$root/third_party/volk/LICENSE.md" "$lic/volk/"
    cp "$root/third_party/zydis/LICENSE" "$lic/Zydis/LICENSE.txt"
    cp "$root/third_party/zydis/dependencies/zycore/LICENSE" "$lic/Zydis/Zycore-LICENSE.txt"
    cp "$root/third_party/fsr1/LICENSE.txt" "$lic/FidelityFX FSR/"
    cp "$root/third_party/smaa/LICENSE.txt" "$lic/SMAA/"
    cp "$root/third_party/xbsymboldatabase/LICENSE" "$lic/XbSymbolDatabase/LICENSE.txt"
    cat > "$lic/nxdk/README.txt" <<'NXDK'
The runtime's Xbox kernel export list (names, ordinals and argument sizes)
was made from nxdk's lib/xboxkrnl/xboxkrnl.exe.def
(https://github.com/XboxDev/nxdk, commit
fafbe41b48fa00a31b79dd49ad1f86a30adcdd9e), whose header says:

    SPDX-License-Identifier: CC0-1.0
    SPDX-FileCopyrightText: 2017 Stefan Schmidt

CC0 1.0 Universal: https://creativecommons.org/publicdomain/zero/1.0/
NXDK
    cat > "$lic/XbSymbolDatabase/README.txt" <<'XBSDB'
The runtime carries the addresses and names of the XDK library functions in
the game's default.xbe (recomp/runtime/host/xdk_symbols.inc), found once
with XbSymbolDatabase (https://github.com/Cxbx-Reloaded/XbSymbolDatabase,
commit 20eced544726f5558c5a408458f38a086cc4e543), under the MIT licence:
LICENSE.txt. None of its code is included.
XBSDB
}
