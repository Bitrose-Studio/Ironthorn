#include "ith/render_graph/RenderGraph.h"

#include "ith/foundation/Assert.h"
#include "ith/render_graph/GraphData.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace ith::rg {

namespace detail {
namespace {

// Serials tell the handles of different graphs, and of one graph before and after a reset,
// apart. Process-wide so that two graphs never share one.
std::atomic<std::uint32_t> g_nextSerial{1};

std::uint32_t nextSerial() {
    std::uint32_t serial = g_nextSerial.fetch_add(1, std::memory_order_relaxed);
    if (serial == 0) { // wrapped: 0 is the null handle's
        serial = g_nextSerial.fetch_add(1, std::memory_order_relaxed);
    }
    return serial;
}

constexpr std::uint16_t bit(TextureUse use) {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(use));
}

constexpr std::uint16_t bit(BufferUse use) {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(use));
}

// The shader stages a pass type's shader uses run in.
rhi::PipelineStage shaderStages(PassType type) {
    switch (type) {
        case PassType::Raster:
            return rhi::PipelineStage::VertexShader | rhi::PipelineStage::PixelShader;
        case PassType::Compute:
            return rhi::PipelineStage::ComputeShader;
        case PassType::Copy:
            break;
    }
    return rhi::PipelineStage::None;
}

bool isSrgb(rhi::Format format) {
    return format == rhi::Format::R8G8B8A8Srgb || format == rhi::Format::B8G8R8A8Srgb;
}

rhi::TextureUsage usageOf(TextureUse use) {
    switch (use) {
        case TextureUse::Sampled:
        case TextureUse::DepthSampled:
            return rhi::TextureUsage::Sampled;
        case TextureUse::Storage:
            return rhi::TextureUsage::Storage;
        case TextureUse::RenderTarget:
            return rhi::TextureUsage::RenderTarget;
        case TextureUse::DepthWrite:
            return rhi::TextureUsage::DepthStencil;
        case TextureUse::CopySrc:
            return rhi::TextureUsage::CopySrc;
        case TextureUse::CopyDst:
            break;
    }
    return rhi::TextureUsage::CopyDst;
}

rhi::BufferUsage usageOf(BufferUse use) {
    switch (use) {
        case BufferUse::ShaderRead:
            return rhi::BufferUsage::ShaderRead;
        case BufferUse::Storage:
            return rhi::BufferUsage::ShaderWrite;
        case BufferUse::Indirect:
            return rhi::BufferUsage::Indirect;
        case BufferUse::Index:
            return rhi::BufferUsage::Index;
        case BufferUse::CopySrc:
            return rhi::BufferUsage::CopySrc;
        case BufferUse::CopyDst:
            break;
    }
    return rhi::BufferUsage::CopyDst;
}

bool isShaderUse(TextureUse use) {
    return use == TextureUse::Sampled || use == TextureUse::Storage || use == TextureUse::DepthSampled;
}

bool isShaderUse(BufferUse use) {
    return use == BufferUse::ShaderRead || use == BufferUse::Storage;
}

// The layout, stages and accesses of a texture use (ADR-0028 rule 5).
Use textureUse(TextureUse use, rhi::PipelineStage shader, rhi::LoadOp load) {
    using rhi::Access;
    using rhi::PipelineStage;
    using rhi::TextureLayout;
    Use result;
    result.uses = bit(use);
    switch (use) {
        case TextureUse::Sampled:
            result.layout = TextureLayout::ShaderRead;
            result.stages = shader;
            result.access = Access::ShaderRead;
            break;
        case TextureUse::Storage:
            result.layout = TextureLayout::ShaderWrite;
            result.stages = shader;
            result.access = Access::ShaderRead | Access::ShaderWrite;
            result.writes = true;
            break;
        case TextureUse::RenderTarget:
            result.layout = TextureLayout::RenderTarget;
            result.stages = PipelineStage::ColorOutput;
            result.access = load == rhi::LoadOp::Load ? Access::RenderTargetRead | Access::RenderTargetWrite
                                                      : Access::RenderTargetWrite;
            result.writes = true;
            result.keepsContents = load == rhi::LoadOp::Load;
            break;
        case TextureUse::DepthWrite:
            // The depth test reads what a load kept; after a clear it reads the clear's values,
            // which rendering orders itself.
            result.layout = TextureLayout::DepthStencilWrite;
            result.stages = PipelineStage::DepthStencil;
            result.access = load == rhi::LoadOp::Load ? Access::DepthStencilRead | Access::DepthStencilWrite
                                                      : Access::DepthStencilWrite;
            result.writes = true;
            result.keepsContents = load == rhi::LoadOp::Load;
            break;
        case TextureUse::DepthSampled:
            result.layout = TextureLayout::DepthStencilRead;
            result.stages = shader;
            result.access = Access::ShaderRead;
            break;
        case TextureUse::CopySrc:
            result.layout = TextureLayout::CopySrc;
            result.stages = PipelineStage::Copy;
            result.access = Access::CopyRead;
            break;
        case TextureUse::CopyDst:
            result.layout = TextureLayout::CopyDst;
            result.stages = PipelineStage::Copy;
            result.access = Access::CopyWrite;
            result.writes = true;
            break;
    }
    return result;
}

