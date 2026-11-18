# Graphics: Direct3D on Vulkan (roadmap step 4)

Last updated 2026-11-18 (v0.22.0: drawing; v0.21.0: resources destroyed as the original; v0.20.0: state setters, draws recorded, push-buffer sink; v0.17.0: vertex programs decoded, pixel-shader objects lifted; v0.16.0: D3D's resource code lifted; guest RAM's kernel windows; v0.14.0: the renderer and the Direct3D device).

The game draws through the Xbox's Direct3D 8 library (D3D8, XDK 5659), which is linked into the XBE. Its calls stay in the recompiled code; what answers them is the runtime. Every D3D function the game calls is replaced natively (`recomp/runtime/host/d3d.cpp`) over a Vulkan 1.3 renderer (`recomp/runtime/gfx/`). There is no DirectX anywhere, and the Xbox's GPU (the NV2A) isn't emulated: the replacements see what the game asks for (clear this, draw that mesh with this texture) and do it with Vulkan.

```
lifted game code  →  D3D call  →  replacement (d3d.cpp)  →  renderer (gfx/)  →  Vulkan  →  the GPU
```

## What the game uses

The game's own code (and Rare's libraries) call 90 D3D functions, 1,095 call sites in all (`docs/code-analysis.md`). XbSymbolDatabase names 84 of them by signature; the other six were identified by reading them (`D3DDevice_BlockUntilIdle`, `D3DDevice_CreateSurface2`, `D3DTexture_GetLevelDesc`, `Direct3D_SetPushBufferSize`, D3D's contiguous-memory allocator, and a push-buffer helper at `0x176E00` still to name). `tools/gen/xdk_symbols.py` turns the database's scan into `recomp/runtime/host/xdk_symbols.inc`, which gives the runtime the names of 390 XDK library functions and variables (D3D8, DSOUND, XACT, XAPI, XGraphics). Reports use them: `no replacement for D3D8 D3DDevice_SetTexture (001749E0)`.

The game never reads the D3D device object, so the device is the runtime's. But it does write D3D's state directly: the XDK's `SetRenderState` and `SetTextureStageState` are inline functions, so the game stores render states into `D3D__RenderState` (`0x181B70`), texture-stage states into the table at `0x181970`, and sets bits in the dirty-flags word at `0x181968`, 148 writes in all. Those tables stay in guest memory in the original layout and are the source of truth: the renderer reads them when it draws.

Resources (surfaces, textures, vertex buffers) stay D3D resource headers in guest memory, as the game passes them around and may read their format and size: `Common` (reference count in the low 16 bits, type, the device's internal references), `Data`, `Lock`, `Format`, `Size`, and a surface's `Parent`. Their data stays in guest memory too, where the game writes it; the renderer copies what it draws into Vulkan images when it draws.

**D3D's resource code is lifted** (v0.16.0, `kLiftedD3d` in `recomp/src/main.cpp`): creating textures, surfaces and vertex buffers, surface levels, descriptions and locks, pixel-shader objects and D3D's contiguous allocator, 21 functions. They're computation over the game's own allocator (`0xF7270`), so lifting them makes headers, sizes, pitches and swizzled layouts exactly the original's. What they call that touches the GPU stays native: `D3D_BlockOnResource`, the wait for the GPU to finish with a resource, returns at once (the GPU never holds guest memory here), and `D3DVertexBuffer_Lock2`, which in the original also writes a wait into the command buffer. The lifted code reads a few fields of the device object, which the runtime keeps current in the original layout: the frame buffers (`+0x1A04`, `+0x1A08`, `+0x1A0C`), the render target (`+0x19F4`), the depth buffer (`+0x19F8`) and the multisample type (`+0x195C`).

**Locks return addresses in the kernel's windows onto RAM**: a vertex buffer's `Data | 0x80000000`, a surface's `Data | 0xF0000000` (write-combined). The game writes vertices and pixels through them, so guest RAM is mapped at both (`runtime.md`).

## Vertex shaders

The game has 24 vertex shaders, written in Xbox shader assembly (`vs.1.0` and `xvs.1.1`, a table at `0x4271E4`) and assembled when it starts by XGraphics' `XGAssembleShader`, which is lifted. So the microcode `CreateVertexShader` receives is exactly what the console's would be.

`recomp/runtime/gfx/vsh.*` decodes it: a header word (program type, instruction count), then 128-bit NV2A instructions, each pairing a MAC operation (`mov mul add mad dp3 dph dp4 dst min max slt sge arl`) with an ILU one (`mov rcp rcc rsq expp logp lit`), three operands (A, B, C) from temporaries, inputs or the 192 constants (D3D's c-96…c95), with swizzles, negation and a0.x-relative constants, writing temporaries, an output register (`oPos oD0 oD1 oFog oPts oB0 oB1 oT0`–`oT3`) or a constant. `ADD` reads A and C; the ILU reads C, and writes `r1` whenever the MAC slot holds an operation (the assembler keeps `r1` free for it).

**Checked against the sources:** each of the game's 24 programs disassembles to its source text, after what the assembler itself does: it renames temporaries, writes `sub` as `add` with a negated operand, narrows write masks, expands `m4x4` into four `dp4`, pairs ILU operations with MAC ones, and appends the screen-space transform (`mul oPos.xyz, oPos, c-38` with `rcc r1.x, oPos.w`, then `mad oPos.xyz, oPos, r1.x, c-37`: D3D's reserved viewport constants). `qilin_gfx_tests` checks one of them, as the assembler produced it, against its disassembly. `QILIN_DUMP_VSH=1` prints every program and its declaration as it's created (`=2` adds the raw words).

`CreateVertexShader` builds the shader object as the original does (v0.20.0), with D3D's own helpers, lifted, over the game's allocator: a reference count, flags (`0x10` a program), the instruction count, 16 attribute slots `{stream, offset, format}` filled from the declaration (format `2` is disabled), then the program and the declaration's constant loads in push-buffer form at `+0x114`. The handle is the object's address with bit 0 set, which tells programmable shaders from fixed-function vertex formats (an FVF becomes D3D's fixed-function object at `0x184478`). `SetVertexShader` with a program loads it at slot 0 and selects it, as the original's `LoadVertexShader` and `SelectVertexShader`: the runtime keeps the GPU's 136-slot program memory and 192 constants (`vs_program`, `vs_constants`), filled from the object's push-buffer form and by the `SetVertexShaderConstant*` calls. The renderer decodes the program at the selected slot when it draws. Next, the programs become SPIR-V.

**Pixel shaders** are the NV2A's register combiners: `CreatePixelShader` and `DeletePixelShader` are lifted (they only allocate, copy the game's `D3DPIXELSHADERDEF` and free), so the renderer reads the combiner settings from the original object.

## The renderer

`gfx::Renderer` (`recomp/runtime/gfx/gfx.hpp`) is the only code that touches Vulkan.

- **Vulkan 1.3 core**, loaded at run time through volk: building needs no Vulkan SDK, and a machine without Vulkan gets a message naming what's missing. It uses dynamic rendering and synchronization2 (both core in 1.3), picks a discrete GPU over an integrated one over a software renderer (`--gpu N` chooses), and enables `VK_KHR_portability_subset` where it's offered (MoltenVK).
- **Frame buffers are the renderer's own images**, B8G8R8A8 as the Xbox's A8R8G8B8, plus a D24S8 depth buffer (D32S8 where a GPU has no D24S8, as on Apple GPUs). The D3D back buffer is drawn into; `present` flips, as the Xbox's `Swap` does. Rendering doesn't depend on a window: a frame can be read back, saved (`--dump-frames`) or later scaled to any window size.
- **Clears:** a clear of the whole buffer before anything is drawn becomes the render pass's load operation, which is what tile-based GPUs (Raspberry Pi, Apple) need to run well; other clears are `vkCmdClearAttachments`.
- **Validation:** `QILIN_VK_VALIDATION=1` turns on the Khronos validation layer if it's installed. The renderer test runs with it in CI, on Mesa's software driver (llvmpipe) on Linux x64 and ARM64; it's skipped where there's no Vulkan device (the macOS and Windows runners).

## Direct3D replacements so far

| Function | Replacement |
|---|---|
| `Direct3D_SetPushBufferSize` | Kept where the original keeps the values; there's no push buffer |
| `Direct3D_CreateDevice` | Reads the presentation parameters and starts the renderer. Makes the frame buffers (front plus back buffers) and the depth buffer as guest surfaces held by the device, with their pixels in contiguous memory, as the original does. The game asks for 640×480, one back buffer, D24S8 depth, no multisampling (`0x11` is `D3DMULTISAMPLE_NONE` on the Xbox, 1×1 samples) and presentation interval 2: 30 frames per second, as on the console. Any other multisample type stops the runtime |
| `D3DDevice_GetBackBuffer2`, `GetDepthStencilSurface2`, `GetRenderTarget2` | The surfaces, with a reference added. Index −1 is the front buffer |
| `D3DResource_Release`, `D3DResource_Register` | As the original (`AddRef` is lifted). A resource with no references left is destroyed by D3D's own `D3D_DestroyResource`, lifted (v0.21.0) |
| `D3D_BlockOnResource`, `D3DVertexBuffer_Lock2` | See the lifted resource code above |
| `D3DDevice_CreateVertexShader` | Builds the original's shader object, and decodes the program (Vertex shaders, above) |
| `D3DDevice_SetVerticalBlankCallback` | Kept; called at each vblank (below) |
| `D3DDevice_SetGammaRamp` | Kept; frames written with `--dump-frames` go through it, as the TV would show them |
| `D3DDevice_BlockUntilIdle` | Waits for the GPU |
| `D3DDevice_Clear` | Colour, depth and stencil, all of the target or a list of rectangles. Clearing some colour channels only stops the runtime |
| `D3DDevice_Swap` | Shows the frame, waits for the vblank the presentation interval asks for, runs the vblank callback (below), returns the swap count |

**Vblanks and the game's clock.** The game's vblank callback (`0xFC3B0`) counts vblanks, and on the one flagged `D3DVBLANK_SWAPDONE` records how many passed since the last frame: that count is the game's time step (`frame-timing.md`). So `Swap` does what the hardware did: it waits for the vblank the interval asks for, at the NTSC rate of 60/1.001 Hz, then calls the callback once for every vblank that went by, the last one flagged. A frame that takes too long gets a bigger time step, as on the console. Higher refresh rates come later (roadmap step 8).

**State setters and draws (v0.20.0).** Each setter stores what the original stores where the original keeps it (D3D's tables, the device's fields, dirty bits) and nothing else; the GPU methods the original would push are left to the renderer, which reads the state when it draws. Where the original derives guest-visible state with internal helpers (the default state, shader objects, a render target's depth range and multisample scale), the runtime calls those helpers, lifted. They write GPU methods as they go, into the device's push buffer: here a **sink** that nothing reads, which `MakeSpace` and `BeginStateBig` rewind.

| Function | Replacement |
|---|---|
| `SetTexture`, `SetStreamSource` (lifted), `SetVertexShaderInput` | The device's internal references, the slots, the dirty bits; a texture's format key |
| `SetPixelShader`, `SetPixelShaderConstant` | The handle and the definition's flags in the device, its registers in `D3D__RenderState`; constants packed to colours (clamped, ×255, rounded to nearest even) into the combiner constants the shader maps to each register |
| `SetVertexShader`, `LoadVertexShader`, `SelectVertexShader`, `SetVertexShaderConstant*` | Above (Vertex shaders) |
| `SetRenderStateNotInline`, `SetTextureStageStateNotInline`, `SetTextureState_*`, `SetScreenSpaceOffset` | By state range, as the original: table slot and dirty bits, or the state's own setter |
| `SetRenderTarget` | References, depth range and format bits, anti-aliasing mode, the multisample scale and the viewport reset to the whole target |
| `DrawVertices`, `DrawIndexedVertices`, `BeginPush`/`EndPush` | Consume the dirty state as the original's flush does, and record the draw with a copy of the vertices it reads (attribute slots of the shader object, D3D's stream table at `0x1842A0`). The game's own methods are decoded: `SET_BEGIN_END` and `INLINE_ARRAY` are all it writes; any other stops the runtime |
| `InsertFence`, `BlockOnFence` | The device's fence counter steps by 2, as the original; every fence has passed, since a draw copies what it reads |

## Drawing (v0.22.0)

Each draw call (and each `SET_BEGIN_END` pair the game pushes itself) becomes a renderer draw at once, in order with clears.

- **Vertices** are read from the shader object's attribute slots and D3D's stream table and converted to vec4 floats, attribute by attribute, as the NV2A reads its formats (D3DCOLOR's byte order, normalized shorts as (2s + 1) / 65535, the packed 11:11:10 format), missing components (0, 0, 0, 1). Quads, quad strips, fans, polygons and line loops become indexed lists (Vulkan has no quads, and Apple's Vulkan no fans).
- **Shaders are generated as SPIR-V** by the runtime's own builder (`gfx/spirv.*`), so nothing compiles shaders at build or run time. A vertex program comes from the GPU's program memory at the selected slot (`gfx/nv2a_vs.cpp`); registers are SSA values, as the program runs straight through. The program's screen-space output (D3D appends the viewport transform to every program) is mapped back to clip space with the target's size and depth scale, times w. The pixel side is the register combiners (`gfx/nv2a_ps.cpp`), keyed by everything in the shader definition but the constants, which are uniforms. Floats replace the hardware's 9-bit fixed point.
- **Derived state at draw time,** as D3D's state flush would push it: the viewport constants c-38 and c-37 (computed as D3D's `0x1710E0` does), blend, depth, stencil, cull and colour-write state from the render states, the combiner constants.
- **Textures** (`gfx/texture.*`): swizzled, linear and DXT1/3/5, every level, face and slice, decoded to RGBA8 and cached; texels are hashed once a frame and re-uploaded when they change. Linear textures are addressed in texels, as the hardware does.
- **Culling:** the original sends `CULLMODE` to the GPU as GL's front/back against `FRONTFACE`, so the winding culled is the one `CULLMODE` names in GL's terms, y up. On the screen, y down, `D3DCULL_CW` culls what looks counter-clockwise.

**Not modelled yet:** the fixed-function vertex and pixel pipelines (the game's draws so far are all programs and combiners), texture modes beyond the six above (bump mapping, dot products), fog factors (the vertex fog value is used as is), rendering into textures (named once and skipped), palettized textures, point sprites, the combiners' fixed-point precision, CPU writes to a frame buffer reaching the screen (the disc-error screen draws into the front buffer this way), the device's own `Release`, draws from `SetVertexShaderInput` streams, `Swap` keeping a render target that isn't the back buffer, a window.

## Next

1. **State** (decided in v0.19.0: API level, as designed). D3D's setters are rewritten natively, not lifted: each stores what the game or lifted code reads back where the original keeps it (render-state table slots, the device's transforms and viewport, dirty bits), and the renderer computes the GPU state D3D derives (fog, depth, the projection-viewport composite) at draw time. Anything derived the game can read back but that isn't reproduced yet stops the runtime (`GetProjectionViewportMatrix`). Done through v0.20.0: the setters above, as the game reaches them.
2. **Shaders to SPIR-V:** the decoded vertex programs, the fixed-function pipeline, and the pixel combiners.
3. **A window** on SDL3, so frames are shown and not only saved.
4. **Drawing:** vertex formats and buffers, the fixed-function pipeline and vertex shaders, the pixel-shader combiners (to SPIR-V), textures (swizzled and DXT formats), render states from the guest tables, render targets. Then the `BeginPush` immediate-mode draws.
5. The improvements the design allows: higher resolutions, SMAA, better texture filtering.
