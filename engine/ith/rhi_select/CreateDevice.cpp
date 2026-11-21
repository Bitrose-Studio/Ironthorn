#include "ith/rhi_select/CreateDevice.h"

#include "ith/rhi/NullDevice.h"
#include "ith/rhi_vulkan/VulkanDevice.h"

// Direct3D 12 exists only on Windows.
#ifdef _WIN32
#include "ith/rhi_d3d12/D3D12Device.h"
#endif

#include <optional>

namespace ith::rhi {

Backend defaultBackend() {
#ifdef _WIN32
    return Backend::D3D12;
#else
    return Backend::Vulkan;
#endif
}

Expected<Device, Error> createDevice(const DeviceDesc& desc, std::optional<Backend> backend) {
    switch (backend.value_or(defaultBackend())) {
        case Backend::D3D12:
#ifdef _WIN32
            return createD3D12Device(desc);
#else
            return makeUnexpected(Error{.code = ErrorCode::BackendUnavailable,
                                        .message = "The Direct3D 12 backend exists only on Windows"});
#endif
        case Backend::Vulkan:
            return createVulkanDevice(desc);
        case Backend::Null:
            return createNullDevice();
    }
    return makeUnexpected(Error{.code = ErrorCode::BackendUnavailable, .message = "Unknown RHI backend"});
}

} // namespace ith::rhi
