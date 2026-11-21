#pragma once

// The Direct3D 12 backend's classes (ADR-0009, ADR-0021). Internal to the rhi_d3d12 module: only
// its sources include this header, which is the only one that names Direct3D 12, DXGI or
// D3D12MA types. Everything backend-neutral (handles, checks, deferred destruction, frame
// pacing) is done by rhi::Device before a call reaches these classes (ith/rhi/Backend.h).

#include "ith/foundation/Expected.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

#include <D3D12MemAlloc.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

// Owns a Win32 event handle.
struct EventDeleter {
    void operator()(void* handle) const { CloseHandle(handle); }
};

using EventHandle = std::unique_ptr<void, EventDeleter>;

// UTF-8 to UTF-16 for the API's names, and back for its messages.
std::wstring widen(std::string_view text);
std::string narrow(const wchar_t* text);

// The DXGI format of an RHI format (Unknown for Format::Unknown).
DXGI_FORMAT dxgiFormat(Format format);

// A buffer slot.
struct BufferRecord {
    ComPtr<D3D12MA::Allocation> allocation;
    // Owned by the allocation.
    ID3D12Resource* resource = nullptr;
    std::uint64_t size = 0;
    // Whether shaders may write it (BufferUsage::ShaderWrite), which also allows reads through
    // its write index.
    bool shaderWrite = false;
};

// A texture slot: a texture or a swapchain image.
struct TextureRecord {
    ComPtr<ID3D12Resource> resource;
    // Null for swapchain images, which DXGI owns.
    ComPtr<D3D12MA::Allocation> allocation;
    // The format of buffer data copied to or from the texture (the depth plane's format for depth
    // textures, the resource's format for swapchain images).
    DXGI_FORMAT copyFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    // Array layers as Direct3D 12 counts them: 1 for 3D textures.
    std::uint32_t layers = 1;
    std::uint32_t mipLevels = 1;
    // 2 for depth-stencil formats, which keep depth and stencil in separate planes.
    std::uint32_t planes = 1;
    TextureUsage usage = TextureUsage::None;
};

struct PipelineRecord {
    ComPtr<ID3D12PipelineState> state;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool compute = false;
};

struct SwapchainRecord {
    ComPtr<IDXGISwapChain3> swapchain;
    // The texture slots the device reserved for the images.
    std::array<std::uint32_t, kMaxSwapchainImages> imageSlots{};
    // How many of imageSlots the swapchain uses.
    std::uint32_t imageCount = 0;
    // The format of the images' render-target views: the sRGB variant when the swapchain's
    // format is sRGB, because flip-model swapchain buffers can't have an sRGB format.
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT bufferFormat = DXGI_FORMAT_UNKNOWN;
    Format format = Format::Unknown;
    UINT flags = 0;
    bool vsync = true;
    std::string name;
};

class D3D12Backend;

// One pooled command list: a command allocator and a graphics command list. The device pools
// lists per frame slot (ith/rhi/Device.cpp), so a list is always begun for the same frame slot
// and one allocator is enough: when it is begun, the GPU has finished the frame that last used
// it.
class D3D12CommandList final : public detail::CommandListBackend {
public:
    D3D12CommandList(D3D12Backend& backend, ComPtr<ID3D12CommandAllocator> allocator,
                     ComPtr<ID3D12GraphicsCommandList7> list);

    Expected<void, Error> begin(std::uint32_t frameSlot, std::string_view debugName) override;
    void barrier(const Barriers& barriers) override;
    void beginRendering(const RenderingDesc& desc) override;
    void endRendering() override;
    void setPipeline(PipelineHandle pipeline) override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect& scissor) override;
    void setIndexBuffer(BufferHandle buffer, std::uint64_t offset, IndexFormat format) override;
    void pushConstants(std::span<const std::byte> data) override;
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                     std::int32_t vertexOffset, std::uint32_t firstInstance) override;
    void drawIndexedIndirectCount(BufferHandle arguments, std::uint64_t argumentsOffset, BufferHandle count,
                                  std::uint64_t countOffset, std::uint32_t maxDraws) override;
    void dispatch(std::uint32_t groupCountX, std::uint32_t groupCountY, std::uint32_t groupCountZ) override;
    void dispatchIndirect(BufferHandle arguments, std::uint64_t offset) override;
    void copyBuffer(BufferHandle source, std::uint64_t sourceOffset, BufferHandle destination,
                    std::uint64_t destinationOffset, std::uint64_t size) override;
    void copyBufferToTexture(const BufferTextureCopy& copy) override;
    void copyTextureToBuffer(const BufferTextureCopy& copy) override;
    void beginMarker(std::string_view name) override;
    void endMarker() override;

    // Ends recording if the list is still recording; called by submit(). Returns the API's result.
    HRESULT close();

    [[nodiscard]] ID3D12CommandList* native() const { return m_list.Get(); }

