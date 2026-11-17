# Prior art: original-Xbox recompilation & decompilation (researched 2026-11-17)

What exists, what each project chose, and what it means for our project (Vulkan, C++, cross-platform incl. Raspberry Pi 4, efficiency and reliability first).

## 1. Ghoulies PC Recomp (SeveerJ) — same game, already playable
- v0.1 beta "test build for playtesters" (story dated 2026-11-06); README says the game is playable incl. cutscenes, challenges and the Reaper. US release only.
- Built on sp00nznet's **xboxrecomp** plus toolkit changes from SeveerJ's Orta PC Recomp. Game code recompiled **to C**; runtime reimplements kernel, **NV2A GPU on Direct3D 11** (behaviour matched against xemu), audio, input, Bink video. **Windows 10/11 x64 only.**
- Game-specific code: `recomp_manual.c` (hand-written replacements incl. **real-time frame timing** and a Bink copy wrapper), ImGui F1 overlay, `seeds/` = function addresses discovered during play that static analysis missed.
- Options: 30 (default) / 60 / uncapped fps while "keeping real-time speed"; widescreen; supersampling up to 2880 lines; FXAA; 5.1 audio.
- Installer downloads Python + capstone + llvm-mingw (~300 MB) and translates/compiles on the user's PC (20–30 min).
- **Known issues** (docs/KNOWN_ISSUES.md): busy rooms (mummy room, fire imps) drop below 60 fps on many PCs; 4-core laptops fall well below 60 fps at higher resolutions; **shader-build stutter on first visit to a room**; supersampling causes pauses of a second or more; a box around enemy **death particles**; missing Yes/No text on quit prompt; occasional hang on first loading screen; one unreproduced crash.
- Licences: xboxrecomp MIT; xemu-derived files LGPL-2.1+; ImGui MIT.
- PCGamingWiki also lists a second effort, **gbtg_recomp** (C++, made with XboxXBERecomp, in development).

**Takeaways for us:** this port proves the game can be recompiled and played, but its approach (GPU emulated at NV2A level, runtime shader compilation, D3D11, Windows-only) is heavy. Its own issue list shows modern PCs struggling to hold 60 fps, which rules that approach out for a Pi 4. Our differentiators stand: Vulkan, cross-platform/ARM, D3D8 replaced at API level, shaders and pipelines built ahead of time. It is also a second behavioural reference next to xemu, and its seed addresses are a free cross-check for our function discovery. The "real-time frame timing" note and 60 fps/uncapped option at correct speed suggest the game may tolerate variable frame time, which could make our 144 Hz goal much easier than render interpolation. Investigate the main loop's timing early.

