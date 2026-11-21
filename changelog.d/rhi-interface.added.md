- `ith/rhi`: the backend-neutral rendering hardware interface. `rhi::Device` creates buffers,
  textures, samplers, graphics and compute pipelines and swapchains, refers to them by
  generational handles that go stale on `destroy()`, and releases them only after the GPU and
  every command list open at the time are done with them. Shaders reach resources through
  bindless indices and 128-byte push constants. Frames run two in flight; swapchains are created
  from a window's native handle and recreated after `resizeSwapchain()` or when out of date.
  `rhi::CommandList` records dynamic rendering, explicit barriers, draws (indexed, and indirect
  with a count), dispatches and copies, from several threads at once, with checks in Debug and
  development builds. Failures, device loss included, are `rhi::Error` values. See
  `docs/development/rhi.md`.
- `ith/rhi/NullDevice.h`: a null backend without a GPU, for tests and for running the editor or
  player without one, with a simulated GPU timeline and fault injection. The Direct3D 12 and
  Vulkan backends are not built yet.