// The stages and accesses of a buffer use (ADR-0028 rule 5).
Use bufferUse(BufferUse use, rhi::PipelineStage shader) {
    using rhi::Access;
    using rhi::PipelineStage;
    Use result;
    result.uses = bit(use);
    switch (use) {
        case BufferUse::ShaderRead:
            result.stages = shader;
            result.access = Access::ShaderRead;
            break;
        case BufferUse::Storage:
            result.stages = shader;
            result.access = Access::ShaderRead | Access::ShaderWrite;
            result.writes = true;
            break;
        case BufferUse::Indirect:
            result.stages = PipelineStage::DrawIndirect;
            result.access = Access::IndirectArgument;
            break;
        case BufferUse::Index:
            result.stages = PipelineStage::IndexInput;
            result.access = Access::IndexBuffer;
            break;
        case BufferUse::CopySrc:
            result.stages = PipelineStage::Copy;
            result.access = Access::CopyRead;
            break;
        case BufferUse::CopyDst:
            result.stages = PipelineStage::Copy;
            result.access = Access::CopyWrite;
            result.writes = true;
            break;
    }
    return result;
}

// Whether a texture of `format` may have the use.
bool formatAllows(TextureUse use, rhi::Format format) {
    const bool depthFormat = rhi::isDepthFormat(format);
    switch (use) {
        case TextureUse::Storage:
            return !depthFormat && !isSrgb(format);
        case TextureUse::Sampled:
        case TextureUse::RenderTarget:
            return !depthFormat;
        case TextureUse::DepthWrite:
        case TextureUse::DepthSampled:
            return depthFormat;
        case TextureUse::CopySrc:
        case TextureUse::CopyDst:
            break;
    }
    return true;
}

std::string quoted(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result += '\'';
    result += text;
    result += '\'';
    return result;
}

} // namespace

// --- Arena -----------------------------------------------------------------------------------

void* Arena::allocate(std::size_t size, std::size_t alignment) {
    for (; m_current < m_blocks.size(); ++m_current) {
        if (void* memory = m_blocks[m_current]->allocate(size, alignment)) {
            return memory;
        }
    }
    m_blocks.push_back(std::make_unique<LinearAllocator>(std::max(kBlockSize, size + alignment)));
    m_current = m_blocks.size() - 1;
    void* memory = m_blocks.back()->allocate(size, alignment);
    ITH_ASSERT(memory != nullptr);
    return memory;
}

std::string_view Arena::copy(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    auto* memory = static_cast<char*>(allocate(text.size(), 1));
    std::memcpy(memory, text.data(), text.size());
    return {memory, text.size()};
}

void Arena::reset() {
    for (const std::unique_ptr<LinearAllocator>& block : m_blocks) {
        (*block).reset(); // the allocator, not the pointer
    }
    m_current = 0;
}

// --- Declaration -----------------------------------------------------------------------------

GraphData::GraphData(const RenderGraphDesc& desc) : name(desc.name), settings(desc), serial(nextSerial()) {
    settings.name = name;
    ITH_ASSERT_MSG(settings.passesPerGroup > 0, "RenderGraphDesc::passesPerGroup must be at least 1");
    settings.passesPerGroup = std::max(settings.passesPerGroup, 1u);
    if constexpr (!kValidate) {
        settings.clobberTransients = false;
    }
}

