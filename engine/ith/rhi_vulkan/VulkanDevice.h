#pragma once

// The Vulkan backend of the RHI (ADR-0009, ADR-0021). Windows and Linux.
//
//   ith::Expected<ith::rhi::Device, ith::rhi::Error> device = ith::rhi::createVulkanDevice({});
//
// Applications don't call this directly: the rhi_select module picks the backend
// (ith/rhi_select/CreateDevice.h). The backend links no Vulkan library: it opens the system's
// Vulkan loader when a device is created (docs/development/rhi.md).
//
// No Vulkan type appears in this header.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

#include <cstdint>

namespace ith::rhi {

// Creates a Vulkan 1.3 device on the GPU desc.adapter names: the best physical device that meets
// the minimum of ADR-0005, discrete GPUs before integrated and virtual ones
// (AdapterPreference::HighPerformance), or a CPU implementation such as lavapipe
// (AdapterPreference::Software). The device reports Backend::Vulkan and takes SPIR-V shaders:
// trusted SPIR-V from the engine's shader compiler, which validates it. The backend runs no SPIR-V
// validator; it only refuses shaders that bind resources outside its bindless set.
//
// With desc.validation, and when the Khronos validation layer is installed, the layer runs with
// synchronization validation; its warnings and errors are written to stderr, and errors are
// asserted. Without the layer the device is created without validation and says so on stderr.
// Shipping builds never load the layer. desc.gpuBasedValidation is ignored.
//
// Fails with:
// - BackendUnavailable when the Vulkan loader is missing or older than 1.3, or no Vulkan
//   driver is installed;
// - UnsupportedDevice when no GPU of the preferred kind supports Vulkan 1.3, or the best one
//   lacks a minimum feature of ADR-0005 or the descriptor capacities of ADR-0009, each of which
//   the message names;
// - OutOfMemory or BackendFailure when creating the device's own objects fails.
//
// Must be called on the thread that will be the render thread.
Expected<Device, Error> createVulkanDevice(const DeviceDesc& desc);

// For tests of device loss: makes the device's next queue submission (in submit(), endFrame()
// or beginFrame() with a swapchain) fail with VK_ERROR_DEVICE_LOST, as a driver reset or hang
// would, without submitting it. The device then reports DeviceLost from every call that can
// fail; the GPU itself keeps working, so the objects are still released safely.
// With `outOfMemory`, for tests of a failed submission the device survives: the next queue
// submission fails with VK_ERROR_OUT_OF_DEVICE_MEMORY instead, submitting nothing, and
// Device::submit() reports OutOfMemory; call it right before submit(), since a failed submission
// in endFrame() or beginFrame() still makes the device lost.
// Precondition (asserted): `device` came from createVulkanDevice() and wasn't moved from.
void failNextVulkanSubmitForTesting(Device& device, bool outOfMemory = false);

// How a Vulkan device times GPU zones (CommandList::beginGpuZone(); docs/development/rhi.md,
// "GPU zones").
enum class VulkanGpuTiming : std::uint8_t {
    // No GPU zones: profiling is off (ITH_ENABLE_PROFILING), or the device can't time them.
    Off,
    // VK_EXT_calibrated_timestamps against the host's clock: the GPU timeline is aligned with the
    // CPU's again at every collection.
    Calibrated,
    // Aligned once when the device is created, from a timestamp written on the graphics queue.
    Uncalibrated,
};

// For tests of GPU zones: how `device` times them; Off without profiling.
// Precondition (asserted): as failNextVulkanSubmitForTesting().
[[nodiscard]] VulkanGpuTiming vulkanGpuTimingForTesting(Device& device);

// For tests of GPU zones: with `avoid`, Vulkan devices created afterwards in this process time them
// as if the GPU lacked calibrated timestamps (VulkanGpuTiming::Uncalibrated where possible).
// Thread-safe; no effect without profiling.
void avoidCalibratedVulkanGpuTimingForTesting(bool avoid);

// For tests of GPU zones: whether the device turned its GPU zones off for good, after a command
// list with zones never ran (docs/development/rhi.md, "GPU zones"); false without profiling or
// while the device has no profiler. Precondition (asserted): as failNextVulkanSubmitForTesting().
[[nodiscard]] bool vulkanGpuZonesDisabledForTesting(Device& device);

} // namespace ith::rhi
