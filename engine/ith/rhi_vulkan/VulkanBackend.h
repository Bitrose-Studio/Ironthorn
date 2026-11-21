#pragma once

// The Vulkan backend's classes (ADR-0009, ADR-0021). Internal to the rhi_vulkan module: only its
// sources include this header, which with VulkanApi.h and VulkanLoader.h is the only one that
// names Vulkan or VMA types. Everything backend-neutral (handles, checks, deferred destruction,
// frame pacing) is done by rhi::Device before a call reaches these classes (ith/rhi/Backend.h).
//
// Binding model (ADR-0009 rules 7 to 9): one descriptor set at set 0, bound to every command list,
// with partially bound, update-after-bind runtime arrays that share the device's bindless
// indices:
//   binding 0  storage buffers   buffers' read and write indices (ByteAddressBuffer and
//                                RWByteAddressBuffer in Slang)
//   binding 1  sampled images    textures' sampled indices
//   binding 2  storage images    textures' storage indices
//   binding 3  samplers          samplers' indices
// and 128 bytes of push constants for every stage, in one pipeline layout for every pipeline.

#include "ith/foundation/Expected.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"
#include "ith/rhi_vulkan/VulkanApi.h"
#include "ith/rhi_vulkan/VulkanLoader.h"

#include <vk_mem_alloc.h>

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

namespace ith::rhi::vulkan {

// The descriptor set's bindings (see above).
inline constexpr std::uint32_t kBufferBinding = 0;
inline constexpr std::uint32_t kSampledImageBinding = 1;
inline constexpr std::uint32_t kStorageImageBinding = 2;
inline constexpr std::uint32_t kSamplerBinding = 3;

// The stages that see push constants and the bindless set.
inline constexpr VkShaderStageFlags kShaderStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

// The Vulkan format of an RHI format (VK_FORMAT_UNDEFINED for Format::Unknown).
VkFormat vulkanFormat(Format format);

// A buffer slot.
struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t size = 0;
};

// A texture slot: a texture or a swapchain image.
struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;
    // Null for swapchain images, which the swapchain owns.
    VmaAllocation allocation = VK_NULL_HANDLE;
    // All mips and layers, for sampling; mip 0 and every layer, for storage; mip 0 and layer 0,
    // for rendering. Null where the usage is missing.
    VkImageView sampledView = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE;
    VkImageView attachmentView = VK_NULL_HANDLE;
    // The aspects barriers cover: color, depth, or depth and stencil.
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    // Bytes per texel of buffer data copied to or from the texture (the depth aspect's for
    // depth textures).
    std::uint32_t texelSize = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    // Array layers as Vulkan counts them: 1 for 3D textures.
    std::uint32_t layers = 1;
    std::uint32_t mipLevels = 1;
    bool volume = false;
};

struct PipelineRecord {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

struct SwapchainRecord {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // Null after a failed recreation; the next resize creates a new one.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    // The texture slots the device reserved for the images.
    std::array<std::uint32_t, kMaxSwapchainImages> imageSlots{};
    // How many of imageSlots the swapchain uses.
    std::uint32_t imageCount = 0;
    // Signaled by acquisition, per frame slot; reused once that frame slot's earlier frame is
    // done.
    std::array<VkSemaphore, kFramesInFlight> acquired{};
    // Signaled when an image's rendering is done and waited for by its presentation, per image.
    std::array<VkSemaphore, kMaxSwapchainImages> rendered{};
    // The image acquired for the open frame.
    std::uint32_t currentImage = 0;
    // Set when acquisition reported the swapchain suboptimal; the next present reports it out
    // of date so that the device recreates it.
    bool suboptimal = false;
    // Set when presenting failed: the semaphores may be left signaled, so the next beginFrame()
    // recreates the swapchain.
    bool presentFailed = false;
    Format format = Format::Unknown;
    VkSurfaceFormatKHR surfaceFormat{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::string name;
};

class VulkanBackend;

// One pooled command list: a command pool with one primary command buffer. The device pools
// lists per frame slot (ith/rhi/Device.cpp), so a list is always begun for the same frame slot
// and one pool is enough: when it is begun, the GPU has finished the frame that last used it.
class VulkanCommandList final : public detail::CommandListBackend {
public:
    VulkanCommandList(VulkanBackend& backend, VkCommandPool pool, VkCommandBuffer commands);
    VulkanCommandList(const VulkanCommandList&) = delete;
    VulkanCommandList& operator=(const VulkanCommandList&) = delete;
    VulkanCommandList(VulkanCommandList&&) = delete;
    VulkanCommandList& operator=(VulkanCommandList&&) = delete;
    ~VulkanCommandList() override;

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
    VkResult close();

