- `ith/rhi_d3d12`: the Direct3D 12 backend of the RHI on Windows. `createD3D12Device()` creates a
  device on the fastest hardware GPU or on WARP, refuses GPUs without Shader Model 6.6, Resource
  Binding Tier 3, wave intrinsics or enhanced barriers, and runs bindless resources, enhanced
  barriers, DXIL pipelines, indirect draws and dispatches, flip-model swapchains, the debug layer
  and DRED device-loss reports. See the Direct3D 12 section of `docs/development/rhi.md`.
- `ith/rhi_select/CreateDevice.h`: `createDevice()` picks the RHI backend: Direct3D 12 by default
  on Windows, otherwise Vulkan (not built yet), or the null backend.
- Player and editor applications on Windows that link `rhi_select` run on the Direct3D 12 Agility
  SDK 1.620.2, whose runtime the build copies next to them (`ith_use_d3d12_agility_sdk()`).
