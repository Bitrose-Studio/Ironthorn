#include "ith/rhi/CommandList.h"

#include "ith/foundation/Assert.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/DeviceState.h"
#include "ith/rhi/Pipeline.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ith::rhi {
namespace {

// Asserts a recording rule; the caller drops the call when it returns false.
bool check(bool condition, [[maybe_unused]] const char* message) {
    ITH_ASSERT_MSG(condition, message);
    return condition;
}

[[maybe_unused]] std::uint32_t mipSize(std::uint32_t size, std::uint32_t mip) {
    return std::max(1u, size >> mip);
}

[[maybe_unused]] bool rangeInside(const SubresourceRange& range, const detail::TextureSlot& texture) {
    const std::uint32_t layers = texture.dimension == TextureDimension::Texture3D ? 1 : texture.layers;
    const bool mipsInside = range.baseMip < texture.mipLevels &&
                            (range.mipCount == SubresourceRange::kAllRemaining ||
                             (range.mipCount != 0 && range.mipCount <= texture.mipLevels - range.baseMip));
    const bool layersInside =
        range.baseLayer < layers && (range.layerCount == SubresourceRange::kAllRemaining ||
                                     (range.layerCount != 0 && range.layerCount <= layers - range.baseLayer));
    return mipsInside && layersInside;
}

[[maybe_unused]] bool bufferRangeInside(const detail::BufferSlot& buffer, std::uint64_t offset, std::uint64_t size) {
    return offset <= buffer.size && size <= buffer.size - offset;
}

} // namespace

CommandList::CommandList(Key /*key*/, detail::DeviceState& device) : m_device(&device) {}

CommandList::~CommandList() = default;

bool CommandList::recording() const {
    const State state = m_state.load(std::memory_order_acquire);
    ITH_ASSERT_MSG(state != State::Closed, "recording into a command list that was submitted or discarded");
    ITH_ASSERT_MSG(state != State::Open || m_frameNumber == m_device->frameNumber(),
                   "recording into a command list of an earlier frame");
    return state == State::Open;
}

// A buffer's data through a live handle, for the checks; nullptr for a stale or null handle.
// The handle stays live while the list records with it (Device.h).
namespace {
[[maybe_unused]] const detail::BufferSlot* liveBuffer(const detail::DeviceState& device, BufferHandle buffer) {
    return device.buffers.isLive(buffer) ? &device.buffers.data(buffer.index()) : nullptr;
}
} // namespace

void CommandList::barrier(const Barriers& barriers) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(!m_rendering, "barriers are recorded outside beginRendering()/endRendering()")) {
            return;
        }
        for (const BufferBarrier& buffer : barriers.buffers) {
            if (!check(m_device->buffers.isLive(buffer.buffer), "a barrier with a stale or null buffer handle")) {
                return;
            }
        }
        for (const TextureBarrier& texture : barriers.textures) {
            if (!check(m_device->textures.isLive(texture.texture), "a barrier with a stale or null texture handle")) {
                return;
            }
            const detail::TextureSlot& slot = m_device->textures.data(texture.texture.index());
            if (!check(texture.layoutAfter != TextureLayout::Undefined, "a barrier's layoutAfter can't be Undefined") ||
                !check(slot.swapchainImage || (texture.layoutBefore != TextureLayout::Present &&
                                               texture.layoutAfter != TextureLayout::Present),
                       "only swapchain images use the Present layout") ||
                !check(rangeInside(texture.range, slot), "a texture barrier's range is outside the texture")) {
                return;
            }
        }
    }
    m_backend->barrier(barriers);
}

void CommandList::barrier(const GlobalBarrier& barrier) {
    this->barrier(Barriers{.global = std::span<const GlobalBarrier>(&barrier, 1)});
}

void CommandList::barrier(const BufferBarrier& barrier) {
    this->barrier(Barriers{.buffers = std::span<const BufferBarrier>(&barrier, 1)});
}

void CommandList::barrier(const TextureBarrier& barrier) {
    this->barrier(Barriers{.textures = std::span<const TextureBarrier>(&barrier, 1)});
}

void CommandList::beginRendering(const RenderingDesc& desc) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(!m_rendering, "beginRendering() while rendering") ||
            !check(desc.colors.size() <= kMaxColorTargets, "more than kMaxColorTargets color attachments") ||
            !check(!desc.colors.empty() || !desc.depth.texture.isNull(), "rendering needs at least one attachment")) {
            return;
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const auto sameSize = [&](const detail::TextureSlot& slot) {
            if (width == 0) {
                width = slot.width;
                height = slot.height;
            }
            return slot.width == width && slot.height == height;
        };
        for (const ColorAttachment& color : desc.colors) {
            if (!check(m_device->textures.isLive(color.texture), "a color attachment with a stale or null handle")) {
                return;
            }
            const detail::TextureSlot& slot = m_device->textures.data(color.texture.index());
            if (!check(hasAny(slot.usage, TextureUsage::RenderTarget), "a color attachment needs RenderTarget usage") ||
                !check(sameSize(slot), "every attachment has the same size")) {
                return;
            }
        }
        if (!desc.depth.texture.isNull()) {
            if (!check(m_device->textures.isLive(desc.depth.texture), "a depth attachment with a stale handle")) {
                return;
            }
            const detail::TextureSlot& slot = m_device->textures.data(desc.depth.texture.index());
            if (!check(hasAny(slot.usage, TextureUsage::DepthStencil), "a depth attachment needs DepthStencil usage") ||
                !check(sameSize(slot), "every attachment has the same size")) {
                return;
            }
        }
    }
    m_rendering = true;
    m_backend->beginRendering(desc);
}

