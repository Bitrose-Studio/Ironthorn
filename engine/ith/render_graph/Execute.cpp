// Executing a compiled render graph (ADR-0028 rules 13, 15-18): pool resources for the slots,
// the RHI barriers of each pass, recording in groups (in parallel on a job system), one
// submission, and the states the pool and the imports keep for the next frame.

#include "ith/foundation/Assert.h"
#include "ith/jobs/JobSystem.h"
#include "ith/render_graph/GraphData.h"

#include <algorithm>
#include <span>
#include <string>

namespace ith::rg::detail {
namespace {

constexpr std::uint64_t kClobberChunk = 1u << 20;

std::uint32_t mipSize(std::uint32_t size, std::uint32_t mip) {
    return std::max(1u, size >> mip);
}

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// The bytes one copy of a texture's mip level and layer reads from a buffer.
std::uint64_t copyBytes(const Resource& texture, std::uint32_t mip) {
    const std::uint32_t rowPitch =
        alignUp(mipSize(texture.width, mip) * rhi::formatTexelSize(texture.format), rhi::kTextureCopyRowAlignment);
    const std::uint32_t slices =
        texture.dimension == rhi::TextureDimension::Texture3D ? mipSize(texture.depth, mip) : 1;
    return std::uint64_t{rowPitch} * mipSize(texture.height, mip) * slices;
}

rhi::Error invalidUsage(const std::string& name, const char* what) {
    return rhi::Error{.code = rhi::ErrorCode::InvalidUsage, .message = "Render graph '" + name + "': " + what};
}

} // namespace

// Takes a pooled resource for every slot, before anything is recorded, fills in the first
// barriers that follow an earlier frame's holder, and readies the clobber source.
Expected<void, rhi::Error> GraphData::acquire(ResourcePool& pool) {
    pool.beginFrame();
    for (Slot& slot : slots) {
        const Resource& first = resources[slot.first];
        Expected<std::uint32_t, rhi::Error> entry =
            first.kind == ResourceKind::Texture
                ? pool.acquireTexture({.dimension = first.dimension,
                                       .width = first.width,
                                       .height = first.height,
                                       .depth = first.depth,
                                       .layers = first.layers,
                                       .mipLevels = first.mipLevels,
                                       .format = first.format,
                                       .usage = first.textureUsage,
                                       .debugName = first.name})
                : pool.acquireBuffer({.size = first.size, .usage = first.bufferUsage, .debugName = first.name});
        if (!entry) {
            pool.evict();
            return makeUnexpected(std::move(entry).error());
        }
        slot.entry = *entry;
    }
    for (Resource& resource : resources) {
        if (resource.imported || resource.slot == kNone) {
            continue;
        }
        const std::uint32_t entry = slots[resource.slot].entry;
        if (resource.kind == ResourceKind::Texture) {
            resource.texture = pool.m_textures[entry].handle;
        } else {
            resource.buffer = pool.m_buffers[entry].handle;
        }
    }
    // The first barriers of the pooled resources name their holders in earlier frames.
    for (PlannedBarrier& barrier : barriers) {
        if (barrier.fromPool) {
            const Resource& resource = resources[barrier.resource];
            const std::uint32_t entry = slots[resource.slot].entry;
            const ResourcePool::LastUse& lastUse =
                resource.kind == ResourceKind::Texture ? pool.m_textures[entry].lastUse : pool.m_buffers[entry].lastUse;
            barrier.stagesBefore = lastUse.stages;
            barrier.accessBefore = lastUse.writes;
            barrier.resolved = true;
        }
    }

    clobberSource = {};
    if (settings.clobberTransients) {
        std::uint64_t size = 0;
        for (const Resource& resource : resources) {
            if (!resource.imported && resource.slot != kNone) {
                size = std::max(size, resource.kind == ResourceKind::Texture ? copyBytes(resource, 0)
                                                                             : std::min(resource.size, kClobberChunk));
            }
        }
        if (size > 0) {
            Expected<rhi::BufferHandle, rhi::Error> source = pool.clobberSource(size);
            if (!source) {
                pool.evict();
                return makeUnexpected(std::move(source).error());
            }
            clobberSource = *source;
        }
    }
    return {};
}

Expected<void, rhi::Error> GraphData::execute(ResourcePool& pool, JobSystem* jobs) {
    ITH_ASSERT_MSG(compiled && !executed, "execute() once, after compile()");
    if (!compiled || executed) {
        return makeUnexpected(invalidUsage(name, "execute() needs a compiled graph that was not executed yet"));
    }
    executed = true;
    if (!valid) {
        return makeUnexpected(invalidUsage(name, "the graph has validation errors"));
    }
    rhi::Device& device = pool.device();

    if (Expected<void, rhi::Error> acquired = acquire(pool); !acquired) {
        return acquired;
    }
    prepareRecording();

    // One list per group, opened here in order; groups record in parallel when there are
    // workers. A graph without passes still records its final transitions.
    lists.clear();
    for (std::uint32_t group = 0; group < groupCount; ++group) {
        const Pass& first = passes[alivePasses[std::size_t{group} * settings.passesPerGroup]];
        lists.push_back(&device.openCommandList(first.name));
    }
    if (groupCount == 0 && !finalTextureBarriers.empty()) {
        rhi::CommandList& list = device.openCommandList(name);
        list.barrier(rhi::Barriers{.textures = finalTextureBarriers});
        lists.push_back(&list);
    }
    const bool parallel = jobs != nullptr && jobs->workerCount(JobLane::Foreground) > 0 && groupCount > 1;
    if (parallel) {
        jobs->parallelFor("Record render graph", groupCount, 1, [&](std::size_t begin, std::size_t end) {
            for (std::size_t group = begin; group < end; ++group) {
                recordGroup(static_cast<std::uint32_t>(group), *lists[group], device);
            }
        });
    } else {
        for (std::uint32_t group = 0; group < groupCount; ++group) {
            recordGroup(group, *lists[group], device);
        }
    }
    if (!lists.empty()) {
        if (Expected<void, rhi::Error> submitted = device.submit(std::span<rhi::CommandList* const>(lists));
            !submitted) {
            // Nothing ran: the imports and the pooled resources keep the states they had.
            pool.evict();
            return submitted;
        }
    }
    finish(pool);
    return {};
}

void GraphData::prepareRecording() {
    recordings.assign(passes.size(), PassRecording{});
    textureBarriers.clear();
    bufferBarriers.clear();
    finalTextureBarriers.clear();
    clobberResources.clear();

    const auto add = [&](const PlannedBarrier& planned, bool clobbered) {
        const Resource& resource = resources[planned.resource];
        // A clobbered transient was copied into first: its first barrier follows that copy.
        const rhi::PipelineStage stagesBefore = clobbered ? rhi::PipelineStage::Copy : planned.stagesBefore;
        const rhi::Access accessBefore = clobbered ? rhi::Access::CopyWrite : planned.accessBefore;
        if (resource.kind == ResourceKind::Texture) {
            textureBarriers.push_back(rhi::TextureBarrier{
                .texture = resource.texture,
                .stagesBefore = stagesBefore,
                .accessBefore = accessBefore,
                .stagesAfter = planned.stagesAfter,
                .accessAfter = planned.accessAfter,
                .layoutBefore = clobbered ? rhi::TextureLayout::CopyDst : planned.layoutBefore,
                .layoutAfter = planned.layoutAfter,
            });
        } else {
            bufferBarriers.push_back(rhi::BufferBarrier{.buffer = resource.buffer,
                                                        .stagesBefore = stagesBefore,
                                                        .accessBefore = accessBefore,
                                                        .stagesAfter = planned.stagesAfter,
                                                        .accessAfter = planned.accessAfter});
        }
    };

    for (const std::uint32_t p : alivePasses) {
        const Pass& pass = passes[p];
        PassRecording& recording = recordings[p];
        // Clobbering: each transient first used here goes to CopyDst after its earlier holder,
        // is filled from the clobber source, and then takes its first barrier from CopyDst.
        recording.firstClobberResource = static_cast<std::uint32_t>(clobberResources.size());
        recording.firstClobberTexture = static_cast<std::uint32_t>(textureBarriers.size());
        recording.firstClobberBuffer = static_cast<std::uint32_t>(bufferBarriers.size());
        if (!clobberSource.isNull()) {
            for (std::uint32_t b = pass.firstBarrier; b < pass.firstBarrier + pass.barrierCount; ++b) {
                const PlannedBarrier& planned = barriers[b];
                const Resource& resource = resources[planned.resource];
                if (resource.imported || resource.firstPass != p) {
                    continue;
                }
                clobberResources.push_back(planned.resource);
                if (resource.kind == ResourceKind::Texture) {
                    textureBarriers.push_back(rhi::TextureBarrier{.texture = resource.texture,
                                                                  .stagesBefore = planned.stagesBefore,
                                                                  .accessBefore = planned.accessBefore,
                                                                  .stagesAfter = rhi::PipelineStage::Copy,
                                                                  .accessAfter = rhi::Access::CopyWrite,
                                                                  .layoutBefore = rhi::TextureLayout::Undefined,
                                                                  .layoutAfter = rhi::TextureLayout::CopyDst});
                } else {
                    bufferBarriers.push_back(rhi::BufferBarrier{.buffer = resource.buffer,
                                                                .stagesBefore = planned.stagesBefore,
                                                                .accessBefore = planned.accessBefore,
                                                                .stagesAfter = rhi::PipelineStage::Copy,
                                                                .accessAfter = rhi::Access::CopyWrite});
                }
            }
        }
        recording.clobberResourceCount =
            static_cast<std::uint32_t>(clobberResources.size()) - recording.firstClobberResource;
        recording.clobberTextureCount =
            static_cast<std::uint32_t>(textureBarriers.size()) - recording.firstClobberTexture;
        recording.clobberBufferCount = static_cast<std::uint32_t>(bufferBarriers.size()) - recording.firstClobberBuffer;

        recording.firstTexture = static_cast<std::uint32_t>(textureBarriers.size());
        recording.firstBuffer = static_cast<std::uint32_t>(bufferBarriers.size());
        for (std::uint32_t b = pass.firstBarrier; b < pass.firstBarrier + pass.barrierCount; ++b) {
            const PlannedBarrier& planned = barriers[b];
            const Resource& resource = resources[planned.resource];
            add(planned, !clobberSource.isNull() && !resource.imported && resource.firstPass == p);
        }
        recording.textureCount = static_cast<std::uint32_t>(textureBarriers.size()) - recording.firstTexture;
        recording.bufferCount = static_cast<std::uint32_t>(bufferBarriers.size()) - recording.firstBuffer;
    }

    for (const PlannedBarrier& planned : finalBarriers) {
        const Resource& resource = resources[planned.resource];
        finalTextureBarriers.push_back(rhi::TextureBarrier{.texture = resource.texture,
                                                           .stagesBefore = planned.stagesBefore,
                                                           .accessBefore = planned.accessBefore,
                                                           .stagesAfter = planned.stagesAfter,
                                                           .accessAfter = planned.accessAfter,
                                                           .layoutBefore = planned.layoutBefore,
                                                           .layoutAfter = planned.layoutAfter});
    }
}

// Copies the clobber source into the transients first used by a pass, after their barriers to
// CopyDst.
void GraphData::recordClobber(const PassRecording& recording, rhi::CommandList& list) const {
    list.barrier(rhi::Barriers{
        .buffers = std::span(bufferBarriers).subspan(recording.firstClobberBuffer, recording.clobberBufferCount),
        .textures = std::span(textureBarriers).subspan(recording.firstClobberTexture, recording.clobberTextureCount)});
    for (std::uint32_t c = 0; c < recording.clobberResourceCount; ++c) {
        const Resource& resource = resources[clobberResources[recording.firstClobberResource + c]];
        if (resource.kind == ResourceKind::Buffer) {
            for (std::uint64_t offset = 0; offset < resource.size; offset += kClobberChunk) {
                list.copyBuffer(clobberSource, 0, resource.buffer, offset,
                                std::min(kClobberChunk, resource.size - offset));
            }
            continue;
        }
        const std::uint32_t layers = resource.dimension == rhi::TextureDimension::Texture3D ? 1 : resource.layers;
        for (std::uint32_t mip = 0; mip < resource.mipLevels; ++mip) {
            const std::uint32_t rowPitch = alignUp(mipSize(resource.width, mip) * rhi::formatTexelSize(resource.format),
                                                   rhi::kTextureCopyRowAlignment);
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                list.copyBufferToTexture({.buffer = clobberSource,
                                          .rowPitch = rowPitch,
                                          .texture = resource.texture,
                                          .mipLevel = mip,
                                          .layer = layer});
            }
        }
    }
}