void GraphData::reset() {
    for (const Pass& pass : passes) {
        if (pass.destroy != nullptr) {
            pass.destroy(pass.object);
        }
    }
    passes.clear();
    resources.clear();
    uses.clear();
    alivePasses.clear();
    barriers.clear();
    finalBarriers.clear();
    slots.clear();
    groupCount = 0;
    arena.reset();
    compiled = false;
    executed = false;
    valid = true;
    inSetup = false;
    serial = nextSerial();
}

std::uint32_t GraphData::addResource(const Resource& resource) {
    resources.push_back(resource);
    return static_cast<std::uint32_t>(resources.size() - 1);
}

void GraphData::fail([[maybe_unused]] const std::string& message) {
    valid = false;
    if constexpr (kValidate) {
        ITH_ASSERT_MSG(false, ("render graph '" + name + "': " + message).c_str());
    }
}

std::string GraphData::where(std::uint32_t pass, std::uint32_t resource) const {
    std::string text;
    if (pass != kNone) {
        text += "pass " + quoted(passes[pass].name);
    }
    if (resource != kNone) {
        if (!text.empty()) {
            text += ", ";
        }
        const Resource& r = resources[resource];
        text += (r.kind == ResourceKind::Texture ? "texture " : "buffer ") + quoted(r.name);
    }
    return text;
}

std::uint32_t GraphData::resolve(std::uint32_t index, std::uint32_t handleSerial, ResourceKind kind,
                                 std::uint32_t pass) {
    if (handleSerial == 0) {
        fail(where(pass, kNone) + ": a null resource handle");
        return kNone;
    }
    if (handleSerial != serial || index >= resources.size() || resources[index].kind != kind) {
        fail(where(pass, kNone) + ": a resource handle of another graph, or from before a reset");
        return kNone;
    }
    return index;
}

void GraphData::addUse(std::uint32_t pass, const Use& use) {
    Pass& owner = passes[pass];
    for (std::uint32_t i = owner.firstUse; i < owner.firstUse + owner.useCount; ++i) {
        Use& existing = uses[i];
        if (existing.resource != use.resource) {
            continue;
        }
        const bool bothBufferReads =
            resources[use.resource].kind == ResourceKind::Buffer && !existing.writes && !use.writes;
        const bool same = existing.uses == use.uses && !existing.attachment && !use.attachment;
        if (!same && !bothBufferReads) {
            fail(where(pass, use.resource) + ": used twice in the pass with conflicting uses");
            return;
        }
        existing.uses = static_cast<std::uint16_t>(existing.uses | use.uses);
        existing.stages = existing.stages | use.stages;
        existing.access = existing.access | use.access;
        existing.bufferUsage = existing.bufferUsage | use.bufferUsage;
        return;
    }
    uses.push_back(use);
    ++owner.useCount;
}

bool GraphData::attachmentFits(const Pass& pass, TextureUse use) const {
    std::uint32_t colors = 0;
    bool hasDepth = false;
    for (std::uint32_t i = pass.firstUse; i < pass.firstUse + pass.useCount; ++i) {
        if (uses[i].attachment) {
            colors += uses[i].layout == rhi::TextureLayout::RenderTarget ? 1 : 0;
            hasDepth = hasDepth || uses[i].layout == rhi::TextureLayout::DepthStencilWrite;
        }
    }
    return use == TextureUse::RenderTarget ? colors < rhi::kMaxColorTargets : !hasDepth;
}

