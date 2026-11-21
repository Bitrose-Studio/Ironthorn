#include "ith/rhi_select/CreateDevice.h"

#include "ith/rhi/NullDevice.h"

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

// `desc` is unused where no GPU backend is built yet.
Expected<Device, Error> createDevice([[maybe_unused]] const DeviceDesc& desc, std::optional<Backend> backend) {
    switch (backend.value_or(defaultBackend())) {
        case Backend::D3D12:
#ifdef _WIN32
            return createD3D12Device(desc);
#else
            return makeUnexpected(Error{.code = ErrorCode::BackendUnavailable,
                                        .message = "The Direct3D 12 backend exists only on Windows"});
#endif
        case Backend::Vulkan:
            return makeUnexpected(
                Error{.code = ErrorCode::BackendUnavailable, .message = "The Vulkan backend is not built yet"});
        case Backend::Null:
            return createNullDevice();
    }
    return makeUnexpected(Error{.code = ErrorCode::BackendUnavailable, .message = "Unknown RHI backend"});
}

} // namespace ith::rhi
