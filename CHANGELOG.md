# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

## [0.25.0] - 2026-11-18
The game makes sound: its music, effects and menu sounds play through the speakers.

### Added
- **Sound output through SDL3** (`platform/audio_out.*`): the mixer's 48 kHz output to the default playback device. A 60 ms cushion keeps the device fed between catch-ups, held there by trimming the playback rate (at most 0.5%) against clock drift. `--mute` plays nothing.
- **DirectSound streams**, which XACT plays its wave banks through: `CreateSoundStream` and the stream's methods, reached through the original's vtable (`Process`, `GetStatus`, `Discontinuity`, `Flush`, `FlushEx`, `Pause`, `SetFormat`, the setters). Voices gain a packet queue: packets play as one signal, starve and resume, stop after a discontinuity. Finished packets are reported as the console's DPC reports them (status, size, event, callback), only below DISPATCH_LEVEL. `GetStatus`, `Pause` and `FlushEx` follow the original's code.
- **`GetVoiceProperties`** for streams and buffers, with each mix bin's volume as the original reads it back from the APU (bin volume plus voice volume less headroom, in the hardware's 1/64 dB steps).
- **The window and sound keep going while the game waits** without swapping (its busy loops call `QueryPerformanceCounter`), so the disc-error screen's loop no longer freezes the window.
- Tests: streams (packets back to back, starvation, discontinuity, flush).

## [0.24.0] - 2026-11-18
The game takes input: past "Press START" to its save-slot menu, its on-screen keyboard and a signed save.

### Added
- **Controllers through SDL3** (`platform/input.*`, `host/xinput.cpp`, `docs/input.md`): XInput's ports over SDL's gamepads (any controller SDL knows, USB or Bluetooth), mapped onto the Xbox controller's layout (Black and White on the shoulder buttons); `XGetDevices`, `XGetDeviceChanges`, `XInputOpen`, `XInputClose`, `XInputGetState`, `XInputSetState` (rumble). Controllers take the lowest free port and keep it. The keyboard is controller 1, merged with the controller there or on its own.
- **`D3DDevice_CopyRects`:** the game copies its frame into textures (a turning page shows the screen it turns from). A frame buffer is read back from the GPU; the destination is written in guest memory in its own layout.
- **Immediate mode:** `Begin`, `End` and `SetVertexData2f`, `4f` and `Color`, as the GPU's attribute registers: a vertex at each position write, drawn at `End`. A register written outside `Begin`/`End` is the constant value of that attribute for later draws without it, as on the NV2A.
- **The fixed-function vertex pipeline as a generated NV2A program** (World × View × Projection, colours and texture coordinates passed through), translated like the game's own programs; **the fixed-function pixel pipeline as register-combiner settings** (select, modulate, add). What isn't modelled of either stops the runtime with what it is.
- **The kernel's SHA-1 and HMAC** (`XcSHAInit`, `XcSHAUpdate`, `XcSHAFinal`, `XcHMAC`), which XAPI signs saves with. Tested against the FIPS 180 and RFC 2202 vectors.

## [0.23.0] - 2026-11-18
The game is shown in a window.

