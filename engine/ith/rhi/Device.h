#pragma once

// The rendering hardware interface: ith::rhi::Device (ADR-0009, ADR-0021).
//
//   // A backend's factory creates the device: createNullDevice() here; createD3D12Device() and
//   // createVulkanDevice() in the backend modules, chosen by a selection module (ADR-0021).
//   ith::Expected<ith::rhi::Device, ith::rhi::Error> device = ...;
//   ith::Expected<ith::rhi::SwapchainHandle, ith::rhi::Error> swapchain = device->createSwapchain(
//       {.window = window.nativeHandle(), .width = size.width, .height = size.height, .debugName = "Main"});
//   while (running) {
//       // on ith::EventType::WindowPixelSizeChanged: device->resizeSwapchain(*swapchain, width, height);
//       auto frame = device->beginFrame(*swapchain);
//       if (!frame) {
//           return fatal(frame.error()); // device lost (ADR-0009 rule 19)
//       }
//       if (!frame->has_value()) {
//           continue; // minimized: nothing to draw
//       }
//       ith::rhi::CommandList& list = device->openCommandList("Main pass");
//       ... record, using (*frame)->backbuffer ...
//       if (auto submitted = device->submit(list); !submitted) { ... }
//       if (auto ended = device->endFrame(); !ended) { ... }
//   }
//
// One backend is active per process. No backend or graphics-API type appears in this module's
// public headers.
//
// Objects are referred to by generational handles (Handle.h) and are destroyed late: destroy()
// makes the handle stale at once, and the object is released only when the GPU has finished
// every submission that could use it and every command list open at that moment has been
// submitted or discarded (rule 5). Shaders reach resources through bindless descriptor indices,
// which are reused only after that release (rule 8).
//
// Threading: creating and destroying buffers, textures, samplers and pipelines, the descriptor
// and mapped-memory queries, isValid() and openCommandList() may be called from any thread.
// Every use of a handle on one thread must happen before its destroy() on another; the caller
// orders them, e.g. by destroying only what no other thread still refers to. A query racing the
// destroy of its own handle is a caller error, asserted where it is seen. Everything else -
// frames, submission, swapchains, waitIdle() - is called by the render thread, the thread that
// created the device, which also destroys it (asserted).

#include "ith/foundation/Expected.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Error.h"
#include "ith/rhi/Handle.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi/Resources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ith::rhi {

class Device;

namespace detail {
class DeviceBackend;
class DeviceState;
// Wraps a backend in a Device; called by the backends' factory functions (Backend.h).
Device makeDevice(std::unique_ptr<DeviceBackend> backend);
} // namespace detail

// Frames the CPU may record ahead of the GPU (ADR-0009 rule 14). Per-frame resources are reused
// only after the GPU has finished the frame that used them, kFramesInFlight frames earlier.
inline constexpr std::uint32_t kFramesInFlight = 2;

// What the Direct3D 12 and Vulkan backends' factories take (ADR-0021). The null backend takes
// NullDeviceDesc instead, so nothing reads these settings until a GPU backend exists.
struct DeviceDesc {
    AdapterPreference adapter = AdapterPreference::HighPerformance;
    // The API's validation layers (the D3D12 debug layer, the Vulkan validation layers with
    // synchronization validation). On by default in Debug builds; shipping builds must never
    // load them (ADR-0009 rule 18).
#ifdef ITH_DEBUG
    bool validation = true;
#else
    bool validation = false;
#endif
    // GPU-based validation on Direct3D 12 (slow); needs `validation`.
    bool gpuBasedValidation = false;
    // The application's name, passed to the API and tools.
    std::string_view applicationName = "Ironthorn";
};

