#include "ith/rhi/NullDevice.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Backend.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith::rhi {

// --- NullGpu ------------------------------------------------------------------------------

void NullGpu::setAutoComplete(bool autoComplete) {
    const MutexLock lock(m_mutex);
    m_autoComplete = autoComplete;
    if (autoComplete) {
        m_completed = m_signaled;
    }
}

void NullGpu::complete(std::uint64_t value) {
    const MutexLock lock(m_mutex);
    m_completed = std::max(m_completed, std::min(value, m_signaled));
}

std::uint64_t NullGpu::signaledValue() const {
    const MutexLock lock(m_mutex);
    return m_signaled;
}

std::uint64_t NullGpu::completedValue() const {
    const MutexLock lock(m_mutex);
    return m_completed;
}

std::uint64_t NullGpu::waitCount() const {
    const MutexLock lock(m_mutex);
    return m_waits;
}

void NullGpu::loseDevice(std::string reason) {
    const MutexLock lock(m_mutex);
    m_lost = true;
    m_lostReason = std::move(reason);
}

void NullGpu::failNextCreate() {
    const MutexLock lock(m_mutex);
    m_failNextCreate = true;
}

void NullGpu::failNextCommandList() {
    const MutexLock lock(m_mutex);
    m_failNextCommandList = true;
}

void NullGpu::outdateNextAcquire() {
    const MutexLock lock(m_mutex);
    m_outdateNextAcquire = true;
}

void NullGpu::outdateNextPresent() {
    const MutexLock lock(m_mutex);
    m_outdateNextPresent = true;
}

void NullGpu::failNextResize() {
    const MutexLock lock(m_mutex);
    m_failNextResize = true;
}

std::uint32_t NullGpu::liveObjectCount() const {
    const MutexLock lock(m_mutex);
    return m_liveObjects;
}

std::uint64_t NullGpu::commandCount() const {
    const MutexLock lock(m_mutex);
    return m_commands;
}

std::uint64_t NullGpu::submitCount() const {
    const MutexLock lock(m_mutex);
    return m_submits;
}

std::uint64_t NullGpu::presentCount() const {
    const MutexLock lock(m_mutex);
    return m_presents;
}

std::uint64_t NullGpu::swapchainBuildCount() const {
    const MutexLock lock(m_mutex);
    return m_swapchainBuilds;
}

// --- The backend --------------------------------------------------------------------------

namespace {

Error lostError(const std::string& reason) {
    return Error{.code = ErrorCode::DeviceLost, .message = "The GPU was lost: " + reason};
}

} // namespace

// Records every recording call as one command and drops it.
class NullCommandList final : public detail::CommandListBackend {
public:
    explicit NullCommandList(NullGpu& gpu) : m_gpu(&gpu) {}

    Expected<void, Error> begin(std::uint32_t /*frameSlot*/, std::string_view debugName) override {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_failNextCommandList) {
            m_gpu->m_failNextCommandList = false;
            return makeUnexpected(
                Error{.code = ErrorCode::OutOfMemory,
                      .message = "Out of memory starting command list '" + std::string(debugName) + "'"});
        }
        return {};
    }

    void barrier(const Barriers& /*barriers*/) override { count(); }

    void beginRendering(const RenderingDesc& /*desc*/) override { count(); }

    void endRendering() override { count(); }

    void setPipeline(PipelineHandle /*pipeline*/) override { count(); }

    void setViewport(const Viewport& /*viewport*/) override { count(); }

    void setScissor(const Rect& /*scissor*/) override { count(); }

    void setIndexBuffer(BufferHandle /*buffer*/, std::uint64_t /*offset*/, IndexFormat /*format*/) override { count(); }

    void pushConstants(std::span<const std::byte> /*data*/) override { count(); }

    void draw(std::uint32_t /*vertexCount*/, std::uint32_t /*instanceCount*/, std::uint32_t /*firstVertex*/,
              std::uint32_t /*firstInstance*/) override {
        count();
    }

    void drawIndexed(std::uint32_t /*indexCount*/, std::uint32_t /*instanceCount*/, std::uint32_t /*firstIndex*/,
                     std::int32_t /*vertexOffset*/, std::uint32_t /*firstInstance*/) override {
        count();
    }

    void drawIndexedIndirectCount(BufferHandle /*arguments*/, std::uint64_t /*argumentsOffset*/, BufferHandle /*count*/,
                                  std::uint64_t /*countOffset*/, std::uint32_t /*maxDraws*/) override {
        count();
    }

    void dispatch(std::uint32_t /*groupCountX*/, std::uint32_t /*groupCountY*/,
                  std::uint32_t /*groupCountZ*/) override {
        count();
    }

    void dispatchIndirect(BufferHandle /*arguments*/, std::uint64_t /*offset*/) override { count(); }

    void copyBuffer(BufferHandle /*source*/, std::uint64_t /*sourceOffset*/, BufferHandle /*destination*/,
                    std::uint64_t /*destinationOffset*/, std::uint64_t /*size*/) override {
        count();
    }

    void copyBufferToTexture(const BufferTextureCopy& /*copy*/) override { count(); }

    void beginMarker(std::string_view /*name*/) override { count(); }

    void endMarker() override { count(); }

