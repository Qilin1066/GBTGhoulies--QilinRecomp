# Graphics: Direct3D on Vulkan (roadmap step 4)

Last updated 2026-11-18 (v0.16.0: D3D's resource code lifted; guest RAM's kernel windows; v0.14.0: the renderer and the Direct3D device).

The game draws through the Xbox's Direct3D 8 library (D3D8, XDK 5659), which is linked into the XBE. Its calls stay in the recompiled code; what answers them is the runtime. Every D3D function the game calls is replaced natively (`recomp/runtime/host/d3d.cpp`) over a Vulkan 1.3 renderer (`recomp/runtime/gfx/`). There is no DirectX anywhere, and the Xbox's GPU (the NV2A) isn't emulated: the replacements see what the game asks for (clear this, draw that mesh with this texture) and do it with Vulkan.

```
lifted game code  →  D3D call  →  replacement (d3d.cpp)  →  renderer (gfx/)  →  Vulkan  →  the GPU
```

## What the game uses

The game's own code (and Rare's libraries) call 90 D3D functions, 1,095 call sites in all (`docs/code-analysis.md`). XbSymbolDatabase names 84 of them by signature; the other six were identified by reading them (`D3DDevice_BlockUntilIdle`, `D3DDevice_CreateSurface2`, `D3DTexture_GetLevelDesc`, `Direct3D_SetPushBufferSize`, D3D's contiguous-memory allocator, and a push-buffer helper at `0x176E00` still to name). `tools/gen/xdk_symbols.py` turns the database's scan into `recomp/runtime/host/xdk_symbols.inc`, which gives the runtime the names of 390 XDK library functions and variables (D3D8, DSOUND, XACT, XAPI, XGraphics). Reports use them: `no replacement for D3D8 D3DDevice_SetTexture (001749E0)`.

The game never reads the D3D device object, so the device is the runtime's. But it does write D3D's state directly: the XDK's `SetRenderState` and `SetTextureStageState` are inline functions, so the game stores render states into `D3D__RenderState` (`0x181B70`), texture-stage states into the table at `0x181970`, and sets bits in the dirty-flags word at `0x181968`, 148 writes in all. Those tables stay in guest memory in the original layout and are the source of truth: the renderer reads them when it draws.

Resources (surfaces, textures, vertex buffers) stay D3D resource headers in guest memory, as the game passes them around and may read their format and size: `Common` (reference count in the low 16 bits, type, the device's internal references), `Data`, `Lock`, `Format`, `Size`, and a surface's `Parent`. Their data stays in guest memory too, where the game writes it; the renderer copies what it draws into Vulkan images when it draws.

**D3D's resource code is lifted** (v0.16.0, `kLiftedD3d` in `recomp/src/main.cpp`): creating textures, surfaces and vertex buffers, surface levels, descriptions and locks, 18 functions. They're computation over the game's own allocator (`0xF7270`), so lifting them makes headers, sizes, pitches and swizzled layouts exactly the original's. What they call that touches the GPU stays native: `D3D_BlockOnResource`, the wait for the GPU to finish with a resource, returns at once (the GPU never holds guest memory here), and `D3DVertexBuffer_Lock2`, which in the original also writes a wait into the command buffer. The lifted code reads a few fields of the device object, which the runtime keeps current in the original layout: the frame buffers (`+0x1A04`, `+0x1A08`, `+0x1A0C`), the render target (`+0x19F4`), the depth buffer (`+0x19F8`) and the multisample type (`+0x195C`).

**Locks return addresses in the kernel's windows onto RAM**: a vertex buffer's `Data | 0x80000000`, a surface's `Data | 0xF0000000` (write-combined). The game writes vertices and pixels through them, so guest RAM is mapped at both (`runtime.md`).

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
| `D3DResource_Release`, `D3DResource_Register` | As the original (`AddRef` is lifted). Freeing a resource isn't modelled yet and stops the runtime |
| `D3D_BlockOnResource`, `D3DVertexBuffer_Lock2` | See the lifted resource code above |
| `D3DDevice_SetVerticalBlankCallback` | Kept; called at each vblank (below) |
| `D3DDevice_SetGammaRamp` | Kept; frames written with `--dump-frames` go through it, as the TV would show them |
| `D3DDevice_BlockUntilIdle` | Waits for the GPU |
| `D3DDevice_Clear` | Colour, depth and stencil, all of the target or a list of rectangles. Clearing some colour channels only stops the runtime |
| `D3DDevice_Swap` | Shows the frame, waits for the vblank the presentation interval asks for, runs the vblank callback (below), returns the swap count |

**Vblanks and the game's clock.** The game's vblank callback (`0xFC3B0`) counts vblanks, and on the one flagged `D3DVBLANK_SWAPDONE` records how many passed since the last frame: that count is the game's time step (`frame-timing.md`). So `Swap` does what the hardware did: it waits for the vblank the interval asks for, at the NTSC rate of 60/1.001 Hz, then calls the callback once for every vblank that went by, the last one flagged. A frame that takes too long gets a bigger time step, as on the console. Higher refresh rates come later (roadmap step 8).

**Not modelled yet:** CPU writes to a frame buffer reaching the screen (the disc-error screen draws into the front buffer this way), the device's own `Release`, drawing, textures, shaders, render targets other than the back buffer, the 17 functions that write GPU commands directly (`BeginPush`), a window.

## Next

1. **Shaders:** the game creates its first vertex shader next. Xbox vertex shaders are NV2A microcode, and pixel shaders configure the register combiners; both become SPIR-V.
2. **A window** on SDL3, so frames are shown and not only saved.
3. **Drawing:** vertex formats and buffers, the fixed-function pipeline and vertex shaders, the pixel-shader combiners (to SPIR-V), textures (swizzled and DXT formats), render states from the guest tables, render targets. Then the `BeginPush` immediate-mode draws.
4. The improvements the design allows: higher resolutions, SMAA, better texture filtering.