## 2. xboxrecomp (sp00nznet) — the general toolkit behind #1
- v0.17.0. Python pipeline: XBE parser → disassembler (**Capstone**) → func_id (CRT/D3D/vtables/game; optional Ghidra FidDb naming) → ABI analysis (calling conventions; otherwise everything falls back to cdecl) → recompiler emitting C (`recomp_NNNN.c`, `recomp_dispatch.c`).
- **Register model:** guest registers are thread-local globals; every function is `void f(void)`; guest stack simulated. Flags in globals, "published" at call/ret only where the far side reads them.
- **Unhandled instructions become `/* mnemonic */` comments** (segment prefixes and rare x87/SSE forms cluster there). Lifter coverage "good but unquantified".
- x87: stack, trig reduction with 66-bit π, fprem/fscale/fnsave; MXCSR modelling; checked against the real CPU with `fpdiff.py`. Changelog: x87 results once **ignored precision control** (fixed: PC=24 rounds to 24 bits); 53-bit host precision gave `cos(100.0)` = −1.27e16 before extended precision was handled; some trig rows differ on AMD hosts because only Intel x87 reduces like the Pentium III.
- **Memory:** Xbox address space reproduced with CreateFileMapping + **28 mirror views** (VirtualAlloc can't alias); several bugs around mirrors and reservations (mirrors landing on host libraries with bare MAP_FIXED; 1.8 GB reservation leak).
- Indirect calls: `RECOMP_ICALL` → generated `recomp_lookup(xbox_va)` + hand-written overrides; ICALL trace ring buffer. Jump tables incl. MSVC byte-index tables. SEH prolog/epilog bridged per title.
- Runtime: all 371 kernel ordinals routed; D3D8 → D3D11 (or GL 3.3 on POSIX) with FFP, combiners, NV2A vertex microcode → HLSL, DrawPrimitiveUP ring buffer, unswizzling; **NV2A pushbuffer executor for titles that statically link the XDK's D3D** (i.e. most retail games, incl. Ghoulies) using xemu register code; MCPX APU emulation from xemu; FMV via Media Foundation (Windows only).
- Performance: Burnout 3 runs on a software NV2A at ~14 fps menus / 2–4 fps races. Rosetta can't run 32-bit x86, so their conformance tests run on i386 Linux in Docker.

**Takeaways:** avoid the global-register model (every register access is a TLS load/store; the host compiler can't keep guest registers in host registers). An unhandled instruction must be a build error, never a comment. Plan the memory-mirror question explicitly. Honour x87 precision control and Pentium III trig reduction where the game depends on it. Their `fpdiff` idea matches our differential-testing plan.

## 3. XboxXBERecomp (TGP482; seen via forks) — closest to our architecture
- Made for Splinter Cell: Double Agent, then BLACK. Output **C++**, ahead of time, 64-bit Windows executables. Decoder: **Zydis**.
- Kernel and XDK libraries (D3D8, DirectSound, XAPI, XNet) **implemented at the API level**. Native **Direct3D 12** renderer translating vertex programs, pixel combiners and the fixed-function pipeline to HLSL, on its own render thread.
- XDK library detection: per-build `xdk/<build>/signatures.toml` + `layout.toml` (function lengths and hashes with addresses masked, never library code). The tool **stops if no signature set exists for the XBE's build**; only `xdk/5849` is visible (**our game is 5659**).
- x87/SSE/MMX translated; x87 register stack tracked at run time; follows the game's x87 precision setting; **C++ exceptions supported**. Headless harness builds and runs a game on Linux without window/GPU/audio for testing. Incremental generation (only rewrites changed files). Deps: Zydis, toml++, ImGui, SDL3. Runtime Windows-only.

**Takeaways:** independent confirmation that API-level D3D8 replacement plus a dedicated render thread is the fast design. Zydis is a proven decoder choice. A headless harness is worth having from day one.

## 4. Halo 2 decompilation (kirklandsig) — matching decomp
- Retail `halo2ship.exe`, XDK 5849, **Visual C++ 7.1 (cl 13.10.3077), `/GL /Gr`, `/LTCG`**. `tools/check.py` builds one LTCG image and compares every function byte for byte; status per function in `config/functions.csv`.
- **Under LTCG, 42.5% of called functions take arguments in eax/ebx/esi/edi** (custom conventions). Library code identified by byte signature from the SDK `.lib` files and not decompiled ("a port would replace them").
- Progress: 10,196 of 11,326 game functions matched (63.9%); decomp runs in reviewed automated lanes driven by Claude and Codex. GPL-3.0-or-later.

**Takeaways:** Ghoulies is the same compiler generation. We've already seen a function taking its argument in `eax` (`0x1DF70`), so expect LTCG custom conventions. The lifter must keep full register state across calls and never assume cdecl/stdcall. ABI recovery is only needed where hand-written C++ replaces a function. A matching decomp is a possible long-term path to readable source, but not our first step.

## 5. FFmpeg Bink decoder
- Revision taken from the fourcc's top byte; `swap_planes` for version ≥ 'h'; for ≥ 'i' 32 bits are skipped before the planes; alpha via `BINK_FLAG_ALPHA`. Our movies are **`BIKi`** → handled. LGPL-2.1+ → link dynamically.

## 6. Other resources found
- **XbSymbolDatabase** (Cxbx-Reloaded, **MIT**): OOVPA signatures that detect XDK symbols across builds from reverse-engineered retail titles; used by ghidra-xbe for library symbol recovery. Best candidate for naming the D3D8/DSOUND/XAPI functions Ghoulies calls (5659 coverage to verify).
- **Anybox**: an original-Xbox static recompiler that produces native **arm64 macOS** apps (lifted C compiled by clang, runtime in Rust). ARM targets are proven viable.

## Decisions changed or added
1. **Decoder:** use **Zydis** (MIT, proven in XboxXBERecomp), vendored, and cross-check against LLVM on every instruction. This replaces the earlier "own decoder" plan (own decoder only as a fallback). Needs network access or the user dropping the source into the project folder.
2. **XDK symbol naming:** XbSymbolDatabase (MIT) first; our own masked-hash signatures only where it lacks 5659 entries.
3. **Register model:** guest registers live in host locals inside each lifted function, with a context struct passed at call boundaries (not TLS globals).
4. **Unknown instruction or decode failure = build error.** Unknown indirect target at runtime = trap with logged address.
5. **Memory:** decide the mirror strategy up front (contiguous allocations we hand out ourselves; trap on unexpected high-address access during bring-up; aliasing via memfd / file mappings only if needed).
6. **x87:** honour precision control; Pentium-III-compatible trig reduction where the game uses fsin/fcos/fptan.
7. **Frame timing:** analyse the main loop early; variable-dt support would turn 144 Hz into "uncap + fix timer".
8. **References:** xemu + Ghoulies PC Recomp for behaviour; its seed list to cross-check our function discovery; its known issues (death-particle box, quit-prompt text) as regression checks for our renderer.
