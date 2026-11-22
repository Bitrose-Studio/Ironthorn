#include "ith/foundation/Assert.h"
#include "ith/rhi/Barrier.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi_d3d12/D3D12Backend.h"

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace ith::rhi::d3d12 {
namespace {

// Barrier.h's stages and accesses map one to one onto enhanced barriers' syncs and accesses.

D3D12_BARRIER_SYNC sync(PipelineStage stages) {
    if (hasAny(stages, PipelineStage::AllCommands)) {
        return D3D12_BARRIER_SYNC_ALL;
    }
    D3D12_BARRIER_SYNC result = D3D12_BARRIER_SYNC_NONE;
    const auto add = [&](PipelineStage stage, D3D12_BARRIER_SYNC bit) {
        if (hasAny(stages, stage)) {
            result |= bit;
        }
    };
    add(PipelineStage::DrawIndirect, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
    add(PipelineStage::IndexInput, D3D12_BARRIER_SYNC_INDEX_INPUT);
    add(PipelineStage::VertexShader, D3D12_BARRIER_SYNC_VERTEX_SHADING);
    add(PipelineStage::PixelShader, D3D12_BARRIER_SYNC_PIXEL_SHADING);
    add(PipelineStage::DepthStencil, D3D12_BARRIER_SYNC_DEPTH_STENCIL);
    add(PipelineStage::ColorOutput, D3D12_BARRIER_SYNC_RENDER_TARGET);
    add(PipelineStage::ComputeShader, D3D12_BARRIER_SYNC_COMPUTE_SHADING);
    add(PipelineStage::Copy, D3D12_BARRIER_SYNC_COPY);
    return result;
}

// Which views a shader read may go through: Direct3D 12 tells reads through shader-resource
// views (read and sampled indices) from reads through unordered-access views (storage indices).
struct ShaderReads {
    bool resourceView = true;
    bool unorderedView = false;
};

D3D12_BARRIER_ACCESS access(Access accesses, D3D12_BARRIER_SYNC syncs, ShaderReads reads) {
    // No stages, no access: an access without a sync is invalid.
    if (accesses == Access::None || syncs == D3D12_BARRIER_SYNC_NONE) {
        return D3D12_BARRIER_ACCESS_NO_ACCESS;
    }
    D3D12_BARRIER_ACCESS result = D3D12_BARRIER_ACCESS_COMMON;
    const auto add = [&](Access access, D3D12_BARRIER_ACCESS bit) {
        if (hasAny(accesses, access)) {
            result |= bit;
        }
    };
    add(Access::IndirectArgument, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    add(Access::IndexBuffer, D3D12_BARRIER_ACCESS_INDEX_BUFFER);
    if (hasAny(accesses, Access::ShaderRead)) {
        if (reads.resourceView) {
            result |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        }
        if (reads.unorderedView) {
            result |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        }
    }
    add(Access::ShaderWrite, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    add(Access::RenderTargetRead | Access::RenderTargetWrite, D3D12_BARRIER_ACCESS_RENDER_TARGET);
    add(Access::DepthStencilRead, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
    add(Access::DepthStencilWrite, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
    add(Access::CopyRead, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    add(Access::CopyWrite, D3D12_BARRIER_ACCESS_COPY_DEST);
    return result == D3D12_BARRIER_ACCESS_COMMON ? D3D12_BARRIER_ACCESS_NO_ACCESS : result;
}

D3D12_BARRIER_LAYOUT layout(TextureLayout textureLayout) {
    switch (textureLayout) {
        case TextureLayout::Undefined:
            return D3D12_BARRIER_LAYOUT_UNDEFINED;
        case TextureLayout::General:
            // The one queue is a direct queue; this layout also allows unordered access.
            return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON;
        case TextureLayout::RenderTarget:
            return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        case TextureLayout::DepthStencilWrite:
            return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        case TextureLayout::DepthStencilRead:
            return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
        case TextureLayout::ShaderRead:
            return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        case TextureLayout::ShaderWrite:
            return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        case TextureLayout::CopySrc:
            return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        case TextureLayout::CopyDst:
            return D3D12_BARRIER_LAYOUT_COPY_DEST;
        case TextureLayout::Present:
            return D3D12_BARRIER_LAYOUT_PRESENT;
    }
    return D3D12_BARRIER_LAYOUT_COMMON;
}

// The shader reads a texture layout allows.
ShaderReads textureReads(TextureLayout textureLayout, const TextureRecord& texture) {
    const bool storage = hasAny(texture.usage, TextureUsage::Storage);
    switch (textureLayout) {
        case TextureLayout::General:
            return {.resourceView = hasAny(texture.usage, TextureUsage::Sampled), .unorderedView = storage};
        case TextureLayout::ShaderWrite:
            return {.resourceView = false, .unorderedView = storage};
        default:
            return {.resourceView = true, .unorderedView = false};
    }
}

std::uint32_t mipSize(std::uint32_t size, std::uint32_t mip) {
    return std::max(1u, size >> mip);
}

} // namespace

D3D12CommandList::D3D12CommandList(D3D12Backend& backend, ComPtr<ID3D12CommandAllocator> allocator,
                                   ComPtr<ID3D12GraphicsCommandList7> list)
    : m_backend(&backend), m_allocator(std::move(allocator)), m_list(std::move(list)) {}

Expected<void, Error> D3D12CommandList::begin(std::uint32_t /*frameSlot*/, std::string_view debugName) {
    // A list left recording (discarded, or its batch failed) is closed before its allocator is
    // reset (Backend.h).
    static_cast<void>(close());
    if (FAILED(m_allocator->Reset()) || FAILED(m_list->Reset(m_allocator.Get(), nullptr))) {
        return makeUnexpected(Error{.code = ErrorCode::OutOfMemory,
                                    .message = "Out of memory starting command list '" + std::string(debugName) + "'"});
    }
    m_recording = true;
    m_list->SetName(widen(debugName).c_str());
    const std::array<ID3D12DescriptorHeap*, 2> heaps = m_backend->shaderVisibleHeaps();
    m_list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
    m_list->SetGraphicsRootSignature(m_backend->rootSignature());
    m_list->SetComputeRootSignature(m_backend->rootSignature());
    return {};
}

HRESULT D3D12CommandList::close() {
    if (!m_recording) {
        return S_OK;
    }
    m_recording = false;
    return m_list->Close();
}

void D3D12CommandList::barrier(const Barriers& barriers) {
    m_globalBarriers.clear();
    m_bufferBarriers.clear();
    m_textureBarriers.clear();
    for (const GlobalBarrier& global : barriers.global) {
        const D3D12_BARRIER_SYNC before = sync(global.stagesBefore);
        const D3D12_BARRIER_SYNC after = sync(global.stagesAfter);
        const ShaderReads anyView{.resourceView = true, .unorderedView = true};
        m_globalBarriers.push_back(D3D12_GLOBAL_BARRIER{.SyncBefore = before,
                                                        .SyncAfter = after,
                                                        .AccessBefore = access(global.accessBefore, before, anyView),
                                                        .AccessAfter = access(global.accessAfter, after, anyView)});
    }
    for (const BufferBarrier& buffer : barriers.buffers) {
        const BufferRecord& record = m_backend->buffer(buffer.buffer);
        const D3D12_BARRIER_SYNC before = sync(buffer.stagesBefore);
        const D3D12_BARRIER_SYNC after = sync(buffer.stagesAfter);
        const ShaderReads reads{.resourceView = true, .unorderedView = record.shaderWrite};
        m_bufferBarriers.push_back(D3D12_BUFFER_BARRIER{.SyncBefore = before,
                                                        .SyncAfter = after,
                                                        .AccessBefore = access(buffer.accessBefore, before, reads),
                                                        .AccessAfter = access(buffer.accessAfter, after, reads),
                                                        .pResource = record.resource,
                                                        .Offset = 0,
                                                        .Size = UINT64_MAX});
    }
    for (const TextureBarrier& texture : barriers.textures) {
        const TextureRecord& record = m_backend->texture(texture.texture);
        const SubresourceRange& range = texture.range;
        const D3D12_BARRIER_SYNC before = sync(texture.stagesBefore);
        const D3D12_BARRIER_SYNC after = sync(texture.stagesAfter);
        D3D12_TEXTURE_BARRIER native{};
        native.SyncBefore = before;
        native.SyncAfter = after;
        // Direct3D 12 refuses an access before a barrier out of the undefined layout: the contents
        // are discarded, so nothing needs flushing, and SyncBefore still waits for the earlier
        // work. Vulkan needs that access to order the layout change after earlier writes, so
        // callers name it, as for a depth buffer reused every frame.
        native.AccessBefore = texture.layoutBefore == TextureLayout::Undefined
                                  ? D3D12_BARRIER_ACCESS_NO_ACCESS
                                  : access(texture.accessBefore, before, textureReads(texture.layoutBefore, record));
        native.AccessAfter = access(texture.accessAfter, after, textureReads(texture.layoutAfter, record));
        native.LayoutBefore = layout(texture.layoutBefore);
        native.LayoutAfter = layout(texture.layoutAfter);
        native.pResource = record.resource.Get();
        native.Subresources.IndexOrFirstMipLevel = range.baseMip;
        native.Subresources.NumMipLevels =
            range.mipCount == SubresourceRange::kAllRemaining ? record.mipLevels - range.baseMip : range.mipCount;
        native.Subresources.FirstArraySlice = range.baseLayer;
        native.Subresources.NumArraySlices =
            range.layerCount == SubresourceRange::kAllRemaining ? record.layers - range.baseLayer : range.layerCount;
        native.Subresources.FirstPlane = 0;
        native.Subresources.NumPlanes = record.planes;
        // A render target or depth buffer coming from Undefined is discarded: its memory may be placed
        // in a heap and hold anything, and Direct3D 12 requires it initialized before rendering.
        const bool toAttachment = texture.layoutAfter == TextureLayout::RenderTarget ||
                                  texture.layoutAfter == TextureLayout::DepthStencilWrite;
        native.Flags = texture.layoutBefore == TextureLayout::Undefined && toAttachment
                           ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                           : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        m_textureBarriers.push_back(native);
    }

    std::array<D3D12_BARRIER_GROUP, 3> groups{};
    UINT count = 0;
    if (!m_globalBarriers.empty()) {
        groups[count].Type = D3D12_BARRIER_TYPE_GLOBAL;
        groups[count].NumBarriers = static_cast<UINT32>(m_globalBarriers.size());
        groups[count].pGlobalBarriers = m_globalBarriers.data();
        ++count;
    }
    if (!m_bufferBarriers.empty()) {
        groups[count].Type = D3D12_BARRIER_TYPE_BUFFER;
        groups[count].NumBarriers = static_cast<UINT32>(m_bufferBarriers.size());
        groups[count].pBufferBarriers = m_bufferBarriers.data();
        ++count;
    }
    if (!m_textureBarriers.empty()) {
        groups[count].Type = D3D12_BARRIER_TYPE_TEXTURE;
        groups[count].NumBarriers = static_cast<UINT32>(m_textureBarriers.size());
        groups[count].pTextureBarriers = m_textureBarriers.data();
        ++count;
    }
    if (count > 0) {
        m_list->Barrier(count, groups.data());
    }
}

void D3D12CommandList::beginRendering(const RenderingDesc& desc) {
    // Render-target views and clears; DontCare loads keep the contents, which is allowed.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxColorTargets> colors{};
    // The device checks the count in Debug and development builds; the clamp keeps Shipping
    // builds inside `colors` too.
    ITH_ASSERT(desc.colors.size() <= kMaxColorTargets);
    const std::size_t colorCount = std::min<std::size_t>(desc.colors.size(), kMaxColorTargets);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const ColorAttachment& color = desc.colors[index];
        colors[index] = m_backend->renderTargetView(color.texture.index());
        if (color.load == LoadOp::Clear) {
            m_list->ClearRenderTargetView(colors[index], color.clearColor.data(), 0, nullptr);
        }
    }
    const bool hasDepth = !desc.depth.texture.isNull();
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    if (hasDepth) {
        depth = m_backend->depthStencilView(desc.depth.texture.index());
        if (desc.depth.load == LoadOp::Clear) {
            const bool stencil = m_backend->texture(desc.depth.texture).planes == 2;
            const auto flags =
                static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
            m_list->ClearDepthStencilView(depth, flags, desc.depth.clearDepth, 0, 0, nullptr);
        }
    }
    m_list->OMSetRenderTargets(static_cast<UINT>(colorCount), colors.data(), FALSE, hasDepth ? &depth : nullptr);

    const TextureRecord& first = m_backend->texture(desc.colors.empty() ? desc.depth.texture : desc.colors[0].texture);
    setViewport({.width = static_cast<float>(first.width), .height = static_cast<float>(first.height)});
    setScissor({.width = first.width, .height = first.height});
}

void D3D12CommandList::endRendering() {
    // Direct3D 12 has nothing to end: the next beginRendering() sets new targets.
}

void D3D12CommandList::setPipeline(PipelineHandle pipeline) {
    const PipelineRecord& record = m_backend->pipeline(pipeline);
    m_list->SetPipelineState(record.state.Get());
    if (!record.compute) {
        m_list->IASetPrimitiveTopology(record.topology);
    }
}

void D3D12CommandList::setViewport(const Viewport& viewport) {
    const D3D12_VIEWPORT native{.TopLeftX = viewport.x,
                                .TopLeftY = viewport.y,
                                .Width = viewport.width,
                                .Height = viewport.height,
                                .MinDepth = viewport.minDepth,
                                .MaxDepth = viewport.maxDepth};
    m_list->RSSetViewports(1, &native);
}

void D3D12CommandList::setScissor(const Rect& scissor) {
    // In 64 bits and clamped, so that a large width or height can't overflow LONG.
    const auto edge = [](std::int32_t start, std::uint32_t extent) {
        const std::int64_t end = std::int64_t{start} + std::int64_t{extent};
        return static_cast<LONG>(std::clamp<std::int64_t>(end, LONG_MIN, LONG_MAX));
    };
    const D3D12_RECT native{.left = scissor.x,
                            .top = scissor.y,
                            .right = edge(scissor.x, scissor.width),
                            .bottom = edge(scissor.y, scissor.height)};
    m_list->RSSetScissorRects(1, &native);
}

void D3D12CommandList::setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format) {
    const BufferRecord& record = m_backend->buffer(buffer);
    const D3D12_INDEX_BUFFER_VIEW view{
        .BufferLocation = record.resource->GetGPUVirtualAddress() + offset,
        .SizeInBytes = static_cast<UINT>(std::min<std::uint64_t>(record.size - offset, UINT32_MAX)),
        .Format = format == IndexFormat::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
    m_list->IASetIndexBuffer(&view);
}

void D3D12CommandList::pushConstants(std::span<const std::byte> data) {
    // Root arguments are kept per pipeline kind, so both are set: the constants then apply to
    // whatever pipeline is bound before the next draw or dispatch.
    const auto values = static_cast<UINT>(data.size() / 4);
    m_list->SetGraphicsRoot32BitConstants(0, values, data.data(), 0);
    m_list->SetComputeRoot32BitConstants(0, values, data.data(), 0);
}

void D3D12CommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                            std::uint32_t firstInstance) {
    m_list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                   std::int32_t vertexOffset, std::uint32_t firstInstance) {
    m_list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D12CommandList::drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset,
                                                BufferHandle count, std::uint64_t countOffset, std::uint32_t maxDraws) {
    m_list->ExecuteIndirect(m_backend->drawIndexedSignature(), maxDraws, m_backend->buffer(arguments).resource,
                            argumentsOffset, m_backend->buffer(count).resource, countOffset);
}

void D3D12CommandList::dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ) {
    m_list->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void D3D12CommandList::dispatchIndirect(BufferHandle arguments, std::uint64_t offset) {
    m_list->ExecuteIndirect(m_backend->dispatchSignature(), 1, m_backend->buffer(arguments).resource, offset, nullptr,
                            0);
}

void D3D12CommandList::copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                                  std::uint64_t destinationOffset, std::uint64_t size) {
    m_list->CopyBufferRegion(m_backend->buffer(destination).resource, destinationOffset,
                             m_backend->buffer(source).resource, sourceOffset, size);
}

void D3D12CommandList::copyLocations(const BufferTextureCopy& copy, D3D12_TEXTURE_COPY_LOCATION& texture,
                                     D3D12_TEXTURE_COPY_LOCATION& buffer) const {
    const TextureRecord& record = m_backend->texture(copy.texture);
    texture = {};
    texture.pResource = record.resource.Get();
    texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture.SubresourceIndex = copy.mipLevel + copy.layer * record.mipLevels;
    buffer = {};
    buffer.pResource = m_backend->buffer(copy.buffer).resource;
    buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buffer.PlacedFootprint.Offset = copy.bufferOffset;
    buffer.PlacedFootprint.Footprint = {.Format = record.copyFormat,
                                        .Width = copy.region.width,
                                        .Height = copy.region.height,
                                        .Depth = copy.region.depth,
                                        .RowPitch = copy.rowPitch};
}

void D3D12CommandList::copyBufferToTexture(const BufferTextureCopy& copy) {
    D3D12_TEXTURE_COPY_LOCATION texture{};
    D3D12_TEXTURE_COPY_LOCATION buffer{};
    copyLocations(copy, texture, buffer);
    m_list->CopyTextureRegion(&texture, copy.region.x, copy.region.y, copy.region.z, &buffer, nullptr);
}

void D3D12CommandList::copyTextureToBuffer(const BufferTextureCopy& copy) {
    D3D12_TEXTURE_COPY_LOCATION texture{};
    D3D12_TEXTURE_COPY_LOCATION buffer{};
    copyLocations(copy, texture, buffer);
    // A null box copies the whole subresource, which depth formats require (they copy only
    // whole levels).
    const TextureRecord& record = m_backend->texture(copy.texture);
    const TexelRegion& region = copy.region;
    const bool wholeLevel =
        region.x == 0 && region.y == 0 && region.z == 0 && region.width == mipSize(record.width, copy.mipLevel) &&
        region.height == mipSize(record.height, copy.mipLevel) && region.depth == mipSize(record.depth, copy.mipLevel);
    const D3D12_BOX box{.left = region.x,
                        .top = region.y,
                        .front = region.z,
                        .right = region.x + region.width,
                        .bottom = region.y + region.height,
                        .back = region.z + region.depth};
    m_list->CopyTextureRegion(&buffer, 0, 0, 0, &texture, wholeLevel ? nullptr : &box);
}

void D3D12CommandList::beginMarker(std::string_view name) {
    // An event in the plain UTF-16 format (metadata 0) that PIX and RenderDoc read, without the
    // PIX runtime library.
    m_markerName = widen(name);
    m_list->BeginEvent(0, m_markerName.c_str(), static_cast<UINT>((m_markerName.size() + 1) * sizeof(wchar_t)));
}

void D3D12CommandList::endMarker() {
    m_list->EndEvent();
}

} // namespace ith::rhi::d3d12
