# The downloads: Windows, Linux, Raspberry Pi and macOS

The game's code may never be distributed (it is made from the disc), so each download carries everything but that code, and the player's own computer recompiles their `default.xbe` and compiles the result. One launcher does it on every system: `tools/player-kit/launcher`, Toy Commander's (ToyCommander--QilinRecomp's, the master copy), copied unchanged. It names no game; what it knows about this one is the download's `kit/game.txt`, written by `tools/player-kit/games.sh`, and what the player's folder is, `qilin identify` says. `launcher/ADOPTING.md` is the contract: the `game.txt` keys, the identify contract, what a project provides, and `LAUNCHER_VERSION` (this copy: 1), which says how far behind the master a copy is. A fix to the launcher belongs in the master and is copied out again.

| System | Download | Compiler | Notes |
|---|---|---|---|
| Windows 10 (1803) or 11, x64 | `GBTGhoulies--QilinRecomp-X.Y.Z-windows-x64.zip`, `Play Grabbed by the Ghoulies.exe` | LLVM-MinGW 20261104, bundled (`trim-toolchain.sh`) | unsigned: SmartScreen's "More info, Run anyway" once |
| Linux x86-64 and ARM64 | `-linux-x86_64.tar.gz`, `-linux-arm64.tar.gz`, `Play Grabbed by the Ghoulies` | zig 0.17.1, bundled: its own libc++, the system's glibc (2.35 or later) | menu entry from the launcher; needs a Vulkan 1.3 driver |
| macOS 14.5+ (Xcode 16's tools need it), Apple silicon and Intel | `-macos.dmg`, `Grabbed by the Ghoulies.app` (universal) | Apple's clang from the Command Line Tools, which the launcher offers to install | MoltenVK v1.4.2 inside the app; signed ad hoc: "Open Anyway" in Privacy & Security once |

## What the player gives it

An extracted disc: the folder holding `default.xbe` and the game's `bundles`, `demand` and `xwavebank` folders (`input=folder` in `game.txt`), made with extract-xiso or xdvdfs. The launcher runs `qilin identify <folder> --json --build "Grabbed by the Ghoulies (USA)"` (`recomp/src/identify.cpp`) when the folder is chosen and at every start:

| Exit | When | The message says |
|---|---|---|
| 0 | the certificate's title ID is 4D530053 (MS-083) and the XBE's timestamp 1064426049 (24 September 2003): the USA release | "This is Grabbed by the Ghoulies (USA), MS-083 (built 24 September 2003): the right disc." |
| 3 | another title ID | the game the certificate names, its region and ID, and what this download is for |
| 3 | the same title ID, another timestamp | another release of the game: its region and build date, and the one needed |
| 2 | no such folder; a file (such as the `.iso`) rather than a folder; no `default.xbe`; an XBE that doesn't parse; no `bundles`, `demand` or `xwavebank` folder (any case) | what's wrong and how to extract the disc |

`title`, `id` (the certificate's title ID, as "MS-083") and `version` (the certificate's version: 2 on the USA disc) describe what was chosen. Nothing is written; only `default.xbe` is read.

## The build on the player's computer

`game.txt`'s one step is `{qilin} lift {input}/default.xbe --out {gen}`: about 7,700 functions into 41 files (61 MB; the largest 2.5 MB) in five seconds. The launcher compiles every `.cpp` there with `build-flags.txt`'s flags (`-O2 -std=c++23 -ffp-contract=off -fno-fast-math`, the same floating-point rules as `qilin_run`, plus this processor's `-march=native` or `-mcpu=native`) and links them with `kit/lib/libghoulies_kit.a`: the runtime's start-up and dispatch (`ghoulies_kit_main`) and every library they need, SDL3 and volk included. The largest file took 11 s and about 340 MB with zig's Clang on a desktop PC, so `compile_memory_mb=800` leaves room: a 2 GB Raspberry Pi runs two compilers at once. The game is started as `work/Ghoulies[.exe] --game-dir <folder> <folder>/default.xbe`, in the download's folder; its output goes to `work/run.log` and `work/run-errors.log`. Saves and settings stay in the runtime's own folder, `qilin/ghoulies` in the system's per-user data folder, which is the launcher's too (the chosen folder in `launcher-disc.txt`, the object cache in `build-cache`).

## How each is built and checked (CI)

- **Windows** (`.github/workflows/player-kit.yml`, `tools/player-kit/package.sh`): cross-compiled on Ubuntu with llvm-mingw (`mingw-cross.cmake`); the kit's archive merged by an MRI script (`merge.cmake`) and linked with stand-ins for the lifted code (`ghoulies_kit_link_check`); the game's icon (`game_icon.rc`, the launcher's) is `kit/lib/icon.o`; a 16 MB main stack. The workflow unzips the result and checks its layout against `kit/files.txt`.
- **Linux** (`player-kit-linux.yml`, `package-linux.sh`): built on Ubuntu 22.04 (x86-64 and ARM64 runners) with the zig the download bundles (`zig cc`/`zig c++` with `-mcpu=baseline`), so the archive's libc++ is the one the player's link uses and glibc 2.35 is the floor. SDL's own Wayland protocol headers come before the system's (`-iquote`), as zig searches `/usr/include` first. The launcher's tests and `qilin_tests` run with the bundled zig; `smoke-linux.sh` unpacks the tarball and checks the layout, compiles and runs a C++23 program with `build-flags.txt`, links the kit with an empty function table and runs it (it must stop with its usage), runs `qilin identify` on stand-ins, and checks every program's glibc symbols and libraries.
- **macOS** (`player-kit-macos.yml`, `package-macos.sh`, `macos.cmake`): universal (`arm64;x86_64`), the archive merged by Apple's `libtool`, `build-flags.txt` with each processor's own flags (`native.arm64`, `native.x86_64`, `ldflags.<arch>`). The app holds the launcher, MoltenVK's own `libMoltenVK.dylib` (pinned by SHA-256; the launcher starts the game with `DYLD_FALLBACK_LIBRARY_PATH` and `SDL_VULKAN_LIBRARY` pointing at it), the kit and the licences, and is signed ad hoc. `check-macos.sh` runs on Apple silicon and Intel: the layout, `lipo`, `codesign --verify --deep --strict`, `files.txt`, `qilin identify`, and a stand-in game compiled with Apple's clang and the kit's flags, started (it must stop with its usage).
- On a `vX.Y.Z` tag each workflow attaches its files to that release, creating it if none of the others has yet.

## Raspberry Pi

The Linux ARM64 download, built on the Pi itself. Vulkan 1.3 for the Pi 4 and 5 came with Mesa 24.3 (Raspberry Pi OS Trixie); the launcher checks the driver before building and says to update an older system. It needs the 64-bit Raspberry Pi OS (the README says so first, with `getconf LONG_BIT`). The first build compiles the whole game on the Pi: measure it there.

## Not in this pass

The iPad ("Play on iPad" from the Mac app, which Toy Commander has): the launcher shows it only where the app carries `Contents/Resources/ios-kit`, which this download doesn't. It would need the runtime ported to iPadOS first (its own stack for the game, MoltenVK linked statically, the disc folder in Documents).

## What only a player's run can confirm

The launcher's window, folder chooser and drag-and-drop on each desktop; a whole first build and the game starting from it on each system (CI builds and links only stand-ins); the menu entry and shortcut; an update from one release to the next; Gatekeeper's "Open Anyway"; and the time and memory of a first build on a Raspberry Pi 4 and 5.
