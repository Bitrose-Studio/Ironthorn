#include "ith/rhi/Device.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/DeviceState.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ith::rhi {
namespace detail {
namespace {

Unexpected<Error> fail(ErrorCode code, std::string message) {
    return makeUnexpected(Error{.code = code, .message = std::move(message)});
}

std::string quoted(std::string_view name) {
    std::string text = "'";
    text += name;
    text += '\'';
    return text;
}

std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    std::uint32_t largest = std::max({width, height, depth});
    std::uint32_t count = 1;
    while (largest > 1) {
        largest /= 2;
        ++count;
    }
    return count;
}

// Each check returns the broken rule, or nullptr when the description is valid.

const char* checkBuffer(const BufferDesc& desc, const Limits& limits) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    if (desc.size == 0 || desc.size > limits.maxBufferSize) {
        return "a buffer's size is from 1 byte to Limits::maxBufferSize";
    }
    if (desc.usage == BufferUsage::None) {
        return "a buffer needs at least one usage";
    }
    if (desc.memory == MemoryType::Upload && hasAny(desc.usage, BufferUsage::ShaderWrite | BufferUsage::CopyDst)) {
        return "an Upload buffer can't have ShaderWrite or CopyDst";
    }
    if (desc.memory == MemoryType::Readback && desc.usage != BufferUsage::CopyDst) {
        return "a Readback buffer has only CopyDst";
    }
    return nullptr;
}

const char* checkTexture(const TextureDesc& desc, const Limits& limits) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.layers == 0) {
        return "a texture's width, height, depth and layers must be positive";
    }
    if (desc.format == Format::Unknown) {
        return "a texture needs a format";
    }
    if (desc.usage == TextureUsage::None) {
        return "a texture needs at least one usage";
    }
    if (desc.dimension == TextureDimension::Texture3D ? desc.layers != 1 : desc.depth != 1) {
        return "only 3D textures have a depth, and they have one layer";
    }
    const bool withinLimits = desc.dimension == TextureDimension::Texture3D
                                  ? std::max({desc.width, desc.height, desc.depth}) <= limits.maxTextureDimension3D
                                  : std::max(desc.width, desc.height) <= limits.maxTextureDimension2D &&
                                        desc.layers <= limits.maxTextureLayers;
    if (!withinLimits) {
        return "a texture's size and layers are within Limits";
    }
    if (desc.dimension == TextureDimension::TextureCube && (desc.width != desc.height || desc.layers % 6 != 0)) {
        return "a cube texture is square and has a multiple of 6 layers";
    }
    if (desc.mipLevels == 0 || desc.mipLevels > fullMipCount(desc.width, desc.height, desc.depth)) {
        return "a texture has from 1 mip level to its full mip chain";
    }
    const bool depthFormat = isDepthFormat(desc.format);
    if (hasAny(desc.usage, TextureUsage::DepthStencil) != depthFormat) {
        return "DepthStencil usage needs a depth format, and a depth format needs DepthStencil usage";
    }
    if (depthFormat && (hasAny(desc.usage, TextureUsage::RenderTarget | TextureUsage::Storage) ||
                        desc.dimension == TextureDimension::Texture3D)) {
        return "a depth texture can't be a render target, a storage texture or 3D";
    }
    return nullptr;
}

const char* checkSampler(const SamplerDesc& desc) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    const bool anisotropyInRange = desc.maxAnisotropy >= 1.0f && desc.maxAnisotropy <= 16.0f; // false for NaN
    if (!anisotropyInRange) {
        return "a sampler's maxAnisotropy is from 1 to 16";
    }
    const bool lodRangeValid = desc.minLod <= desc.maxLod;
    if (!lodRangeValid) {
        return "a sampler's minLod is at most its maxLod";
    }
    return nullptr;
}

const char* checkSwapchain(const SwapchainDesc& desc) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    if (desc.width == 0 || desc.height == 0) {
        return "a swapchain is created with a positive size";
    }
    switch (desc.format) {
        case Format::B8G8R8A8Srgb:
        case Format::B8G8R8A8Unorm:
        case Format::R8G8B8A8Srgb:
        case Format::R8G8B8A8Unorm:
        case Format::R10G10B10A2Unorm:
            return nullptr;
        default:
            return "a swapchain's format is B8G8R8A8, R8G8B8A8 (Srgb or Unorm) or R10G10B10A2Unorm";
    }
}

const char* checkShader(const ShaderDesc& shader) {
    if (shader.code.empty()) {
        return "a shader needs bytecode";
    }
    if (shader.entryPoint.empty()) {
        return "a shader needs an entry point";
    }
    return nullptr;
}

const char* checkGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    std::uint32_t vertex = 0;
    std::uint32_t pixel = 0;
    for (const ShaderDesc& shader : desc.shaders) {
        if (const char* problem = checkShader(shader)) {
            return problem;
        }
        switch (shader.stage) {
            case ShaderStage::Vertex:
                ++vertex;
                break;
            case ShaderStage::Pixel:
                ++pixel;
                break;
            case ShaderStage::Compute:
                return "a graphics pipeline can't have a compute shader";
        }
    }
    if (vertex != 1 || pixel > 1) {
        return "a graphics pipeline has one vertex shader and at most one pixel shader";
    }
    if (desc.colorTargets.size() > kMaxColorTargets) {
        return "a graphics pipeline has at most kMaxColorTargets color targets";
    }
    for (const ColorTarget& target : desc.colorTargets) {
        if (target.format == Format::Unknown || isDepthFormat(target.format)) {
            return "a color target needs a color format";
        }
    }
    if (desc.depthFormat != Format::Unknown && !isDepthFormat(desc.depthFormat)) {
        return "a pipeline's depth format must be a depth format";
    }
    if (desc.colorTargets.empty() && desc.depthFormat == Format::Unknown) {
        return "a graphics pipeline needs a color target or a depth format";
    }
    return nullptr;
}