private:
    // The texture copy location of `copy`'s texture and buffer.
    void copyLocations(const BufferTextureCopy& copy, D3D12_TEXTURE_COPY_LOCATION& texture,
                       D3D12_TEXTURE_COPY_LOCATION& buffer) const;

    D3D12Backend* m_backend;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList7> m_list;
    bool m_recording = false;
    // Scratch space for barrier batches and marker names, reused to avoid allocations.
    std::vector<D3D12_GLOBAL_BARRIER> m_globalBarriers;
    std::vector<D3D12_BUFFER_BARRIER> m_bufferBarriers;
    std::vector<D3D12_TEXTURE_BARRIER> m_textureBarriers;
    std::wstring m_markerName;
};

// The device side. Created by createD3D12Device() (D3D12Device.h).
class D3D12Backend final : public detail::DeviceBackend {
public:
    // Creates the device on the adapter `desc` asks for, or fails with BackendUnavailable,
    // UnsupportedDevice (naming every missing minimum feature) or the API's error.
    static Expected<std::unique_ptr<D3D12Backend>, Error> create(const DeviceDesc& desc);

    D3D12Backend() = default;
    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;
    D3D12Backend(D3D12Backend&&) = delete;
    D3D12Backend& operator=(D3D12Backend&&) = delete;
    ~D3D12Backend() override;

    Capabilities capabilities() const override { return m_capabilities; }

    Expected<std::byte*, Error> createBuffer(std::uint32_t slot, const BufferDesc& desc,
                                             const detail::BufferDescriptors& descriptors) override;
    void destroyBuffer(std::uint32_t slot) override;
    Expected<void, Error> createTexture(std::uint32_t slot, const TextureDesc& desc,
                                        const detail::TextureDescriptors& descriptors) override;
    void destroyTexture(std::uint32_t slot) override;
    Expected<void, Error> createSampler(std::uint32_t slot, const SamplerDesc& desc, std::uint32_t descriptor) override;
    void destroySampler(std::uint32_t slot) override;
    Expected<void, Error> createGraphicsPipeline(std::uint32_t slot, const GraphicsPipelineDesc& desc) override;
    Expected<void, Error> createComputePipeline(std::uint32_t slot, const ComputePipelineDesc& desc) override;
    void destroyPipeline(std::uint32_t slot) override;
    Expected<detail::SwapchainImages, Error> createSwapchain(std::uint32_t slot, const SwapchainDesc& desc,
                                                             std::span<const std::uint32_t> imageSlots) override;
    Expected<detail::SwapchainImages, Error> resizeSwapchain(std::uint32_t slot, std::uint32_t width,
                                                             std::uint32_t height) override;
    void destroySwapchain(std::uint32_t slot) override;
    Expected<detail::AcquireResult, Error> acquireImage(std::uint32_t swapchainSlot, std::uint32_t frameSlot) override;
    Expected<std::unique_ptr<detail::CommandListBackend>, Error> createCommandList() override;
    void beginFrame(std::uint32_t frameSlot) override;
    Expected<void, Error> submit(std::span<detail::CommandListBackend* const> lists,
                                 std::uint64_t signalValue) override;
    Expected<detail::PresentStatus, Error> endFrame(std::uint32_t frameSlot, std::optional<std::uint32_t> swapchainSlot,
                                                    std::uint64_t signalValue) override;
    std::uint64_t completedValue() override;
    Expected<void, Error> waitForValue(std::uint64_t value) override;