void CommandList::endRendering() {
    if (!recording() || !check(m_rendering, "endRendering() without beginRendering()")) {
        return;
    }
    m_rendering = false;
    m_backend->endRendering();
}

void CommandList::setPipeline(PipelineHandle pipeline) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(m_device->pipelines.isLive(pipeline), "setPipeline() with a stale or null handle")) {
            m_pipeline = BoundPipeline::None;
            return;
        }
        switch (m_device->pipelines.data(pipeline.index()).kind) {
            case detail::PipelineKind::Graphics:
                m_pipeline = BoundPipeline::Graphics;
                break;
            case detail::PipelineKind::Compute:
                m_pipeline = BoundPipeline::Compute;
                break;
        }
    }
    m_backend->setPipeline(pipeline);
}

void CommandList::setViewport(const Viewport& viewport) {
    if (!recording() || !check(m_rendering, "setViewport() outside rendering")) {
        return;
    }
    m_backend->setViewport(viewport);
}

void CommandList::setScissor(const Rect& scissor) {
    if (!recording() || !check(m_rendering, "setScissor() outside rendering")) {
        return;
    }
    m_backend->setScissor(scissor);
}

void CommandList::setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        const std::uint64_t indexSize = format == IndexFormat::Uint16 ? 2 : 4;
        const detail::BufferSlot* slot = liveBuffer(*m_device, buffer);
        if (!check(slot != nullptr, "setIndexBuffer() with a stale or null handle") ||
            !check(hasAny(slot->usage, BufferUsage::Index), "an index buffer needs Index usage") ||
            !check(offset % indexSize == 0, "an index buffer offset is a multiple of the index size") ||
            !check(bufferRangeInside(*slot, offset, indexSize), "an index buffer offset lies inside the buffer")) {
            return;
        }
    }
    m_indexBufferSet = true;
    m_backend->setIndexBuffer(buffer, offset, format);
}

void CommandList::pushConstants(std::span<const std::byte> data) {
    if (!recording() || !check(data.size() <= kMaxPushConstantSize && data.size() % 4 == 0,
                               "push constants are at most kMaxPushConstantSize bytes, a multiple of 4")) {
        return;
    }
    m_backend->pushConstants(data);
}

void CommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                       std::uint32_t firstInstance) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(m_rendering, "draw() outside rendering") ||
            !check(m_pipeline == BoundPipeline::Graphics, "draw() needs a graphics pipeline")) {
            return;
        }
    }
    m_backend->draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

void CommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                              std::int32_t vertexOffset, std::uint32_t firstInstance) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(m_rendering, "drawIndexed() outside rendering") ||
            !check(m_pipeline == BoundPipeline::Graphics, "drawIndexed() needs a graphics pipeline") ||
            !check(m_indexBufferSet, "drawIndexed() needs an index buffer")) {
            return;
        }
    }
    m_backend->drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void CommandList::drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset, BufferHandle count,
                                           std::uint64_t countOffset, std::uint32_t maxDraws) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        const detail::BufferSlot* argumentSlot = liveBuffer(*m_device, arguments);
        const detail::BufferSlot* countSlot = liveBuffer(*m_device, count);
        const auto indirect = [](const detail::BufferSlot* slot) {
            return slot != nullptr && hasAny(slot->usage, BufferUsage::Indirect);
        };
        if (!check(m_rendering, "drawIndexedIndirectCount() outside rendering") ||
            !check(m_pipeline == BoundPipeline::Graphics, "indirect draws need a graphics pipeline") ||
            !check(m_indexBufferSet, "drawIndexedIndirectCount() needs an index buffer") ||
            !check(indirect(argumentSlot) && indirect(countSlot),
                   "indirect buffers are valid and have Indirect usage") ||
            !check(argumentsOffset % 4 == 0 && countOffset % 4 == 0, "indirect offsets are multiples of 4") ||
            !check(bufferRangeInside(*argumentSlot, argumentsOffset,
                                     std::uint64_t{maxDraws} * sizeof(DrawIndexedIndirectArguments)),
                   "the indirect arguments lie inside their buffer") ||
            !check(bufferRangeInside(*countSlot, countOffset, sizeof(std::uint32_t)),
                   "the indirect count lies inside its buffer")) {
            return;
        }
    }
    m_backend->drawIndexedIndirectCount(arguments, argumentsOffset, count, countOffset, maxDraws);
}

