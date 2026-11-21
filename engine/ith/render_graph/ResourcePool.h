#pragma once

// The pool of the render graph's transient resources (ADR-0028 rule 13).
//
//   ith::rg::ResourcePool pool(device);       // one per device, owned by the renderer
//   ...                                       // every frame:
//   graph.execute(pool);
//
// Each transient of a graph takes, at execute, a pooled RHI resource whose description and usage
// match it exactly (the debug name aside), or a new one. Transients with equal descriptions and
// disjoint lifetimes share one resource within a frame, and the same resources serve the next
// frames. There is no memory aliasing between different descriptions in v1. The pool remembers
// each resource's last use, which the next holder's first barrier names (Barrier.h). A resource
// unused for `maxUnusedFrames` executes is destroyed through the RHI's deferred destruction.
//
// Not thread-safe: the render thread uses it.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Barrier.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Format.h"
#include "ith/rhi/Handle.h"
#include "ith/rhi/Resources.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ith::rg {

namespace detail {
struct GraphData;
} // namespace detail

struct ResourcePoolDesc {
    // Executes a resource may stay unused before it is destroyed. At least 1.
    std::uint32_t maxUnusedFrames = 4;
};

class ResourcePool {
public:
    // The device must outlive the pool.
    explicit ResourcePool(rhi::Device& device, const ResourcePoolDesc& desc = {});
    // Destroys every pooled resource (deferred by the device).
    ~ResourcePool();
    ResourcePool(const ResourcePool&) = delete;
    ResourcePool& operator=(const ResourcePool&) = delete;
    ResourcePool(ResourcePool&&) = delete;
    ResourcePool& operator=(ResourcePool&&) = delete;

    [[nodiscard]] rhi::Device& device() const { return *m_device; }

    // Pooled resources alive, for tests and statistics.
    [[nodiscard]] std::size_t textureCount() const { return m_textures.size(); }

    [[nodiscard]] std::size_t bufferCount() const { return m_buffers.size(); }

    // The number of executes so far.
    [[nodiscard]] std::uint64_t frame() const { return m_frame; }

    // Destroys every pooled resource now (deferred by the device), e.g. before the device.
    void clear();

private:
    friend struct detail::GraphData;

    // What a pooled resource's next holder orders its first barrier after.
    struct LastUse {
        rhi::PipelineStage stages = rhi::PipelineStage::None;
        // Only the write accesses (Barrier.h).
        rhi::Access writes = rhi::Access::None;
    };

    struct TextureEntry {
        rhi::TextureDimension dimension = rhi::TextureDimension::Texture2D;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 1;
        std::uint32_t layers = 1;
        std::uint32_t mipLevels = 1;
        rhi::Format format = rhi::Format::Unknown;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        rhi::TextureHandle handle;
        LastUse lastUse;
        // The execute that used it last.
        std::uint64_t lastFrame = 0;
    };

    struct BufferEntry {
        std::uint64_t size = 0;
        rhi::BufferUsage usage = rhi::BufferUsage::None;
        rhi::BufferHandle handle;
        LastUse lastUse;
        std::uint64_t lastFrame = 0;
    };

    // Starts an execute: entries taken from now on are this frame's.
    void beginFrame();
    // An entry matching the description not taken this frame, or a new one. Fails with the
    // device's creation error.
    Expected<std::uint32_t, rhi::Error> acquireTexture(const rhi::TextureDesc& desc);
    Expected<std::uint32_t, rhi::Error> acquireBuffer(const rhi::BufferDesc& desc);
    // Destroys the entries unused for maxUnusedFrames executes.
    void evict();
    // An Upload buffer of at least `size` bytes, every byte 0xFF, for clobbering transients.
    Expected<rhi::BufferHandle, rhi::Error> clobberSource(std::uint64_t size);

    rhi::Device* m_device;
    ResourcePoolDesc m_desc;
    std::uint64_t m_frame = 0;
    std::vector<TextureEntry> m_textures;
    std::vector<BufferEntry> m_buffers;
    rhi::BufferHandle m_clobber;
    std::uint64_t m_clobberSize = 0;
};

} // namespace ith::rg