private:
    void count() {
        const MutexLock lock(m_gpu->m_mutex);
        ++m_gpu->m_commands;
    }

    NullGpu* m_gpu;
};

// Keeps the mapped memory of Upload and Readback buffers and a little swapchain state; the
// device does everything else.
class NullBackend final : public detail::DeviceBackend {
public:
    static constexpr std::uint32_t kImageCount = 3;

    NullBackend(const Capabilities& capabilities, NullGpu* gpu)
        : m_capabilities(capabilities),
          m_ownedGpu(gpu == nullptr ? std::make_unique<NullGpu>() : nullptr),
          m_gpu(gpu == nullptr ? m_ownedGpu.get() : gpu),
          m_mapped(capabilities.limits.buffers),
          m_swapchains(capabilities.limits.swapchains) {}

    Capabilities capabilities() const override { return m_capabilities; }

    Expected<std::byte*, Error> createBuffer(std::uint32_t slot, const BufferDesc& desc,
                                             const detail::BufferDescriptors& /*descriptors*/) override {
        if (desc.memory != MemoryType::GpuOnly && desc.size > kNullMaxMappedSize) {
            return makeUnexpected(Error{.code = ErrorCode::OutOfMemory,
                                        .message = "The null backend maps at most kNullMaxMappedSize bytes; '" +
                                                   std::string(desc.debugName) + "' is larger"});
        }
        if (std::optional<Error> failure = created(desc.debugName)) {
            return makeUnexpected(std::move(*failure));
        }
        if (desc.memory == MemoryType::GpuOnly) {
            return nullptr;
        }
        m_mapped[slot].assign(static_cast<std::size_t>(desc.size), std::byte{0});
        return m_mapped[slot].data();
    }

    void destroyBuffer(std::uint32_t slot) override {
        m_mapped[slot] = {};
        destroyed();
    }

    Expected<void, Error> createTexture(std::uint32_t /*slot*/, const TextureDesc& desc,
                                        const detail::TextureDescriptors& /*descriptors*/) override {
        return result(created(desc.debugName));
    }

    void destroyTexture(std::uint32_t /*slot*/) override { destroyed(); }

    Expected<void, Error> createSampler(std::uint32_t /*slot*/, const SamplerDesc& desc,
                                        std::uint32_t /*descriptor*/) override {
        return result(created(desc.debugName));
    }

    void destroySampler(std::uint32_t /*slot*/) override { destroyed(); }

    Expected<void, Error> createGraphicsPipeline(std::uint32_t /*slot*/, const GraphicsPipelineDesc& desc) override {
        return result(created(desc.debugName));
    }

    Expected<void, Error> createComputePipeline(std::uint32_t /*slot*/, const ComputePipelineDesc& desc) override {
        return result(created(desc.debugName));
    }

    void destroyPipeline(std::uint32_t /*slot*/) override { destroyed(); }

    Expected<detail::SwapchainImages, Error>
    createSwapchain(std::uint32_t slot, const SwapchainDesc& desc,
                    [[maybe_unused]] std::span<const std::uint32_t> imageSlots) override {
        ITH_ASSERT(imageSlots.size() >= kImageCount);
        if (std::optional<Error> failure = created(desc.debugName)) {
            return makeUnexpected(std::move(*failure));
        }
        m_swapchains[slot] = Swapchain{.format = desc.format};
        return build(slot, desc.width, desc.height);
    }

    Expected<detail::SwapchainImages, Error> resizeSwapchain(std::uint32_t slot, std::uint32_t width,
                                                             std::uint32_t height) override {
        {
            const MutexLock lock(m_gpu->m_mutex);
            if (m_gpu->m_failNextResize) {
                m_gpu->m_failNextResize = false;
                return makeUnexpected(
                    Error{.code = ErrorCode::OutOfMemory, .message = "Out of memory recreating a swapchain"});
            }
        }
        return build(slot, width, height);
    }

    void destroySwapchain(std::uint32_t slot) override {
        m_swapchains[slot] = {};
        destroyed();
    }