void CommandList::dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(!m_rendering, "dispatch() inside rendering") ||
            !check(m_pipeline == BoundPipeline::Compute, "dispatch() needs a compute pipeline")) {
            return;
        }
    }
    m_backend->dispatch(groupCountX, groupCountY, groupCountZ);
}

void CommandList::dispatchIndirect(BufferHandle arguments, std::uint64_t offset) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        const detail::BufferSlot* slot = liveBuffer(*m_device, arguments);
        if (!check(!m_rendering, "dispatchIndirect() inside rendering") ||
            !check(m_pipeline == BoundPipeline::Compute, "dispatchIndirect() needs a compute pipeline") ||
            !check(slot != nullptr && hasAny(slot->usage, BufferUsage::Indirect),
                   "the indirect buffer is valid and has Indirect usage") ||
            !check(offset % 4 == 0, "indirect offsets are multiples of 4") ||
            !check(bufferRangeInside(*slot, offset, sizeof(DispatchIndirectArguments)),
                   "the indirect arguments lie inside their buffer")) {
            return;
        }
    }
    m_backend->dispatchIndirect(arguments, offset);
}

void CommandList::copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                             std::uint64_t destinationOffset, std::uint64_t size) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(!m_rendering, "copies are recorded outside rendering") ||
            !check(m_device->buffers.isLive(source) && m_device->buffers.isLive(destination),
                   "copyBuffer() with a stale or null handle") ||
            !check(source != destination, "copyBuffer() between different buffers")) {
            return;
        }
        const detail::BufferSlot& from = m_device->buffers.data(source.index());
        const detail::BufferSlot& to = m_device->buffers.data(destination.index());
        if (!check(hasAny(from.usage, BufferUsage::CopySrc) && hasAny(to.usage, BufferUsage::CopyDst),
                   "copyBuffer() needs CopySrc on the source and CopyDst on the destination") ||
            !check(bufferRangeInside(from, sourceOffset, size) && bufferRangeInside(to, destinationOffset, size),
                   "copyBuffer() ranges lie inside their buffers")) {
            return;
        }
    }
    m_backend->copyBuffer(source, sourceOffset, destination, destinationOffset, size);
}

void CommandList::copyBufferToTexture(const BufferTextureCopy& copy) {
    if (!recording()) {
        return;
    }
    if constexpr (detail::kCheckRecording) {
        if (!check(!m_rendering, "copies are recorded outside rendering") ||
            !check(m_device->buffers.isLive(copy.buffer) && m_device->textures.isLive(copy.texture),
                   "copyBufferToTexture() with a stale or null handle")) {
            return;
        }
        const detail::BufferSlot& buffer = m_device->buffers.data(copy.buffer.index());
        const detail::TextureSlot& texture = m_device->textures.data(copy.texture.index());
        const std::uint32_t layers = texture.dimension == TextureDimension::Texture3D ? 1 : texture.layers;
        if (!check(hasAny(buffer.usage, BufferUsage::CopySrc) && hasAny(texture.usage, TextureUsage::CopyDst),
                   "copyBufferToTexture() needs CopySrc on the buffer and CopyDst on the texture") ||
            !check(copy.mipLevel < texture.mipLevels && copy.layer < layers,
                   "copyBufferToTexture() names a mip level and layer of the texture")) {
            return;
        }
        const std::uint64_t rowBytes =
            static_cast<std::uint64_t>(mipSize(texture.width, copy.mipLevel)) * formatTexelSize(texture.format);
        const std::uint64_t rows =
            static_cast<std::uint64_t>(mipSize(texture.height, copy.mipLevel)) *
            (texture.dimension == TextureDimension::Texture3D ? mipSize(texture.depth, copy.mipLevel) : 1);
        if (!check(copy.rowPitch >= rowBytes && copy.rowPitch % kTextureCopyRowAlignment == 0,
                   "a texture copy's row pitch covers a row and is a multiple of kTextureCopyRowAlignment") ||
            !check(copy.bufferOffset % kTextureCopyOffsetAlignment == 0,
                   "a texture copy's buffer offset is a multiple of kTextureCopyOffsetAlignment") ||
            !check(bufferRangeInside(buffer, copy.bufferOffset, copy.rowPitch * (rows - 1) + rowBytes),
                   "a texture copy's data lies inside the buffer")) {
            return;
        }
    }
    m_backend->copyBufferToTexture(copy);
}

void CommandList::beginMarker(std::string_view name) {
    if (!recording()) {
        return;
    }
    ++m_markerDepth;
    m_backend->beginMarker(name);
}

void CommandList::endMarker() {
    if (!recording() || !check(m_markerDepth > 0, "endMarker() without beginMarker()")) {
        return;
    }
    --m_markerDepth;
    m_backend->endMarker();
}

} // namespace ith::rhi