const char* checkComputePipeline(const ComputePipelineDesc& desc) {
    if (desc.debugName.empty()) {
        return "every RHI object needs a debug name";
    }
    if (desc.shader.stage != ShaderStage::Compute) {
        return "a compute pipeline needs a compute shader";
    }
    return checkShader(desc.shader);
}

// The constants-size rule of ADR-0009 rule 16: data from the shader pipeline, so an error, not
// an assert.
std::optional<Error> checkConstants(std::span<const ShaderDesc> shaders, std::string_view pipelineName) {
    for (const ShaderDesc& shader : shaders) {
        if (shader.constantsSize > kMaxPushConstantSize) {
            return Error{.code = ErrorCode::InvalidShader,
                         .message = "Shader '" + std::string(shader.entryPoint) + "' of pipeline " +
                                    quoted(pipelineName) + " declares " + std::to_string(shader.constantsSize) +
                                    " bytes of constants; the limit is " + std::to_string(kMaxPushConstantSize)};
        }
    }
    return std::nullopt;
}

} // namespace

Error unsupportedDeviceError(std::string_view adapterName, std::span<const std::string_view> missingFeatures) {
    std::string message = "The GPU " + quoted(adapterName) + " lacks features Ironthorn needs: ";
    for (std::size_t index = 0; index < missingFeatures.size(); ++index) {
        if (index != 0) {
            message += ", ";
        }
        message += missingFeatures[index];
    }
    return Error{.code = ErrorCode::UnsupportedDevice, .message = std::move(message)};
}

Device makeDevice(std::unique_ptr<DeviceBackend> backend) {
    ITH_ASSERT_MSG(backend != nullptr, "makeDevice needs a backend");
    return Device(std::make_unique<DeviceState>(std::move(backend)));
}

// --- DeviceState --------------------------------------------------------------------------

