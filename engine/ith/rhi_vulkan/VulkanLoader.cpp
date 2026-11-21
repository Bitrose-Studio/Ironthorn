#include "ith/rhi_vulkan/VulkanLoader.h"

#include "ith/foundation/Expected.h"
#include "ith/rhi/Error.h"
#include "ith/rhi_vulkan/VulkanApi.h"

// Platform code: opening a shared library is the operating system's job.
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <cstdint>
#include <string>

namespace ith::rhi::vulkan {
namespace {

#ifdef _WIN32
constexpr const char* kLoaderName = "vulkan-1.dll";

void* openLibrary() {
    // Only the system directory, where the driver installs the loader: never one next to the
    // executable or in the working directory.
    return LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

PFN_vkGetInstanceProcAddr findGetInstanceProcAddr(void* library) {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): an exported function, cast through
    // void (*)(), the type that converts to any function pointer without a warning
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(library), "vkGetInstanceProcAddr")));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}
#else
constexpr const char* kLoaderName = "libvulkan.so.1";

void* openLibrary() {
    return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
}

PFN_vkGetInstanceProcAddr findGetInstanceProcAddr(void* library) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an exported function
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
}
#endif

Unexpected<Error> unavailable(std::string message) {
    return makeUnexpected(Error{.code = ErrorCode::BackendUnavailable, .message = std::move(message)});
}

} // namespace

void VulkanLibrary::Closer::operator()(void* library) const {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

Expected<VulkanLibrary, Error> VulkanLibrary::open(VulkanFunctions& functions) {
    VulkanLibrary library;
    library.m_library.reset(openLibrary());
    if (library.m_library == nullptr) {
        return unavailable(std::string("The Vulkan loader (") + kLoaderName +
                           ") is not installed: install a GPU driver with Vulkan support");
    }
    functions.vkGetInstanceProcAddr = findGetInstanceProcAddr(library.m_library.get());
    if (functions.vkGetInstanceProcAddr == nullptr) {
        return unavailable(std::string("The Vulkan loader (") + kLoaderName + ") has no vkGetInstanceProcAddr");
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan returns generic function pointers
#define ITH_VULKAN_LOAD_GLOBAL(name)                                                                                   \
    functions.name = reinterpret_cast<PFN_##name>(functions.vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    ITH_VULKAN_GLOBAL_FUNCTIONS(ITH_VULKAN_LOAD_GLOBAL)
#undef ITH_VULKAN_LOAD_GLOBAL
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    // vkEnumerateInstanceVersion exists from Vulkan 1.1 on.
    std::uint32_t version = VK_API_VERSION_1_0;
    if (functions.vkEnumerateInstanceVersion != nullptr) {
        functions.vkEnumerateInstanceVersion(&version);
    }
    if (functions.vkCreateInstance == nullptr || version < VK_API_VERSION_1_3) {
        return unavailable("The Vulkan loader supports Vulkan " + std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                           std::to_string(VK_API_VERSION_MINOR(version)) + "; Ironthorn needs 1.3");
    }
    return library;
}

void VulkanFunctions::loadInstance(VkInstance instance) {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan returns generic function pointers
#define ITH_VULKAN_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    ITH_VULKAN_INSTANCE_FUNCTIONS(ITH_VULKAN_LOAD_INSTANCE)
#undef ITH_VULKAN_LOAD_INSTANCE
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

void VulkanFunctions::loadDevice(VkDevice device) {
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan returns generic function pointers
#define ITH_VULKAN_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    ITH_VULKAN_DEVICE_FUNCTIONS(ITH_VULKAN_LOAD_DEVICE)
#undef ITH_VULKAN_LOAD_DEVICE
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

} // namespace ith::rhi::vulkan
