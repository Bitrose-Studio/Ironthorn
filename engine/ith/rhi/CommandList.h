#pragma once

// Recording GPU commands (ADR-0009 rule 12).
//
//   ith::rhi::CommandList& list = device.openCommandList("Main pass");
//   list.barrier(toRenderTarget);
//   list.beginRendering({.colors = colors});
//   list.setPipeline(pipeline);
//   list.pushConstants(drawConstants);
//   list.draw(3);
//   list.endRendering();
//   list.barrier(toPresent);
//   device.submit(list);
//
// A command list comes from the device's per-frame pool, is recorded by one thread at a time,
// and must be submitted or discarded in the frame it was opened in. Lists may be recorded in
// parallel on different threads. Recording calls return nothing: misuse (a stale handle, a draw
// outside rendering, a missing usage flag) is asserted in Debug and development builds and the
// call is dropped, and real failures (out of memory) surface at Device::submit().

#include "ith/rhi/Barrier.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Handle.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ith::rhi {

namespace detail {
class CommandListBackend;
class DeviceState;
} // namespace detail

enum class LoadOp : std::uint8_t {
    // Keep the attachment's contents.
    Load,
    // Clear the attachment to its clear value.
    Clear,
    // The contents are undefined; every pixel will be written.
    DontCare,
};

enum class StoreOp : std::uint8_t {
    Store,
    // The contents are not needed after rendering (a transient depth buffer).
    DontCare,
};

struct ColorAttachment {
    // A texture with TextureUsage::RenderTarget, in TextureLayout::RenderTarget. Mip 0, layer 0.
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachment {
    // A texture with TextureUsage::DepthStencil, in TextureLayout::DepthStencilWrite or
    // DepthStencilRead; the null handle for none. Mip 0, layer 0.
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    // Reversed depth (ADR-0006): 0 is the far plane.
    float clearDepth = 0.0f;
};

struct RenderingDesc {
    // At most kMaxColorTargets (Pipeline.h). There must be at least one color or depth
    // attachment, and every attachment has the same size.
    std::span<const ColorAttachment> colors;
    DepthAttachment depth;
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class IndexFormat : std::uint8_t {
    Uint16,
    Uint32
};

// The layout of one indexed indirect draw in an argument buffer, the same on both APIs.
struct DrawIndexedIndirectArguments {
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0;
};

// The layout of one indirect dispatch in an argument buffer.
struct DispatchIndirectArguments {
    std::uint32_t groupCountX = 0;
    std::uint32_t groupCountY = 0;
    std::uint32_t groupCountZ = 0;
};

// Row pitch and offset alignment of buffer data copied to or from a texture (the stricter
// Direct3D 12 rules, so the same upload and readback code works on both APIs).
inline constexpr std::uint32_t kTextureCopyRowAlignment = 256;
inline constexpr std::uint32_t kTextureCopyOffsetAlignment = 512;

// A box of texels inside one mip level of a texture, for copies. A zero extent reaches from the
// offset to the end of the level in that dimension, so the default box is the whole level. z and
// depth select slices of a 3D texture; other textures have one slice (z 0, depth 0 or 1).
struct TexelRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
};

// Copies between buffer data and a box of texels of one mip level of one array layer of a
// texture, in either direction (CommandList::copyBufferToTexture(),
// CommandList::copyTextureToBuffer()). The buffer holds the box's rows `rowPitch` bytes apart,
// and the slices of a 3D box one after another.
struct BufferTextureCopy {
    // A buffer with BufferUsage::CopySrc when it is the source, BufferUsage::CopyDst when it is
    // the destination.
    BufferHandle buffer;
    // A multiple of kTextureCopyOffsetAlignment.
    std::uint64_t bufferOffset = 0;
    // Bytes from one row of texels to the next: at least the box's width * texel size and a
    // multiple of kTextureCopyRowAlignment.
    std::uint32_t rowPitch = 0;
    // A texture with TextureUsage::CopyDst in TextureLayout::CopyDst when it is the destination,
    // TextureUsage::CopySrc in TextureLayout::CopySrc when it is the source.
    TextureHandle texture;
    std::uint32_t mipLevel = 0;
    std::uint32_t layer = 0;
    // The texels copied, inside the level; the default is the whole level. Depth formats copy
    // whole levels only, as Direct3D 12 does.
    TexelRegion region;
};

class CommandList {
public:
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    CommandList(CommandList&&) = delete;
    CommandList& operator=(CommandList&&) = delete;
    ~CommandList();

    // The name given to Device::openCommandList().
    [[nodiscard]] std::string_view debugName() const { return m_debugName; }

    // Records barriers (Barrier.h). Preconditions: outside beginRendering()/endRendering();
    // every handle valid; texture ranges inside the texture; layoutAfter is not Undefined; the
    // Present layout only for swapchain images.
    void barrier(const Barriers& barriers);
    void barrier(const GlobalBarrier& barrier);
    void barrier(const BufferBarrier& barrier);
    void barrier(const TextureBarrier& barrier);

