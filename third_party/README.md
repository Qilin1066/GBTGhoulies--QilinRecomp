# Third-party code

| Library | Version | Licence | Path | Notes |
|---|---|---|---|---|
| [Zydis](https://github.com/zyantific/zydis) | v4.1.1 (`a2278f1d254e492f6a6b39f6cb5d1f5d515659dc`) | MIT | `zydis/` | x86 decoder used by the recompiler |
| [Zycore](https://github.com/zyantific/zycore-c) | `0b2432ced0884fd152b471d97ecf0258ff4d859f` (Zydis v4.1.1 submodule) | MIT | `zydis/dependencies/zycore/` | Zydis support library |
| [nxdk](https://github.com/XboxDev/nxdk) | `lib/xboxkrnl/xboxkrnl.exe.def` at `fafbe41b48fa00a31b79dd49ad1f86a30adcdd9e` | CC0-1.0 | `nxdk/xboxkrnl.exe.def` | Xbox kernel export names, ordinals and argument sizes; `tools/gen/kernel_exports.py` turns it into `recomp/runtime/host/kernel_exports.inc` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.3.296.0` | Apache-2.0 OR MIT | `vulkan-headers/` | The Vulkan C headers (`include/vulkan/*.h`, `include/vk_video/*.h`); the C++ bindings were dropped |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.3.296.0` | MIT | `volk/` | Loads the Vulkan library at run time: no Vulkan SDK needed to build, and a clear message where there's no Vulkan |
| [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) | `20eced544726f5558c5a408458f38a086cc4e543` (not vendored) | MIT | | Run once by `tools/gen/xdk_symbols.py` to name the XDK library functions in the game's XBE; the result is `recomp/runtime/host/xdk_symbols.inc` (addresses and names only) |

Vendored as plain source (no submodules) so a clone builds offline on every
platform. Only `CMakeLists.txt`, `cmake/`, `include/`, `src/`, `LICENSE` and
`README.md` were kept; tests, examples, docs and tools were dropped.

To update: fetch the new tag with `--recurse-submodules`, copy the same
directories over, update this table, rebuild, and rerun the LLVM decoder
cross-check (`tools/check/check_xbe.sh`).
