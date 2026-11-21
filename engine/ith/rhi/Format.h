#pragma once

// Texture and render-target formats of the RHI, and their sizes.
//
// The list holds what the first renderer needs; formats are added when a pass needs them, and
// every backend maps each one. Block-compressed formats come with texture streaming.

#include <cstdint>

namespace ith::rhi {

enum class Format : std::uint8_t {
    Unknown,
    R8Unorm,
    R8G8Unorm,
    R8G8B8A8Unorm,
    R8G8B8A8Srgb,
    B8G8R8A8Unorm,
    B8G8R8A8Srgb,
    R10G10B10A2Unorm,
    R11G11B10Float,
    R16G16Float,
    R16G16B16A16Float,
    R32Uint,
    R32Float,
    R32G32Float,
    R32G32B32A32Float,
    // Depth formats. Scene depth is reversed (ADR-0006), so D32Float is the default.
    D32Float,
    D32FloatS8Uint,
};

// The size of one texel in bytes, as laid out in a buffer for copies; 0 for Unknown.
// D32FloatS8Uint reports 4: copies address its depth plane only.
[[nodiscard]] std::uint32_t formatTexelSize(Format format);

// True for the depth (and depth-stencil) formats.
[[nodiscard]] bool isDepthFormat(Format format);

} // namespace ith::rhi