DeviceState::DeviceState(std::unique_ptr<DeviceBackend> backend)
    : capabilities(backend->capabilities()),
      buffers(capabilities.limits.buffers),
      textures(capabilities.limits.textures),
      samplers(capabilities.limits.samplers),
      pipelines(capabilities.limits.pipelines),
      swapchains(capabilities.limits.swapchains),
      m_backend(std::move(backend)),
      m_resourceDescriptors(capabilities.limits.resourceDescriptors),
      m_samplerDescriptors(capabilities.limits.samplerDescriptors),
      m_deadList(std::make_unique<CommandList>(CommandList::Key{}, *this)) {
    m_deadList->m_debugName = "Opened outside a frame";
    m_deadList->m_error =
        Error{.code = ErrorCode::InvalidUsage, .message = "A command list was opened outside a frame"};
    m_deadList->m_state.store(CommandList::State::Failed, std::memory_order_relaxed);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads m_renderThread when asserts are on
void DeviceState::assertRenderThread() const {
    ITH_ASSERT_MSG(std::this_thread::get_id() == m_renderThread,
                   "this rhi::Device call belongs to the render thread, the thread that created the device");
}

DeviceState::~DeviceState() {
    assertRenderThread();
    std::uint64_t lastSubmitted = 0;
    {
        const MutexLock lock(m_mutex);
        ITH_ASSERT_MSG(!m_inFrame, "rhi::Device destroyed while a frame is open");
        ITH_ASSERT_MSG(m_openLists == 0, "rhi::Device destroyed while command lists are open");
        lastSubmitted = m_lastSubmitted;
    }
    if (!isLost()) {
        // A failure here means the device was lost; everything is released below either way.
        static_cast<void>(m_backend->waitForValue(lastSubmitted));
    }
    collectGarbage(true);

    // Objects the caller did not destroy. Swapchains own their image slots.
    for (std::uint32_t index = 0; index < swapchains.capacity(); ++index) {
        if (swapchains.isLiveIndex(index)) {
            m_backend->destroySwapchain(index);
        }
    }
    for (std::uint32_t index = 0; index < buffers.capacity(); ++index) {
        if (buffers.isLiveIndex(index)) {
            m_backend->destroyBuffer(index);
        }
    }
    for (std::uint32_t index = 0; index < textures.capacity(); ++index) {
        if (textures.isLiveIndex(index) && !textures.data(index).swapchainImage) {
            m_backend->destroyTexture(index);
        }
    }
    for (std::uint32_t index = 0; index < samplers.capacity(); ++index) {
        if (samplers.isLiveIndex(index)) {
            m_backend->destroySampler(index);
        }
    }
    for (std::uint32_t index = 0; index < pipelines.capacity(); ++index) {
        if (pipelines.isLiveIndex(index)) {
            m_backend->destroyPipeline(index);
        }
    }
    {
        const MutexLock lock(m_mutex);
        for (auto& pool : m_lists) {
            pool.clear(); // the backend's command lists go before the backend
        }
    }
    m_deadList.reset();
    m_backend.reset();
}

std::optional<Error> DeviceState::lostError() const {
    if (!isLost()) {
        return std::nullopt;
    }
    const MutexLock lock(m_mutex);
    return Error{.code = ErrorCode::DeviceLost, .message = m_lostMessage};
}

Error DeviceState::noteError(Error error) {
    if (error.code == ErrorCode::DeviceLost && !isLost()) {
        const MutexLock lock(m_mutex);
        m_lostMessage = error.message;
        m_lost.store(true, std::memory_order_release);
    }
    return error;
}

Error DeviceState::invalidUsage(const char* message) {
    ITH_ASSERT_MSG(false, message);
    return Error{.code = ErrorCode::InvalidUsage, .message = message};
}

template <typename Data>
Expected<std::uint32_t, Error> DeviceState::allocateSlot(SlotTable<Data>& table, std::string_view kind,
                                                         std::string_view name) {
    const MutexLock lock(m_mutex);
    const std::optional<std::uint32_t> index = table.allocate();
    if (!index) {
        return fail(ErrorCode::OutOfHandles, "No " + std::string(kind) + " slot is free for " + quoted(name) +
                                                 " (limit " + std::to_string(table.capacity()) + ")");
    }
    return *index;
}

void DeviceState::retire(ObjectKind kind, std::uint32_t index) {
    if (m_openLists > 0) {
        m_waitingForLists.push_back(Retired{.kind = kind, .index = index});
    } else {
        m_retired.push_back(Retired{.kind = kind, .index = index, .value = m_lastSubmitted});
    }
}

void DeviceState::closeLists(std::size_t count) {
    ITH_ASSERT(count <= m_openLists);
    m_openLists -= std::min(count, m_openLists);
    if (m_openLists == 0) {
        for (Retired& retired : m_waitingForLists) {
            retired.value = m_lastSubmitted;
            m_retired.push_back(retired);
        }
        m_waitingForLists.clear();
    }
}

void DeviceState::collectGarbage(bool all) {
    const std::uint64_t completed = all ? std::numeric_limits<std::uint64_t>::max() : m_backend->completedValue();
    std::vector<Retired> ready;
    {
        const MutexLock lock(m_mutex);
        while (!m_retired.empty() && m_retired.front().value <= completed) {
            ready.push_back(m_retired.front());
            m_retired.pop_front();
        }
        if (all) {
            ready.insert(ready.end(), m_waitingForLists.begin(), m_waitingForLists.end());
            m_waitingForLists.clear();
        }
    }
    for (const Retired& retired : ready) {
        release(retired.kind, retired.index);
    }
}

void DeviceState::release(ObjectKind kind, std::uint32_t index) {
    switch (kind) {
        case ObjectKind::Buffer:
            m_backend->destroyBuffer(index);
            break;
        case ObjectKind::Texture:
            m_backend->destroyTexture(index);
            break;
        case ObjectKind::Sampler:
            m_backend->destroySampler(index);
            break;
        case ObjectKind::Pipeline:
            m_backend->destroyPipeline(index);
            break;
    }
    const MutexLock lock(m_mutex);
    switch (kind) {
        case ObjectKind::Buffer:
            m_resourceDescriptors.release(buffers.data(index).descriptors.read);
            m_resourceDescriptors.release(buffers.data(index).descriptors.write);
            buffers.release(index);
            break;
        case ObjectKind::Texture:
            m_resourceDescriptors.release(textures.data(index).descriptors.sampled);
            m_resourceDescriptors.release(textures.data(index).descriptors.storage);
            textures.release(index);
            break;
        case ObjectKind::Sampler:
            m_samplerDescriptors.release(samplers.data(index).descriptor);
            samplers.release(index);
            break;
        case ObjectKind::Pipeline:
            pipelines.release(index);
            break;
    }
}

std::size_t DeviceState::pendingDestructionCount() const {
    const MutexLock lock(m_mutex);
    return m_retired.size() + m_waitingForLists.size();
}

// --- Creation -----------------------------------------------------------------------------

Expected<BufferHandle, Error> DeviceState::createBuffer(const BufferDesc& desc) {
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkBuffer(desc, capabilities.limits)) {
        return makeUnexpected(invalidUsage(problem));
    }
    Expected<std::uint32_t, Error> index = allocateSlot(buffers, "buffer", desc.debugName);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    BufferDescriptors descriptors;
    {
        const MutexLock lock(m_mutex);
        std::optional<std::uint32_t> read;
        std::optional<std::uint32_t> write;
        bool ok = true;
        if (hasAny(desc.usage, BufferUsage::ShaderRead)) {
            read = m_resourceDescriptors.allocate();
            ok = read.has_value();
        }
        if (ok && hasAny(desc.usage, BufferUsage::ShaderWrite)) {
            write = m_resourceDescriptors.allocate();
            ok = write.has_value();
        }
        if (!ok) {
            m_resourceDescriptors.release(read.value_or(kInvalidDescriptorIndex));
            buffers.invalidate(*index);
            buffers.release(*index);
            return fail(ErrorCode::OutOfDescriptors, "No bindless descriptor is free for buffer " +
                                                         quoted(desc.debugName) + " (limit " +
                                                         std::to_string(capabilities.limits.resourceDescriptors) + ")");
        }
        descriptors.read = read.value_or(kInvalidDescriptorIndex);
        descriptors.write = write.value_or(kInvalidDescriptorIndex);
    }
    Expected<std::byte*, Error> created = m_backend->createBuffer(*index, desc, descriptors);
    if (!created) {
        {
            const MutexLock lock(m_mutex);
            m_resourceDescriptors.release(descriptors.read);
            m_resourceDescriptors.release(descriptors.write);
            buffers.invalidate(*index);
            buffers.release(*index);
        }
        return makeUnexpected(noteError(std::move(created).error()));
    }
    buffers.data(*index) = BufferSlot{
        .size = desc.size, .mapped = *created, .descriptors = descriptors, .usage = desc.usage, .memory = desc.memory};
    return buffers.handle<BufferTag>(*index);
}

Expected<TextureHandle, Error> DeviceState::createTexture(const TextureDesc& desc) {
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkTexture(desc, capabilities.limits)) {
        return makeUnexpected(invalidUsage(problem));
    }
    Expected<std::uint32_t, Error> index = allocateSlot(textures, "texture", desc.debugName);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    TextureDescriptors descriptors;
    {
        const MutexLock lock(m_mutex);
        std::optional<std::uint32_t> sampled;
        std::optional<std::uint32_t> storage;
        bool ok = true;
        if (hasAny(desc.usage, TextureUsage::Sampled)) {
            sampled = m_resourceDescriptors.allocate();
            ok = sampled.has_value();
        }
        if (ok && hasAny(desc.usage, TextureUsage::Storage)) {
            storage = m_resourceDescriptors.allocate();
            ok = storage.has_value();
        }
        if (!ok) {
            m_resourceDescriptors.release(sampled.value_or(kInvalidDescriptorIndex));
            textures.invalidate(*index);
            textures.release(*index);
            return fail(ErrorCode::OutOfDescriptors, "No bindless descriptor is free for texture " +
                                                         quoted(desc.debugName) + " (limit " +
                                                         std::to_string(capabilities.limits.resourceDescriptors) + ")");
        }
        descriptors.sampled = sampled.value_or(kInvalidDescriptorIndex);
        descriptors.storage = storage.value_or(kInvalidDescriptorIndex);
    }
    Expected<void, Error> created = m_backend->createTexture(*index, desc, descriptors);
    if (!created) {
        {
            const MutexLock lock(m_mutex);
            m_resourceDescriptors.release(descriptors.sampled);
            m_resourceDescriptors.release(descriptors.storage);
            textures.invalidate(*index);
            textures.release(*index);
        }
        return makeUnexpected(noteError(std::move(created).error()));
    }
    textures.data(*index) = TextureSlot{.width = desc.width,
                                        .height = desc.height,
                                        .depth = desc.depth,
                                        .layers = desc.layers,
                                        .mipLevels = desc.mipLevels,
                                        .descriptors = descriptors,
                                        .dimension = desc.dimension,
                                        .format = desc.format,
                                        .usage = desc.usage};
    return textures.handle<TextureTag>(*index);
}

