# The Vulkan backend of the RHI (ADR-0009, ADR-0021), on Windows and Linux, and a client module
# like rhi. Vulkan-Headers and VMA are private: no Vulkan type leaves the module. It links no
# Vulkan library; VulkanLoader.cpp opens the system's Vulkan loader at run time.
ith_add_module(rhi_vulkan
    LAYER platform
    KIND client
    SOURCES
        VulkanApi.h
        VulkanBackend.h
        VulkanBackend.cpp
        VulkanCommandList.cpp
        VulkanDevice.h
        VulkanGpuTiming.h
        VulkanLoader.h
        VulkanLoader.cpp
        VulkanMemoryAllocator.cpp
        VulkanProfiler.h
        VulkanProfiler.cpp
        SpirvCheck.h
        SpirvCheck.cpp
    DEPENDS
        foundation
        rhi
    PRIVATE_LIBRARIES
        ith::vma
        Vulkan::Headers
        ${CMAKE_DL_LIBS})

# TRACY_VK_USE_SYMBOL_TABLE: with VK_NO_PROTOTYPES, Tracy's Vulkan helpers (VulkanProfiler.cpp, in
# profiling builds) fetch the functions they call through vkGetInstanceProcAddr and
# vkGetDeviceProcAddr instead of naming Vulkan's symbols.
if(TARGET ith_rhi_vulkan)
    target_compile_definitions(ith_rhi_vulkan PRIVATE VK_NO_PROTOTYPES TRACY_VK_USE_SYMBOL_TABLE)
endif()
