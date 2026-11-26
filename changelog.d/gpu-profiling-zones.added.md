- GPU profiling zones: `rhi::CommandList::beginGpuZone()` and `endGpuZone()` time the GPU work
  between them for the Tracy 0.15.0 viewer, on the Vulkan and Direct3D 12 backends, and the
  render graph wraps every pass in a zone named after it. Like the CPU macros they compile to
  nothing unless `IRONTHORN_PROFILING` is on. Vulkan uses `VK_EXT_calibrated_timestamps` with the
  host clock where the GPU has it and otherwise aligns the GPU and CPU timelines once at start-up;
  neither changes the minimum GPU features. Discarding a command list with an open GPU zone is
  asserted, like submitting one.
- The CMake option `IRONTHORN_PROFILING_ON_DEMAND` (on by default): turned off, a profiling build
  records from the start without a viewer, as CI's profiling builds do.