// A frame begun by Device::beginFrame().
struct Frame {
    // Counts frames from 0.
    std::uint64_t number = 0;
    // number % kFramesInFlight: which set of per-frame resources this frame uses.
    std::uint32_t slot = 0;
    // The swapchain image to draw into; the null handle for a frame without a swapchain. Valid
    // only until endFrame(): the next frame may hand out another image, and recreating or
    // destroying the swapchain makes it stale. Its layout at the start of the frame is undefined:
    // transition it from TextureLayout::Undefined, and to TextureLayout::Present before
    // endFrame().
    TextureHandle backbuffer;
    Format backbufferFormat = Format::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Owns a device and every object created on it. Move-only; a moved-from Device owns nothing and
// may only be destroyed or assigned to. Devices come from a backend's factory function, such as
// createNullDevice() (NullDevice.h).
class Device {
public:
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    // Waits for the GPU, then releases every object still alive, including those not destroyed
    // by the caller. Preconditions (asserted): no frame is open and no command list is open.
    ~Device();

    // What the device is and can do; fixed for its lifetime.
    [[nodiscard]] const Capabilities& capabilities() const;

    // Creation. Preconditions (asserted; the call fails with InvalidUsage when asserts continue
    // or are compiled out): the description's documented rules hold, its sizes are within
    // Capabilities::limits, and its debug name is not empty. Fails with OutOfHandles,
    // OutOfDescriptors, OutOfMemory, InvalidShader (pipelines), or DeviceLost once the device is
    // lost.
    Expected<BufferHandle, Error> createBuffer(const BufferDesc& desc);
    Expected<TextureHandle, Error> createTexture(const TextureDesc& desc);
    Expected<SamplerHandle, Error> createSampler(const SamplerDesc& desc);
    Expected<PipelineHandle, Error> createGraphicsPipeline(const GraphicsPipelineDesc& desc);
    Expected<PipelineHandle, Error> createComputePipeline(const ComputePipelineDesc& desc);

    // Deferred destruction (rule 5): the handle is stale at once; the object, its slot and its
    // descriptor indices are released later. Destroying the null handle does nothing.
    // Precondition (asserted; otherwise nothing happens): the handle is not stale. Swapchain
    // images belong to their swapchain and can't be destroyed.
    void destroy(BufferHandle buffer);
    void destroy(TextureHandle texture);
    void destroy(SamplerHandle sampler);
    void destroy(PipelineHandle pipeline);

    // Creates a swapchain for a window (render thread). Fails with SurfaceFailed when the window
    // can't be presented to, or as the create functions above.
    Expected<SwapchainHandle, Error> createSwapchain(const SwapchainDesc& desc);
    // Sets the size in pixels the swapchain should have, e.g. on
    // ith::EventType::WindowPixelSizeChanged (render thread). The swapchain is recreated at the
    // next beginFrame() with this swapchain, which then hands out new image handles; the old
    // ones become stale. A zero size makes beginFrame() draw nothing until a real size arrives.
    // Precondition (asserted): a valid handle.
    void resizeSwapchain(SwapchainHandle swapchain, std::uint32_t width, std::uint32_t height);
    // Waits for the GPU and destroys the swapchain and its images at once, not deferred, so the
    // window may be destroyed right after (render thread). Preconditions (asserted): a valid
    // handle, not the swapchain of the open frame.
    void destroy(SwapchainHandle swapchain);

    // Whether a handle refers to a live object of this device: false for the null handle and for
    // a destroyed object's handle.
    [[nodiscard]] bool isValid(BufferHandle buffer) const;
    [[nodiscard]] bool isValid(TextureHandle texture) const;
    [[nodiscard]] bool isValid(SamplerHandle sampler) const;
    [[nodiscard]] bool isValid(PipelineHandle pipeline) const;
    [[nodiscard]] bool isValid(SwapchainHandle swapchain) const;

