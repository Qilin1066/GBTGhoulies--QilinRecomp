volk, tag `vulkan-sdk-1.3.296.0` (https://github.com/zeux/volk), MIT (see
`LICENSE.md`): loads the Vulkan library at run time, so building needs no
Vulkan SDK and a machine without Vulkan gets a message, not a link error.
Only `volk.c` and `volk.h` are kept.