Expected<SamplerHandle, Error> DeviceState::createSampler(const SamplerDesc& desc) {
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkSampler(desc)) {
        return makeUnexpected(invalidUsage(problem));
    }
    Expected<std::uint32_t, Error> index = allocateSlot(samplers, "sampler", desc.debugName);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    std::uint32_t descriptor = kInvalidDescriptorIndex;
    {
        const MutexLock lock(m_mutex);
        const std::optional<std::uint32_t> allocated = m_samplerDescriptors.allocate();
        if (!allocated) {
            samplers.invalidate(*index);
            samplers.release(*index);
            return fail(ErrorCode::OutOfDescriptors, "No sampler descriptor is free for " + quoted(desc.debugName) +
                                                         " (limit " +
                                                         std::to_string(capabilities.limits.samplerDescriptors) + ")");
        }
        descriptor = *allocated;
    }
    Expected<void, Error> created = m_backend->createSampler(*index, desc, descriptor);
    if (!created) {
        {
            const MutexLock lock(m_mutex);
            m_samplerDescriptors.release(descriptor);
            samplers.invalidate(*index);
            samplers.release(*index);
        }
        return makeUnexpected(noteError(std::move(created).error()));
    }
    samplers.data(*index) = SamplerSlot{.descriptor = descriptor};
    return samplers.handle<SamplerTag>(*index);
}

Expected<PipelineHandle, Error> DeviceState::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkGraphicsPipeline(desc)) {
        return makeUnexpected(invalidUsage(problem));
    }
    if (std::optional<Error> constants = checkConstants(desc.shaders, desc.debugName)) {
        return makeUnexpected(std::move(*constants));
    }
    Expected<std::uint32_t, Error> index = allocateSlot(pipelines, "pipeline", desc.debugName);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    Expected<void, Error> created = m_backend->createGraphicsPipeline(*index, desc);
    if (!created) {
        {
            const MutexLock lock(m_mutex);
            pipelines.invalidate(*index);
            pipelines.release(*index);
        }
        return makeUnexpected(noteError(std::move(created).error()));
    }
    pipelines.data(*index) = PipelineSlot{.kind = PipelineKind::Graphics};
    return pipelines.handle<PipelineTag>(*index);
}

Expected<PipelineHandle, Error> DeviceState::createComputePipeline(const ComputePipelineDesc& desc) {
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkComputePipeline(desc)) {
        return makeUnexpected(invalidUsage(problem));
    }
    if (std::optional<Error> constants = checkConstants(std::span<const ShaderDesc>(&desc.shader, 1), desc.debugName)) {
        return makeUnexpected(std::move(*constants));
    }
    Expected<std::uint32_t, Error> index = allocateSlot(pipelines, "pipeline", desc.debugName);
    if (!index) {
        return makeUnexpected(std::move(index).error());
    }
    Expected<void, Error> created = m_backend->createComputePipeline(*index, desc);
    if (!created) {
        {
            const MutexLock lock(m_mutex);
            pipelines.invalidate(*index);
            pipelines.release(*index);
        }
        return makeUnexpected(noteError(std::move(created).error()));
    }
    pipelines.data(*index) = PipelineSlot{.kind = PipelineKind::Compute};
    return pipelines.handle<PipelineTag>(*index);
}

// --- Destruction --------------------------------------------------------------------------

void DeviceState::destroy(BufferHandle buffer) {
    if (buffer.isNull()) {
        return;
    }
    const MutexLock lock(m_mutex);
    if (!buffers.isLive(buffer)) {
        ITH_ASSERT_MSG(false, "destroying a stale buffer handle");
        return;
    }
    buffers.invalidate(buffer.index());
    retire(ObjectKind::Buffer, buffer.index());
}

void DeviceState::destroy(TextureHandle texture) {
    if (texture.isNull()) {
        return;
    }
    const MutexLock lock(m_mutex);
    if (!textures.isLive(texture)) {
        ITH_ASSERT_MSG(false, "destroying a stale texture handle");
        return;
    }
    if (textures.data(texture.index()).swapchainImage) {
        ITH_ASSERT_MSG(false, "swapchain images belong to their swapchain and can't be destroyed");
        return;
    }
    textures.invalidate(texture.index());
    retire(ObjectKind::Texture, texture.index());
}

void DeviceState::destroy(SamplerHandle sampler) {
    if (sampler.isNull()) {
        return;
    }
    const MutexLock lock(m_mutex);
    if (!samplers.isLive(sampler)) {
        ITH_ASSERT_MSG(false, "destroying a stale sampler handle");
        return;
    }
    samplers.invalidate(sampler.index());
    retire(ObjectKind::Sampler, sampler.index());
}

void DeviceState::destroy(PipelineHandle pipeline) {
    if (pipeline.isNull()) {
        return;
    }
    const MutexLock lock(m_mutex);
    if (!pipelines.isLive(pipeline)) {
        ITH_ASSERT_MSG(false, "destroying a stale pipeline handle");
        return;
    }
    pipelines.invalidate(pipeline.index());
    retire(ObjectKind::Pipeline, pipeline.index());
}