void GraphData::declareTexture(std::uint32_t pass, Texture texture, TextureUse use, rhi::PipelineStage stages,
                               bool write, const Use* attachment) {
    ITH_ASSERT_MSG(!compiled, "declare a pass's resources before compile()");
    const std::uint32_t r = resolve(texture.index(), texture.serial(), ResourceKind::Texture, pass);
    if (r == kNone) {
        return;
    }
    const Pass& owner = passes[pass];
    const Resource& resource = resources[r];
    const rhi::PipelineStage defaultStages = shaderStages(owner.type);
    const bool isAttachment = use == TextureUse::RenderTarget || use == TextureUse::DepthWrite;
    const bool isWrite = use == TextureUse::Storage || use == TextureUse::CopyDst;
    // Messages are built only on failure: declaring must not allocate.
    const auto context = [&] { return where(pass, r); };
    if (!isAttachment && write != isWrite) {
        fail(context() + ": " + std::string(toString(use)) + (write ? " is not a write use" : " is not a read use"));
        return;
    }
    if (isAttachment && owner.type != PassType::Raster) {
        fail(context() + ": attachments belong to raster passes");
        return;
    }
    if (owner.type == PassType::Copy && isShaderUse(use)) {
        fail(context() + ": a copy pass has no shader uses");
        return;
    }
    if (stages != rhi::PipelineStage::None && (!isShaderUse(use) || (stages & defaultStages) != stages)) {
        fail(context() + ": the stages narrow only a shader use, to the pass type's shader stages");
        return;
    }
    if (!formatAllows(use, resource.format)) {
        fail(context() + ": " + std::string(toString(use)) + " is not allowed for the format " +
             std::string(toString(resource.format)));
        return;
    }
    if (resource.imported) {
        if (!rhi::hasAny(resource.textureUsage, usageOf(use))) {
            fail(context() + ": " + std::string(toString(use)) + " needs a usage the imported texture lacks");
            return;
        }
        if ((isWrite || isAttachment) && !resource.writable) {
            fail(context() + ": writes a read-only import");
            return;
        }
    }
    if (isAttachment && !attachmentFits(owner, use)) {
        fail(context() + ": more than kMaxColorTargets color attachments or one depth attachment");
        return;
    }
    Use result = textureUse(use, stages == rhi::PipelineStage::None ? defaultStages : stages,
                            attachment != nullptr ? attachment->load : rhi::LoadOp::Load);
    result.resource = r;
    result.textureUsage = usageOf(use);
    if (attachment != nullptr) {
        result.attachment = true;
        result.load = attachment->load;
        result.clearColor = attachment->clearColor;
        result.clearDepth = attachment->clearDepth;
    }
    addUse(pass, result);
}

void GraphData::declareBuffer(std::uint32_t pass, Buffer buffer, BufferUse use, rhi::PipelineStage stages, bool write) {
    ITH_ASSERT_MSG(!compiled, "declare a pass's resources before compile()");
    const std::uint32_t r = resolve(buffer.index(), buffer.serial(), ResourceKind::Buffer, pass);
    if (r == kNone) {
        return;
    }
    const Pass& owner = passes[pass];
    const Resource& resource = resources[r];
    const rhi::PipelineStage defaultStages = shaderStages(owner.type);
    const bool isWrite = use == BufferUse::Storage || use == BufferUse::CopyDst;
    // Messages are built only on failure: declaring must not allocate.
    const auto context = [&] { return where(pass, r); };
    if (write != isWrite) {
        fail(context() + ": " + std::string(toString(use)) + (write ? " is not a write use" : " is not a read use"));
        return;
    }
    if (owner.type == PassType::Copy && isShaderUse(use)) {
        fail(context() + ": a copy pass has no shader uses");
        return;
    }
    if (stages != rhi::PipelineStage::None && (!isShaderUse(use) || (stages & defaultStages) != stages)) {
        fail(context() + ": the stages narrow only a shader use, to the pass type's shader stages");
        return;
    }
    if (resource.imported) {
        if (!rhi::hasAny(resource.bufferUsage, usageOf(use))) {
            fail(context() + ": " + std::string(toString(use)) + " needs a usage the imported buffer lacks");
            return;
        }
        if (isWrite && !resource.writable) {
            fail(context() + ": writes a read-only import");
            return;
        }
    }
    Use result = bufferUse(use, stages == rhi::PipelineStage::None ? defaultStages : stages);
    result.resource = r;
    result.bufferUsage = usageOf(use);
    addUse(pass, result);
}

const Use* GraphData::findUse(std::uint32_t pass, std::uint32_t resource) const {
    const Pass& owner = passes[pass];
    for (std::uint32_t i = owner.firstUse; i < owner.firstUse + owner.useCount; ++i) {
        if (uses[i].resource == resource) {
            return &uses[i];
        }
    }
    return nullptr;
}

const Use* GraphData::contextUse(std::uint32_t pass, std::uint32_t index, std::uint32_t handleSerial, ResourceKind kind,
                                 std::uint16_t allowed, [[maybe_unused]] const char* what) const {
    const bool known = handleSerial == serial && index < resources.size() && resources[index].kind == kind;
    const Use* use = known ? findUse(pass, index) : nullptr;
    if (use == nullptr || (use->uses & allowed) == 0) {
        if constexpr (kValidate) {
            ITH_ASSERT_MSG(false, ("render graph '" + name + "', pass " + quoted(passes[pass].name) + ": " + what +
                                   (known ? " of " + where(kNone, index) +
                                                ", which the pass didn't declare with "
                                                "a matching use"
                                          : " with a handle of another graph, or from before a reset"))
                                      .c_str());
        }
        return nullptr;
    }
    return use;
}

} // namespace detail

