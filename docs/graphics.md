# Graphics: Direct3D on Vulkan (roadmap step 4)

Last updated 2026-11-18 (v0.14.0: the renderer and the Direct3D device).

The game draws through the Xbox's Direct3D 8 library (D3D8, XDK 5659), which is linked into the XBE. Its calls stay in the recompiled code; what answers them is the runtime. Every D3D function the game calls is replaced natively (`recomp/runtime/host/d3d.cpp`) over a Vulkan 1.3 renderer (`recomp/runtime/gfx/`). There is no DirectX anywhere, and the Xbox's GPU (the NV2A) isn't emulated: the replacements see what the game asks for (clear this, draw that mesh with this texture) and do it with Vulkan.

```
lifted game code  →  D3D call  →  replacement (d3d.cpp)  →  renderer (gfx/)  →  Vulkan  →  the GPU
```

## What the game uses

The game's own code (and Rare's libraries) call 90 D3D functions, 1,095 call sites in all (`docs/code-analysis.md`). XbSymbolDatabase names 84 of them by signature; the other six were identified by reading them (`D3DDevice_BlockUntilIdle`, `D3DDevice_CreateSurface2`, `D3DTexture_GetLevelDesc`, `Direct3D_SetPushBufferSize`, D3D's contiguous-memory allocator, and a push-buffer helper at `0x176E00` still to name). `tools/gen/xdk_symbols.py` turns the database's scan into `recomp/runtime/host/xdk_symbols.inc`, which gives the runtime the names of 390 XDK library functions and variables (D3D8, DSOUND, XACT, XAPI, XGraphics). Reports use them: `no replacement for D3D8 D3DDevice_SetTexture (001749E0)`.

The game never reads the D3D device object, so the device is the runtime's. But it does write D3D's state directly: the XDK's `SetRenderState` and `SetTextureStageState` are inline functions, so the game stores render states into `D3D__RenderState` (`0x181B70`), texture-stage states into the table at `0x181970`, and sets bits in the dirty-flags word at `0x181968`, 148 writes in all. Those tables stay in guest memory in the original layout and are the source of truth: the renderer reads them when it draws.

Resources (surfaces, textures, vertex buffers) stay D3D resource headers in guest memory, as the game passes them around and may read their format and size: `Common` (reference count in the low 16 bits, type, the device's internal references), `Data`, `Lock`, `Format`, `Size`, and a surface's `Parent`. The runtime's headers are in contiguous memory; the pixels behind them are Vulkan images.

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
| `Direct3D_CreateDevice` | Reads the presentation parameters and starts the renderer. Makes the frame buffers (front plus back buffers) and the depth buffer as guest surfaces held by the device, as the original does. The game asks for 640×480, one back buffer, D24S8 depth, 2× multisampling (`0x11`) and presentation interval 2: 30 frames per second, as on the console |
| `D3DDevice_GetBackBuffer2`, `GetDepthStencilSurface2`, `GetRenderTarget2` | The surfaces, with a reference added. Index −1 is the front buffer |
| `D3DResource_AddRef`, `D3DResource_Release`, `D3DResource_Register` | As the original. Freeing a resource isn't modelled yet and stops the runtime |
| `D3DDevice_SetVerticalBlankCallback` | Kept; called at each vblank (below) |
| `D3DDevice_SetGammaRamp` | Kept; frames written with `--dump-frames` go through it, as the TV would show them |
| `D3DDevice_BlockUntilIdle` | Waits for the GPU |
| `D3DDevice_Clear` | Colour, depth and stencil, all of the target or a list of rectangles. Clearing some colour channels only stops the runtime |
| `D3DDevice_Swap` | Shows the frame, waits for the vblank the presentation interval asks for, runs the vblank callback (below), returns the swap count |

**Vblanks and the game's clock.** The game's vblank callback (`0xFC3B0`) counts vblanks, and on the one flagged `D3DVBLANK_SWAPDONE` records how many passed since the last frame: that count is the game's time step (`frame-timing.md`). So `Swap` does what the hardware did: it waits for the vblank the interval asks for, at the NTSC rate of 60/1.001 Hz, then calls the callback once for every vblank that went by, the last one flagged. A frame that takes too long gets a bigger time step, as on the console. Higher refresh rates come later (roadmap step 8).

**Not modelled yet:** multisampling (frames are rendered at 640×480, one sample per pixel, and the runtime says so), the device's own `Release`, drawing, textures, shaders, render targets other than the back buffer, the 17 functions that write GPU commands directly (`BeginPush`), a window.

## Next

1. **A window**, so frames are shown and not only saved. The roadmap has SDL3 for windows, input and audio.
2. **Drawing:** vertex formats and buffers, the fixed-function pipeline and vertex shaders, the pixel-shader combiners (to SPIR-V), textures (swizzled and DXT formats), render states from the guest tables, render targets. Then the `BeginPush` immediate-mode draws.
3. **Multisampling**, then the improvements the design allows: higher resolutions, SMAA, better texture filtering.