// --- Swapchains ---------------------------------------------------------------------------

void DeviceState::writeImageData(const SwapchainSlot& swapchain) {
    for (std::uint32_t image = 0; image < kMaxSwapchainImages; ++image) {
        const bool used = image < swapchain.images.count;
        textures.data(swapchain.imageSlots[image]) =
            TextureSlot{.width = used ? swapchain.images.width : 0,
                        .height = used ? swapchain.images.height : 0,
                        .format = swapchain.images.format,
                        .usage = TextureUsage::RenderTarget | TextureUsage::CopyDst,
                        .swapchainImage = true};
    }
}

Expected<void, Error> DeviceState::reissueImages(SwapchainSlot& swapchain) {
    bool reissued = true;
    {
        const MutexLock lock(m_mutex);
        for (const std::uint32_t index : swapchain.imageSlots) {
            reissued = textures.reissue(index) && reissued;
        }
    }
    writeImageData(swapchain);
    if (!reissued) {
        return fail(ErrorCode::OutOfHandles, "The image handles of swapchain " + quoted(swapchain.name) +
                                                 " are used up; destroy and create the swapchain again");
    }
    return {};
}

Expected<SwapchainHandle, Error> DeviceState::createSwapchain(const SwapchainDesc& desc) {
    assertRenderThread();
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    if (const char* problem = checkSwapchain(desc)) {
        return makeUnexpected(invalidUsage(problem));
    }
    std::uint32_t index = 0;
    std::array<std::uint32_t, kMaxSwapchainImages> imageSlots{};
    {
        const MutexLock lock(m_mutex);
        const std::optional<std::uint32_t> slot = swapchains.allocate();
        std::uint32_t reserved = 0;
        for (; slot && reserved < kMaxSwapchainImages; ++reserved) {
            const std::optional<std::uint32_t> image = textures.allocate();
            if (!image) {
                break;
            }
            imageSlots[reserved] = *image;
        }
        if (!slot || reserved < kMaxSwapchainImages) {
            for (std::uint32_t image = 0; image < reserved; ++image) {
                textures.invalidate(imageSlots[image]);
                textures.release(imageSlots[image]);
            }
            if (slot) {
                swapchains.invalidate(*slot);
                swapchains.release(*slot);
            }
            return fail(ErrorCode::OutOfHandles,
                        "No swapchain or swapchain image slot is free for " + quoted(desc.debugName));
        }
        index = *slot;
    }
    Expected<SwapchainImages, Error> images = m_backend->createSwapchain(index, desc, imageSlots);
    if (!images) {
        {
            const MutexLock lock(m_mutex);
            for (const std::uint32_t image : imageSlots) {
                textures.invalidate(image);
                textures.release(image);
            }
            swapchains.invalidate(index);
            swapchains.release(index);
        }
        return makeUnexpected(noteError(std::move(images).error()));
    }
    SwapchainSlot& swapchain = swapchains.data(index);
    swapchain = SwapchainSlot{.imageSlots = imageSlots,
                              .images = *images,
                              .requestedWidth = desc.width,
                              .requestedHeight = desc.height,
                              .name = std::string(desc.debugName)};
    writeImageData(swapchain);
    return swapchains.handle<SwapchainTag>(index);
}

void DeviceState::resizeSwapchain(SwapchainHandle swapchain, std::uint32_t width, std::uint32_t height) {
    assertRenderThread();
    if (!swapchains.isLive(swapchain)) {
        ITH_ASSERT_MSG(false, "resizing a stale or null swapchain handle");
        return;
    }
    SwapchainSlot& slot = swapchains.data(swapchain.index());
    slot.requestedWidth = width;
    slot.requestedHeight = height;
}

Expected<void, Error> DeviceState::rebuildSwapchain(std::uint32_t index) {
    std::uint64_t lastSubmitted = 0;
    {
        const MutexLock lock(m_mutex);
        lastSubmitted = m_lastSubmitted;
    }
    if (Expected<void, Error> idle = m_backend->waitForValue(lastSubmitted); !idle) {
        return makeUnexpected(noteError(std::move(idle).error()));
    }
    SwapchainSlot& swapchain = swapchains.data(index);
    Expected<SwapchainImages, Error> images =
        m_backend->resizeSwapchain(index, swapchain.requestedWidth, swapchain.requestedHeight);
    if (!images) {
        // The old images may be gone: their handles go stale, and the next beginFrame() tries
        // again (Backend.h).
        swapchain.images.count = 0;
        swapchain.images.width = 0;
        swapchain.images.height = 0;
        swapchain.outOfDate = true;
        static_cast<void>(reissueImages(swapchain));
        return makeUnexpected(noteError(std::move(images).error()));
    }
    swapchain.images = *images;
    swapchain.outOfDate = false;
    return reissueImages(swapchain);
}

void DeviceState::destroy(SwapchainHandle swapchain) {
    assertRenderThread();
    if (swapchain.isNull()) {
        return;
    }
    std::uint64_t lastSubmitted = 0;
    {
        const MutexLock lock(m_mutex);
        if (!swapchains.isLive(swapchain)) {
            ITH_ASSERT_MSG(false, "destroying a stale swapchain handle");
            return;
        }
        if (m_inFrame && m_frameSwapchain == swapchain) {
            ITH_ASSERT_MSG(false, "destroying the swapchain of the open frame");
            return;
        }
        lastSubmitted = m_lastSubmitted;
    }
    if (!isLost()) {
        // On failure the device is lost and the GPU no longer runs anything.
        if (Expected<void, Error> idle = m_backend->waitForValue(lastSubmitted); !idle) {
            static_cast<void>(noteError(std::move(idle).error()));
        }
    }
    m_backend->destroySwapchain(swapchain.index());
    const MutexLock lock(m_mutex);
    for (const std::uint32_t image : swapchains.data(swapchain.index()).imageSlots) {
        textures.invalidate(image);
        textures.release(image);
    }
    swapchains.invalidate(swapchain.index());
    swapchains.release(swapchain.index());
}

