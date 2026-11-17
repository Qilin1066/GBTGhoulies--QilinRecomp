# Grabbed by the Ghoulies — Recomp Roadmap & Design Targets

**Guiding principle: efficiency and reliability come first.** Every design choice is judged on whether it makes the game cheaper to run on low-end hardware and whether it fails loudly and predictably instead of silently misbehaving.

## Targets
- **Platforms:** 64-bit Windows, Linux and macOS (Apple Silicon), on x86-64 or ARM64, with Vulkan. See *Hardware scope* below.
- **Language:** **C++23** throughout (recompiler, generated code and runtime). Minimum compilers: GCC 14, Clang 19, Xcode 16, Visual Studio 2022 17.10. No DirectX anywhere.
- **Graphics API:** **Vulkan only**, restricted to a small baseline feature set (today: what Mesa's v3dv driver on a Pi 4 supports), so one render path works everywhere (MoltenVK on macOS).
- **Reference:** xemu running the original game is ground truth for behaviour and visuals. The existing Windows-only Ghoulies PC Recomp (xboxrecomp-based, D3D11) is a second behavioural reference; its known-issue list doubles as a regression checklist (see `prior-art.md`).
- **Enhancement example target:** 3440×1440 (21:9) at 144 Hz.

## Architecture
- **Game code:** static recompilation, x86 → **generated C++**, compiled AOT by clang/MSVC for each target. No interpreter, no JIT. Hand-written C++ replaces lifted functions over time (hot paths, bugs, enhancement hooks) through a function override table.
- **Decoder:** **Zydis** (MIT, proven in XboxXBERecomp), vendored into the repo, and **verified instruction by instruction against LLVM's disassembler across the whole binary** in CI. Own decoder only as a fallback. (Revised after prior-art review; see `prior-art.md`.)
- **XDK symbol naming:** XbSymbolDatabase (MIT, Cxbx-Reloaded's OOVPA signatures) to name D3D8/DSOUND/XAPI/CRT functions; own masked-hash signatures only where it lacks XDK 5659 entries.
- **Register model:** guest registers held in host locals inside each lifted function and synced to a context struct only at call/indirect boundaries. No global or thread-local register variables (that model blocks host register allocation).
- **Calling conventions:** the game is VC++ 7-era LTCG code with custom register conventions (e.g. `eax` argument at `0x1DF70`). Lifted code keeps full register state across calls; ABI recovery only where hand-written C++ replaces a function.
- **Fail loudly:** an undecodable or unsupported instruction is a build error, never a comment; an unknown indirect-call target at runtime traps with its address logged.
- **Control flow:** recursive-descent CFG with resolved switch tables (done in analysis tooling, ported to the C++ recompiler). Indirect calls dispatch through a generated sorted function table; an unknown target traps with the address logged (never silently continues), and the address is added as a seed.
- **Lifter efficiency:** flag elimination via dataflow analysis (compute only flags that are read); scalar SSE → native `float` (bit-exact on ARM); x87 → native `double`, honouring the game's x87 precision-control setting and Pentium-III-compatible trig reduction where it matters; flat memory (4 GB host reservation, `base + addr`, no bounds checks). Both platforms are little-endian.
- **Memory mirrors:** the Xbox maps RAM at several aliases (xboxrecomp needs 28 mirror views). Because we replace D3D/DSOUND, we hand out contiguous memory ourselves; during bring-up any access above the game's normal range traps so we learn exactly which aliases (if any) the game uses, and only those get aliased mappings (file mapping / memfd).
- **Exceptions:** game uses C++ EH / SEH frames (`fs:0`); strategy to be fixed during lifter prototyping (map to host C++ EH vs explicit frame chain).
- **Kernel + XDK libraries:** XAPI, CRT, D3D8, DSOUND, XACT, XPP, WMADEC, Bink are replaced with native C++ implementations, not recompiled. Library boundaries are known (see `code-analysis.md`).
- **Graphics:** D3D8 API-level translation to Vulkan, not NV2A emulation. **Plus a minimal NV2A command interpreter for `BeginPush`/`EndPush`**: the game writes raw `SET_BEGIN_END` + `INLINE_ARRAY` immediate-mode draws in 17 functions; these become `DrawPrimitiveUP`-equivalents with the current vertex format. Unknown methods assert. Shaders/combiners converted to SPIR-V offline; prebuilt pipeline cache (no runtime compile stutter).
- **Video:** Bink 1 movies decoded with FFmpeg (libavcodec), uploaded as Vulkan textures.
- **Audio/Input:** DirectSound/XACT → native mixer on SDL3 or miniaudio; WMA via FFmpeg; controller → SDL.
- **Assets:** `.bnl` bundles are zlib streams → system zlib (replaces the zlib copy inside the game).
- **Threads:** audio mixing and Vulkan submission off the game thread.
- **Portability rules:** no inline asm or x86 intrinsics in the runtime; ARM64 Linux builds from day one.

## Hardware scope
**Supported: 64-bit hosts with a Vulkan driver.** In practice that means:
- **Windows and Linux** on x86-64 or ARM64 with a Vulkan-capable GPU and driver;
- **macOS on Apple Silicon**, through MoltenVK (Vulkan on top of Metal);
- **ARM boards** with Vulkan drivers, e.g. Raspberry Pi 4 and newer (Mesa v3dv).

**Out of scope:** anything without Vulkan (or, on Apple, a Vulkan translation layer), and 32-bit hosts. That rules out the Raspberry Pi 1–3 and Zero (OpenGL ES 2 only), and Intel Macs aren't a target. Because of this there is one renderer (Vulkan) and one memory model: the Xbox's 4 GB address space mapped flat into a 64-bit host's address space, with no windowing cost on memory accesses.

**Low-end reference:** a Raspberry Pi 4–class board is our reference for typical low-end hardware. It's what we measure efficiency against and test on. It is not a floor: anything Vulkan-capable that's slower is fair game if the efficiency work gets us there. The Vulkan feature set we use is kept small (today: what v3dv on a Pi 4 supports) so the one render path runs on all of it.

## Settings, not platform branches
All enhancements live behind a settings file with presets, e.g.:
- *Low-end:* 1× resolution, no AF, no MSAA, original aspect and frame rate.
- *Desktop:* scaled resolution, 16× AF, MSAA, widescreen, interpolated high refresh rate.

## Roadmap
1. **Analysis** ✅ headers, sections, kernel imports, XDK versions; function discovery, CFG, library boundaries, instruction profile, raw NV2A usage, asset formats.
2. **Recompiler core (C++23):** 🔨 in progress, repo `Tecboy1066/GBTG-Qilincomp`. ✅ XBE loader, ✅ Zydis decoder (100% agreement with LLVM on every reachable instruction), ✅ CFG port + data-as-code validation (9,197 functions), ✅ lifter slice 1: leaf functions, integer + scalar SSE (1,373 functions across game code and Rare libraries), ✅ differential test against the original x86 under Unicorn (10,984 trials at -O2 plus a UBSan run, 0 mismatches), ✅ CI with warnings as errors on Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64, Windows. Next: slice 2 (calls, function table, indirect-call trap), slice 3 (x87), fold epilogue fragments into their parents. See `recompiler-core.md`.
3. **Runtime skeleton:** memory map, function table + trap, kernel/XAPI HLE stubs, threads, file system over the extracted disc.
4. **Boot to first frame:** run the entry point natively, push the first Clear/Present through Vulkan.
5. **Playable at original spec:** 640×480, 4:3, original frame rate; matches xemu; tested on low-end reference hardware at every milestone.
6. **Resolution / AF / MSAA:** scaled render targets (decide per render-to-texture target), HUD half-pixel fixups, CPU framebuffer readback handling.
7. **Widescreen 16:9 → 21:9:** patch the projection (Hor+), widen the culling frustum, re-anchor the HUD, pillarbox 2D/FMV, per-scene aspect caps where the extra view reveals unfinished geometry.
8. **High refresh rate:** ✅ investigated (`frame-timing.md`). The game is variable-timestep by design (dt = vblanks since last present / 60, clamped to 5 frames, plus a 300 Hz tick accumulator, computed in `0xFC030`). Plan: override `0xFC030` to feed real elapsed time, replace the busy-wait loops (`0x32680`/`0x32700`) with sleeps, pace frames from the Vulkan swapchain. Playtest 30/60/144/240 Hz against xemu; optional fixed-step + interpolation mode kept only as a fallback for systems that assume dt ≥ 1/60.
9. **Extras:** texture replacement packs (hash on upload), etc.
