# 0.48.2 graphics and camera investigation

Checked on 2026-11-27. The unpublished 0.48.1 patch was restored against
upstream `45495d4` because the resumed workspace contained an older snapshot.
The combined fixes are checked together. No game disc or scene recording was
available here; the basement pickup camera report is not marked resolved.

## Confirmed fixes and regressions

| Area | Evidence |
|---|---|
| Final combiner | Pixel readback checks unclamped/explicitly clamped sums, signed cancellation, signed complements and zero alpha on RGB-only special registers. Restoring the previous translator failed 21 checks before adding the blended-transparent-rectangle case. |
| Two-sided colours | Front/back diffuse and specular colour/alpha, culling both windings, and one-sided fallback are checked through generated SPIR-V and pixel readback. |
| Descriptor rollover | 64 distinct texture sets with a 32-set test pool, indexed geometry and changing constants preserve every column across submission switches. Uploads follow descriptor allocation, including its failure/retry path. |
| Texture mip limits | Red/green/blue mip levels select 0/1/2 as requested; oversized limits clamp; disabled mip filtering uses level 0. |
| Back-buffer depth/stencil | Unbinding depth stops depth/stencil tests from hiding pixels; alternate depth/stencil values come from the selected attachment. Destroying the bound alternate depth detaches it. Restoring the old binding code fails six pixel checks. |
| Viewport bounds | Native SetViewport is called with normal, clipped, off-target, zero-sized and overflowing extents over two device lifetimes. Restoring the previous arithmetic fails the native test. |
| Interrupted rendering | Two passes retain and blend attachment contents across a temporary target switch. Explicit same-layout write barriers replace reliance on driver ordering. The old no-barrier path also passes on lavapipe, so this is a specification correction rather than a reproduced visual failure on this driver. |

Vulkan explicitly does not guarantee ordering across separate render passes,
even with matching attachments; see
[vkCmdEndRendering](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdEndRendering.html).
The final-combiner model follows
[xemu's translator](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/psh.c).

## Completed checks

- Full Release build with GCC 14 and warnings as errors, including the
  empty-game-table player link check.
- All seven CTest suites: core, 200,000-case x87 consistency, Vulkan graphics,
  audio, host, Vulkan device lifetime/native viewport, and platform.
- Both Vulkan suites are required to run (`QILIN_REQUIRE_VULKAN=1`).
- Mesa lavapipe 25.0.7/LLVM 19.1.7, Vulkan validation layers 1.4.309.
  The loader confirms the Khronos validation layer is inserted for both
  instance and device. Tests report zero validation errors.
- Dumped shaders are checked by SPIRV-Tools with Vulkan 1.3 as the target.

Reproduction on a machine with Vulkan, validation layers and SPIRV-Tools:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
mkdir -p build/spv-check
QILIN_REQUIRE_VULKAN=1 QILIN_SPV_DIR="$PWD/build/spv-check" ctest --test-dir build --output-on-failure
for shader in build/spv-check/*.spv; do
    spirv-val --target-env vulkan1.3 "$shader" || exit 1
done
```

## Basement pickup and visible effect edges

The renderer faults above can affect visibility and effect masks, but the
reported basement camera transition has not been reproduced. The code does
not forcibly recenter the camera, alter pickup scripts, or hide rectangular
geometry to mask an unknown cause.

F9 now records every model-view/composite update, projection update and
viewport change during its capture window, including ordinary back-buffer
passes that the detailed draw filter omitted. Each record includes film
format, widescreen flag, aspect, time step and selected colour/depth targets.
This can distinguish a guest camera moving away from a rendering state that
hides or clips the character. Capture the wrong view with F9 and retain the
resulting tar file from the data folder's `captures` directory. An exact item,
scene/save and frame-rate/aspect settings are still needed to reproduce and
verify that specific gameplay bug, plus testing on the player's GPU.
