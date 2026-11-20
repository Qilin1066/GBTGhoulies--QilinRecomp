#!/bin/sh
# Builds the Windows download: tools/player-kit/package.sh <version> <out.zip>
# (as ApbArcade-QilinRecomp's and PunchOutWii-QilinRecomp's).
#
# Cross-compiles on Linux with llvm-mingw, then lays out:
#   GBTGhoulies-QilinRecomp/Play Ghoulies.bat, README.txt, LICENSES/
#   GBTGhoulies-QilinRecomp/kit/  qilin.exe, lib/libghoulies_kit.a, include/, kit.ninja,
#                            play.ps1, ninja.exe, toolchain/ (llvm-mingw, trimmed), VERSION
# Nothing in it is made from the game: the player's PC lifts their own
# default.xbe and compiles that code with the bundled compiler (play.ps1).
#
# Environment:
#   LLVM_MINGW_CROSS    the Linux-hosted llvm-mingw release, unpacked
#   LLVM_MINGW_WINDOWS  the x86_64 Windows-hosted release of the same version, unpacked
#   NINJA_WINDOWS       a folder with ninja.exe (Windows) and Ninja's COPYING
#   BUILD_DIR           where to build (default build-player-kit)
#   CMAKE_EXTRA         more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_SDL3=...)
set -eu
version=$1
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${BUILD_DIR:-$root/build-player-kit}
stage=$(mktemp -d)
pkg="$stage/GBTGhoulies-QilinRecomp"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/tools/player-kit/mingw-cross.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DQILIN_PLAYER_KIT=ON -DQILIN_SYSTEM_SDL=OFF -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin ghoulies_kit ghoulies_kit_link_check --parallel
cmake --install "$build" --component player --prefix "$pkg"

"$root/tools/player-kit/trim-toolchain.sh" "$LLVM_MINGW_WINDOWS" "$pkg/kit/toolchain"
cp "$NINJA_WINDOWS/ninja.exe" "$pkg/kit/"
cp "$root/tools/player-kit/play.ps1" "$pkg/kit/"
sed 's/$/\r/' "$root/tools/player-kit/Play Ghoulies.bat" > "$pkg/Play Ghoulies.bat"
sed 's/$/\r/' "$root/tools/player-kit/README.txt" > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"

# Every licence of what's included, by component.
lic="$pkg/LICENSES"
mkdir -p "$lic/llvm-mingw" "$lic/ninja" "$lic/SDL3" "$lic/Vulkan-Headers" "$lic/volk" "$lic/Zydis" "$lic/Dear ImGui" "$lic/FidelityFX FSR"
mv "$pkg/kit/toolchain/licenses/"* "$lic/llvm-mingw/"
rmdir "$pkg/kit/toolchain/licenses"
cp "$NINJA_WINDOWS/COPYING" "$lic/ninja/"
sdl=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_SDL3:PATH=//p' "$build/CMakeCache.txt")
[ -n "$sdl" ] || sdl="$build/_deps/sdl3-src"
cp "$sdl/LICENSE.txt" "$lic/SDL3/"
cp -r "$root"/third_party/vulkan-headers/LICENSE.md "$root"/third_party/vulkan-headers/LICENSES "$lic/Vulkan-Headers/"
cp "$root"/third_party/volk/LICENSE* "$lic/volk/"
cp "$root"/third_party/imgui/LICENSE.txt "$lic/Dear ImGui/"
cp "$root"/third_party/fsr1/LICENSE.txt "$lic/FidelityFX FSR/"
cp "$root"/third_party/zydis/LICENSE "$lic/Zydis/"
cp "$root"/third_party/zydis/dependencies/zycore/LICENSE "$lic/Zydis/LICENSE-Zycore" 2>/dev/null || true
# (nxdk's kernel export list, CC0-1.0, is in the runtime too: third_party/README.md.)

rm -f "$out"
(cd "$stage" && zip -q -r -9 "$out" GBTGhoulies-QilinRecomp)
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
