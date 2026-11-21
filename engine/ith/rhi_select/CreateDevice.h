#pragma once

// Creates an RHI device on the backend the platform or the caller chooses (ADR-0021).
//
//   ith::Expected<ith::rhi::Device, ith::rhi::Error> device = ith::rhi::createDevice({});
//   if (!device) {
//       return fail(device.error()); // e.g. no GPU meets the minimum: rhi.unsupported_device
//   }
//
// Applications call this instead of a backend's factory, so that they depend on no backend
// module. On Windows the executable must run on the Direct3D 12 Agility SDK, which
// ith_add_application sets up for player and editor applications that link this module
// (docs/development/rhi.md).

#include "ith/foundation/Expected.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

#include <optional>

namespace ith::rhi {

// The backend createDevice() uses without a choice: Direct3D 12 on Windows, Vulkan elsewhere.
[[nodiscard]] Backend defaultBackend();

// Creates a device on `backend`, or on defaultBackend() when it is std::nullopt.
// - Backend::D3D12 calls createD3D12Device(desc) (ith/rhi_d3d12/D3D12Device.h);
// - Backend::Vulkan will call createVulkanDevice(desc);
// - Backend::Null creates a null device without a GPU (ith/rhi/NullDevice.h), ignoring `desc`.
//
// Fails with BackendUnavailable when the backend is not built into this program (Direct3D 12
// outside Windows; Vulkan until its backend exists), or with the backend's own errors, such as
// UnsupportedDevice. Must be called on the thread that will be the render thread.
Expected<Device, Error> createDevice(const DeviceDesc& desc, std::optional<Backend> backend = std::nullopt);

} // namespace ith::rhi
