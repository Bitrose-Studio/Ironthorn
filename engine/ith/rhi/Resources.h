#pragma once

// Descriptions of the RHI's resources: buffers, textures, samplers and swapchains (ADR-0009).
//
// Descriptions are read during the create call only; nothing in them needs to outlive it. Every
// object takes a debug name (rule 6), which the backend passes to the graphics API and tools.
// Shaders reach buffers, textures and samplers through bindless descriptor indices (rule 8),
// which the device hands out per usage (Device::shaderReadIndex() and friends).

#include "ith/platform/NativeWindowHandle.h"
#include "ith/rhi/Flags.h"
#include "ith/rhi/Format.h"

#include <cstdint>
#include <string_view>

namespace ith::rhi {

// How a buffer may be used. The shader usages give the buffer a bindless index each.
enum class BufferUsage : std::uint8_t {
    None = 0,
    // Read in shaders through Device::shaderReadIndex() (a read-only structured or byte buffer).
    ShaderRead = 1u << 0,
    // Written in shaders through Device::shaderWriteIndex() (a read-write buffer).
    ShaderWrite = 1u << 1,
    // Bound with CommandList::setIndexBuffer().
    Index = 1u << 2,
    // Holds arguments or counts of indirect draws and dispatches.
    Indirect = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
};
template <>
inline constexpr bool kIsFlags<BufferUsage> = true;

// Where a buffer's memory lives.
enum class MemoryType : std::uint8_t {
    // GPU memory; filled by copies or shaders.
    GpuOnly,
    // CPU-writable, GPU-readable; persistently mapped (Device::mappedMemory()). For uploads and
    // per-frame constants.
    Upload,
    // GPU-writable, CPU-readable; persistently mapped. For reading results back.
    Readback,
};

struct BufferDesc {
    // Size in bytes. Must be positive.
    std::uint64_t size = 0;
    // At least one usage.
    BufferUsage usage = BufferUsage::None;
    // Upload buffers can't have ShaderWrite or CopyDst; Readback buffers have only CopyDst.
    MemoryType memory = MemoryType::GpuOnly;
    // Must not be empty.
    std::string_view debugName;
};

// How a texture may be used.
enum class TextureUsage : std::uint8_t {
    None = 0,
    // Sampled in shaders through Device::sampledIndex().
    Sampled = 1u << 0,
    // Read and written in shaders through Device::storageIndex() (mip 0). Not with an sRGB format:
    // shaders can't write sRGB textures directly on every API.
    Storage = 1u << 1,
    // A color attachment of CommandList::beginRendering(). Not with a depth format.
    RenderTarget = 1u << 2,
    // The depth attachment of CommandList::beginRendering(). Needs a depth format.
    DepthStencil = 1u << 3,
    CopySrc = 1u << 4,
    CopyDst = 1u << 5,
};
template <>
inline constexpr bool kIsFlags<TextureUsage> = true;

enum class TextureDimension : std::uint8_t {
    // width x height, with `layers` array layers.
    Texture2D,
    // width x height x depth.
    Texture3D,
    // Six square faces per cube; `layers` is a multiple of 6 (6 for one cube).
    TextureCube,
};

struct TextureDesc {
    TextureDimension dimension = TextureDimension::Texture2D;
    // Size of mip 0 in texels. width and height must be positive; depth is 1 except for 3D.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    // Array layers: 1 for 3D textures, at least 1 otherwise.
    std::uint32_t layers = 1;
    // Mip levels, from 1 to the full chain of the largest dimension.
    std::uint32_t mipLevels = 1;
    // Must not be Unknown.
    Format format = Format::Unknown;
    // At least one usage.
    TextureUsage usage = TextureUsage::None;
    // Must not be empty.
    std::string_view debugName;
};

enum class Filter : std::uint8_t {
    Nearest,
    Linear
};

enum class AddressMode : std::uint8_t {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder
};

// A comparison, for depth tests and comparison samplers. Scene depth is reversed (ADR-0006), so
// nearer is greater.
enum class CompareOp : std::uint8_t {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always
};

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    // 1 turns anisotropic filtering off; at most 16.
    float maxAnisotropy = 1.0f;
    // Whether this is a comparison sampler (for shadow maps), and its comparison.
    bool compare = false;
    CompareOp compareOp = CompareOp::Greater;
    float mipLodBias = 0.0f;
    float minLod = 0.0f;
    float maxLod = 1000.0f;
    // Must not be empty.
    std::string_view debugName;
};

struct SwapchainDesc {
    // The window to present to, from ith::Window::nativeHandle(). The swapchain must be destroyed
    // before the window. The real backends reject std::monostate; the null backend accepts it.
    NativeWindowHandle window;
    // The size in pixels (ith::Window::pixelSize()); both positive. Later sizes, including zero
    // for a minimized window, go to Device::resizeSwapchain().
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // B8G8R8A8Srgb, B8G8R8A8Unorm, R8G8B8A8Srgb, R8G8B8A8Unorm or R10G10B10A2Unorm.
    Format format = Format::B8G8R8A8Srgb;
    // Wait for vertical blank (no tearing) when presenting.
    bool vsync = true;
    // Must not be empty.
    std::string_view debugName;
};

// The most images a swapchain has; backends create 2 or 3.
inline constexpr std::uint32_t kMaxSwapchainImages = 4;

} // namespace ith::rhi
