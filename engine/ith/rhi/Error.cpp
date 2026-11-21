#include "ith/rhi/Error.h"

#include <string>
#include <string_view>

namespace ith::rhi {

std::string_view codeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::DeviceLost:
            return "rhi.device_lost";
        case ErrorCode::OutOfMemory:
            return "rhi.out_of_memory";
        case ErrorCode::BackendUnavailable:
            return "rhi.backend_unavailable";
        case ErrorCode::UnsupportedDevice:
            return "rhi.unsupported_device";
        case ErrorCode::InvalidUsage:
            return "rhi.invalid_usage";
        case ErrorCode::InvalidShader:
            return "rhi.invalid_shader";
        case ErrorCode::OutOfHandles:
            return "rhi.out_of_handles";
        case ErrorCode::OutOfDescriptors:
            return "rhi.out_of_descriptors";
        case ErrorCode::SurfaceFailed:
            return "rhi.surface_failed";
        case ErrorCode::BackendFailure:
            return "rhi.backend_failure";
    }
    return "rhi.unknown";
}

std::string Error::toString() const {
    std::string text = message;
    text += " [";
    text += codeName(code);
    text += ']';
    return text;
}

} // namespace ith::rhi