    Expected<detail::AcquireResult, Error> acquireImage(std::uint32_t swapchainSlot,
                                                        std::uint32_t /*frameSlot*/) override {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_lost) {
            return makeUnexpected(lostError(m_gpu->m_lostReason));
        }
        if (m_gpu->m_outdateNextAcquire) {
            m_gpu->m_outdateNextAcquire = false;
            return detail::AcquireResult{.status = detail::AcquireStatus::OutOfDate};
        }
        Swapchain& swapchain = m_swapchains[swapchainSlot];
        const std::uint32_t image = swapchain.nextImage;
        swapchain.nextImage = (image + 1) % kImageCount;
        return detail::AcquireResult{.status = detail::AcquireStatus::Acquired, .imageIndex = image};
    }

    Expected<std::unique_ptr<detail::CommandListBackend>, Error> createCommandList() override {
        return std::unique_ptr<detail::CommandListBackend>(std::make_unique<NullCommandList>(*m_gpu));
    }

    void beginFrame(std::uint32_t /*frameSlot*/) override {}

    Expected<void, Error> submit(std::span<detail::CommandListBackend* const> /*lists*/,
                                 std::uint64_t signalValue) override {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_lost) {
            return makeUnexpected(lostError(m_gpu->m_lostReason));
        }
        signal(signalValue);
        ++m_gpu->m_submits;
        return {};
    }

    Expected<detail::PresentStatus, Error> endFrame(std::uint32_t /*frameSlot*/,
                                                    std::optional<std::uint32_t> swapchainSlot,
                                                    std::uint64_t signalValue) override {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_lost) {
            return makeUnexpected(lostError(m_gpu->m_lostReason));
        }
        signal(signalValue);
        if (!swapchainSlot) {
            return detail::PresentStatus::Presented;
        }
        ++m_gpu->m_presents;
        if (m_gpu->m_outdateNextPresent) {
            m_gpu->m_outdateNextPresent = false;
            return detail::PresentStatus::OutOfDate;
        }
        return detail::PresentStatus::Presented;
    }

    std::uint64_t completedValue() override {
        const MutexLock lock(m_gpu->m_mutex);
        return m_gpu->m_completed;
    }

    Expected<void, Error> waitForValue(std::uint64_t value) override {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_lost) {
            return makeUnexpected(lostError(m_gpu->m_lostReason));
        }
        ITH_ASSERT_MSG(value <= m_gpu->m_signaled, "waiting for a value that was never signaled");
        if (value > m_gpu->m_completed) {
            // The simulated GPU finishes the work the device waits for.
            m_gpu->m_completed = std::min(value, m_gpu->m_signaled);
            ++m_gpu->m_waits;
        }
        return {};
    }

private:
    struct Swapchain {
        Format format = Format::Unknown;
        std::uint32_t nextImage = 0;
    };

    // An injected creation failure, or nothing after counting the new object.
    std::optional<Error> created(std::string_view debugName) {
        const MutexLock lock(m_gpu->m_mutex);
        if (m_gpu->m_failNextCreate) {
            m_gpu->m_failNextCreate = false;
            return Error{.code = ErrorCode::OutOfMemory,
                         .message = "Out of GPU memory creating '" + std::string(debugName) + "'"};
        }
        ++m_gpu->m_liveObjects;
        return std::nullopt;
    }

    void destroyed() {
        const MutexLock lock(m_gpu->m_mutex);
        ITH_ASSERT(m_gpu->m_liveObjects > 0);
        --m_gpu->m_liveObjects;
    }

    static Expected<void, Error> result(std::optional<Error> failure) {
        if (failure) {
            return makeUnexpected(std::move(*failure));
        }
        return {};
    }

    Expected<detail::SwapchainImages, Error> build(std::uint32_t slot, std::uint32_t width, std::uint32_t height) {
        {
            const MutexLock lock(m_gpu->m_mutex);
            ++m_gpu->m_swapchainBuilds;
        }
        m_swapchains[slot].nextImage = 0;
        return detail::SwapchainImages{
            .count = kImageCount, .format = m_swapchains[slot].format, .width = width, .height = height};
    }

    void signal(std::uint64_t value) ITH_REQUIRES(m_gpu->m_mutex) {
        ITH_ASSERT_MSG(value > m_gpu->m_signaled, "timeline values must increase");
        m_gpu->m_signaled = value;
        if (m_gpu->m_autoComplete) {
            m_gpu->m_completed = value;
        }
    }

    Capabilities m_capabilities;
    std::unique_ptr<NullGpu> m_ownedGpu;
    NullGpu* m_gpu;
    // Per buffer slot: the memory of a mapped buffer. Sized once, so slots never move.
    std::vector<std::vector<std::byte>> m_mapped;
    std::vector<Swapchain> m_swapchains;
};

Expected<Device, Error> createNullDevice(const NullDeviceDesc& desc) {
    static constexpr std::string_view kAdapterName = "Null GPU";
    if (!desc.missingFeatures.empty()) {
        return makeUnexpected(detail::unsupportedDeviceError(kAdapterName, desc.missingFeatures));
    }
    Capabilities capabilities = desc.capabilities;
    capabilities.backend = Backend::Null;
    capabilities.shaderFormat = ShaderFormat::None;
    capabilities.adapter = AdapterInfo{.name = std::string(kAdapterName), .software = true};
    return detail::makeDevice(std::make_unique<NullBackend>(capabilities, desc.gpu));
}

} // namespace ith::rhi
