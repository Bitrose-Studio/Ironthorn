// Compiling a render graph (ADR-0028 rules 9-12): validation of the order, culling, lifetimes,
// pool slots, usages, barriers, store operations and groups. Needs no device.

#include "ith/foundation/Assert.h"
#include "ith/render_graph/GraphData.h"

#include <algorithm>

namespace ith::rg::detail {
namespace {

// The access a final layout is left in for whatever uses the resource next.
rhi::Access finalAccess(rhi::TextureLayout layout) {
    using rhi::Access;
    switch (layout) {
        case rhi::TextureLayout::General:
        case rhi::TextureLayout::ShaderWrite:
            return Access::ShaderRead | Access::ShaderWrite;
        case rhi::TextureLayout::RenderTarget:
            return Access::RenderTargetWrite;
        case rhi::TextureLayout::DepthStencilWrite:
            return Access::DepthStencilWrite;
        case rhi::TextureLayout::DepthStencilRead:
            return Access::DepthStencilRead;
        case rhi::TextureLayout::ShaderRead:
            return Access::ShaderRead;
        case rhi::TextureLayout::CopySrc:
            return Access::CopyRead;
        case rhi::TextureLayout::CopyDst:
            return Access::CopyWrite;
        case rhi::TextureLayout::Undefined:
        case rhi::TextureLayout::Present:
            break;
    }
    return Access::None;
}

bool sameDescription(const Resource& a, const Resource& b) {
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == ResourceKind::Buffer) {
        return a.size == b.size && a.bufferUsage == b.bufferUsage;
    }
    return a.dimension == b.dimension && a.width == b.width && a.height == b.height && a.depth == b.depth &&
           a.layers == b.layers && a.mipLevels == b.mipLevels && a.format == b.format &&
           a.textureUsage == b.textureUsage;
}

} // namespace

void GraphData::compile() {
    compiled = true;
    if constexpr (kValidate) {
        validateOrder();
    }
    cull();

    alivePasses.clear();
    for (std::uint32_t p = 0; p < passes.size(); ++p) {
        Pass& pass = passes[p];
        pass.group = kNone;
        if (pass.alive) {
            pass.group = static_cast<std::uint32_t>(alivePasses.size()) / settings.passesPerGroup;
            alivePasses.push_back(p);
        }
    }
    groupCount =
        static_cast<std::uint32_t>((alivePasses.size() + settings.passesPerGroup - 1) / settings.passesPerGroup);

    // Lifetimes over the alive passes, and the transients' usages from their uses.
    for (Resource& resource : resources) {
        resource.firstPass = kNone;
        resource.lastPass = kNone;
        resource.slot = kNone;
        if (!resource.imported) {
            resource.textureUsage = rhi::TextureUsage::None;
            resource.bufferUsage = rhi::BufferUsage::None;
        }
    }
    for (const std::uint32_t p : alivePasses) {
        const Pass& pass = passes[p];
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
            const Use& use = uses[i];
            Resource& resource = resources[use.resource];
            if (resource.firstPass == kNone) {
                resource.firstPass = p;
            }
            resource.lastPass = p;
            if (!resource.imported) {
                resource.textureUsage = resource.textureUsage | use.textureUsage;
                resource.bufferUsage = resource.bufferUsage | use.bufferUsage;
            }
        }
    }
    if (settings.clobberTransients) {
        for (Resource& resource : resources) {
            if (!resource.imported && resource.firstPass != kNone) {
                resource.textureUsage = resource.kind == ResourceKind::Texture
                                            ? resource.textureUsage | rhi::TextureUsage::CopyDst
                                            : resource.textureUsage;
                resource.bufferUsage = resource.kind == ResourceKind::Buffer
                                           ? resource.bufferUsage | rhi::BufferUsage::CopyDst
                                           : resource.bufferUsage;
            }
        }
    }
    planBarriers();
}

void GraphData::validateOrder() {
    needed.assign(resources.size(), 0); // here: whether an earlier pass wrote the resource
    for (std::uint32_t p = 0; p < passes.size(); ++p) {
        const Pass& pass = passes[p];
        std::uint32_t attachments = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
            const Use& use = uses[i];
            const Resource& resource = resources[use.resource];
            if (!resource.imported && needed[use.resource] == 0 &&
                (!use.writes || (use.attachment && use.load == rhi::LoadOp::Load))) {
                fail(where(p, use.resource) + ": reads a transient that no earlier pass wrote");
            }
            if (use.attachment) {
                if (attachments > 0 && (resource.width != width || resource.height != height)) {
                    fail(where(p, use.resource) + ": attachments of a pass must have the same size");
                }
                ++attachments;
                width = resource.width;
                height = resource.height;
            }
        }
        if (pass.type == PassType::Raster && attachments == 0) {
            fail(where(p, kNone) + ": a raster pass needs a render target or depth attachment");
        }
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
            if (uses[i].writes) {
                needed[uses[i].resource] = 1;
            }
        }
    }
}

// Walks back from the outputs (ADR-0028 rule 10): a pass is kept when it never culls, writes an
// import, or writes a resource whose contents a kept later pass needs. On the way, each kept
// attachment stores its contents only when a later kept pass needs them or it is imported.
void GraphData::cull() {
    needed.assign(resources.size(), 0);
    for (std::uint32_t r = 0; r < resources.size(); ++r) {
        needed[r] = resources[r].imported ? 1 : 0;
    }
    for (auto p = static_cast<std::uint32_t>(passes.size()); p-- > 0;) {
        Pass& pass = passes[p];
        bool alive =
            !settings.culling || (static_cast<unsigned>(pass.flags) & static_cast<unsigned>(PassFlags::NeverCull)) != 0;
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount && !alive; ++i) {
            const Use& use = uses[i];
            alive = use.writes && (resources[use.resource].imported || needed[use.resource] != 0);
        }
        pass.alive = alive;
        if (!alive) {
            continue;
        }
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
            Use& use = uses[i];
            if (use.attachment) {
                use.store = needed[use.resource] != 0 ? rhi::StoreOp::Store : rhi::StoreOp::DontCare;
            }
            needed[use.resource] = use.keepsContents ? 1 : 0;
        }
    }
}

