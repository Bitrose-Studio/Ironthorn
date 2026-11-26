#pragma once

// Loading Vulkan at run time. Internal to the rhi_vulkan module.
//
// The backend links no Vulkan library: VulkanLibrary opens the system's Vulkan loader
// (vulkan-1.dll on Windows, libvulkan.so.1 elsewhere) when a device is created, and
// VulkanFunctions holds the functions the backend calls, fetched through vkGetInstanceProcAddr
// and vkGetDeviceProcAddr. A machine without the loader gets an error, not a failure to start.
// Everything about how Vulkan is found is in this header and VulkanLoader.cpp.

#include "ith/foundation/Expected.h"
#include "ith/rhi/Error.h"
#include "ith/rhi_vulkan/VulkanApi.h"

#include <memory>

namespace ith::rhi::vulkan {

// Functions available before an instance exists.
#define ITH_VULKAN_GLOBAL_FUNCTIONS(X)                                                                                 \
    X(vkCreateInstance)                                                                                                \
    X(vkEnumerateInstanceVersion)                                                                                      \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)

// Platform code: the surface extension of each platform's window systems (ADR-0012).
#ifdef _WIN32
#define ITH_VULKAN_SURFACE_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#else
#define ITH_VULKAN_SURFACE_FUNCTIONS(X)                                                                                \
    X(vkCreateXlibSurfaceKHR)                                                                                          \
    X(vkCreateWaylandSurfaceKHR)
#endif

// Instance functions; those of instance extensions are null when the extension isn't enabled.
#define ITH_VULKAN_INSTANCE_FUNCTIONS(X)                                                                               \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                       \
    X(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    X(vkDestroyDebugUtilsMessengerEXT)                                                                                 \
    X(vkSetDebugUtilsObjectNameEXT)                                                                                    \
    X(vkCmdBeginDebugUtilsLabelEXT)                                                                                    \
    X(vkCmdEndDebugUtilsLabelEXT)                                                                                      \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)                                                                  \
    ITH_VULKAN_SURFACE_FUNCTIONS(X)

// Device functions; those of device extensions are null when the extension isn't enabled.
#define ITH_VULKAN_DEVICE_FUNCTIONS(X)                                                                                 \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkQueueWaitIdle)                                                                                                 \
    X(vkQueueSubmit2)                                                                                                  \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkGetSemaphoreCounterValue)                                                                                      \
    X(vkWaitSemaphores)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkResetCommandPool)                                                                                              \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkGetDeviceFaultInfoEXT)                                                                                         \
    X(vkGetCalibratedTimestampsEXT)                                                                                    \
    X(vkCreateQueryPool)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkCmdWriteTimestamp)                                                                                             \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkCmdPipelineBarrier2)                                                                                           \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdDrawIndexedIndirectCount)                                                                                   \
    X(vkCmdDispatch)                                                                                                   \
    X(vkCmdDispatchIndirect)                                                                                           \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdCopyBufferToImage2)                                                                                         \
    X(vkCmdCopyImageToBuffer2)

#define ITH_VULKAN_DECLARE_FUNCTION(name) PFN_##name name = nullptr;

// The Vulkan functions the backend calls. Filled once while the device is created and only read
// afterwards, so every thread may call through it.
struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    ITH_VULKAN_GLOBAL_FUNCTIONS(ITH_VULKAN_DECLARE_FUNCTION)
    ITH_VULKAN_INSTANCE_FUNCTIONS(ITH_VULKAN_DECLARE_FUNCTION)
    ITH_VULKAN_DEVICE_FUNCTIONS(ITH_VULKAN_DECLARE_FUNCTION)

    // Fetches the instance functions after vkCreateInstance().
    void loadInstance(VkInstance instance);
    // Fetches the device functions after vkCreateDevice(), through vkGetDeviceProcAddr so that
    // calls skip the loader's dispatch.
    void loadDevice(VkDevice device);
};

#undef ITH_VULKAN_DECLARE_FUNCTION

// The system's Vulkan loader, open while this object lives. Move-only.
class VulkanLibrary {
public:
    // Opens the loader and fetches vkGetInstanceProcAddr and the global functions into
    // `functions`. Fails with BackendUnavailable when there is no loader, or when it is older
    // than Vulkan 1.3.
    static Expected<VulkanLibrary, Error> open(VulkanFunctions& functions);

    VulkanLibrary() = default;
    VulkanLibrary(VulkanLibrary&&) noexcept = default;
    VulkanLibrary& operator=(VulkanLibrary&&) noexcept = default;
    VulkanLibrary(const VulkanLibrary&) = delete;
    VulkanLibrary& operator=(const VulkanLibrary&) = delete;
    ~VulkanLibrary() = default;

private:
    struct Closer {
        void operator()(void* library) const;
    };

    std::unique_ptr<void, Closer> m_library;
};

} // namespace ith::rhi::vulkan
