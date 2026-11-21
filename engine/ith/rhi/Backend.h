#pragma once

// The contract between the device and a backend (ADR-0009 rule 2). Renderer code never includes
// this header; backends (rhi_d3d12, rhi_vulkan, the null backend, console backends) implement
// it.
//
// The device does the backend-neutral work once for every backend: handle tables and stale-handle
// checks, descriptor index allocation, deferred destruction, frame pacing, swapchain image
// handles, device-loss bookkeeping and the recording checks. A backend only talks to its API. It
// keeps its native objects in arrays indexed by the slot index of their handle, sized from
// Capabilities::limits at creation, so the device never stores an API type and a lookup is one
// array index.
//
// The backend is called through virtual functions, one call per RHI call; bindless keeps the
// number of calls per draw low (rule 2).
//
// Rules for every backend:
// - Calls reach it already checked: valid slots, descriptions that meet their documented rules,
//   recording calls in a legal state. A backend asserts only what it can see and the device
//   can't (API limits).
// - create*() and destroy*() of buffers, textures, samplers and pipelines may be called from
//   several threads at once, always for different slots, and createCommandList() from any thread
//   (never twice at once). Everything else is called on the render thread, except
//   CommandListBackend calls, which come from the thread recording that list.
// - A pooled command list may be begun again after it was submitted, or after it was recorded
//   and then dropped without a submit (a discard, or a batch that failed): a list that was left
//   recording must be closed (ended) by begin() before its pool or allocator is reset.
// - destroy*() is called only once the GPU no longer uses the object.
// - Timeline values: submit() and endFrame() receive strictly increasing values and signal them
//   on the graphics queue when the work before them completes. A failed submit() signals
//   nothing. endFrame() signals its value even when presenting fails, unless the device is lost.
// - Device loss is reported as ErrorCode::DeviceLost, with the API's diagnostics (DRED,
//   VK_EXT_device_fault) in the message. After that the device calls only destroy*() and the
//   destructor.
//
// A backend module's own test hooks (e.g. removeD3D12DeviceForTesting()) reach their backend
// through detail::backendOf() (Device.h) and cast it to their own class: no RTTI, so they check
// Capabilities::backend first.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Barrier.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi/Resources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ith::rhi::detail {

// The bindless indices the device allocated for a buffer; kInvalidDescriptorIndex for a usage
// the buffer doesn't have. The backend writes the descriptors at these indices.
struct BufferDescriptors {
    std::uint32_t read = kInvalidDescriptorIndex;
    std::uint32_t write = kInvalidDescriptorIndex;
};

// The bindless indices of a texture, as for buffers.
struct TextureDescriptors {
    std::uint32_t sampled = kInvalidDescriptorIndex;
    std::uint32_t storage = kInvalidDescriptorIndex;
};

// A swapchain's images, written by the backend into the texture slots the device reserved.
struct SwapchainImages {
    // How many of the reserved slots the backend filled: 2 to kMaxSwapchainImages.
    std::uint32_t count = 0;
    // The images' format and size, which the device records for its checks. Their usage is
    // TextureUsage::RenderTarget | TextureUsage::CopyDst.
    Format format = Format::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class AcquireStatus : std::uint8_t {
    Acquired,
    // The swapchain no longer matches its window; the device recreates it.
    OutOfDate,
};

struct AcquireResult {
    AcquireStatus status = AcquireStatus::Acquired;
    // Which reserved texture slot holds the acquired image; valid when Acquired. An index at or
    // beyond SwapchainImages::count is treated as a backend failure.
    std::uint32_t imageIndex = 0;
};

enum class PresentStatus : std::uint8_t {
    Presented,
    // Presented, but the swapchain no longer matches its window; the device recreates it at the
    // next beginFrame().
    OutOfDate,
};

// The recording side of one pooled command list. Calls come from the thread recording the list.
class CommandListBackend {
public:
    CommandListBackend() = default;
    CommandListBackend(const CommandListBackend&) = delete;
    CommandListBackend& operator=(const CommandListBackend&) = delete;
    CommandListBackend(CommandListBackend&&) = delete;
    CommandListBackend& operator=(CommandListBackend&&) = delete;
    virtual ~CommandListBackend() = default;

    // Starts recording into the per-frame pool of `frameSlot`, which the GPU has finished.
    // Fails with OutOfMemory; the device then drops the list's commands and reports the error
    // at submit.
    virtual Expected<void, Error> begin(std::uint32_t frameSlot, std::string_view debugName) = 0;

    virtual void barrier(const Barriers& barriers) = 0;
    // Also sets the viewport and scissor to the attachments' full size.
    virtual void beginRendering(const RenderingDesc& desc) = 0;
    virtual void endRendering() = 0;
    virtual void setPipeline(PipelineHandle pipeline) = 0;
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect& scissor) = 0;
    virtual void setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format) = 0;
    virtual void pushConstants(std::span<const std::byte> data) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                      std::uint32_t firstInstance) = 0;
    virtual void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                             std::int32_t vertexOffset, std::uint32_t firstInstance) = 0;
    virtual void drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset, BufferHandle count,
                                          std::uint64_t countOffset, std::uint32_t maxDraws) = 0;
    virtual void dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ) = 0;
    virtual void dispatchIndirect(BufferHandle arguments, std::uint64_t offset) = 0;
    virtual void copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                            std::uint64_t destinationOffset, std::uint64_t size) = 0;
    virtual void copyBufferToTexture(const BufferTextureCopy& copy) = 0;
    virtual void copyTextureToBuffer(const BufferTextureCopy& copy) = 0;
    virtual void beginMarker(std::string_view name) = 0;
    virtual void endMarker() = 0;
};

