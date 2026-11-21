// The implementation of the Vulkan Memory Allocator (VMA), compiled once, in this module, with the
// engine's flags: no exceptions and no RTTI, which VMA documents it doesn't use. The module has
// no Vulkan prototypes (VK_NO_PROTOTYPES), so VMA fetches its functions through the
// vkGetInstanceProcAddr and vkGetDeviceProcAddr the backend passes to vmaCreateAllocator().
// CPU-side allocations go to the C runtime: the engine has no general-purpose allocator yet.

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION

#include "ith/rhi_vulkan/VulkanApi.h"

#include <vk_mem_alloc.h>