// The first use of a transient takes a pool slot: a free one of an earlier transient with the
// same description, whose last use the barrier names, or a new one, whose earlier frame's use
// execute fills in.
void GraphData::takeSlot(std::uint32_t pass, std::uint32_t r, PlannedBarrier& barrier) {
    Resource& resource = resources[r];
    for (std::uint32_t s = 0; s < slots.size(); ++s) {
        Slot& slot = slots[s];
        if (slot.busyUntil < pass && sameDescription(resources[slot.first], resource)) {
            const UseState& previous = resources[slot.holder].last;
            barrier.stagesBefore = previous.stages;
            barrier.accessBefore = previous.access & kWriteAccesses;
            slot.holder = r;
            slot.busyUntil = resource.lastPass;
            resource.slot = s;
            return;
        }
    }
    slots.push_back(Slot{.first = r, .holder = r, .busyUntil = resource.lastPass});
    resource.slot = static_cast<std::uint32_t>(slots.size() - 1);
    barrier.fromPool = true;
}

// Follows each resource's uses in order (ADR-0028 rule 11) and emits a barrier before a use when
// the layout changes or either side writes; consecutive reads in one layout merge their stages
// into the barrier before the first of them.
void GraphData::planBarriers() {
    barriers.clear();
    slots.clear();
    // The barrier the resource's current run of reads started with; kNone when none.
    std::vector<std::uint32_t>& open = openBarriers;
    open.assign(resources.size(), kNone);
    for (Resource& resource : resources) {
        resource.last = resource.imported ? UseState{.layout = resource.initial.layout,
                                                     .stages = resource.initial.stages,
                                                     .access = resource.initial.access}
                                          : UseState{};
    }

    for (const std::uint32_t p : alivePasses) {
        Pass& pass = passes[p];
        pass.firstBarrier = static_cast<std::uint32_t>(barriers.size());
        for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
            const Use& use = uses[i];
            Resource& resource = resources[use.resource];
            UseState& last = resource.last;
            const bool texture = resource.kind == ResourceKind::Texture;
            PlannedBarrier barrier{.resource = use.resource,
                                   .stagesAfter = use.stages,
                                   .accessAfter = use.access,
                                   .layoutAfter = use.layout};
            if (!resource.imported && resource.firstPass == p) {
                takeSlot(p, use.resource, barrier);
            } else if (!use.keepsContents) {
                // A cleared or discarded attachment: out of Undefined, after the earlier use.
                barrier.stagesBefore = last.stages;
                barrier.accessBefore = last.access & kWriteAccesses;
            } else if ((texture && last.layout != use.layout) ||
                       (last.stages != rhi::PipelineStage::None &&
                        (use.writes || rhi::hasAny(last.access, kWriteAccesses)))) {
                barrier.stagesBefore = last.stages;
                barrier.accessBefore =
                    last.layout == rhi::TextureLayout::Undefined ? last.access & kWriteAccesses : last.access;
                barrier.layoutBefore = last.layout;
            } else {
                // A read after reads in the same layout (or a first use with nothing to wait
                // for): no barrier; the run's barrier covers its stages too.
                if (open[use.resource] != kNone) {
                    PlannedBarrier& start = barriers[open[use.resource]];
                    start.stagesAfter = start.stagesAfter | use.stages;
                    start.accessAfter = start.accessAfter | use.access;
                }
                last.layout = use.layout;
                last.stages = last.stages | use.stages;
                last.access = last.access | use.access;
                continue;
            }
            if (!texture) {
                barrier.layoutBefore = rhi::TextureLayout::Undefined;
                barrier.layoutAfter = rhi::TextureLayout::Undefined;
            }
            open[use.resource] = static_cast<std::uint32_t>(barriers.size());
            barriers.push_back(barrier);
            last = UseState{.layout = use.layout, .stages = use.stages, .access = use.access};
        }
        pass.barrierCount = static_cast<std::uint32_t>(barriers.size()) - pass.firstBarrier;
    }
    planFinalBarriers();
}

// Imports that must end in a given layout.
void GraphData::planFinalBarriers() {
    finalBarriers.clear();
    for (std::uint32_t r = 0; r < resources.size(); ++r) {
        Resource& resource = resources[r];
        if (!resource.imported || !resource.finalLayout || *resource.finalLayout == resource.last.layout) {
            continue;
        }
        const rhi::TextureLayout layout = *resource.finalLayout;
        const bool present = layout == rhi::TextureLayout::Present;
        const PlannedBarrier barrier{.resource = r,
                                     .stagesBefore = resource.last.stages,
                                     .accessBefore = resource.last.layout == rhi::TextureLayout::Undefined
                                                         ? resource.last.access & kWriteAccesses
                                                         : resource.last.access,
                                     .stagesAfter =
                                         present ? rhi::PipelineStage::None : rhi::PipelineStage::AllCommands,
                                     .accessAfter = finalAccess(layout),
                                     .layoutBefore = resource.last.layout,
                                     .layoutAfter = layout};
        finalBarriers.push_back(barrier);
        resource.last = UseState{.layout = layout, .stages = barrier.stagesAfter, .access = barrier.accessAfter};
    }
}

} // namespace ith::rg::detail