// The device side of a backend.
class DeviceBackend {
public:
    DeviceBackend() = default;
    DeviceBackend(const DeviceBackend&) = delete;
    DeviceBackend& operator=(const DeviceBackend&) = delete;
    DeviceBackend(DeviceBackend&&) = delete;
    DeviceBackend& operator=(DeviceBackend&&) = delete;
    // Called after the device has released every object and waited for the GPU.
    virtual ~DeviceBackend() = default;

    // Fixed for the backend's lifetime; the device copies it once.
    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    // Creates a buffer in `slot` and writes its descriptors. Returns the mapped memory of an
    // Upload or Readback buffer (desc.size bytes, valid until destroyBuffer()), nullptr for
    // GpuOnly.
    virtual Expected<std::byte*, Error> createBuffer(std::uint32_t slot, const BufferDesc& desc,
                                                     const BufferDescriptors& descriptors) = 0;
    virtual void destroyBuffer(std::uint32_t slot) = 0;
    virtual Expected<void, Error> createTexture(std::uint32_t slot, const TextureDesc& desc,
                                                const TextureDescriptors& descriptors) = 0;
    virtual void destroyTexture(std::uint32_t slot) = 0;
    // `descriptor` is the sampler's index in the sampler heap.
    virtual Expected<void, Error> createSampler(std::uint32_t slot, const SamplerDesc& desc,
                                                std::uint32_t descriptor) = 0;
    virtual void destroySampler(std::uint32_t slot) = 0;
    // Checks the bytecode format (ErrorCode::InvalidShader) and creates the pipeline.
    virtual Expected<void, Error> createGraphicsPipeline(std::uint32_t slot, const GraphicsPipelineDesc& desc) = 0;
    virtual Expected<void, Error> createComputePipeline(std::uint32_t slot, const ComputePipelineDesc& desc) = 0;
    virtual void destroyPipeline(std::uint32_t slot) = 0;

    // Creates a swapchain in `slot` whose images go into the texture slots `imageSlots`
    // (kMaxSwapchainImages of them, reserved by the device). desc.width and desc.height are
    // positive.
    virtual Expected<SwapchainImages, Error> createSwapchain(std::uint32_t slot, const SwapchainDesc& desc,
                                                             std::span<const std::uint32_t> imageSlots) = 0;
    // Recreates the swapchain's images at a new, positive size, into the same texture slots. The
    // GPU is idle. On failure the swapchain may have no images left: the device then treats it
    // as out of date, acquires nothing from it, and calls resizeSwapchain() again at the next
    // beginFrame() or destroySwapchain() when it is destroyed; both must work in that state.
    virtual Expected<SwapchainImages, Error> resizeSwapchain(std::uint32_t slot, std::uint32_t width,
                                                             std::uint32_t height) = 0;
    // The GPU is idle.
    virtual void destroySwapchain(std::uint32_t slot) = 0;
    // Acquires the next image for the frame in `frameSlot`.
    virtual Expected<AcquireResult, Error> acquireImage(std::uint32_t swapchainSlot, std::uint32_t frameSlot) = 0;

    // A new pooled command list. Fails with OutOfMemory.
    virtual Expected<std::unique_ptr<CommandListBackend>, Error> createCommandList() = 0;

    // The GPU has finished the frame that last used `frameSlot`: per-frame pools may be reset.
    virtual void beginFrame(std::uint32_t frameSlot) = 0;
    // Ends recording of the lists, submits them in order and signals `signalValue` after them.
    virtual Expected<void, Error> submit(std::span<CommandListBackend* const> lists, std::uint64_t signalValue) = 0;
    // Presents the acquired image of `swapchainSlot` (if any) and signals `signalValue`.
    virtual Expected<PresentStatus, Error> endFrame(std::uint32_t frameSlot, std::optional<std::uint32_t> swapchainSlot,
                                                    std::uint64_t signalValue) = 0;
    // The highest value the GPU has signaled so far.
    virtual std::uint64_t completedValue() = 0;
    // Blocks until the GPU has signaled `value`. Fails with DeviceLost.
    virtual Expected<void, Error> waitForValue(std::uint64_t value) = 0;
};

// The error of a GPU that lacks minimum features (ADR-0009 rule 17), naming each of them, e.g.
// "The GPU 'X' lacks features Ironthorn needs: Shader Model 6.6, enhanced barriers".
Error unsupportedDeviceError(std::string_view adapterName, std::span<const std::string_view> missingFeatures);

} // namespace ith::rhi::detail