    // Starts dynamic rendering into the attachments, and sets the viewport and scissor to their
    // full size. Preconditions: not rendering already; see RenderingDesc.
    void beginRendering(const RenderingDesc& desc);
    // Precondition: rendering.
    void endRendering();

    // Binds a graphics or compute pipeline. Precondition: a valid handle.
    void setPipeline(PipelineHandle pipeline);
    // Precondition: rendering.
    void setViewport(const Viewport& viewport);
    // Precondition: rendering.
    void setScissor(const Rect& scissor);
    // Preconditions: a buffer with BufferUsage::Index; `offset` is a multiple of the index size
    // and at least one index lies inside the buffer from it.
    void setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format);

    // Sets the push constants of the following draws and dispatches. The layout is the same for
    // every pipeline, so they may be set before or after setPipeline(). Preconditions: at most
    // kMaxPushConstantSize bytes, a multiple of 4.
    void pushConstants(std::span<const std::byte> data);

    // Sets a struct as the push constants, under the same preconditions.
    template <typename T>
        requires std::is_trivially_copyable_v<T> && (!std::is_convertible_v<const T&, std::span<const std::byte>>)
    void pushConstants(const T& constants) {
        pushConstants(std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&constants, 1))));
    }

    // Draws with the bound graphics pipeline. Preconditions: rendering; a graphics pipeline is
    // bound.
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0,
              std::uint32_t firstInstance = 0);
    // As draw(), with the bound index buffer (setIndexBuffer()).
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0,
                     std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);
    // GPU-driven indexed draws: up to `maxDraws` DrawIndexedIndirectArguments from `arguments`
    // at `argumentsOffset`, as many as the uint32 in `count` at `countOffset` says.
    // Preconditions: as drawIndexed(); both buffers have BufferUsage::Indirect; offsets are
    // multiples of 4; `maxDraws` arguments fit in `arguments` from `argumentsOffset`, and the
    // count fits in `count` from `countOffset`. Mesh-shader draws come with the renderer's mesh
    // path (ADR-0021).
    void drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset, BufferHandle count,
                                  std::uint64_t countOffset, std::uint32_t maxDraws);

    // Preconditions: not rendering; a compute pipeline is bound.
    void dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY = 1, std::uint32_t groupCountZ = 1);
    // One DispatchIndirectArguments from `arguments` at `offset`. Preconditions: as dispatch();
    // the buffer has BufferUsage::Indirect; `offset` is a multiple of 4 and the arguments fit in
    // the buffer from it.
    void dispatchIndirect(BufferHandle arguments, std::uint64_t offset);

    // Copies `size` bytes. Preconditions: not rendering; different buffers; `source` has
    // BufferUsage::CopySrc and `destination` BufferUsage::CopyDst; both ranges inside their
    // buffers.
    void copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                    std::uint64_t destinationOffset, std::uint64_t size);
    // Copies buffer data into a box of a texture's mip level and layer: a whole level, or a
    // changed rectangle of it (a font atlas update). Preconditions: not rendering; see
    // BufferTextureCopy; the data lies inside the buffer.
    void copyBufferToTexture(const BufferTextureCopy& copy);
    // Copies a box of a texture's mip level and layer into the buffer, laid out as
    // copyBufferToTexture() reads it, e.g. into a Readback buffer to check rendered pixels.
    // Preconditions: as copyBufferToTexture(), with the roles of source and destination swapped.
    void copyTextureToBuffer(const BufferTextureCopy& copy);

    // Opens and closes a named region for GPU debuggers and profilers. Markers nest, and every
    // marker is closed before the list is submitted.
    void beginMarker(std::string_view name);
    // Precondition: a marker is open.
    void endMarker();

    // Only the device creates command lists; the key keeps the constructor out of reach.
    class Key {
        friend class detail::DeviceState;
        Key() = default;
    };

    CommandList(Key key, detail::DeviceState& device);

private:
    friend class detail::DeviceState;

    enum class State : std::uint8_t {
        Closed,
        Open,
        Failed
    };
    enum class BoundPipeline : std::uint8_t {
        None,
        Graphics,
        Compute
    };

    // Asserts that the list is open and belongs to the open frame; false when it is closed or
    // failed, so the call is dropped.
    bool recording() const;

    detail::DeviceState* m_device;
    std::unique_ptr<detail::CommandListBackend> m_backend;
    std::string m_debugName;
    // The error of a list whose backend could not start recording; reported by submit().
    Error m_error;
    // Atomic because endFrame() closes lists left open while their thread may still look.
    std::atomic<State> m_state{State::Closed};
    // The frame the list was opened in, for the checks.
    std::uint64_t m_frameNumber = 0;
    std::uint32_t m_frameSlot = 0;
    // Recording state for the checks.
    bool m_rendering = false;
    bool m_indexBufferSet = false;
    BoundPipeline m_pipeline = BoundPipeline::None;
    std::uint32_t m_markerDepth = 0;
};

} // namespace ith::rhi
