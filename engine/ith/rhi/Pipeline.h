#pragma once

// Shaders and pipelines (ADR-0009 rules 9 and 16, ADR-0011).
//
// The RHI takes compiled shaders: SPIR-V on Vulkan, DXIL on Direct3D 12
// (Capabilities::shaderFormat says which), each with the reflection data the shader pipeline
// stores next to it. Shaders reach resources only through bindless indices, and per-draw data
// travels in at most kMaxPushConstantSize bytes of push constants. Rendering is dynamic: a
// graphics pipeline names the formats of its attachments instead of a render-pass object.

#include "ith/rhi/Format.h"
#include "ith/rhi/Resources.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ith::rhi {

// The most bytes of push constants (Vulkan) or root constants (Direct3D 12) a draw or dispatch
// gets (ADR-0009 rule 9). Larger data goes in a buffer whose index travels in the constants.
inline constexpr std::uint32_t kMaxPushConstantSize = 128;

// The most color attachments of a pipeline and of CommandList::beginRendering().
inline constexpr std::uint32_t kMaxColorTargets = 8;

enum class ShaderStage : std::uint8_t {
    Vertex,
    Pixel,
    Compute,
};

// One compiled entry point.
struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    // The bytecode in the device's Capabilities::shaderFormat. Must not be empty. Read during the
    // create call only.
    std::span<const std::byte> code;
    // The entry point's name in the bytecode. Must not be empty.
    std::string_view entryPoint = "main";
    // Bytes of push constants the shader declares, from the shader's reflection data. At most
    // kMaxPushConstantSize; a larger value fails with ErrorCode::InvalidShader.
    std::uint32_t constantsSize = 0;
};

enum class PrimitiveTopology : std::uint8_t {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList
};

enum class CullMode : std::uint8_t {
    None,
    Front,
    Back
};

// Counter-clockwise triangles face the viewer in the engine's right-handed space (ADR-0006).
enum class FrontFace : std::uint8_t {
    CounterClockwise,
    Clockwise
};

// How a color target blends; one of a few fixed modes until a pass needs more.
enum class BlendMode : std::uint8_t {
    // The output replaces the target.
    Opaque,
    // Straight alpha: src * a + dst * (1 - a).
    Alpha,
    // Premultiplied alpha: src + dst * (1 - a).
    Premultiplied,
    // src + dst.
    Additive,
};

struct ColorTarget {
    // Must not be Unknown or a depth format. An integer format (isIntegerFormat()) is never blended:
    // only Opaque.
    Format format = Format::Unknown;
    BlendMode blend = BlendMode::Opaque;
};

struct DepthState {
    bool test = false;
    bool write = false;
    // Reversed depth (ADR-0006): nearer is greater.
    CompareOp compare = CompareOp::Greater;
};

struct GraphicsPipelineDesc {
    // Exactly one Vertex shader and at most one Pixel shader (none for depth-only passes).
    std::span<const ShaderDesc> shaders;
    // At most kMaxColorTargets.
    std::span<const ColorTarget> colorTargets;
    // Unknown when the pipeline has no depth attachment; otherwise a depth format.
    Format depthFormat = Format::Unknown;
    DepthState depth;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    // Must not be empty.
    std::string_view debugName;
};

struct ComputePipelineDesc {
    // A Compute shader.
    ShaderDesc shader;
    // Must not be empty.
    std::string_view debugName;
};

} // namespace ith::rhi