using detail::GraphData;
using detail::ResourceKind;

// --- PassBuilder ---------------------------------------------------------------------------

void PassBuilder::read(Texture texture, TextureUse use, rhi::PipelineStage stages) {
    m_graph->declareTexture(m_pass, texture, use, stages, false, nullptr);
}

void PassBuilder::write(Texture texture, TextureUse use, rhi::PipelineStage stages) {
    m_graph->declareTexture(m_pass, texture, use, stages, true, nullptr);
}

void PassBuilder::read(Buffer buffer, BufferUse use, rhi::PipelineStage stages) {
    m_graph->declareBuffer(m_pass, buffer, use, stages, false);
}

void PassBuilder::write(Buffer buffer, BufferUse use, rhi::PipelineStage stages) {
    m_graph->declareBuffer(m_pass, buffer, use, stages, true);
}

void PassBuilder::renderTarget(Texture texture, rhi::LoadOp load, const std::array<float, 4>& clearColor) {
    detail::Use attachment;
    attachment.load = load;
    attachment.clearColor = clearColor;
    m_graph->declareTexture(m_pass, texture, TextureUse::RenderTarget, rhi::PipelineStage::None, true, &attachment);
}

void PassBuilder::depth(Texture texture, rhi::LoadOp load, float clearDepth) {
    detail::Use attachment;
    attachment.load = load;
    attachment.clearDepth = clearDepth;
    m_graph->declareTexture(m_pass, texture, TextureUse::DepthWrite, rhi::PipelineStage::None, true, &attachment);
}

// --- PassContext ---------------------------------------------------------------------------

namespace {

constexpr std::uint16_t kAnyUse = 0xFFFF;

constexpr std::uint16_t useBits(TextureUse use) {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(use));
}

constexpr std::uint16_t useBits(BufferUse use) {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(use));
}

} // namespace

std::string_view PassContext::passName() const {
    return m_graph->passes[m_pass].name;
}

rhi::TextureHandle PassContext::texture(Texture texture) const {
    const detail::Use* use =
        m_graph->contextUse(m_pass, texture.index(), texture.serial(), ResourceKind::Texture, kAnyUse, "texture()");
    return use != nullptr ? m_graph->resources[use->resource].texture : rhi::TextureHandle{};
}

rhi::BufferHandle PassContext::buffer(Buffer buffer) const {
    const detail::Use* use =
        m_graph->contextUse(m_pass, buffer.index(), buffer.serial(), ResourceKind::Buffer, kAnyUse, "buffer()");
    return use != nullptr ? m_graph->resources[use->resource].buffer : rhi::BufferHandle{};
}

std::uint32_t PassContext::sampledIndex(Texture texture) const {
    const detail::Use* use =
        m_graph->contextUse(m_pass, texture.index(), texture.serial(), ResourceKind::Texture,
                            useBits(TextureUse::Sampled) | useBits(TextureUse::DepthSampled), "sampledIndex()");
    return use != nullptr ? m_device->sampledIndex(m_graph->resources[use->resource].texture)
                          : rhi::kInvalidDescriptorIndex;
}

std::uint32_t PassContext::storageIndex(Texture texture) const {
    const detail::Use* use = m_graph->contextUse(m_pass, texture.index(), texture.serial(), ResourceKind::Texture,
                                                 useBits(TextureUse::Storage), "storageIndex()");
    return use != nullptr ? m_device->storageIndex(m_graph->resources[use->resource].texture)
                          : rhi::kInvalidDescriptorIndex;
}

std::uint32_t PassContext::shaderReadIndex(Buffer buffer) const {
    const detail::Use* use = m_graph->contextUse(m_pass, buffer.index(), buffer.serial(), ResourceKind::Buffer,
                                                 useBits(BufferUse::ShaderRead), "shaderReadIndex()");
    return use != nullptr ? m_device->shaderReadIndex(m_graph->resources[use->resource].buffer)
                          : rhi::kInvalidDescriptorIndex;
}