### Added
- **A window, through SDL3** (`recomp/runtime/platform/`): sized to the largest whole multiple of 640×480 that fits the main display, resizable, the picture scaled to fit with its shape kept (black bars where the window's shape differs). F11 or Alt+Enter switch fullscreen; closing the window stops the runtime. Events are handled at each `Swap`, on the game's own thread.
- **Presenting in the renderer:** a swapchain made again when the window changes size or the driver says it's out of date, MAILBOX where the GPU has it and FIFO otherwise (the game paces itself, as on the console), nothing presented while minimised. The renderer still knows nothing of SDL: the window gives it the surface, the instance extensions and Vulkan's loader, so both use the same Vulkan library. Clean under the validation layer, resizes included.
- **`--headless`** runs without a window (frames are still drawn, for `--dump-frames`); **`--fullscreen`** starts fullscreen. Where there's no display, `qilin_run` says so and runs headless.
- **SDL3 in the build:** an installed SDL3 (3.2 or later) is used if CMake finds one; otherwise SDL 3.4.20 is downloaded (checked against its SHA-256) and built static, without the parts the runtime doesn't use. CI installs the Linux headers SDL's back ends need; the README lists them.

## [0.22.0] - 2026-11-18
The game draws. The attract sequence renders on Vulkan: the candle, the book opening, the "Press START" text and its effects.

### Added
- **A SPIR-V builder** (`gfx/spirv.*`): the renderer makes its shaders itself, with no shader compiler to build or ship.
- **NV2A vertex programs as SPIR-V** (`gfx/nv2a_vs.cpp`): every MAC and ILU operation, swizzles, negation, write masks, `a0.x`-relative constants, outputs clamped as the hardware does; screen space (what the program computes, D3D's viewport transform included) to clip space.
- **The register combiners as SPIR-V** (`gfx/nv2a_ps.cpp`): texture stages (PROJECT2D, PROJECT3D, CUBEMAP, PASSTHRU, CLIPPLANE), up to eight general combiner stages with every input and output mapping, dot products, muxes and blue-to-alpha, the final combiner, the alpha test. Other texture modes stop the runtime.
- **Textures** (`gfx/texture.*`): swizzled, linear and DXT1/3/5 formats, with mip levels, cube faces and volumes, decoded to RGBA8; cached by header and address and re-uploaded when their texels change.
- **The renderer draws:** pipelines (cached), dynamic depth, stencil, cull and scissor state, a ring buffer for each submission's vertices, indices and uniforms, samplers. Draws issue as the game makes them, in order with clears. Quads, fans, loops and polygons become indexed lists. Clean under the Vulkan validation layer.
- **Derived GPU state at draw time:** the viewport's screen-space scale and offset (as D3D computes them, read by the programs as c-38 and c-37), blend, depth, stencil, cull and colour-write state from D3D's render states, constants packed by the combiners.
- **Tests:** generated shaders checked with `spirv-val`'s rules (both validate), a draw through a generated vertex program and combiner read back, texture creation, swizzling.

### Fixed
- **`SetVertexShaderConstantNotInline(Fast)` take a count of dwords,** not registers: four times too much was read, filling later constants with whatever followed.
- **The device's initialisation sets its render target** with `SetRenderTarget`, as the original, so the multisample scale (from the multisample type's factors) and the viewport derived from it are set; `Swap` moves a back-buffer render target to the new back buffer.
- **Frame dumps are opaque,** as a TV shows them.

## [0.21.0] - 2026-11-18
Bink is lifted, and the game runs its attract sequence: about 1,100 frames in two minutes, loading movies and bundles from the disc as it goes. Its draws are still recorded, not drawn.

### Added
- **Bink lifted:** RAD's video codec and its pixel-format blitters (`0x1A59E0`–`0x1BABA0`). Its MMX probe is answered as the Xbox's Pentium III (it has MMX).
- **MMX arithmetic for Bink** (`movd`, adds, saturating subtracts, multiplies, logic, shifts, unpacks) and **`rdtsc`** (the runtime's 733 MHz counter) in the lifter.
- **Function discovery:** Bink's blitter tables in `BINKDATA`; immediate code pointers after a function's final `jmp`; the immediate-pointer round repeated until it finds nothing (DirectSound's full-HRTF table). The game's own code is unchanged.
- **DirectSound submixes** (`DSBCAPS_MIXIN`, `SetOutputBuffer`) in the audio engine, with a test.
- **DirectSound's 3D calculator lifted** (the maths XACT uses to position sounds), with the full-HRTF table and DirectSound's start-up initializers, which now run.
- **`D3DResource_Release` as the original:** a surface releases its texture, and a resource with no references left is destroyed by D3D's own `D3D_DestroyResource` (lifted), which frees it to the game's allocator; so is one the device drops last.
- **Missing disc files are named** once (`… isn't in the game folder`): a game folder copied without one shows up as the game's disc-error screen.

## [0.20.0] - 2026-11-18
The game gets through its first frames: shaders, streams, textures, constants, render targets and draws are in, and each frame's draws are recorded for the renderer (not drawn yet). The first frames are the attract sequence's loading; it stops next at a DirectSound submix buffer.

### Added
- **Shader and stream state, as the original:** `SetTexture`, `SetStreamSource` (lifted), `SetVertexShaderInput`, `SetPixelShader` (the definition's registers into `D3D__RenderState`), `SetPixelShaderConstant` (packed to colours and routed to the combiner constants the shader maps), `SetVertexShader`, `LoadVertexShader` and `SelectVertexShader` (the GPU's 136-slot program memory kept by the runtime), the four `SetVertexShaderConstant*`, `SetRenderStateNotInline`, `SetTextureStageStateNotInline` and the four `SetTextureState_*`, `SetScreenSpaceOffset`.
- **The device's default state:** `CreateDevice` now does what the original's initialisation does to guest-visible state: the fixed-function shader for `XYZRHW` then `XYZ`, identity transforms, the render- and texture-stage-state defaults (D3D's own routine at `0x179FE0`, lifted).
- **Vertex shader objects as the original builds them** (attribute slots from the declaration, the program and constant loads in push-buffer form), with D3D's own helpers, lifted, from the game's allocator. `SetVertexShader` and the draws read them.
- **`SetRenderTarget`:** internal references, the depth buffer's range and format bits, the anti-aliasing mode and the multisample scale and viewport, with D3D's helpers.
- **Draws:** `DrawVertices`, `DrawIndexedVertices`, and `BeginPush`/`EndPush` (the game's own GPU methods: `SET_BEGIN_END` and `INLINE_ARRAY`, decoded; any other method stops the runtime). Each draw consumes the dirty state as the original's state flush does and is recorded with a copy of the vertices it reads.
- **Fences:** `InsertFence` (the device's counter, as the original) and `BlockOnFence`.
- **A push-buffer sink:** D3D's internals that run lifted write their GPU methods into it; `MakeSpace` and `BeginStateBig` rewind it. Nothing reads it: the renderer works from D3D's state.
- Lifted helpers difftested: 0 mismatches (`0x17B080`'s jump table is unbounded on purpose; random formats outside the depth formats fault).

### Fixed
- **`CreateVertexShader` made a 16-byte placeholder object;** `SetVertexShader` and the draws read the attribute slots and flags of a real one.

## [0.19.0] - 2026-11-18
D3D state stays at API level (decided): the setters are rewritten natively. The game sets its viewport and render states and stops at its first `SetTexture`.

### Added
- **`D3DDevice_SetViewport`:** as the original, the viewport clipped to the render target into the device (`+0xED0`) and its dirty bit.
- **The 30 `D3DDevice_SetRenderState_X` functions:** each value into its slot of D3D's render-state table, with the dirty bits the original sets, from a table read off the originals.
- **`D3DDevice_GetProjectionViewportMatrix`** stops the runtime, named, until the derived matrix is reproduced (it was lifted, and would have read a matrix nothing computes).


## [0.18.0] - 2026-11-18
D3DX is lifted, and the first state setters are in. The game sets its transforms and stops at its first viewport.

### Added
- **D3DX lifted** (the `D3DX` section, 24 functions of matrix and vector maths): 2,376 difftest trials, 0 mismatches.
- **`D3DDevice_SetTransform`** (the matrix into the device and its dirty bits, as the original) and **`D3DDevice_SetRenderState_Simple`** (the GPU method and value kept; the D3D value is already in `D3D__RenderState`). **`D3DDevice_GetTransform`** lifted.
- **`graphics.md`: the open design question** for D3D state: native setters at API level, or lifting D3D's setters and reading the GPU commands they write.


## [0.17.0] - 2026-11-18
Vertex programs are decoded, and the game creates all its shaders. It stops at its first transform.

### Added
- **NV2A vertex programs** (`recomp/runtime/gfx/vsh.*`): decoding the microcode `CreateVertexShader` receives (MAC and ILU operations, operands with swizzles and negation, temporaries, inputs, constants with a0.x, output, temporary and constant writes) and disassembling it. The game's 24 shaders, assembled at start-up by the lifted `XGAssembleShader`, each match their source text once the assembler's own rewrites are allowed for; `qilin_gfx_tests` checks one, as the assembler produced it. `QILIN_DUMP_VSH=1` (or `2`, with raw words).
- **`D3DDevice_CreateVertexShader`:** keeps the program and its declaration; the handle is the object's address with bit 0 set, as the original's.
- **Lifted:** `D3DDevice_CreatePixelShader`, `D3DDevice_DeletePixelShader` and `D3D_AllocContiguousMemory` (allocation and copies over the game's allocator).

### Fixed
- **`qilin_gfx_tests`** reports failures of its GPU-free checks even where there's no Vulkan device to run the rest.


## [0.16.0] - 2026-11-18
Guest RAM is mapped at the Xbox kernel's windows, D3D's resource code is lifted, and the game creates its textures. It stops at its first vertex shader.

### Added
- **Guest RAM at three addresses** (`recomp/runtime/host/guest_ram.cpp`): one shared-memory object mapped at 0, `0x80000000` and `0xF0000000` in a 4 GB host reservation, as the kernel maps physical memory. D3D's locks hand out addresses in those windows and the game writes vertices and pixels through them. Linux `memfd`, macOS `shm_open`, Windows placeholder views (`MapViewOfFile3`, Windows 10 1803+). The checked build accepts the two windows and still stops on anything else outside RAM. A host test checks the views share pages on every CI platform.
- **D3D's resource code lifted** (18 functions, `kLiftedD3d`): creating textures, surfaces and vertex buffers, surface levels, descriptions, locks. Native: `D3D_BlockOnResource` (no GPU holds guest memory) and `D3DVertexBuffer_Lock2`. The device fields the lifted code reads are kept current in the original layout.
- **Function discovery:** an immediate pointer right after a `ret n` of a function already found is an entry. Six callbacks found, including XAPI's thread start routine and XACT's DPC routine; game code unchanged; 594 difftest trials, 0 mismatches.
- **`audio.md`: the surround plan** (5.1 and 7.1 output from the engine's six speaker bins, with the game told the console is set to surround).

### Fixed
- **Multisampling:** `0x11` is `D3DMULTISAMPLE_NONE` on the Xbox; the game asks for none, and the runtime no longer says it's unmodelled. Other types now stop the runtime.
- **Frame buffers have guest memory** behind them, as the original's do, so a CPU lock (the disc-error screen) writes RAM and not address 0.


## [0.15.0] - 2026-11-18
Audio starts. XACT, the audio engine, is lifted; DirectSound beneath it is replaced over a software mixer. XGraphics is lifted too. The game gets through its audio start-up and first asset loads, and stops at its first texture.

### Added
- **XACT lifted** (the `XACTENG` section, the 332 functions lifted code reaches) and **XGraphics lifted** (the `XGRPH` section, 647 functions): 7,221 functions lift, 1,478 of them library functions. Three pure DirectSound helpers are lifted with them (`XAudioCalculatePitch` and the PCM and ADPCM format builders). Differential tests: XACT 7,950 trials, XGraphics 15,516, the helpers 1,584, **0 mismatches**.
- **Lifter:** 16-bit `imul`; `rol`/`ror` by an immediate or `cl`; `movntps`; MMX (`movq`, `punpcklwd`, `punpckhwd`, `emms`) with its own registers (`Context::mm`) and the x87 state set as the hardware does; device registers: constant-address loads and stores in `0xFD000000`–`0xFEFFFFFF` lift to `rt::mmio_read32`/`rt::mmio_write32`. Unit tests for each.
- **The audio engine** (`recomp/runtime/audio/`): voices playing 8/16-bit PCM and Xbox ADPCM from guest memory, pitch, volume, mix bins folded to stereo, play and loop regions; a 48 kHz stereo mix on the host's clock; a WAV writer. `qilin_audio_tests` in CI.
- **DirectSound** (`recomp/runtime/host/dsound.cpp`): `DirectSoundCreate`, buffers (create, data, format, volume, pitch, headroom, mix bins and their volumes, regions, position, play, stop, `StopEx`, status), `CommitDeferredSettings`, `DoWork`, `SetMixBinHeadroom`, `DownloadEffectsImage`, `SetI3DL2Listener`, `UseFullHRTF`. Envelopes, LFOs, filters, 3D and the DSP effects are recorded and named once as not yet applied.
- **The APU's sample counter** (`0xFE80200C`), which XACT reads as its clock, from the mixer's clock.
- **Kernel:** `KfRaiseIrql`, `KfLowerIrql`, `KeRaiseIrqlToDpcLevel`, on the KPCR's IRQL byte.
- **`qilin_run --dump-audio FILE.wav`.**
- **`docs/audio.md`.**

### Fixed
- **Preloaded XBE sections** start with a reference count of 1, as the kernel's loader counts them; a title's `XeLoadSection` on one stopped the runtime.
- **Difftest:** the guest context gained the MMX registers.


## [0.14.0] - 2026-11-18
The renderer, on Vulkan 1.3, and the first Direct3D replacements over it. The game creates its graphics device on Vulkan and runs as far as its audio engine.

### Added
- **The renderer** (`recomp/runtime/gfx/`): Vulkan 1.3 core with dynamic rendering and synchronization2, loaded at run time through volk, so building needs no Vulkan SDK. Picks the best GPU (`--gpu N` to choose), uses `VK_KHR_portability_subset` on MoltenVK, D24S8 depth (D32S8 where there's none). Frame buffers are its own images: clears (whole-buffer clears become the render pass's load operation, for tile-based GPUs), flips, read-back, PNG output. `QILIN_VK_VALIDATION=1` turns on the validation layer.
- **Direct3D** (`recomp/runtime/host/d3d.cpp`): `Direct3D_SetPushBufferSize`, `Direct3D_CreateDevice`, `GetBackBuffer2`, `GetDepthStencilSurface2`, `GetRenderTarget2`, `D3DResource_AddRef`/`Release`/`Register`, `SetVerticalBlankCallback`, `SetGammaRamp`, `BlockUntilIdle`, `Clear`, `Swap`. Resources are D3D headers in guest memory, as the original's; D3D's state tables stay in guest memory, where the game's inline state setters write them. `Swap` waits for the vblank the presentation interval asks for, at the NTSC rate, and runs the game's vblank callback for each vblank, which is what the game's time step comes from. Each replacement's argument size matches the original's.
- **XDK library names:** `tools/gen/xdk_symbols.py` runs XbSymbolDatabase (MIT) on the XBE and writes `recomp/runtime/host/xdk_symbols.inc`: 390 functions and variables of D3D8, DSOUND, XACT, XAPI and XGraphics, plus five identified by reading them. Reports name them.
- **XInput** with nothing plugged in (`XInitDevices`, `XGetDevices`, `XGetDeviceChanges`), and the kernel's `FscSetCacheSize`.
- **`qilin_run --dump-frames DIR`, `--frames N`, `--gpu N`.**
- **`host::call_guest`:** the runtime calls a lifted guest function, such as a callback the game registered.
- **Third-party:** the Vulkan C headers (Apache-2.0 or MIT) and volk (MIT), vendored at `vulkan-sdk-1.3.296.0`.
- **Tests:** `qilin_gfx_tests`: clears, rectangles, flips and read-back on any Vulkan device, with validation; skipped without one. Linux CI installs Mesa's software Vulkan driver, so it runs on x64 and ARM64.
- **`docs/graphics.md`.**

### Changed
- **Contiguous memory stays below 256 MB** whatever the RAM size, as the Xbox's 28-bit physical addresses do: D3D masks resource addresses to them.


## [0.13.0] - 2026-11-18
The file system. XAPI's and the C runtime's start-up now complete, and the game's `main` runs until its first Direct3D call.

### Added
- **The kernel's I/O manager** (`recomp/runtime/host/kernel_io.cpp`) over host directories: `\Device\CdRom0` is the extracted disc, read-only; the hard disk's partitions are folders in a data directory; partition 0's configuration area is a sparse file. Symbolic links, handles, case-insensitive names relative to a handle or `ObDosDevicesDirectory()`, and `NtCreateFile`, `NtOpenFile`, `NtClose`, `NtReadFile`, `NtWriteFile`, `NtFlushBuffersFile`, `NtQueryInformationFile`, `NtSetInformationFile`, `NtQueryFullAttributesFile`, `NtQueryVolumeInformationFile`, `NtQueryDirectoryFile`, `NtDeviceIoControlFile` (disk geometry and partition information from the retail table), `NtFsControlFile` (dismount), `IoCreateSymbolicLink`, `IoDeleteSymbolicLink`, `NtOpenSymbolicLinkObject`, `NtQuerySymbolicLinkObject`.
- **Cache partitions:** XAPI's format of the title's cache partition empties its folder and sets the volume's cluster size from the FATX header it writes, kept across runs. This title's Z: drive uses 64 KB clusters.
- **`qilin_run --game-dir DIR` and `--data-dir DIR`:** the disc defaults to `default.xbe`'s folder, the hard disk to the platform's per-user data folder (`qilin/ghoulies`).
- **`QILIN_TRACE_HLE=1`:** logs every replacement call with its caller, arguments and result, and every object name resolved.
- **Kernel:** `RtlInitAnsiString`, `RtlEqualString`, `RtlUnicodeStringToAnsiString`, `RtlCompareMemoryUlong`, `RtlRaiseException` (stops with the code), `KeQuerySystemTime`, `XeLoadSection`, `XeUnloadSection`; `XeImageFileName` and `HalDiskCachePartitionCount` filled in.
- **XBE sections without the preload flag** start as zero and are read in by `XeLoadSection`, with the kernel's reference counts.
- **Tests:** the file system on a scratch disc and hard disk, in `qilin_host_tests`.

### Fixed
- **The main thread's TLS block** is at the top of its stack, as the kernel places it, with its pointer word and template copied as XAPI's thread start routine does. Before, the stack started over it and `SetLastError` wrote through a stray pointer, so the first failed call returned to the wrong place.


## [0.12.0] - 2026-11-18
XAPI is lifted too, and the runtime implements the Xbox kernel beneath it. Start-up runs XAPI's own process initialisation and gets as far as its first file.

### Added
- **XAPI in the lifted set**, like the C runtime: the functions lifted code reaches, plus XAPI's process start-up (`0x151011`). 6,214 functions lift, 471 of them in the two libraries. `kNative` lists the four XAPI functions that can't be lifted, with why (`rdtsc`, `int 0x2D`, kernel patching, a GDT reload); `qilin lift` doesn't follow them.
- **Lifter:** packed SSE for XGraphics' maths (add/sub/mul/div/min/max ps, shufps, unpcklps/unpckhps, movlps/movhps, movhlps/movlhps), `bsf`/`bsr`, `fsincos`.
- **Kernel** (`recomp/runtime/host/kernel.cpp`): critical sections, `Nt*VirtualMemory`, contiguous memory, `MmQueryStatistics`, DPC and timer objects (timers recorded until the scheduler exists), `ExQueryNonVolatileSetting` (a North American retail console), `RtlNtStatusToDosError`, `HalRegisterShutdownNotification`. Each is checked against the export table's argument size.
- **Virtual memory** (`host::VirtualMemory`, replacing the page heap): 64 KB reservations, committed pages zeroed, top-down allocation, queries.
- **The main thread's KPCR and KTHREAD** at the top of RAM; `fs:[0x28]` is the current thread.
- **`qilin_run --ram-mb N`:** guest RAM from 64 MB (the default, the retail Xbox's) to 1 GB. RAM is `calloc`ed, so untouched pages cost nothing.
- **Start-up:** the XBE entry point's TLS sizing, then XAPI's process initialisation, then the C runtime's.
- **`tools/check/xg_math.py`:** XGraphics' maths with aligned operands: 7 functions, 4,200 trials, all matching.
- **macOS:** every 64-bit Mac with a Metal GPU running macOS 10.15 or later, Intel included, is a target. CI builds on Apple Silicon and Intel (`macos-15-intel`). The runtime (`qilin_host`, the XBE loader `qilin_xbe`, `qilin_run`) is built for macOS 10.15 and formats with `qilin::strf` instead of `std::format`, which Apple's libc++ only has from 13.3; the developer tools are built for 13.3. `qilin info`'s JSON writer moved to `xbe_json.cpp`, and the region table to `regions.hpp`.

### Changed
- The XAPI replacements for `QueryPerformanceFrequency`, `GlobalMemoryStatus`, `XGetLaunchInfo` and `XGetLanguage` are gone: the lifted originals run, over the kernel.

### Fixed
- **Difftest:** a trial's `setup` blocks were dropped from the restore list when a variable of the same name was reused. Unicorn's missing SSE alignment check is now emulated (a misaligned 16-byte operand faults in the original too).

### Verified
- XAPI: 192 functions, 9,205 trials, 0 mismatches; 14 newly reached CRT functions, 671 trials, 0 mismatches. The game's generated code is unchanged.

## [0.11.0] - 2026-11-18
The C runtime is lifted, not replaced, so `sprintf`, string conversions and the x87 maths give exactly the original's results. Its start-up runs as XAPI ran it.

### Added
- **The C runtime in the lifted set.** `qilin lift` adds the CRT functions lifted code can reach (calls, tail calls, function addresses in code, and CRT functions stored in data sections), transitively: 265 of 434. 6,008 functions lift, with no failures, and `qilin lift` fails if a reachable CRT function doesn't lift.
- **Lifter conventions for hand-written assembly** (`Conventions`, listed in `crt_conventions()`): flags passed into or out of a call through `c.eflags`, for the x87 maths' classify-then-branch helpers; and "stack glue" (`__SEH_prolog`/`__SEH_epilog`, `__EH_prolog`, shared epilogues that pop their caller's saved word), which skips the stack-balance check.
- **Instructions:** `scas` (with `rep`/`repne`), `shld`/`shrd` (by an immediate or `cl`), `rcl`/`rcr` by 1, `jecxz`, and for the x87 `frndint`, `fscale`, `fxam`, `fnclex`, `fistp` m64, `fld` m80 (to the nearest double), `f2xm1`, `fnsave` and `frstor`.
- **`f2xm1`, correctly rounded** (`x87_math.hpp`), with the same fast-path-then-check design and checks as the other transcendentals: 0 wrong against mpmath (GCC, Clang, ARM64 under qemu), 6 million fast-against-slow cases, 11 golden vectors.
- **SEH scope tables in the CFG.** `__except` filters and handlers are entry points inside their parent function (46 tables, 24 `__except` entries in the CRT and XAPI).
- **Negated jump-table indices** in the value-range analysis (`neg ecx; jmp [4*ecx + T]`, `memmove`).
- **C runtime start-up** in `qilin_run`: `_mtinit`, `_ioinit`, `_FPinit` and the other CRT initializers, in XAPI's order; library initializers (XGRPH, DSOUND) are skipped. `rt::is_lifted`.
- **`tools/check/crt_printf.py`:** the lifted `sprintf` against the original on generated format strings (all conversions, flags, widths, precisions, doubles across the range): 3,000 of 3,000 match.
- **Difftest:** trial set-ups for the CRT's irregular contracts (`GLUE_POPS`, `X87_ARGS`), `f2xm1` in the mpmath reference, and three more Unicorn deviations handled (`fst st(i)` tagging, `fnsave` pointers, CRT values outside double's range). Trials can take explicit arguments and memory blocks.

### Fixed
- **`_input` (`scanf`'s engine) was cut into about 60 fragments** joined by tail calls, because an `__except` handler that a scope table points to was taken for a function. Fixed by the scope-table reading above. The lifted fragments would have grown the host stack on every loop iteration.
- **`memmove`'s backward tail-copy table** was sized at one entry by scanning, so the lifted code trapped on 7 of its 8 indices. Found by the difftest; fixed by the negated-index analysis.

### Verified
- Difftest of the whole lifted set: 6,008 functions, 36,048 trials, 0 mismatches; also under UBSan. CRT alone: 265 functions, 12,631 trials, 0 mismatches. The game's generated code is byte-identical to v0.10.0's.

## [0.10.0] - 2026-11-18
The runtime's first slice: the game runs. `qilin_run` calls the lifted `main`, which runs until the first function the runtime doesn't provide yet.

### Added
- **`qilin_run`** (built with `-DQILIN_GEN_DIR=<qilin lift output>`). It loads `default.xbe` into 64 MB of guest RAM and sets up the main thread's stack and information block. Kernel imports keep `0x80000000 | ordinal`, and data exports point at objects. It then calls the game's `main` at `0x32390`, replacing XAPI's start-up. See `docs/runtime.md`.
- **The replacement registry** (`recomp/runtime/host/`): `rt::call_unlifted` runs the native replacement registered for a guest address. Anything else stops the game and names the target ("kernel NtCreateFile (ordinal 190)", "crt code at …") and its caller. It includes stdcall helpers and a page-granular guest heap.
- **Kernel export table** from nxdk's `xboxkrnl.exe.def` (CC0, vendored in `third_party/nxdk/`): names, ordinals, argument sizes, and data and fastcall flags.
- **First XAPI replacements:** `QueryPerformanceCounter` (host time at the Xbox's 733 MHz), `QueryPerformanceFrequency`, `GlobalMemoryStatus`, `XGetLaunchInfo` (cold boot) and `XGetLanguage` (English). The game now gets through them and stops at the C runtime's `sprintf`.
- **`qilin_host_tests`** (CI): the export table, the registry and stdcall conventions, and the guest heap.

## [0.9.0] - 2026-11-18
The x87 transcendentals give the same bits on every host. That was the last open item in the recompiler core.

### Added
- **`recomp/runtime/x87_math.hpp`:** `fsin`, `fcos`, `fptan`, `fpatan` and `fyl2x` now return the correctly rounded double of the true function at their operands, except where the true value lies within about 2^-100 of a rounding boundary.
  - **Portable:** only IEEE add, subtract, multiply, divide and square root. Exact products use hardware `fma` where the target has it and Dekker's method otherwise, with identical bits either way.
  - **Fast:** each function evaluates to below 2^-64, then checks the rounding. About 99.7% of calls finish there; the rest take a double-double slow path with exact Payne–Hanek reduction. That's about 50–100 ns per call on x86-64, against 4–23 ns for glibc, which isn't portable and, as measured, often isn't correctly rounded.
- **Checks:**
  - `tools/check/x87/transcendental.py` checks against mpmath: 480,000 cases per seed over 24 operand ranges, 0 wrong, with GCC and Clang, with and without FMA, and for ARM64 under qemu.
  - `tools/check/x87/consistency.cpp`, the new `qilin_x87_consistency` CTest, checks fast paths against slow paths on every CI platform.
  - 34 golden vectors in the unit tests.
- **Difftest:** Unicorn's transcendentals (the host libm; `fyl2x` rounds three times) are replaced in both emulators with the correctly rounded result from mpmath. The 626 x87 functions: 10,015 trials, 0 mismatches. The difftest now needs `pip install mpmath`.

### Changed
- **Special operands** of the five instructions are handled explicitly, following the x87 tables (C's for `atan2`, IEEE's for `y * log2(x)`). Before, they depended on the host libm.
- **`fyl2x` rounds once.** Before, it computed `y * log2(x)`, rounding twice.

## [0.8.0] - 2026-11-18
Every function in the lifted set lifts: all 5,743 in the game code and Rare libraries, with zero failures. That closes check 4 of "How we'll know everything is recompiled".

### Added
- **Replaced functions inside the lifted ranges.** `kReplaced` in `recomp/src/main.cpp` lists them, with the reason for each, and `qilin lift` skips them. It exits with code 3 if anything it lifts could reach one: a call or tail call, the address as an immediate or displacement, or the address stored in a data section.

### Changed
- **Three functions in the Rare-library range are replaced with Bink, not lifted:** Bink's MMX probe at `0x162840` (the last function that didn't lift, because of `pushfd`/`cpuid`), and two helpers at `0x162570` and `0x1626B0`. Each is called once, from the BINK section, which FFmpeg replaces, and nothing else refers to them. Lifting `pushfd`, `popfd` and `cpuid` for code that never runs would have added more risk than it removed.
- **The lifted set is 5,743 functions,** not 5,746. The generated code is otherwise byte-identical to 0.7.0's, so 0.7.0's difftest results (0 mismatches) still apply.

## [0.7.0] - 2026-11-18
C++ exception frames. 5,745 of the 5,746 functions in the game code and Rare libraries lift now. The one left is a CPU-ID probe in the Rare libraries, which the runtime replaces.

### Added
- **`fs:` memory operands in lifted code,** relative to `c.fs_base`, the guest address of the thread's information block. The 20 functions that link a C++ exception record into `fs:[0]` and unlink it lift now, and the chain lives in guest memory exactly as in the original.
- **Exception-handling policy, decided:** keep the frame chain explicit, and have the runtime's `_CxxThrowException` stop the game with the exception's type and message instead of unwinding. No exception the game can throw is ever handled:
  - its throws are STL `logic_error`s (`length_error`, `out_of_range`);
  - 16 of the 17 `catch (...)` blocks in lifted code clean up and rethrow;
  - the last one guards an `operator new` call that returns NULL rather than throwing.

  The analysis is in `docs/recompiler-core.md`, and `tools/check/eh_report.py` reproduces it.
- **Difftest segments:** both emulators get a GDT with flat segments and `fs` on a thread information block, which the lifted side gets as `c.fs_base`. Unicorn ignores a plain FS base in 32-bit mode.
- **Results:** 5,745 functions, 45,959 trials, 0 mismatches, also under UBSan. 7 of the 20 exception-frame functions complete a trial; the rest allocate memory, which needs the kernel the harness doesn't have. A planted bug in the epilogue's unlink was caught in every one that completed.
- **A unit test** for the exception-frame prologue and epilogue, and for an `fs:` operand with a base register.

### Fixed
- **The game code ended at `0x14A895`, inside XAPI's `IsBadReadPtr`** (`0x14A840`). That function was lifted as game code without the part past `0x14A895`, which holds its `__except` handler and its NULL-pointer return. A bad pointer would have crashed the recomp, and a NULL one would have jumped to a stub for `0x14A8A0` and trapped; the original returns TRUE for both. The game range now ends at `0x14A840`, and `IsBadReadPtr` is a stub for the runtime to replace, like the rest of XAPI. The lifted ranges hold 5,746 functions, not 5,747.

- **Difftest inputs:** in 0.6.0, the float-heavy heap option's new parameter took the place of `rand_word`'s kind weights, so the xmm registers and the `ST(0)` argument were drawn with the float-heavy mix by mistake. They use their own mix again. The 0.6.0 full-run results were produced before that change; the float-heavy x87 run was after it.

### Changed
- **Stubs:** 276, all library entry points (D3D 90, XAPI 79, CRT 41, XACT 28, D3DX 17, BINK 7, XPP 7, XGRPH 6, DSOUND 1). No game function is a stub any more.
- **`rt::Context` has a new field, `fs_base`.** The runtime must set it per guest thread.

## [0.6.0] - 2026-11-18
Lifter slice 3 (x87). 5,726 of the 5,747 functions in the game code and Rare libraries lift now (99.6%), up from 5,098. The 21 left are 20 functions with C++ exception frames and one CPU-ID probe.

### Added
- **x87 in lifted code:** 9,646 instructions in 628 functions.
  - **State:** the register stack is kept in the Context in physical order, as doubles, with TOP, the tag bits, the control word and the status word. Values cross calls as on the hardware, including float return values in `ST(0)`.
  - **Precision control is honoured at runtime.** The game sets 24-bit precision at startup (`_controlfp` at `0x33462`). `rt::fadd`, `fsub`, `fmul`, `fdiv` and `fsqrt` then round to a 24-bit significand with the register's full exponent range, correctly even when an operand is wider than 24 bits (round-to-odd from the exact error). At 53-bit precision they're plain double arithmetic.
  - **NaN results follow x87 rules on every host:** a QNaN beats an SNaN, the larger significand wins, and invalid operations give the indefinite `0xFFF8000000000000`.
  - **Also covered:** integer loads, `fist` by the control word's rounding mode, the constants, `fxch`, `fchs`, `fabs`, `fcom`/`fucom`/`ftst` with `fnstsw`, `fcomi`/`fucomi`, `fsin`, `fcos`, `fptan`, `fpatan`, `fyl2x`, `fldcw`/`fnstcw` and `ffree`.
  - **Not exact, by design:** values wider than 53 bits (64-bit integers above 2^53, the x87's 64-bit constants, transcendental results) are held as doubles. Transcendentals use the host's maths library, which may differ in the last bit between platforms. That affects 23 instructions, and a bundled implementation is next.
- **An exact cross-check of the x87 rounding** (`tools/check/x87/`): 40,000 random operations per seed, compared with exact rational arithmetic rounded to 24 bits. 0 mismatches with GCC 14 and Clang 19. Unit tests cover the tie cases, NaN rules, conversions and compare codes.
- **Differential test with x87 state.**
  - **Compared:** TOP, the valid registers and their values, and the control word. Half the trials start with a value in `ST(0)`. Each function runs under both 24-bit and 53-bit precision control.
  - **Results:** 5,726 functions, 45,808 trials, 0 mismatches, also under UBSan. 511 of the 628 x87 functions completed at least one matching trial.
  - **Float-heavy heaps** with `--variants 4`. On the 628 x87 functions: 10,047 trials, 0 mismatches, and 540 functions exercised.
  - **Mutation checks:** four planted x87 bugs. A dropped pop was caught in 270 of 449 functions, `fcomi` with swapped operands in 92 of 155, and ignored precision control in 56 of 310. An untagged `fst st(i)` destination was caught in 0 of 30: in the game's code the destination already holds a value, and a probe for an empty one never fired.
- **Emulator corrections in the difftest,** where Unicorn 2.1 (QEMU 5) differs from the hardware: `rdtsc` returns a per-trial counter instead of the host's, the `fcomi` family clears OF, SF and AF, and trials where `fyl2x` gets an operand ≤ 0 (which Unicorn doesn't pop) are skipped. The difftest build now also writes `qilin insns` output, which gives the addresses.

### Changed
- **24 more library entry points are called directly,** from the newly lifted functions: 275 in all (D3D 90, XAPI 79, CRT 40, XACT 28, D3DX 17, BINK 7, XPP 7, XGRPH 6, DSOUND 1). Generated stubs fall from 699 to 287.

## [0.5.0] - 2026-11-18
Lifter slice 2 (calls), and every jump table in lifted code proven. 5,098 of the 5,747 functions in the game code and Rare libraries lift now (88.7%), up from 1,276.

### Added
- **Value-range analysis for jump tables** (`recomp/src/vrange.*`). It tracks each register's possible values along every path through a function and narrows them at conditional branches. Every jump table in the game code and the Rare libraries now has a proven size: 35 by the new analysis, and the rest by a `cmp` or an `and` next to the jump. The 7 still sized by scanning are all in XDK libraries that get replaced. On the 311 tables already bounded by `cmp` or `and`, the analysis agrees.
- **Jump tables with a negative first index,** for MSVC's `sub ecx, 4; jb` … `jmp [4*ecx + T]` form, used in `memcpy`.
- Unit tests for a register-held bound a block away from the jump, adjacent tables, and a negative-index table, lifted to C++.
- **Calls in lifted code.**
  - **Direct calls** push the guest return address, store modified registers back to the Context, call `fn_T(c)` as a plain C++ call, reload the registers, and trap if the callee returned anywhere but the call site.
  - **Tail calls:** a `jmp` or `jcc` out of the function.
  - **Indirect calls and jumps** go through `rt::call` (`recomp/runtime/dispatch.cpp`). It binary-searches the generated function table and falls back to `rt::call_unlifted`, which the host provides: HLE in the runtime, original code in the difftest, otherwise a trap with the address.
  - **Generated files:** `qilin lift` now writes `functions.hpp` (declarations) and `stubs.cpp`. That's one stub per call target without a lifted body, 699 in all: 448 game and Rare functions waiting on x87 or exception handling, and 251 library entry points. Those 251 are the first concrete list of what the runtime must replace (XAPI 71, D3D 87, CRT 36, XACT 25, D3DX 11, BINK 7, XPP 7, XGRPH 6, DSOUND 1).
- **String instructions:** `movsw`, `stosw` and `cmps` with `repe`/`repne`. `cmps` leaves the flags untouched when ecx is 0, as the CPU does.
- **Direction flag** tracked statically (`std` … `cld`), with backward `rep movs` for the Rare `memcpy`'s overlapping copies. DF must be clear at every call and exit.
- **Scalar SSE arithmetic follows x86 NaN rules on every host** (`rt::addss` and friends): the first NaN operand propagates, quietened, and invalid operations give `0xFFC00000`. Before, the result depended on the host CPU and on the compiler's operand order. Unit tests cover the rules.
- **Differential test with calls.**
  - **Coverage:** whole call trees are compared. A callee without a lifted body runs from its original bytes in a second Unicorn instance that shares the lifted side's memory.
  - **Results:** 5,098 functions, 40,784 trials, 0 mismatches, also under UBSan. 1,222 matching trials ran through original callee code.
- **Test hooks** (`QILIN_TEST_HOOKS`, compiled out otherwise): a frame per lifted function and a tick per backward branch. The harness uses them to stop runaway recursion and loops, and to place tail-called callees' returns.
- Difftest options `--test` (test chosen functions from the full lift) and `--reuse` (skip the rebuild).

### Changed
- **The difftest restores a saved CPU context in both emulators before every trial,** and runs each chunk of 120 functions in a fresh process.
- **Trials whose only differences are NaN against NaN are counted as `pass-nan`:** Unicorn propagates SSE NaNs by x87 rules.
- **Trials that call into non-code** (null or random pointers) are skipped.
- **A flag read after a call is reported as a lift error,** since callee flags aren't modelled. No function in the game does this.

### Fixed
- **Jump tables that were sized by scanning took in the next table.** 15 tables at `0x108xxx` were read as 10 slots instead of 5, and `0x16322D` as 14 instead of 10. That gave those functions phantom switch cases.
- **Two jumps through tables of records in `.rdata`** (`0xD410B`, `0xD4179`) were treated as switches. They're indirect jumps now.
- **String instructions were taken to overwrite every register** when searching backwards for a switch bound. They write only esi, edi, ecx (with `rep`) and, for `lods`, eax.
- **A branch to the next instruction (`jnz $+2`)** left its label undefined in the generated code.
- **`if ((sf == of))` in generated code** tripped Clang's `-Wparentheses-equality`. All generated code now compiles warning-free under GCC 14 and Clang 19.
- **CFG:** tail-call entries for branch targets that are the function's own instructions are dropped.
- **CFG:** a weakly evidenced "entry" that the previous instruction falls into is a label, not a function: `0x83540`, matched by a constant in `.rdata`, and `0xC4000`, a call's return address.
- **Difftest:** CPU state leaked between trials after the original executed data (231 false mismatches).
- **Difftest:** the two emulators kept separate x87/MXCSR state.
- **Difftest:** Unicorn ignored a new stop address for blocks it had already translated.
- **Difftest:** tail calls into shared SEH epilogues stopped at the wrong place.
- **Difftest:** workers ran out of memory.
- **MSVC:** `rt::divss` tripped warning C4723 ("potential divide by 0") wherever a constant zero divisor was inlined. IEEE division by zero is intended there.


### Documentation
- **The renderer targets Vulkan 1.3.** It's widely supported: the Raspberry Pi 4 and 5 (Mesa v3dv, conformant from Mesa 24.3), macOS through MoltenVK 1.3, and current Windows and Linux drivers. 1.3 core is the floor; optional features and limits follow what v3dv on a Pi 4 provides. See `roadmap-and-design-targets.md`.

## [0.4.0] - 2026-11-17
Every byte of the lifted code is accounted for.

### Added
- **`qilin coverage <xbe> [--list]`** sorts every byte of each text region into code, tables, padding or unknown. It exits with code 3 while any byte of a lifted region is unknown. Today there are none: the game code, the Rare libraries and the C++ EH funclets are at 100%, down from 15,198 unknown bytes in the game code.
- **C++ exception handling tables are parsed** (`FuncInfo`, unwind maps, try blocks, catch handlers and `__ehhandler` stubs): 70 tables, 68 stubs, 147 unwind funclets and 20 catch continuations. The funclets have their own lifted region, `0x1707B0–0x171020`.
- **Function discovery rules grounded in MSVC's layout:**
  - 16-byte function alignment in the game and Rare library regions;
  - strong evidence (calls, entry point, EH tables, tail jumps that land after `int3` padding) versus weak evidence (data values, immediates);
  - merging of fragments back into their parent functions (2,834 merged);
  - dropping labels that were mistaken for tail-call targets, including loop heads behind `jmp short` over alignment filler;
  - data and immediate pointers that take over the weaker seeds inside them;
  - a fixpoint that repeats all of this until nothing changes.
- **Switch tables:** `and reg, 2^k - 1` bounds, index-register tracking through `movzx` and `mov`, byte index tables clipped against their dword table, and no table entry landing on another function's start. 558 tables in all.
- `qilin --version`.
- Unit tests for the alignment-filler label case, EH stub and funclet discovery, Zydis flag effects and zero-unknown coverage.

### Changed
- **The CFG pass takes about 4 s.** Fragment merging carries on in place after each merge rather than rescanning every function.
- **The lifter takes the flag effects of instructions it doesn't list from Zydis's tables.** Before, those instructions (`fcomi`, `bt`, `bsf`, rotates and so on) were treated as leaving flags alone. The hand table was checked against Zydis at every decodable address: it agrees, or is deliberately stricter.
- `pushfd` is reported as an unsupported instruction rather than as a read of undefined flags.

### Fixed
- **Every "function" that was really a fragment of another is gone:** 61 in v0.3.0, 0 now. The lift report lists only instruction classes still to be implemented.
- **Misaligned overlaps drop the weaker side.** Before, the inner seed always lost. A real function at `0xD3310` was thrown away because a jump table in front of it had been decoded as an orphan function.
- **Memory displacements (`[ecx + 0x1AAB0]`) are no longer taken as function addresses,** which had created a false function over a byte table.
- **Jump-table bound search** could pick up a `cmp` on a register that had been overwritten since.

### Documentation
- **Docs:** hardware scope decided. Supported hosts are 64-bit machines with Vulkan, with macOS on Apple Silicon covered through MoltenVK. Hardware without Vulkan, and 32-bit hosts, are out of scope, so there is one renderer and one flat memory model. The Raspberry Pi 4 is described as a reference point for typical low-end hardware, not a minimum spec.
- **Docs:** naming convention for `qilin` (see `docs/naming.md`).
- **Docs:** `recompiler-core.md` explains that function and jump-table counts are discoveries, not known totals, and defines the checks that mean everything is recompiled.

## [0.3.0] - 2026-11-17
C++23 migration and full code review.

### Changed
- **The whole project is C++23**: recompiler, runtime header and generated code.
  - CMake enforces GCC 14, Clang 19, Xcode 16 or Visual Studio 2022 17.10.
  - The code now uses `std::expected`, `std::print`/`std::format`, `std::ranges`, `std::unreachable` and `std::bit_cast`.
- **CI treats warnings as errors** on Linux x64 (GCC 14 and Clang 19), Linux ARM64, macOS ARM64 and Windows (MSVC).
- **Lifting covers the Rare engine libraries too**, not just the game code: 1,373 functions, up from 1,243.
- **Generated functions touch fewer registers.** They declare only the registers they use and store back only the ones they modify.
- **New `qilin_runtime` CMake target.** It carries the floating-point rules lifted code needs (`-ffp-contract=off`). `guest.hpp` refuses `-ffast-math`.

### Fixed
- **Function discovery:**
  - Jump-table extents used the unique-target count instead of the table length, and byte index tables weren't recorded. Gap seeding could start "functions" inside switch data; two such were removed.
  - `cmp al, imm8` switch bounds weren't masked to the register width.
- **Statistics:** gap statistics were merged across sections that share a name (`BINKYUY2` ×4).
- **Lifter:**
  - OF/AF read after a shift by `cl` is now an error rather than a stale value.
  - `sar` defines OF only for a count of 1.
  - `F2`-prefixed `stos`/`movs` are treated as `rep`.
- **XBE loader:**
  - Header tables are bounds-checked as a whole.
  - UTF-16 titles with surrogate pairs decode correctly.
- **Validation:** `cli`/`sti` count as implausible in game code.
- **Test harness:** a `memcpy` from a null pointer, found by UBSan.
- **MSVC build:** a non-conforming preprocessor and `fopen` deprecation errors.

### Performance
- **CFG pass:** 1.9 s / 288 MB → 1.1 s / 127 MB.
  - The decoder cache is a flat address-indexed table of 120-byte entries.
  - The linear sweep no longer uses the cache.
  - Reachable instructions are tracked in a bitmap.
- **String operations:** `rep stos`/`rep movs` use `memset`/`memmove` wherever that gives the same result.

### Verified
- **Differential test at `-O2`:** 10,984 trials, 0 mismatches.
- **UBSan run:** 5,492 trials, 0 mismatches, no undefined behaviour.
- **Decoder:** 100% agreement with LLVM on 529,718 reachable instructions.
- **ARM64:** CFG output and lifted C++ are byte-identical to x86-64.

## [0.2.0] - 2026-11-17
Validation pass, lifter slice 1, differential test.

### Added
- **CFG validation pass:** drops data decoded as code.
  - Functions whose entry is inside another function's instruction.
  - Decode failures and overlapping instructions.
  - Instructions retail MSVC code never contains.
  - Call targets are kept and listed by `qilin suspects`.
- **Lifter slice 1 (`qilin lift`):** x86 → C++ for leaf functions.
  - Registers become host locals, and flags are computed only where they're live.
  - Labels/gotos for control flow, `switch` for jump tables.
  - Integer operations and bit-exact scalar SSE.
  - A stack-balance check rejects function fragments.
- **Runtime header `recomp/runtime/guest.hpp`.**
- **Differential test (`tools/difftest`):** each lifted function is checked against the original x86 under Unicorn. 9,944 trials, 0 mismatches.

### Fixed
- **MSVC build:** a local struct with default member initialisers, and shadowed names.

## [0.1.0] - 2026-11-17
Recompiler core foundations.

### Added
- **C++20 CMake project** with Zydis v4.1.1 and Zycore vendored (MIT).
- **XBE loader:** headers, certificate, sections, libraries, kernel thunks and flat image. Its output is identical to the Python prototype.
- **Function discovery and recursive-descent CFG,** ported from `tools/analysis/cfg.py`.
- **`tools/check/verify_decoder.py`:** cross-checks Zydis against LLVM on every reachable instruction and on a linear sweep.
- **Unit tests** on a synthetic XBE.
- **CI** on Linux x64/ARM64, macOS ARM64 and Windows x64.
- **Design and analysis notes** in `docs/`.

### Fixed
- **CI:** use `macos-latest` (the `macos-14` image was retired).

[Unreleased]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Qilin1066/GBTGhoulies-Qilincomp/releases/tag/v0.1.0