    // Bindless descriptor indices for shaders (rule 8); fixed for the object's lifetime.
    // Preconditions (asserted; kInvalidDescriptorIndex otherwise): a valid handle created with
    // the usage the index is for: BufferUsage::ShaderRead, BufferUsage::ShaderWrite,
    // TextureUsage::Sampled, TextureUsage::Storage.
    [[nodiscard]] std::uint32_t shaderReadIndex(BufferHandle buffer) const;
    [[nodiscard]] std::uint32_t shaderWriteIndex(BufferHandle buffer) const;
    [[nodiscard]] std::uint32_t sampledIndex(TextureHandle texture) const;
    [[nodiscard]] std::uint32_t storageIndex(TextureHandle texture) const;
    // Index in the sampler heap. Precondition (asserted): a valid handle.
    [[nodiscard]] std::uint32_t samplerIndex(SamplerHandle sampler) const;

    // The persistently mapped memory of an Upload or Readback buffer, valid until the buffer's
    // destroy(). The memory is host-coherent: no flush or invalidate is needed, but the caller
    // makes sure the GPU is not using the bytes it writes or reads (frames in flight, or
    // waitIdle()). Preconditions (asserted; empty otherwise): a valid handle of a buffer that is
    // not GpuOnly.
    [[nodiscard]] std::span<std::byte> mappedMemory(BufferHandle buffer) const;

    // Begins a frame: waits until the GPU has finished the frame kFramesInFlight frames earlier,
    // releases objects whose deferred destruction is complete, and acquires the next image of
    // `swapchain` (the null handle for a frame without one). Returns std::nullopt, and begins no
    // frame, when nothing should be drawn: the swapchain's size is zero, or it is being
    // recreated. Recreates the swapchain after resizeSwapchain() or when the window changed under
    // it; if that fails, the error is returned, the old image handles are stale, and the next
    // beginFrame() tries again. Fails with DeviceLost, or another error of the backend.
    // Preconditions (asserted): no frame is open; a valid swapchain handle or the null handle.
    Expected<std::optional<Frame>, Error> beginFrame(SwapchainHandle swapchain = {});

    // Opens a command list from the open frame's pool. Thread-safe. The list must be submitted
    // or discarded before endFrame(). If the backend can't start the list (out of memory), the
    // list drops what is recorded and submit() reports the error. Preconditions (asserted): a
    // frame is open; the name is not empty. Outside a frame it returns a list that records
    // nothing and whose submit() fails with InvalidUsage.
    CommandList& openCommandList(std::string_view debugName);

    // Submits command lists to the graphics queue in order (rule 13), and closes them. Fails
    // with the error of a list that could not start (nothing is then submitted), DeviceLost or
    // OutOfMemory. Preconditions (asserted): a frame is open; every list is open, came from this
    // device in this frame, is not rendering and has no open marker.
    Expected<void, Error> submit(CommandList& list);
    Expected<void, Error> submit(std::span<CommandList* const> lists);
    // Closes a list without submitting it. Precondition (asserted): the list is open.
    void discard(CommandList& list);

    // Ends the frame: presents the swapchain image (if the frame has a swapchain) and marks the
    // frame's work so beginFrame() can wait for it. Fails with DeviceLost, or another error of
    // the backend; the frame is closed either way. Preconditions (asserted): a frame is open and
    // every list of it was submitted or discarded (open ones are discarded).
    Expected<void, Error> endFrame();

    // Waits until the GPU has finished all submitted work, and releases what deferred
    // destruction allows. Fails with DeviceLost. Precondition (asserted): no frame is open.
    Expected<void, Error> waitIdle();

    // True once a call reported DeviceLost; the device can then only be destroyed.
    [[nodiscard]] bool isLost() const;

    // The number of the next frame beginFrame() begins.
    [[nodiscard]] std::uint64_t frameNumber() const;

    // Objects destroyed but not yet released.
    [[nodiscard]] std::size_t pendingDestructionCount() const;

private:
    friend Device detail::makeDevice(std::unique_ptr<detail::DeviceBackend> backend);

    explicit Device(std::unique_ptr<detail::DeviceState> state);

    std::unique_ptr<detail::DeviceState> m_state;
};

} // namespace ith::rhi
