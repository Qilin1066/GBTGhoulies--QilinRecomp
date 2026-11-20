# Third-party code

| Library | Version | Licence | Path | Notes |
|---|---|---|---|---|
| [Zydis](https://github.com/zyantific/zydis) | v4.1.1 (`a2278f1d254e492f6a6b39f6cb5d1f5d515659dc`) | MIT | `zydis/` | x86 decoder used by the recompiler |
| [Zycore](https://github.com/zyantific/zycore-c) | `0b2432ced0884fd152b471d97ecf0258ff4d859f` (Zydis v4.1.1 submodule) | MIT | `zydis/dependencies/zycore/` | Zydis support library |
| [nxdk](https://github.com/XboxDev/nxdk) | `lib/xboxkrnl/xboxkrnl.exe.def` at `fafbe41b48fa00a31b79dd49ad1f86a30adcdd9e` | CC0-1.0 | `nxdk/xboxkrnl.exe.def` | Xbox kernel export names, ordinals and argument sizes; `tools/gen/kernel_exports.py` turns it into `recomp/runtime/host/kernel_exports.inc` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.3.296.0` | Apache-2.0 OR MIT | `vulkan-headers/` | The Vulkan C headers (`include/vulkan/*.h`, `include/vk_video/*.h`); the C++ bindings were dropped |
| [Dear ImGui](https://github.com/ocornut/imgui) | `v1.93.0` (`c1aaac65e896a45f81f47a4d68f9959e0f86c859`) | MIT (Omar Cornut) | `imgui/` | The settings menu (`recomp/runtime/ui/menu.cpp`), as ApbArcade-QilinRecomp vendors it: the core (`imgui*.cpp`, `imgui*.h`, `imconfig.h`, the three `imstb_*.h`) and two backends (`backends/imgui_impl_sdl3.*`, `backends/imgui_impl_vulkan.*`, with the shaders' GLSL sources in `backends/vulkan/`), all unmodified. Built with `IMGUI_IMPL_VULKAN_USE_VOLK` so it loads Vulkan through volk as the renderer does |
| [FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | `a21ffb8f6c13233ba336352bdff293894c706575` (FSR 1.0) | MIT (AMD) | `fsr1/` | AMD FidelityFX Super Resolution 1 for the present pass: `ffx_a.h` and `ffx_fsr1.h`, unmodified, with `LICENSE.txt`. `recomp/runtime/gfx/fsr/fsr.comp` includes them for its two compute passes, which `tools/gen/fsr_spirv.sh` compiles to SPIR-V once (`fsr_spirv.inc`, committed); `fsr_constants.cpp` builds them for the CPU for the passes' constants |
| [SMAA](https://github.com/iryoku/smaa) | `71c806a838bdd7d517df19192a20f0c61b3ca29d` | MIT (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) | `smaa/` | SMAA 1x anti-aliasing for the present pass: `SMAA.hlsl` and the area and search tables (`AreaTex.h`, `SearchTex.h`), unmodified, with `LICENSE.txt`. `recomp/runtime/gfx/smaa/smaa.glsl` includes it (at the HIGH preset, every sample at level 0) for its three passes, which `tools/gen/smaa_spirv.sh` compiles to SPIR-V once (`smaa_spirv.inc`, committed); the renderer includes the two tables |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.3.296.0` | MIT | `volk/` | Loads the Vulkan library at run time: no Vulkan SDK needed to build, and a clear message where there's no Vulkan |
| [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) | `20eced544726f5558c5a408458f38a086cc4e543` (not vendored) | MIT | | Run once by `tools/gen/xdk_symbols.py` to name the XDK library functions in the game's XBE; the result is `recomp/runtime/host/xdk_symbols.inc` (addresses and names only) |

Vendored as plain source (no submodules) so a clone builds offline on every
platform. Only `CMakeLists.txt`, `cmake/`, `include/`, `src/`, `LICENSE` and
`README.md` were kept; tests, examples, docs and tools were dropped.

To update: fetch the new tag with `--recurse-submodules`, copy the same
directories over, update this table, rebuild, and rerun the LLVM decoder
cross-check (`tools/check/check_xbe.sh`).