// --- Frames -------------------------------------------------------------------------------

Expected<std::optional<Frame>, Error> DeviceState::beginFrame(SwapchainHandle swapchain) {
    assertRenderThread();
    if (std::optional<Error> lost = lostError()) {
        return makeUnexpected(std::move(*lost));
    }
    {
        const MutexLock lock(m_mutex);
        if (m_inFrame) {
            return makeUnexpected(invalidUsage("beginFrame() while a frame is open"));
        }
    }
    if (!swapchain.isNull() && !swapchains.isLive(swapchain)) {
        return makeUnexpected(invalidUsage("beginFrame() with a stale swapchain handle"));
    }

    // Frame pacing (ADR-0009 rule 14): the frame kFramesInFlight frames back must be done
    // before its per-frame resources are reused.
    const std::uint64_t number = frameNumber();
    const auto slot = static_cast<std::uint32_t>(number % kFramesInFlight);
    if (Expected<void, Error> waited = m_backend->waitForValue(m_frameValues[slot]); !waited) {
        return makeUnexpected(noteError(std::move(waited).error()));
    }
    collectGarbage(false);
    {
        const MutexLock lock(m_mutex);
        m_listsUsed[slot] = 0;
    }
    m_backend->beginFrame(slot);

    Frame frame{.number = number, .slot = slot};
    if (!swapchain.isNull()) {
        SwapchainSlot& chain = swapchains.data(swapchain.index());
        if (chain.requestedWidth == 0 || chain.requestedHeight == 0) {
            return std::optional<Frame>();
        }
        if (chain.outOfDate || chain.requestedWidth != chain.images.width ||
            chain.requestedHeight != chain.images.height) {
            if (Expected<void, Error> rebuilt = rebuildSwapchain(swapchain.index()); !rebuilt) {
                return makeUnexpected(std::move(rebuilt).error());
            }
        }
        Expected<AcquireResult, Error> acquired = m_backend->acquireImage(swapchain.index(), slot);
        if (acquired && acquired->status == AcquireStatus::OutOfDate) {
            if (Expected<void, Error> rebuilt = rebuildSwapchain(swapchain.index()); !rebuilt) {
                return makeUnexpected(std::move(rebuilt).error());
            }
            acquired = m_backend->acquireImage(swapchain.index(), slot);
        }
        if (!acquired) {
            return makeUnexpected(noteError(std::move(acquired).error()));
        }
        if (acquired->status == AcquireStatus::OutOfDate) {
            return std::optional<Frame>(); // still being recreated; try again next frame
        }
        if (acquired->imageIndex >= chain.images.count) {
            return makeUnexpected(Error{
                .code = ErrorCode::BackendFailure,
                .message = "The backend acquired image " + std::to_string(acquired->imageIndex) + " of swapchain " +
                           quoted(chain.name) + ", which has " + std::to_string(chain.images.count) + " images"});
        }
        frame.backbuffer = textures.handle<TextureTag>(chain.imageSlots[acquired->imageIndex]);
        frame.backbufferFormat = chain.images.format;
        frame.width = chain.images.width;
        frame.height = chain.images.height;
    }

    const MutexLock lock(m_mutex);
    m_inFrame = true;
    m_frameSlot = slot;
    m_frameSwapchain = swapchain;
    return std::optional<Frame>(frame);
}

CommandList& DeviceState::openCommandList(std::string_view debugName) {
    ITH_ASSERT_MSG(!debugName.empty(), "every RHI object needs a debug name");
    CommandList* list = nullptr;
    std::uint32_t slot = 0;
    {
        const MutexLock lock(m_mutex);
        if (!m_inFrame) {
            // Neither the pool nor the open-list count changes: the dead list records nothing,
            // and submit() and discard() leave the counts alone.
            ITH_ASSERT_MSG(false, "openCommandList() outside a frame");
            return *m_deadList;
        }
        slot = m_frameSlot;
        std::vector<std::unique_ptr<CommandList>>& pool = m_lists[slot];
        if (m_listsUsed[slot] == pool.size()) {
            pool.push_back(std::make_unique<CommandList>(CommandList::Key{}, *this));
        }
        list = pool[m_listsUsed[slot]++].get();
        ++m_openLists;
        // Failed until the backend has begun it; endFrame() may close it at any time.
        list->m_state.store(CommandList::State::Failed, std::memory_order_release);
        if (list->m_backend == nullptr) {
            Expected<std::unique_ptr<CommandListBackend>, Error> created = m_backend->createCommandList();
            if (created) {
                list->m_backend = std::move(*created);
            } else {
                list->m_error = std::move(created).error();
            }
        }
    }

    list->m_debugName = debugName;
    list->m_frameNumber = frameNumber();
    list->m_frameSlot = slot;
    list->m_rendering = false;
    list->m_indexBufferSet = false;
    list->m_pipeline = CommandList::BoundPipeline::None;
    list->m_markerDepth = 0;
    if (list->m_backend != nullptr) {
        if (Expected<void, Error> begun = list->m_backend->begin(slot, debugName); begun) {
            CommandList::State expected = CommandList::State::Failed;
            list->m_state.compare_exchange_strong(expected, CommandList::State::Open, std::memory_order_acq_rel);
        } else {
            list->m_error = noteError(std::move(begun).error());
        }
    }
    return *list;
}

