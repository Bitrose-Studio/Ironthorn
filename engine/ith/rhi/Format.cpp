#include "ith/rhi/Format.h"

#include <cstdint>

namespace ith::rhi {

std::uint32_t formatTexelSize(Format format) {
    switch (format) {
        case Format::Unknown:
            return 0;
        case Format::R8Unorm:
            return 1;
        case Format::R8G8Unorm:
            return 2;
        case Format::R8G8B8A8Unorm:
        case Format::R8G8B8A8Srgb:
        case Format::B8G8R8A8Unorm:
        case Format::B8G8R8A8Srgb:
        case Format::R10G10B10A2Unorm:
        case Format::R11G11B10Float:
        case Format::R16G16Float:
        case Format::R32Uint:
        case Format::R32Float:
        case Format::D32Float:
        case Format::D32FloatS8Uint:
            return 4;
        case Format::R16G16B16A16Float:
        case Format::R32G32Float:
            return 8;
        case Format::R32G32B32A32Float:
            return 16;
    }
    return 0;
}

bool isDepthFormat(Format format) {
    return format == Format::D32Float || format == Format::D32FloatS8Uint;
}

bool isLinearFilterable(Format format) {
    switch (format) {
        case Format::R8Unorm:
        case Format::R8G8Unorm:
        case Format::R8G8B8A8Unorm:
        case Format::R8G8B8A8Srgb:
        case Format::B8G8R8A8Unorm:
        case Format::B8G8R8A8Srgb:
        case Format::R10G10B10A2Unorm:
        case Format::R11G11B10Float:
        case Format::R16G16Float:
        case Format::R16G16B16A16Float:
            return true;
        case Format::Unknown:
        case Format::R32Uint:
        case Format::R32Float:
        case Format::R32G32Float:
        case Format::R32G32B32A32Float:
        case Format::D32Float:
        case Format::D32FloatS8Uint:
            return false;
    }
    return false;
}

} // namespace ith::rhi