std::uint32_t PassContext::shaderWriteIndex(Buffer buffer) const {
    const detail::Use* use = m_graph->contextUse(m_pass, buffer.index(), buffer.serial(), ResourceKind::Buffer,
                                                 useBits(BufferUse::Storage), "shaderWriteIndex()");
    return use != nullptr ? m_device->shaderWriteIndex(m_graph->resources[use->resource].buffer)
                          : rhi::kInvalidDescriptorIndex;
}

// --- RenderGraph ---------------------------------------------------------------------------

RenderGraph::RenderGraph(const RenderGraphDesc& desc) : m_data(std::make_unique<GraphData>(desc)) {}

RenderGraph::~RenderGraph() {
    m_data->reset();
}

void RenderGraph::reset() {
    m_data->reset();
}

Texture RenderGraph::createTexture(const TextureDesc& desc) {
    ITH_ASSERT_MSG(!m_data->compiled, "create resources before compile()");
    const bool depthOk = desc.dimension == rhi::TextureDimension::Texture3D ? desc.depth > 0 : desc.depth == 1;
    if (desc.width == 0 || desc.height == 0 || !depthOk || desc.layers == 0 || desc.mipLevels == 0 ||
        desc.format == rhi::Format::Unknown || desc.debugName.empty()) {
        m_data->fail("createTexture('" + std::string(desc.debugName) +
                     "'): the size, layers and mips must be positive, the format known and the name not empty");
        return {};
    }
    detail::Resource resource;
    resource.name = m_data->arena.copy(desc.debugName);
    resource.dimension = desc.dimension;
    resource.width = desc.width;
    resource.height = desc.height;
    resource.depth = desc.depth;
    resource.layers = desc.layers;
    resource.mipLevels = desc.mipLevels;
    resource.format = desc.format;
    return Texture::fromParts(m_data->addResource(resource), m_data->serial);
}

Buffer RenderGraph::createBuffer(const BufferDesc& desc) {
    ITH_ASSERT_MSG(!m_data->compiled, "create resources before compile()");
    if (desc.size == 0 || desc.debugName.empty()) {
        m_data->fail("createBuffer('" + std::string(desc.debugName) +
                     "'): the size must be positive and the name not empty");
        return {};
    }
    detail::Resource resource;
    resource.kind = ResourceKind::Buffer;
    resource.name = m_data->arena.copy(desc.debugName);
    resource.size = desc.size;
    return Buffer::fromParts(m_data->addResource(resource), m_data->serial);
}

Texture RenderGraph::importTexture(const TextureImport& import) {
    ITH_ASSERT_MSG(!m_data->compiled, "import resources before compile()");
    const auto name = [&] { return "importTexture('" + std::string(import.desc.debugName) + "')"; };
    if (import.texture.isNull() || import.desc.usage == rhi::TextureUsage::None || import.desc.width == 0 ||
        import.desc.height == 0 || import.desc.format == rhi::Format::Unknown || import.desc.debugName.empty() ||
        import.finalLayout == rhi::TextureLayout::Undefined) {
        m_data->fail(name() + ": needs a texture, its description with usage, size, format and name, and a final "
                              "layout other than Undefined");
        return {};
    }
    if (import.finalLayout && !import.writable) {
        // The final transition changes the layout, which writes the texture.
        m_data->fail(name() + ": a final layout needs a writable import");
        return {};
    }
    for (const detail::Resource& existing : m_data->resources) {
        if (existing.imported && existing.kind == ResourceKind::Texture && existing.texture == import.texture) {
            m_data->fail(name() + ": the texture is already imported as '" + std::string(existing.name) + "'");
            return {};
        }
    }
    detail::Resource resource;
    resource.name = m_data->arena.copy(import.desc.debugName);
    resource.imported = true;
    resource.writable = import.writable;
    resource.dimension = import.desc.dimension;
    resource.width = import.desc.width;
    resource.height = import.desc.height;
    resource.depth = import.desc.depth;
    resource.layers = import.desc.layers;
    resource.mipLevels = import.desc.mipLevels;
    resource.format = import.desc.format;
    resource.textureUsage = import.desc.usage;
    resource.texture = import.texture;
    resource.state = import.state;
    resource.initial = import.state != nullptr ? *import.state : ImportedState{};
    resource.finalLayout = import.finalLayout;
    return Texture::fromParts(m_data->addResource(resource), m_data->serial);
}