Expected<void, Error> DeviceState::submit(std::span<CommandList* const> lists) {
    assertRenderThread();
    std::vector<CommandListBackend*> backendLists;
    backendLists.reserve(lists.size());
    std::optional<Error> failure;
    std::size_t closed = 0;
    {
        const MutexLock lock(m_mutex);
        ITH_ASSERT_MSG(m_inFrame, "submit() outside a frame");
    }
    for (CommandList* list : lists) {
        if (list == m_deadList.get()) {
            if (!failure) {
                failure = list->m_error;
            }
            continue;
        }
        if (list == nullptr || list->m_device != this) {
            ITH_ASSERT_MSG(false, "submitting a command list of another device");
            continue;
        }
        const CommandList::State state = list->m_state.exchange(CommandList::State::Closed, std::memory_order_acq_rel);
        if (state == CommandList::State::Closed) {
            ITH_ASSERT_MSG(false, "submitting a command list that is not open");
            continue;
        }
        ITH_ASSERT_MSG(!list->m_rendering, "submitting a command list inside beginRendering()");
        ITH_ASSERT_MSG(list->m_markerDepth == 0, "submitting a command list with an open marker");
        if (state == CommandList::State::Failed && !failure) {
            failure = list->m_error;
        }
        if (list->m_backend != nullptr) {
            backendLists.push_back(list->m_backend.get());
        }
        ++closed;
    }
    if (!failure) {
        failure = lostError();
    }
    if (!failure) {
        std::uint64_t value = 0;
        {
            const MutexLock lock(m_mutex);
            value = m_lastSubmitted + 1;
        }
        if (Expected<void, Error> submitted = m_backend->submit(backendLists, value); submitted) {
            const MutexLock lock(m_mutex);
            m_lastSubmitted = value;
        } else {
            failure = noteError(std::move(submitted).error());
        }
    }
    {
        const MutexLock lock(m_mutex);
        closeLists(closed);
    }
    if (failure) {
        return makeUnexpected(std::move(*failure));
    }
    return {};
}

void DeviceState::discard(CommandList& list) {
    if (&list == m_deadList.get()) {
        return;
    }
    if (list.m_device != this) {
        ITH_ASSERT_MSG(false, "discarding a command list of another device");
        return;
    }
    const MutexLock lock(m_mutex);
    if (list.m_state.exchange(CommandList::State::Closed, std::memory_order_acq_rel) == CommandList::State::Closed) {
        ITH_ASSERT_MSG(false, "discarding a command list that is not open");
        return;
    }
    closeLists(1);
}

Expected<void, Error> DeviceState::endFrame() {
    assertRenderThread();
    std::uint32_t slot = 0;
    SwapchainHandle swapchain;
    {
        const MutexLock lock(m_mutex);
        if (!m_inFrame) {
            return makeUnexpected(invalidUsage("endFrame() without an open frame"));
        }
        slot = m_frameSlot;
        swapchain = m_frameSwapchain;
        ITH_ASSERT_MSG(m_openLists == 0, "endFrame() while command lists are open; they are discarded");
        std::size_t open = 0;
        for (std::size_t index = 0; index < m_listsUsed[slot]; ++index) {
            CommandList& list = *m_lists[slot][index];
            if (list.m_state.exchange(CommandList::State::Closed, std::memory_order_acq_rel) !=
                CommandList::State::Closed) {
                ++open;
            }
        }
        closeLists(open);
    }

    std::optional<Error> failure = lostError();
    if (!failure) {
        std::uint64_t value = 0;
        {
            const MutexLock lock(m_mutex);
            value = m_lastSubmitted + 1;
        }
        const std::optional<std::uint32_t> swapchainSlot =
            swapchain.isNull() ? std::nullopt : std::optional<std::uint32_t>(swapchain.index());
        Expected<PresentStatus, Error> ended = m_backend->endFrame(slot, swapchainSlot, value);
        if (!ended) {
            failure = noteError(std::move(ended).error());
        } else if (*ended == PresentStatus::OutOfDate && swapchainSlot) {
            swapchains.data(*swapchainSlot).outOfDate = true;
        }
        if (!isLost()) {
            // The value is signaled even when presenting failed (Backend.h).
            const MutexLock lock(m_mutex);
            m_lastSubmitted = value;
            m_frameValues[slot] = value;
        }
    }

    const MutexLock lock(m_mutex);
    m_inFrame = false;
    m_frameSwapchain = {};
    m_frameNumber.fetch_add(1, std::memory_order_acq_rel);
    if (failure) {
        return makeUnexpected(std::move(*failure));
    }
    return {};
}

Expected<void, Error> DeviceState::waitIdle() {
    assertRenderThread();
    std::uint64_t lastSubmitted = 0;
    {
        const MutexLock lock(m_mutex);
        ITH_ASSERT_MSG(!m_inFrame, "waitIdle() while a frame is open");
        lastSubmitted = m_lastSubmitted;
    }
    if (std::optional<Error> lost = lostError()) {
        collectGarbage(true);
        return makeUnexpected(std::move(*lost));
    }
    if (Expected<void, Error> waited = m_backend->waitForValue(lastSubmitted); !waited) {
        Error error = noteError(std::move(waited).error());
        if (isLost()) {
            collectGarbage(true);
        }
        return makeUnexpected(std::move(error));
    }
    collectGarbage(false);
    return {};
}

} // namespace detail

// --- Device -------------------------------------------------------------------------------

Device::Device(std::unique_ptr<detail::DeviceState> state) : m_state(std::move(state)) {}

Device::Device(Device&& other) noexcept = default;
Device& Device::operator=(Device&& other) noexcept = default;
Device::~Device() = default;

const Capabilities& Device::capabilities() const {
    return m_state->capabilities;
}

Expected<BufferHandle, Error> Device::createBuffer(const BufferDesc& desc) {
    return m_state->createBuffer(desc);
}

Expected<TextureHandle, Error> Device::createTexture(const TextureDesc& desc) {
    return m_state->createTexture(desc);
}

Expected<SamplerHandle, Error> Device::createSampler(const SamplerDesc& desc) {
    return m_state->createSampler(desc);
}

Expected<PipelineHandle, Error> Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    return m_state->createGraphicsPipeline(desc);
}

Expected<PipelineHandle, Error> Device::createComputePipeline(const ComputePipelineDesc& desc) {
    return m_state->createComputePipeline(desc);
}

