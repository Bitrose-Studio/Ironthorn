#include "ith/render_graph/ResourcePool.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace ith::rg {

ResourcePool::ResourcePool(rhi::Device& device, const ResourcePoolDesc& desc) : m_device(&device), m_desc(desc) {
    ITH_ASSERT_MSG(desc.maxUnusedFrames > 0, "ResourcePoolDesc::maxUnusedFrames must be at least 1");
    m_desc.maxUnusedFrames = std::max(m_desc.maxUnusedFrames, 1u);
}

ResourcePool::~ResourcePool() {
    clear();
}

void ResourcePool::clear() {
    for (const TextureEntry& entry : m_textures) {
        m_device->destroy(entry.handle);
    }
    for (const BufferEntry& entry : m_buffers) {
        m_device->destroy(entry.handle);
    }
    m_textures.clear();
    m_buffers.clear();
    m_device->destroy(m_clobber);
    m_clobber = {};
    m_clobberSize = 0;
}

void ResourcePool::beginFrame() {
    ++m_frame;
}

Expected<std::uint32_t, rhi::Error> ResourcePool::acquireTexture(const rhi::TextureDesc& desc) {
    for (std::uint32_t i = 0; i < m_textures.size(); ++i) {
        TextureEntry& entry = m_textures[i];
        if (entry.lastFrame != m_frame && entry.dimension == desc.dimension && entry.width == desc.width &&
            entry.height == desc.height && entry.depth == desc.depth && entry.layers == desc.layers &&
            entry.mipLevels == desc.mipLevels && entry.format == desc.format && entry.usage == desc.usage) {
            entry.lastFrame = m_frame;
            return i;
        }
    }
    Expected<rhi::TextureHandle, rhi::Error> texture = m_device->createTexture(desc);
    if (!texture) {
        return makeUnexpected(std::move(texture).error());
    }
    m_textures.push_back(TextureEntry{.dimension = desc.dimension,
                                      .width = desc.width,
                                      .height = desc.height,
                                      .depth = desc.depth,
                                      .layers = desc.layers,
                                      .mipLevels = desc.mipLevels,
                                      .format = desc.format,
                                      .usage = desc.usage,
                                      .handle = *texture,
                                      .lastFrame = m_frame});
    return static_cast<std::uint32_t>(m_textures.size() - 1);
}

Expected<std::uint32_t, rhi::Error> ResourcePool::acquireBuffer(const rhi::BufferDesc& desc) {
    for (std::uint32_t i = 0; i < m_buffers.size(); ++i) {
        BufferEntry& entry = m_buffers[i];
        if (entry.lastFrame != m_frame && entry.size == desc.size && entry.usage == desc.usage) {
            entry.lastFrame = m_frame;
            return i;
        }
    }
    Expected<rhi::BufferHandle, rhi::Error> buffer = m_device->createBuffer(desc);
    if (!buffer) {
        return makeUnexpected(std::move(buffer).error());
    }
    m_buffers.push_back(BufferEntry{.size = desc.size, .usage = desc.usage, .handle = *buffer, .lastFrame = m_frame});
    return static_cast<std::uint32_t>(m_buffers.size() - 1);
}

void ResourcePool::evict() {
    const auto expired = [this](std::uint64_t lastFrame) { return m_frame - lastFrame >= m_desc.maxUnusedFrames; };
    std::erase_if(m_textures, [&](const TextureEntry& entry) {
        if (!expired(entry.lastFrame)) {
            return false;
        }
        m_device->destroy(entry.handle);
        return true;
    });
    std::erase_if(m_buffers, [&](const BufferEntry& entry) {
        if (!expired(entry.lastFrame)) {
            return false;
        }
        m_device->destroy(entry.handle);
        return true;
    });
}

Expected<rhi::BufferHandle, rhi::Error> ResourcePool::clobberSource(std::uint64_t size) {
    if (!m_clobber.isNull() && m_clobberSize >= size) {
        return m_clobber;
    }
    // Earlier frames may still read the old one: destruction is deferred.
    m_device->destroy(m_clobber);
    m_clobber = {};
    m_clobberSize = 0;
    Expected<rhi::BufferHandle, rhi::Error> buffer = m_device->createBuffer({.size = size,
                                                                             .usage = rhi::BufferUsage::CopySrc,
                                                                             .memory = rhi::MemoryType::Upload,
                                                                             .debugName = "Render graph clobber"});
    if (!buffer) {
        return buffer;
    }
    const std::span<std::byte> memory = m_device->mappedMemory(*buffer);
    std::memset(memory.data(), 0xFF, memory.size());
    m_clobber = *buffer;
    m_clobberSize = size;
    return m_clobber;
}

} // namespace ith::rg