    [[nodiscard]] VkCommandBuffer native() const { return m_commands; }

private:
    // The region of `copy` in its buffer and texture.
    [[nodiscard]] VkBufferImageCopy2 region(const BufferTextureCopy& copy) const;

    VulkanBackend* m_backend;
    const VulkanFunctions* m_vk;
    VkCommandPool m_pool;
    VkCommandBuffer m_commands;
    bool m_recording = false;
    // Scratch space for barrier batches and marker names, reused to avoid allocations.
    std::vector<VkMemoryBarrier2> m_globalBarriers;
    std::vector<VkBufferMemoryBarrier2> m_bufferBarriers;
    std::vector<VkImageMemoryBarrier2> m_imageBarriers;
    std::string m_markerName;
};

// The device side. Created by createVulkanDevice() (VulkanDevice.h).
class VulkanBackend final : public detail::DeviceBackend {
public:
    // Creates the instance and the device on the physical device `desc` asks for, or fails with
    // BackendUnavailable, UnsupportedDevice (naming every missing minimum feature) or the API's
    // error.
    static Expected<std::unique_ptr<VulkanBackend>, Error> create(const DeviceDesc& desc);

    VulkanBackend() = default;
    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;
    VulkanBackend(VulkanBackend&&) = delete;
    VulkanBackend& operator=(VulkanBackend&&) = delete;
    ~VulkanBackend() override;

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

    // Makes the next queue submission fail with VK_ERROR_DEVICE_LOST
    // (failNextVulkanSubmitForTesting()).
    void failNextSubmit() { m_failNextSubmit.store(true, std::memory_order_release); }

    // What command lists read. Slots are written by the thread that creates the object before
    // its handle is handed out, and stay unchanged while a list records with the handle.
    [[nodiscard]] const BufferRecord& buffer(BufferHandle handle) const { return m_buffers[handle.index()]; }

    [[nodiscard]] const TextureRecord& texture(TextureHandle handle) const { return m_textures[handle.index()]; }

    [[nodiscard]] const PipelineRecord& pipeline(PipelineHandle handle) const { return m_pipelines[handle.index()]; }

    [[nodiscard]] const VulkanFunctions& functions() const { return m_vk; }

    [[nodiscard]] VkDevice device() const { return m_device; }

    // Whether VK_EXT_debug_utils (object names, command labels) is enabled.
    [[nodiscard]] bool debugUtils() const { return m_debugUtils; }

    [[nodiscard]] VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }

    [[nodiscard]] VkDescriptorSet descriptorSet() const { return m_descriptorSet; }

    // Gives an object a debug name, when VK_EXT_debug_utils is enabled.
    void setName(VkObjectType type, std::uint64_t handle, std::string_view name) const;

private:
    // Creates the instance, picks the physical device and creates the device.
    Expected<void, Error> createInstance(const DeviceDesc& desc);
    Expected<void, Error> pickPhysicalDevice(AdapterPreference preference);
    Expected<void, Error> createDevice(const DeviceDesc& desc);
    // Creates the device's own objects: allocator, timelines, descriptor set, pipeline layout.
    Expected<void, Error> initialize(const DeviceDesc& desc);