void Device::destroy(BufferHandle buffer) {
    m_state->destroy(buffer);
}

void Device::destroy(TextureHandle texture) {
    m_state->destroy(texture);
}

void Device::destroy(SamplerHandle sampler) {
    m_state->destroy(sampler);
}

void Device::destroy(PipelineHandle pipeline) {
    m_state->destroy(pipeline);
}

Expected<SwapchainHandle, Error> Device::createSwapchain(const SwapchainDesc& desc) {
    return m_state->createSwapchain(desc);
}

void Device::resizeSwapchain(SwapchainHandle swapchain, std::uint32_t width, std::uint32_t height) {
    m_state->resizeSwapchain(swapchain, width, height);
}

void Device::destroy(SwapchainHandle swapchain) {
    m_state->destroy(swapchain);
}

bool Device::isValid(BufferHandle buffer) const {
    return m_state->buffers.isLive(buffer);
}

bool Device::isValid(TextureHandle texture) const {
    return m_state->textures.isLive(texture);
}

bool Device::isValid(SamplerHandle sampler) const {
    return m_state->samplers.isLive(sampler);
}

bool Device::isValid(PipelineHandle pipeline) const {
    return m_state->pipelines.isLive(pipeline);
}

bool Device::isValid(SwapchainHandle swapchain) const {
    return m_state->swapchains.isLive(swapchain);
}

namespace {

// A copy of a slot's data through a handle, or nullopt when the handle is stale (asserted with
// `staleMessage`) or went stale while the data was read: a destroy() racing this query, which
// the caller must order after its last use (Device.h).
template <typename Data, typename Tag>
std::optional<Data> readSlot(const detail::SlotTable<Data>& table, Handle<Tag> handle,
                             [[maybe_unused]] const char* staleMessage) {
    if (!table.isLive(handle)) {
        ITH_ASSERT_MSG(false, staleMessage);
        return std::nullopt;
    }
    const Data data = table.data(handle.index());
    if (!table.isLive(handle)) {
        ITH_ASSERT_MSG(false, "a handle was destroyed while another thread read it");
        return std::nullopt;
    }
    return data;
}

} // namespace

std::uint32_t Device::shaderReadIndex(BufferHandle buffer) const {
    const std::optional<detail::BufferSlot> slot =
        readSlot(m_state->buffers, buffer, "shaderReadIndex() of a stale or null buffer handle");
    if (!slot) {
        return kInvalidDescriptorIndex;
    }
    ITH_ASSERT_MSG(slot->descriptors.read != kInvalidDescriptorIndex,
                   "shaderReadIndex() of a buffer without ShaderRead usage");
    return slot->descriptors.read;
}

std::uint32_t Device::shaderWriteIndex(BufferHandle buffer) const {
    const std::optional<detail::BufferSlot> slot =
        readSlot(m_state->buffers, buffer, "shaderWriteIndex() of a stale or null buffer handle");
    if (!slot) {
        return kInvalidDescriptorIndex;
    }
    ITH_ASSERT_MSG(slot->descriptors.write != kInvalidDescriptorIndex,
                   "shaderWriteIndex() of a buffer without ShaderWrite usage");
    return slot->descriptors.write;
}

std::uint32_t Device::sampledIndex(TextureHandle texture) const {
    const std::optional<detail::TextureSlot> slot =
        readSlot(m_state->textures, texture, "sampledIndex() of a stale or null texture handle");
    if (!slot) {
        return kInvalidDescriptorIndex;
    }
    ITH_ASSERT_MSG(slot->descriptors.sampled != kInvalidDescriptorIndex,
                   "sampledIndex() of a texture without Sampled usage");
    return slot->descriptors.sampled;
}

std::uint32_t Device::storageIndex(TextureHandle texture) const {
    const std::optional<detail::TextureSlot> slot =
        readSlot(m_state->textures, texture, "storageIndex() of a stale or null texture handle");
    if (!slot) {
        return kInvalidDescriptorIndex;
    }
    ITH_ASSERT_MSG(slot->descriptors.storage != kInvalidDescriptorIndex,
                   "storageIndex() of a texture without Storage usage");
    return slot->descriptors.storage;
}

std::uint32_t Device::samplerIndex(SamplerHandle sampler) const {
    const std::optional<detail::SamplerSlot> slot =
        readSlot(m_state->samplers, sampler, "samplerIndex() of a stale or null sampler handle");
    return slot ? slot->descriptor : kInvalidDescriptorIndex;
}

std::span<std::byte> Device::mappedMemory(BufferHandle buffer) const {
    const std::optional<detail::BufferSlot> slot =
        readSlot(m_state->buffers, buffer, "mappedMemory() of a stale or null buffer handle");
    if (!slot) {
        return {};
    }
    if (slot->mapped == nullptr) {
        ITH_ASSERT_MSG(false, "mappedMemory() of a GpuOnly buffer");
        return {};
    }
    return {slot->mapped, static_cast<std::size_t>(slot->size)};
}

Expected<std::optional<Frame>, Error> Device::beginFrame(SwapchainHandle swapchain) {
    return m_state->beginFrame(swapchain);
}

CommandList& Device::openCommandList(std::string_view debugName) {
    return m_state->openCommandList(debugName);
}

Expected<void, Error> Device::submit(CommandList& list) {
    const std::array<CommandList*, 1> lists{&list};
    return m_state->submit(lists);
}

Expected<void, Error> Device::submit(std::span<CommandList* const> lists) {
    return m_state->submit(lists);
}

void Device::discard(CommandList& list) {
    m_state->discard(list);
}

Expected<void, Error> Device::endFrame() {
    return m_state->endFrame();
}

Expected<void, Error> Device::waitIdle() {
    return m_state->waitIdle();
}

bool Device::isLost() const {
    return m_state->isLost();
}

std::uint64_t Device::frameNumber() const {
    return m_state->frameNumber();
}

std::size_t Device::pendingDestructionCount() const {
    return m_state->pendingDestructionCount();
}

} // namespace ith::rhi