    // Removes the device as a driver reset would (removeD3D12DeviceForTesting()).
    void removeDevice();

    // Makes the next timeline signal fail (failNextD3D12SignalForTesting()).
    void failNextSignal() { m_failNextSignal.store(true, std::memory_order_release); }

    // What command lists read. Slots are written by the thread that creates the object before
    // its handle is handed out, and stay unchanged while a list records with the handle.
    [[nodiscard]] const BufferRecord& buffer(BufferHandle handle) const { return m_buffers[handle.index()]; }

    [[nodiscard]] const TextureRecord& texture(TextureHandle handle) const { return m_textures[handle.index()]; }

    [[nodiscard]] const PipelineRecord& pipeline(PipelineHandle handle) const { return m_pipelines[handle.index()]; }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView(std::uint32_t textureSlot) const;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView(std::uint32_t textureSlot) const;

    [[nodiscard]] std::array<ID3D12DescriptorHeap*, 2> shaderVisibleHeaps() const {
        return {m_resourceHeap.Get(), m_samplerHeap.Get()};
    }

    [[nodiscard]] ID3D12RootSignature* rootSignature() const { return m_rootSignature.Get(); }

    [[nodiscard]] ID3D12CommandSignature* drawIndexedSignature() const { return m_drawIndexedSignature.Get(); }

    [[nodiscard]] ID3D12CommandSignature* dispatchSignature() const { return m_dispatchSignature.Get(); }

private:
    // Sets up everything after the device exists; fails with the API's error.
    Expected<void, Error> initialize(const DeviceDesc& desc);
    // The error for a failed API call: DeviceLost (with the removal reason and DRED's report)
    // once the device is removed, OutOfMemory for E_OUTOFMEMORY, otherwise BackendFailure.
    Error failure(HRESULT result, std::string_view what) const;
    // DeviceLost with the removal reason and DRED's breadcrumbs and page-fault report.
    Error lostError(HRESULT reason) const;

    [[nodiscard]] bool removed() const { return m_device->GetDeviceRemovedReason() != S_OK; }

    // Signals `value` on the graphics queue, or fails once after failNextSignal().
    HRESULT signal(std::uint64_t value);
    // The error after a signal failed with work queued before it: removes the device.
    Error signalFailed(HRESULT result);

    // Takes the swapchain's buffers into its image slots and creates their render-target views.
    Expected<detail::SwapchainImages, Error> takeImages(SwapchainRecord& swapchain, std::uint32_t width,
                                                        std::uint32_t height);
    void releaseImages(SwapchainRecord& swapchain);
    static D3D12_CPU_DESCRIPTOR_HANDLE descriptor(ID3D12DescriptorHeap* heap, std::uint32_t size, std::uint32_t index);

    Capabilities m_capabilities;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12InfoQueue1> m_infoQueue;
    DWORD m_messageCallbackCookie = 0;
    ComPtr<D3D12MA::Allocator> m_allocator;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    // Set when the fence reaches the value waitForValue() waits for.
    EventHandle m_fenceEvent;
    std::atomic<bool> m_failNextSignal{false};
    ComPtr<ID3D12DescriptorHeap> m_resourceHeap;
    ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
    // CPU-only heaps with one view per texture slot.
    ComPtr<ID3D12DescriptorHeap> m_renderTargetHeap;
    ComPtr<ID3D12DescriptorHeap> m_depthStencilHeap;
    std::uint32_t m_resourceDescriptorSize = 0;
    std::uint32_t m_samplerDescriptorSize = 0;
    std::uint32_t m_renderTargetDescriptorSize = 0;
    std::uint32_t m_depthStencilDescriptorSize = 0;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12CommandSignature> m_drawIndexedSignature;
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    bool m_tearingSupported = false;
    // Per slot, sized from Capabilities::limits at creation so that they never move.
    std::vector<BufferRecord> m_buffers;
    std::vector<TextureRecord> m_textures;
    std::vector<PipelineRecord> m_pipelines;
    std::vector<SwapchainRecord> m_swapchains;
    // Render-thread scratch space for submit().
    std::vector<ID3D12CommandList*> m_submitLists;
};

} // namespace ith::rhi::d3d12