Buffer RenderGraph::importBuffer(const BufferImport& import) {
    ITH_ASSERT_MSG(!m_data->compiled, "import resources before compile()");
    const auto name = [&] { return "importBuffer('" + std::string(import.desc.debugName) + "')"; };
    if (import.buffer.isNull() || import.desc.usage == rhi::BufferUsage::None || import.desc.size == 0 ||
        import.desc.debugName.empty()) {
        m_data->fail(name() + ": needs a buffer and its description with usage, size and name");
        return {};
    }
    for (const detail::Resource& existing : m_data->resources) {
        if (existing.imported && existing.kind == ResourceKind::Buffer && existing.buffer == import.buffer) {
            m_data->fail(name() + ": the buffer is already imported as '" + std::string(existing.name) + "'");
            return {};
        }
    }
    detail::Resource resource;
    resource.kind = ResourceKind::Buffer;
    resource.name = m_data->arena.copy(import.desc.debugName);
    resource.imported = true;
    resource.writable = import.writable;
    resource.size = import.desc.size;
    resource.memory = import.desc.memory;
    resource.bufferUsage = import.desc.usage;
    resource.buffer = import.buffer;
    resource.state = import.state;
    resource.initial = import.state != nullptr ? *import.state : ImportedState{};
    return Buffer::fromParts(m_data->addResource(resource), m_data->serial);
}

Texture RenderGraph::importBackbuffer(const rhi::Frame& frame) {
    ITH_ASSERT_MSG(!frame.backbuffer.isNull(), "importBackbuffer(): the frame has no swapchain image");
    return importTexture({.texture = frame.backbuffer,
                          .desc = {.width = frame.width,
                                   .height = frame.height,
                                   .format = frame.backbufferFormat,
                                   .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopyDst,
                                   .debugName = "Backbuffer"},
                          .writable = true,
                          .finalLayout = rhi::TextureLayout::Present});
}

bool RenderGraph::canAddPass() {
    if (m_data->inSetup) {
        m_data->fail("addPass() inside a pass's setup callable; add passes one after another");
        return false;
    }
    return true;
}

void RenderGraph::setupStarted() {
    m_data->inSetup = true;
}

void RenderGraph::setupFinished() {
    m_data->inSetup = false;
}

void* RenderGraph::allocateCallable(std::size_t size, std::size_t alignment) {
    return m_data->arena.allocate(size, alignment);
}

std::uint32_t RenderGraph::beginPass(std::string_view name, PassType type, PassFlags flags, const Callable& callable) {
    ITH_ASSERT_MSG(!m_data->compiled, "add passes before compile()");
    detail::Pass pass;
    pass.name = m_data->arena.copy(name.empty() ? std::string_view("(unnamed)") : name);
    pass.type = type;
    pass.flags = flags;
    pass.object = callable.object;
    pass.call = callable.call;
    pass.destroy = callable.destroy;
    pass.firstUse = static_cast<std::uint32_t>(m_data->uses.size());
    m_data->passes.push_back(pass);
    const auto index = static_cast<std::uint32_t>(m_data->passes.size() - 1);
    if (name.empty()) {
        m_data->fail("a pass needs a name");
    }
    return index;
}

void RenderGraph::compile() {
    ITH_ASSERT_MSG(!m_data->compiled, "compile() once per graph, between resets");
    if (!m_data->compiled) {
        m_data->compile();
    }
}

Expected<void, rhi::Error> RenderGraph::execute(ResourcePool& pool, JobSystem* jobs) {
    return m_data->execute(pool, jobs);
}

std::string RenderGraph::dump() const {
    ITH_ASSERT_MSG(m_data->compiled, "dump() needs a compiled graph");
    return m_data->compiled ? m_data->dump() : std::string();
}

std::string RenderGraph::dumpGraphviz() const {
    ITH_ASSERT_MSG(m_data->compiled, "dumpGraphviz() needs a compiled graph");
    return m_data->compiled ? m_data->dumpGraphviz() : std::string();
}

std::uint32_t RenderGraph::passCount() const {
    return static_cast<std::uint32_t>(m_data->passes.size());
}

std::uint32_t RenderGraph::groupCount() const {
    return m_data->groupCount;
}

bool RenderGraph::isCulled(std::uint32_t pass) const {
    ITH_ASSERT(m_data->compiled && pass < m_data->passes.size());
    return pass < m_data->passes.size() && !m_data->passes[pass].alive;
}

} // namespace ith::rg
