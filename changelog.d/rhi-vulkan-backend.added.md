- `ith/rhi_vulkan`: the Vulkan backend of the RHI, on Windows and Linux. `createVulkanDevice()`
  creates a Vulkan 1.3 device on the best hardware GPU or a software one (lavapipe) after the
  ADR-0005 minimum check, with dynamic rendering, synchronization2 barriers, one bindless
  descriptor set, 128-byte push constants, VMA memory, timeline semaphores, swapchains for Win32,
  X11 and Wayland windows, and `VK_EXT_device_fault` reports on device loss. It opens the system's
  Vulkan loader at run time and links no Vulkan library. In Debug builds it runs the Khronos
  validation layer with synchronization validation when the layer is installed. See
  `docs/development/rhi.md`.
- `createDevice()` creates Vulkan devices: the default outside Windows, and on request on Windows.
- `window_player --backend d3d12|vulkan|null` picks the backend, and `--adapter hardware|software` the GPU.
- The shader compiler keeps entry-point names in SPIR-V instead of renaming them to `main`.