// Begins rendering on a raster pass's attachments, colors in declaration order.
void GraphData::beginRendering(const Pass& pass, rhi::CommandList& list) const {
    std::array<rhi::ColorAttachment, rhi::kMaxColorTargets> colors{};
    std::size_t colorCount = 0;
    rhi::DepthAttachment depth;
    for (std::uint32_t u = pass.firstUse; u < pass.firstUse + pass.useCount; ++u) {
        const Use& use = uses[u];
        if (!use.attachment) {
            continue;
        }
        const rhi::TextureHandle texture = resources[use.resource].texture;
        if (use.layout == rhi::TextureLayout::RenderTarget) {
            colors[colorCount++] = rhi::ColorAttachment{
                .texture = texture, .load = use.load, .store = use.store, .clearColor = use.clearColor};
        } else {
            depth = rhi::DepthAttachment{
                .texture = texture, .load = use.load, .store = use.store, .clearDepth = use.clearDepth};
        }
    }
    list.beginRendering({.colors = std::span(colors).first(colorCount), .depth = depth});
}

void GraphData::recordGroup(std::uint32_t group, rhi::CommandList& list, const rhi::Device& device) const {
    const std::size_t begin = std::size_t{group} * settings.passesPerGroup;
    const std::size_t end = std::min(alivePasses.size(), begin + settings.passesPerGroup);
    for (std::size_t i = begin; i < end; ++i) {
        const std::uint32_t p = alivePasses[i];
        const Pass& pass = passes[p];
        const PassRecording& recording = recordings[p];
        // A marker for GPU debuggers and, in profiling builds, a GPU zone for the profiler, around
        // everything the pass records, its barriers included.
        list.beginMarker(pass.name);
        list.beginGpuZone(pass.name);
        if (recording.clobberResourceCount > 0) {
            recordClobber(recording, list);
        }
        if (recording.textureCount > 0 || recording.bufferCount > 0) {
            list.barrier(rhi::Barriers{
                .buffers = std::span(bufferBarriers).subspan(recording.firstBuffer, recording.bufferCount),
                .textures = std::span(textureBarriers).subspan(recording.firstTexture, recording.textureCount)});
        }
        const bool raster = pass.type == PassType::Raster;
        if (raster) {
            beginRendering(pass, list);
        }
        PassContext context(*this, p, list, device);
        pass.call(pass.object, context);
        if (raster) {
            list.endRendering();
        }
        list.endGpuZone();
        list.endMarker();
    }
    if (group + 1 == groupCount && !finalTextureBarriers.empty()) {
        list.barrier(rhi::Barriers{.textures = finalTextureBarriers});
    }
}

void GraphData::finish(ResourcePool& pool) {
    for (const Slot& slot : slots) {
        const Resource& holder = resources[slot.holder];
        const ResourcePool::LastUse lastUse{.stages = holder.last.stages,
                                            .writes = holder.last.access & kWriteAccesses};
        if (holder.kind == ResourceKind::Texture) {
            pool.m_textures[slot.entry].lastUse = lastUse;
        } else {
            pool.m_buffers[slot.entry].lastUse = lastUse;
        }
    }
    for (const Resource& resource : resources) {
        if (resource.imported && resource.state != nullptr) {
            *resource.state = ImportedState{
                .layout = resource.last.layout, .stages = resource.last.stages, .access = resource.last.access};
        }
    }
    pool.evict();
}

} // namespace ith::rg::detail