    // The error for a failed API call: DeviceLost (with VK_EXT_device_fault's report) for
    // VK_ERROR_DEVICE_LOST or once the device is lost, OutOfMemory for the out-of-memory
    // results, otherwise BackendFailure.
    Error failure(VkResult result, std::string_view what);
    // Marks the device lost and returns its error, with VK_EXT_device_fault's report the first
    // time.
    Error lostError();
    // As lostError(), for the render thread, which owns the queue: also waits until the device
    // is idle, so that no object the GPU may still use is released after a simulated loss.
    Error lostOnQueue();

    // Submits one batch to the graphics queue; fails once after failNextSubmit().
    VkResult submitBatch(const VkSubmitInfo2& batch);
    // The acquire-timeline wait the next batch needs, if an image was acquired in this frame.
    [[nodiscard]] std::optional<VkSemaphoreSubmitInfo> acquireWait() const;

    // Writes one descriptor of the bindless set; thread-safe.
    void writeDescriptor(const VkWriteDescriptorSet& write);
    // Creates an image view.
    Expected<VkImageView, Error> createView(VkImage image, VkImageViewType type, VkFormat format,
                                            VkImageAspectFlags aspects, std::uint32_t mipCount,
                                            std::uint32_t layerCount, std::string_view name);
    // A shader module from SPIR-V that uses only the bindless set; InvalidShader otherwise.
    Expected<VkShaderModule, Error> createShaderModule(const ShaderDesc& shader, std::string_view pipelineName);

    // Creates the swapchain of `record` at the given size and takes its images into the slots.
    Expected<detail::SwapchainImages, Error> buildSwapchain(SwapchainRecord& record, std::uint32_t width,
                                                            std::uint32_t height);
    void releaseImages(SwapchainRecord& swapchain);
    void releaseSwapchain(SwapchainRecord& swapchain);

    Capabilities m_capabilities;
    VulkanFunctions m_vk;
    VulkanLibrary m_library;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;
    VkQueue m_queue = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    // Which optional extensions are enabled. Set once during creation, then only read.
    bool m_debugUtils = false;
    bool m_swapchainExtension = false;
    bool m_deviceFault = false;
    std::uint32_t m_maxStorageBufferRange = 0;
    float m_maxAnisotropy = 1.0f;
    // The graphics timeline (Backend.h), and a second timeline that the batch after each
    // acquisition signals, which later batches of the frame wait for.
    VkSemaphore m_timeline = VK_NULL_HANDLE;
    VkSemaphore m_acquireTimeline = VK_NULL_HANDLE;
    // Render thread only (frames, submission and swapchains, Backend.h), so not locked.
    std::uint64_t m_acquireValue = 0;
    bool m_acquiredThisFrame = false;
    std::atomic<bool> m_lost{false};
    std::atomic<bool> m_failNextSubmit{false};
    // Set when the loss came from failNextSubmit(): there is no driver report to read.
    std::atomic<bool> m_simulatedLoss{false};
    // The bindless set (see above).
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    // Held around vkUpdateDescriptorSets, which needs the set externally synchronized: buffers,
    // textures and samplers are created on several threads. It guards the set's contents, not a
    // member, so there is nothing to annotate with ITH_GUARDED_BY.
    Mutex m_descriptorMutex;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    // The device-loss report, built once.
    Mutex m_lostMutex;
    std::string m_lostMessage ITH_GUARDED_BY(m_lostMutex);
    // Per slot, sized from Capabilities::limits at creation so that they never move. A slot is
    // written only by the thread creating or destroying its object (Backend.h), so not locked.
    std::vector<BufferRecord> m_buffers;
    std::vector<TextureRecord> m_textures;
    std::vector<VkSampler> m_samplers;
    std::vector<PipelineRecord> m_pipelines;
    std::vector<SwapchainRecord> m_swapchains;
    // Render-thread scratch space for submit().
    std::vector<VkCommandBufferSubmitInfo> m_submitLists;
};

} // namespace ith::rhi::vulkan
