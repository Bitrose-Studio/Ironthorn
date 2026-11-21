#pragma once

// What a device is and what it can do (ADR-0009 rules 10 and 17, ADR-0005).
//
// A device exists only if its GPU meets the ADR-0005 minimum (Vulkan 1.3 with bindless, or
// Direct3D 12 with Shader Model 6.6, Resource Binding Tier 3 and enhanced barriers); otherwise
// device creation fails with ErrorCode::UnsupportedDevice and names every missing feature. The
// optional features below are reported here and enabled per scalability tier by the renderer: no
// feature the Low tier needs may depend on one of them. Calling an optional feature's commands
// on a device without it is asserted.

#include <cstdint>
#include <string>

namespace ith::rhi {

// Which backend a device runs on; reported in Capabilities. Each backend has its own factory
// function (createNullDevice() here; the Direct3D 12 and Vulkan backends are separate modules,
// ADR-0021).
enum class Backend : std::uint8_t {
    D3D12,
    Vulkan,
    // No GPU: objects and frames are bookkept, commands are dropped. For tests, and for the
    // editor or player without a GPU (NullDevice.h).
    Null,
};

// The bytecode a device takes in ShaderDesc::code.
enum class ShaderFormat : std::uint8_t {
    // Any non-empty bytes (the null backend).
    None,
    Spirv,
    Dxil,
};

// Which GPU a device is created on.
enum class AdapterPreference : std::uint8_t {
    // The fastest hardware GPU: a discrete one before an integrated one.
    HighPerformance,
    // The API's software rasterizer: WARP on Direct3D 12, lavapipe on Vulkan. CI runs the
    // functional GPU tests on it (ADR-0005).
    Software,
};

struct AdapterInfo {
    // The driver's name of the GPU.
    std::string name;
    // PCI vendor and device ids; 0 when the API reports none.
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    // Memory dedicated to the GPU, in bytes; 0 for integrated and software GPUs.
    std::uint64_t dedicatedMemory = 0;
    // True for a software rasterizer.
    bool software = false;
};

// Capacities fixed at device creation (ADR-0009 rule 10). Creating an object beyond its type's
// capacity fails with ErrorCode::OutOfHandles, and beyond a descriptor heap's capacity with
// ErrorCode::OutOfDescriptors. A buffer or texture larger than the size limits is a programmer
// error (ErrorCode::InvalidUsage).
//
// The device allocates its handle tables and descriptor free lists for these capacities up
// front, so that they never move while other threads read them: about 6.5 MB with the defaults
// (40 bytes per buffer slot and 36 per texture slot on 64-bit targets, plus 4 bytes per entry
// of the slot and descriptor free lists). Devices with less memory, such as the Steam Deck, may
// lower the per-type capacities. Backends add their own per-slot arrays.
struct Limits {
    // Bindless resource descriptors (buffers and textures), and samplers: the D3D12
    // shader-visible sampler heap's maximum.
    std::uint32_t resourceDescriptors = 262'144;
    std::uint32_t samplerDescriptors = 2'048;
    // Live objects per type, counting objects waiting for deferred destruction. Textures include
    // swapchain images (kMaxSwapchainImages per swapchain).
    std::uint32_t buffers = 65'536;
    std::uint32_t textures = 65'536;
    std::uint32_t samplers = 2'048;
    std::uint32_t pipelines = 4'096;
    std::uint32_t swapchains = 8;
    // Largest width and height of a 2D or cube texture, largest size of a 3D texture, most array
    // layers, and largest buffer in bytes. The defaults are the Direct3D 12 feature level 12_0
    // limits and 4 GiB; backends report their device's values.
    std::uint32_t maxTextureDimension2D = 16'384;
    std::uint32_t maxTextureDimension3D = 2'048;
    std::uint32_t maxTextureLayers = 2'048;
    std::uint64_t maxBufferSize = 4ull << 30;
};

struct Capabilities {
    Backend backend = Backend::Null;
    AdapterInfo adapter;
    ShaderFormat shaderFormat = ShaderFormat::None;
    Limits limits;

    // Optional features (ADR-0005 rule 1).
    bool meshShaders = false;
    bool rayTracing = false;
    bool variableRateShading = false;
    bool samplerFeedback = false;
};

} // namespace ith::rhi
