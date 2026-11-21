#pragma once

// Explicit barriers (ADR-0009 rule 11).
//
// The RHI does not track resource state. A barrier names the work before it and the work after
// it as pipeline stages and accesses, and for textures the layout before and after, which maps
// directly onto Direct3D 12 enhanced barriers and Vulkan synchronization2. The render graph
// (ith/render_graph, ADR-0028) computes them for its passes; code outside a graph declares them
// by hand:
//
//   const ith::rhi::TextureBarrier toTarget{
//       .texture = frame.backbuffer,
//       .stagesBefore = PipelineStage::None,          .accessBefore = Access::None,
//       .stagesAfter = PipelineStage::ColorOutput,    .accessAfter = Access::RenderTargetWrite,
//       .layoutBefore = TextureLayout::Undefined,     .layoutAfter = TextureLayout::RenderTarget,
//   };
//   list.barrier(toTarget);

#include "ith/rhi/Flags.h"
#include "ith/rhi/Handle.h"

#include <cstdint>
#include <span>

namespace ith::rhi {

// Pipeline stages that work before or after a barrier runs in.
enum class PipelineStage : std::uint16_t {
    None = 0,
    // Reading indirect arguments and counts.
    DrawIndirect = 1u << 0,
    // Reading the index buffer.
    IndexInput = 1u << 1,
    VertexShader = 1u << 2,
    PixelShader = 1u << 3,
    // Depth and stencil tests and writes.
    DepthStencil = 1u << 4,
    // Color attachment reads, blending and writes.
    ColorOutput = 1u << 5,
    ComputeShader = 1u << 6,
    Copy = 1u << 7,
    // Every stage; simple and slow, for rare barriers.
    AllCommands = 1u << 8,
};
template <>
inline constexpr bool kIsFlags<PipelineStage> = true;

// Kinds of memory access before or after a barrier.
enum class Access : std::uint16_t {
    None = 0,
    IndirectArgument = 1u << 0,
    IndexBuffer = 1u << 1,
    // Shader reads through a read index, a sampled index or a storage index.
    ShaderRead = 1u << 2,
    // Shader writes through a write index or a storage index.
    ShaderWrite = 1u << 3,
    RenderTargetRead = 1u << 4,
    RenderTargetWrite = 1u << 5,
    DepthStencilRead = 1u << 6,
    DepthStencilWrite = 1u << 7,
    CopyRead = 1u << 8,
    CopyWrite = 1u << 9,
};
template <>
inline constexpr bool kIsFlags<Access> = true;

// The memory layout of a texture's subresources.
enum class TextureLayout : std::uint8_t {
    // The contents are discarded. Only as layoutBefore. A barrier out of it still orders the
    // texture's earlier use: name that use's stages and accesses before (see TextureBarrier).
    Undefined,
    // Any access; slow, for rare cases.
    General,
    RenderTarget,
    DepthStencilWrite,
    DepthStencilRead,
    ShaderRead,
    // Storage (unordered) access in shaders.
    ShaderWrite,
    CopySrc,
    CopyDst,
    // Ready to present; only for swapchain images.
    Present,
};

// Orders all memory accesses of the given stages, for every resource.
struct GlobalBarrier {
    PipelineStage stagesBefore = PipelineStage::None;
    Access accessBefore = Access::None;
    PipelineStage stagesAfter = PipelineStage::None;
    Access accessAfter = Access::None;
};

// Orders accesses to the whole of one buffer.
struct BufferBarrier {
    BufferHandle buffer;
    PipelineStage stagesBefore = PipelineStage::None;
    Access accessBefore = Access::None;
    PipelineStage stagesAfter = PipelineStage::None;
    Access accessAfter = Access::None;
};

// A range of a texture's mip levels and array layers.
struct SubresourceRange {
    std::uint32_t baseMip = 0;
    // kAllRemaining covers every mip from baseMip on.
    std::uint32_t mipCount = 0xFFFF'FFFFu;
    std::uint32_t baseLayer = 0;
    // kAllRemaining covers every layer from baseLayer on.
    std::uint32_t layerCount = 0xFFFF'FFFFu;

    static constexpr std::uint32_t kAllRemaining = 0xFFFF'FFFFu;
};

// Orders accesses to a range of one texture and changes its layout.
//
// Out of TextureLayout::Undefined the contents are discarded, but the layout change still writes
// the texture, so it must come after the texture's earlier use: name that use's stages before,
// and its accesses before where it wrote, e.g. DepthStencil and DepthStencilWrite for a depth
// buffer reused every frame, or only Copy for a target last read by a copy. Vulkan needs the
// written access to order the change (synchronization validation reports a write-after-write
// hazard without it); Direct3D 12 waits for the stages and drops the access, which it refuses out
// of an undefined layout. A texture not used before, or a swapchain image (acquiring it orders it
// after its presentation), needs neither.
struct TextureBarrier {
    TextureHandle texture;
    PipelineStage stagesBefore = PipelineStage::None;
    Access accessBefore = Access::None;
    PipelineStage stagesAfter = PipelineStage::None;
    Access accessAfter = Access::None;
    TextureLayout layoutBefore = TextureLayout::Undefined;
    // Must not be Undefined.
    TextureLayout layoutAfter = TextureLayout::General;
    SubresourceRange range;
};

// A batch of barriers recorded together, which lets the backend issue them in one call.
struct Barriers {
    std::span<const GlobalBarrier> global;
    std::span<const BufferBarrier> buffers;
    std::span<const TextureBarrier> textures;
};

} // namespace ith::rhi
