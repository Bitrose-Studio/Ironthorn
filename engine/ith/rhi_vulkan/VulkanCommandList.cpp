#include "ith/foundation/Assert.h"
#include "ith/rhi/Barrier.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi_vulkan/VulkanApi.h"
#include "ith/rhi_vulkan/VulkanBackend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ith::rhi::vulkan {
namespace {

// Barrier.h's stages and accesses map one to one onto synchronization2's.

VkPipelineStageFlags2 stageMask(PipelineStage stages) {
    if (hasAny(stages, PipelineStage::AllCommands)) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkPipelineStageFlags2 result = VK_PIPELINE_STAGE_2_NONE;
    const auto add = [&](PipelineStage stage, VkPipelineStageFlags2 bits) {
        if (hasAny(stages, stage)) {
            result |= bits;
        }
    };
    add(PipelineStage::DrawIndirect, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    add(PipelineStage::IndexInput, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
    add(PipelineStage::VertexShader, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    add(PipelineStage::PixelShader, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    add(PipelineStage::DepthStencil,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    add(PipelineStage::ColorOutput, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    add(PipelineStage::ComputeShader, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    add(PipelineStage::Copy, VK_PIPELINE_STAGE_2_COPY_BIT);
    return result;
}

VkAccessFlags2 accessMask(Access accesses, VkPipelineStageFlags2 stages) {
    // No stages, no access: an access without a stage is invalid.
    if (stages == VK_PIPELINE_STAGE_2_NONE) {
        return VK_ACCESS_2_NONE;
    }
    VkAccessFlags2 result = VK_ACCESS_2_NONE;
    const auto add = [&](Access access, VkAccessFlags2 bits) {
        if (hasAny(accesses, access)) {
            result |= bits;
        }
    };
    add(Access::IndirectArgument, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    add(Access::IndexBuffer, VK_ACCESS_2_INDEX_READ_BIT);
    add(Access::ShaderRead, VK_ACCESS_2_SHADER_READ_BIT);
    add(Access::ShaderWrite, VK_ACCESS_2_SHADER_WRITE_BIT);
    add(Access::RenderTargetRead, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    add(Access::RenderTargetWrite, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    add(Access::DepthStencilRead, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    add(Access::DepthStencilWrite, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    add(Access::CopyRead, VK_ACCESS_2_TRANSFER_READ_BIT);
    add(Access::CopyWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return result;
}

// Sampled descriptors are written for READ_ONLY_OPTIMAL and storage descriptors for GENERAL
// (VulkanBackend.cpp), so shader reads use those layouts.
VkImageLayout imageLayout(TextureLayout layout) {
    switch (layout) {
        case TextureLayout::Undefined:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case TextureLayout::General:
        case TextureLayout::ShaderWrite:
            return VK_IMAGE_LAYOUT_GENERAL;
        case TextureLayout::RenderTarget:
        case TextureLayout::DepthStencilWrite:
            return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        case TextureLayout::DepthStencilRead:
        case TextureLayout::ShaderRead:
            return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        case TextureLayout::CopySrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureLayout::CopyDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case TextureLayout::Present:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_GENERAL;
}

VkAttachmentLoadOp loadOp(LoadOp op) {
    switch (op) {
        case LoadOp::Load:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkAttachmentStoreOp storeOp(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

} // namespace

VulkanCommandList::VulkanCommandList(VulkanBackend& backend, VkCommandPool pool, VkCommandBuffer commands)
    : m_backend(&backend), m_vk(&backend.functions()), m_pool(pool), m_commands(commands) {}

VulkanCommandList::~VulkanCommandList() {
    // The device destroys its lists only when the GPU is done with them (Backend.h).
#ifdef ITH_ENABLE_PROFILING
    // Zones left open (after a failed assert) end into the list while it still exists.
    if (m_recording) {
        m_gpuZones.endAll();
    }
#endif
    VkDevice device = m_backend->device();
    m_vk->vkDestroyCommandPool(device, m_pool, nullptr);
}

Expected<void, Error> VulkanCommandList::begin(std::uint32_t /*frameSlot*/, std::string_view debugName) {
    // Resetting the pool also resets a list left recording (discarded, or its batch failed or was
    // skipped; the device reported it abandoned()). Zones left open after a failed assert end into
    // it first, while it still records.
#ifdef ITH_ENABLE_PROFILING
    if (m_recording) {
        m_gpuZones.endAll();
    }
    m_gpuZones.restart();
#endif
    VkDevice device = m_backend->device();
    m_recording = false;
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (m_vk->vkResetCommandPool(device, m_pool, 0) != VK_SUCCESS ||
        m_vk->vkBeginCommandBuffer(m_commands, &info) != VK_SUCCESS) {
        return makeUnexpected(Error{.code = ErrorCode::OutOfMemory,
                                    .message = "Out of memory starting command list '" + std::string(debugName) + "'"});
    }
    m_recording = true;
    m_backend->setName(VK_OBJECT_TYPE_COMMAND_BUFFER,
                       reinterpret_cast<std::uint64_t>(m_commands), // NOLINT(*-reinterpret-cast)
                       debugName);
    VkDescriptorSet set = m_backend->descriptorSet();
    for (const VkPipelineBindPoint bindPoint : {VK_PIPELINE_BIND_POINT_GRAPHICS, VK_PIPELINE_BIND_POINT_COMPUTE}) {
        m_vk->vkCmdBindDescriptorSets(m_commands, bindPoint, m_backend->pipelineLayout(), 0, 1, &set, 0, nullptr);
    }
    return {};
}

VkResult VulkanCommandList::close() {
    if (!m_recording) {
        return VK_SUCCESS;
    }
    m_recording = false;
#ifdef ITH_ENABLE_PROFILING
    // A zone left open (asserted by the device) still gets its end timestamp.
    m_gpuZones.endAll();
#endif
    // Makes the list's writes visible to the CPU: Device::mappedMemory() of a Readback buffer is
    // read once the timeline says the GPU is done, and a semaphore signal alone doesn't make
    // writes visible to host reads. One barrier at the end of every list is the simplest rule
    // that covers every copy into a Readback buffer.
    const VkMemoryBarrier2 toHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &toHost};
    m_vk->vkCmdPipelineBarrier2(m_commands, &dependency);
    return m_vk->vkEndCommandBuffer(m_commands);
}

void VulkanCommandList::barrier(const Barriers& barriers) {
    m_globalBarriers.clear();
    m_bufferBarriers.clear();
    m_imageBarriers.clear();
    for (const GlobalBarrier& global : barriers.global) {
        const VkPipelineStageFlags2 before = stageMask(global.stagesBefore);
        const VkPipelineStageFlags2 after = stageMask(global.stagesAfter);
        m_globalBarriers.push_back(VkMemoryBarrier2{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                                    .srcStageMask = before,
                                                    .srcAccessMask = accessMask(global.accessBefore, before),
                                                    .dstStageMask = after,
                                                    .dstAccessMask = accessMask(global.accessAfter, after)});
    }
    for (const BufferBarrier& buffer : barriers.buffers) {
        const VkPipelineStageFlags2 before = stageMask(buffer.stagesBefore);
        const VkPipelineStageFlags2 after = stageMask(buffer.stagesAfter);
        m_bufferBarriers.push_back(VkBufferMemoryBarrier2{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                          .srcStageMask = before,
                                                          .srcAccessMask = accessMask(buffer.accessBefore, before),
                                                          .dstStageMask = after,
                                                          .dstAccessMask = accessMask(buffer.accessAfter, after),
                                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                          .buffer = m_backend->buffer(buffer.buffer).buffer,
                                                          .offset = 0,
                                                          .size = VK_WHOLE_SIZE});
    }
    for (const TextureBarrier& texture : barriers.textures) {
        const TextureRecord& record = m_backend->texture(texture.texture);
        const SubresourceRange& range = texture.range;
        const VkPipelineStageFlags2 before = stageMask(texture.stagesBefore);
        const VkPipelineStageFlags2 after = stageMask(texture.stagesAfter);
        m_imageBarriers.push_back(VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = before,
            .srcAccessMask = accessMask(texture.accessBefore, before),
            .dstStageMask = after,
            .dstAccessMask = accessMask(texture.accessAfter, after),
            .oldLayout = imageLayout(texture.layoutBefore),
            .newLayout = imageLayout(texture.layoutAfter),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = record.image,
            .subresourceRange = {
                .aspectMask = record.aspects,
                .baseMipLevel = range.baseMip,
                .levelCount =
                    range.mipCount == SubresourceRange::kAllRemaining ? VK_REMAINING_MIP_LEVELS : range.mipCount,
                .baseArrayLayer = range.baseLayer,
                .layerCount = range.layerCount == SubresourceRange::kAllRemaining ? VK_REMAINING_ARRAY_LAYERS
                                                                                  : range.layerCount}});
    }
    if (m_globalBarriers.empty() && m_bufferBarriers.empty() && m_imageBarriers.empty()) {
        return;
    }
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = static_cast<std::uint32_t>(m_globalBarriers.size()),
                                      .pMemoryBarriers = m_globalBarriers.data(),
                                      .bufferMemoryBarrierCount = static_cast<std::uint32_t>(m_bufferBarriers.size()),
                                      .pBufferMemoryBarriers = m_bufferBarriers.data(),
                                      .imageMemoryBarrierCount = static_cast<std::uint32_t>(m_imageBarriers.size()),
                                      .pImageMemoryBarriers = m_imageBarriers.data()};
    m_vk->vkCmdPipelineBarrier2(m_commands, &dependency);
}

void VulkanCommandList::beginRendering(const RenderingDesc& desc) {
    std::array<VkRenderingAttachmentInfo, kMaxColorTargets> colors{};
    // The device checks the count in Debug and development builds; the clamp keeps Shipping
    // builds inside `colors` too.
    ITH_ASSERT(desc.colors.size() <= kMaxColorTargets);
    const std::size_t colorCount = std::min<std::size_t>(desc.colors.size(), kMaxColorTargets);
    for (std::size_t index = 0; index < colorCount; ++index) {
        const ColorAttachment& color = desc.colors[index];
        VkClearValue clear{};
        std::ranges::copy(color.clearColor, static_cast<float*>(clear.color.float32));
        colors[index] = VkRenderingAttachmentInfo{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                  .imageView = m_backend->texture(color.texture).attachmentView,
                                                  .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
                                                  .loadOp = loadOp(color.load),
                                                  .storeOp = storeOp(color.store),
                                                  .clearValue = clear};
    }
    const bool hasDepth = !desc.depth.texture.isNull();
    VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool stencil = false;
    if (hasDepth) {
        const TextureRecord& record = m_backend->texture(desc.depth.texture);
        stencil = (record.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        VkClearValue clear{};
        clear.depthStencil = {.depth = desc.depth.clearDepth, .stencil = 0};
        depth.imageView = record.attachmentView;
        depth.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        depth.loadOp = loadOp(desc.depth.load);
        depth.storeOp = storeOp(desc.depth.store);
        depth.clearValue = clear;
    }
    const TextureRecord& first = m_backend->texture(desc.colors.empty() ? desc.depth.texture : desc.colors[0].texture);
    const VkRenderingInfo info{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {.offset = {0, 0}, .extent = {first.width, first.height}},
                               .layerCount = 1,
                               .colorAttachmentCount = static_cast<std::uint32_t>(colorCount),
                               .pColorAttachments = colors.data(),
                               .pDepthAttachment = hasDepth ? &depth : nullptr,
                               .pStencilAttachment = stencil ? &depth : nullptr};
    m_vk->vkCmdBeginRendering(m_commands, &info);
    setViewport({.width = static_cast<float>(first.width), .height = static_cast<float>(first.height)});
    setScissor({.width = first.width, .height = first.height});
}

void VulkanCommandList::endRendering() {
    m_vk->vkCmdEndRendering(m_commands);
}

void VulkanCommandList::setPipeline(PipelineHandle pipeline) {
    const PipelineRecord& record = m_backend->pipeline(pipeline);
    m_vk->vkCmdBindPipeline(m_commands, record.bindPoint, record.pipeline);
}

void VulkanCommandList::setViewport(const Viewport& viewport) {
    // A negative height turns Vulkan's y-down viewport into the engine's y-up clip space, the
    // same as Direct3D 12's (ADR-0006).
    const VkViewport native{.x = viewport.x,
                            .y = viewport.y + viewport.height,
                            .width = viewport.width,
                            .height = -viewport.height,
                            .minDepth = viewport.minDepth,
                            .maxDepth = viewport.maxDepth};
    m_vk->vkCmdSetViewport(m_commands, 0, 1, &native);
}

void VulkanCommandList::setScissor(const Rect& scissor) {
    // Vulkan refuses negative offsets: the part left of or above the target is cut off.
    const std::int32_t x = std::max(scissor.x, 0);
    const std::int32_t y = std::max(scissor.y, 0);
    const auto shrink = [](std::uint32_t extent, std::int32_t start) {
        const std::int64_t end = std::int64_t{start} + std::int64_t{extent};
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(end, 0, INT32_MAX) - std::max(start, 0));
    };
    const VkRect2D native{.offset = {x, y},
                          .extent = {shrink(scissor.width, scissor.x), shrink(scissor.height, scissor.y)}};
    m_vk->vkCmdSetScissor(m_commands, 0, 1, &native);
}

void VulkanCommandList::setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format) {
    m_vk->vkCmdBindIndexBuffer(m_commands, m_backend->buffer(buffer).buffer, offset,
                               format == IndexFormat::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::pushConstants(std::span<const std::byte> data) {
    m_vk->vkCmdPushConstants(m_commands, m_backend->pipelineLayout(), kShaderStages, 0,
                             static_cast<std::uint32_t>(data.size()), data.data());
}

void VulkanCommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                             std::uint32_t firstInstance) {
    m_vk->vkCmdDraw(m_commands, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                    std::int32_t vertexOffset, std::uint32_t firstInstance) {
    m_vk->vkCmdDrawIndexed(m_commands, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset,
                                                 BufferHandle count, std::uint64_t countOffset,
                                                 std::uint32_t maxDraws) {
    m_vk->vkCmdDrawIndexedIndirectCount(m_commands, m_backend->buffer(arguments).buffer, argumentsOffset,
                                        m_backend->buffer(count).buffer, countOffset, maxDraws,
                                        sizeof(DrawIndexedIndirectArguments));
}

void VulkanCommandList::dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ) {
    m_vk->vkCmdDispatch(m_commands, groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::dispatchIndirect(BufferHandle arguments, std::uint64_t offset) {
    m_vk->vkCmdDispatchIndirect(m_commands, m_backend->buffer(arguments).buffer, offset);
}

void VulkanCommandList::copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                                   std::uint64_t destinationOffset, std::uint64_t size) {
    const VkBufferCopy region{.srcOffset = sourceOffset, .dstOffset = destinationOffset, .size = size};
    m_vk->vkCmdCopyBuffer(m_commands, m_backend->buffer(source).buffer, m_backend->buffer(destination).buffer, 1,
                          &region);
}

VkBufferImageCopy2 VulkanCommandList::region(const BufferTextureCopy& copy) const {
    const TextureRecord& record = m_backend->texture(copy.texture);
    // Copies reach a depth texture's depth only, as on Direct3D 12.
    const VkImageAspectFlags aspect = (record.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0
                                          ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                                          : record.aspects;
    const TexelRegion& box = copy.region;
    return VkBufferImageCopy2{.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                              .bufferOffset = copy.bufferOffset,
                              .bufferRowLength = copy.rowPitch / record.texelSize,
                              .bufferImageHeight = box.height,
                              .imageSubresource = {.aspectMask = aspect,
                                                   .mipLevel = copy.mipLevel,
                                                   .baseArrayLayer = copy.layer,
                                                   .layerCount = 1},
                              .imageOffset = {static_cast<std::int32_t>(box.x), static_cast<std::int32_t>(box.y),
                                              static_cast<std::int32_t>(box.z)},
                              .imageExtent = {.width = box.width, .height = box.height, .depth = box.depth}};
}

void VulkanCommandList::copyBufferToTexture(const BufferTextureCopy& copy) {
    const VkBufferImageCopy2 copyRegion = region(copy);
    const VkCopyBufferToImageInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                                        .srcBuffer = m_backend->buffer(copy.buffer).buffer,
                                        .dstImage = m_backend->texture(copy.texture).image,
                                        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .regionCount = 1,
                                        .pRegions = &copyRegion};
    m_vk->vkCmdCopyBufferToImage2(m_commands, &info);
}

void VulkanCommandList::copyTextureToBuffer(const BufferTextureCopy& copy) {
    const VkBufferImageCopy2 copyRegion = region(copy);
    const VkCopyImageToBufferInfo2 info{.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
                                        .srcImage = m_backend->texture(copy.texture).image,
                                        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        .dstBuffer = m_backend->buffer(copy.buffer).buffer,
                                        .regionCount = 1,
                                        .pRegions = &copyRegion};
    m_vk->vkCmdCopyImageToBuffer2(m_commands, &info);
}

void VulkanCommandList::beginMarker(std::string_view name) {
    // A debug-utils label, which RenderDoc and the vendors' tools read.
    if (!m_backend->debugUtils()) {
        return;
    }
    m_markerName = name;
    const VkDebugUtilsLabelEXT label{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                                     .pLabelName = m_markerName.c_str()};
    m_vk->vkCmdBeginDebugUtilsLabelEXT(m_commands, &label);
}

void VulkanCommandList::endMarker() {
    if (m_backend->debugUtils()) {
        m_vk->vkCmdEndDebugUtilsLabelEXT(m_commands);
    }
}

// Profiling builds only call these (Backend.h).
void VulkanCommandList::beginGpuZone([[maybe_unused]] std::string_view name) {
#ifdef ITH_ENABLE_PROFILING
    m_gpuZones.begin(m_backend->gpuProfiler(), m_commands, name);
#endif
}

void VulkanCommandList::endGpuZone() {
#ifdef ITH_ENABLE_PROFILING
    m_gpuZones.end();
#endif
}

void VulkanCommandList::abandoned([[maybe_unused]] bool stillOpen) {
#ifdef ITH_ENABLE_PROFILING
    if (stillOpen) {
        // Its recording may be unfinished, so the zones aren't read.
        m_backend->gpuProfiler().disable("endFrame() closed a command list that was still open");
    } else {
        m_gpuZones.abandoned(m_backend->gpuProfiler());
    }
#endif
}

} // namespace ith::rhi::vulkan
