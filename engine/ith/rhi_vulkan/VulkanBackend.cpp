#include "ith/rhi_vulkan/VulkanBackend.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Backend.h"
#include "ith/rhi/Pipeline.h"
#include "ith/rhi_vulkan/SpirvCheck.h"
#include "ith/rhi_vulkan/VulkanApi.h"
#include "ith/rhi_vulkan/VulkanDevice.h"
#include "ith/rhi_vulkan/VulkanGpuTiming.h"
#include "ith/rhi_vulkan/VulkanLoader.h"
#include "ith/rhi_vulkan/VulkanProfiler.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ith::rhi::vulkan {
namespace {

// The indirect argument layouts of CommandList.h are Vulkan's.
static_assert(sizeof(DrawIndexedIndirectArguments) == sizeof(VkDrawIndexedIndirectCommand));
static_assert(sizeof(DispatchIndirectArguments) == sizeof(VkDispatchIndirectCommand));
// Swapchain images asked for; the surface may need more, up to kMaxSwapchainImages.
constexpr std::uint32_t kSwapchainImageCount = 3;
// How long one wait for the GPU or for a swapchain image lasts before the device is checked for
// loss again (ADR-0009 rule 19): no wait is without a limit.
constexpr std::uint64_t kWaitStepNs = 1'000'000'000;
// How many of those steps acquiring a swapchain image may take before it fails.
constexpr std::uint32_t kMaxAcquireSteps = 10;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Whether this build may load the validation layer: never in Shipping.
#ifdef ITH_SHIPPING
constexpr bool kValidationAllowed = false;
#else
constexpr bool kValidationAllowed = true;
#endif
// Set by avoidCalibratedVulkanGpuTimingForTesting(); read when a device is created.
std::atomic<bool> g_avoidCalibratedGpuTiming{false};

const char* resultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:
            return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:
            return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY:
            return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INVALID_SHADER_NV:
            return "VK_ERROR_INVALID_SHADER_NV";
        default:
            return "VkResult";
    }
}

std::string describe(VkResult result) {
    return std::string(resultName(result)) + " " + std::to_string(static_cast<int>(result));
}

Unexpected<Error> fail(ErrorCode code, std::string message) {
    return makeUnexpected(Error{.code = code, .message = std::move(message)});
}

std::string quoted(std::string_view name) {
    return "'" + std::string(name) + "'";
}

[[maybe_unused]] bool isSrgb(Format format) {
    return format == Format::R8G8B8A8Srgb || format == Format::B8G8R8A8Srgb;
}

VkImageAspectFlags aspectsOf(Format format) {
    switch (format) {
        case Format::D32Float:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case Format::D32FloatS8Uint:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkCompareOp compareOp(CompareOp op) {
    switch (op) {
        case CompareOp::Never:
            return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:
            return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:
            return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:
            return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:
            return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkSamplerAddressMode addressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFilter filter(Filter value) {
    return value == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkPipelineColorBlendAttachmentState blendState(BlendMode mode) {
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (mode) {
        case BlendMode::Opaque:
            blend.blendEnable = VK_FALSE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            return blend;
        case BlendMode::Alpha:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Premultiplied:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    // As on Direct3D 12: alpha blends like color, except that straight alpha keeps the source's
    // alpha whole.
    blend.blendEnable = VK_TRUE;
    blend.srcAlphaBlendFactor =
        blend.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA ? VK_BLEND_FACTOR_ONE : blend.srcColorBlendFactor;
    blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
    return blend;
}

VkPrimitiveTopology topology(PrimitiveTopology value) {
    switch (value) {
        case PrimitiveTopology::TriangleList:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::PointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkShaderStageFlagBits shaderStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Pixel:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_COMPUTE_BIT;
}

#ifndef ITH_SHIPPING
// Writes the validation layer's warnings and errors to stderr (there is no engine log yet) and
// asserts on validation errors, so that they stop tests and development builds.
VKAPI_ATTR VkBool32 VKAPI_CALL onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*context*/) {
    // The message id (a VUID, or SYNC-HAZARD-... for synchronization hazards) names the problem;
    // the text doesn't always.
    const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    const char* id = data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "";
    std::fprintf(stderr, "Vulkan %s [%s]: %s\n", error ? "error" : "warning", id,
                 data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    ITH_ASSERT_MSG(!error || (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0,
                   "the Vulkan validation layer reported an error (see stderr)");
    return VK_FALSE;
}
#endif

bool hasExtension(std::span<const VkExtensionProperties> extensions, const char* name) {
    return std::ranges::any_of(extensions, [&](const VkExtensionProperties& extension) {
        return std::strcmp(static_cast<const char*>(extension.extensionName), name) == 0;
    });
}

// Everything the device check reads about a physical device.
struct DeviceInfo {
    VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties properties11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties properties12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties properties13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFaultFeaturesEXT fault{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    std::vector<VkExtensionProperties> extensions;
    std::optional<std::uint32_t> queueFamily;

    DeviceInfo() = default;
    DeviceInfo(const DeviceInfo&) = delete;
    DeviceInfo& operator=(const DeviceInfo&) = delete;
    DeviceInfo(DeviceInfo&&) = delete;
    DeviceInfo& operator=(DeviceInfo&&) = delete;
    ~DeviceInfo() = default;

    // Reads it; the chains point into this object, which therefore never moves.
    void read(const VulkanFunctions& vk, VkPhysicalDevice device) {
        properties.pNext = &properties11;
        properties11.pNext = &properties12;
        properties12.pNext = &properties13;
        vk.vkGetPhysicalDeviceProperties2(device, &properties);
        std::uint32_t count = 0;
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        extensions.resize(count);
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
        extensions.resize(count);
        features.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;
        if (hasExtension(extensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
            features13.pNext = &fault;
        }
        vk.vkGetPhysicalDeviceFeatures2(device, &features);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
        constexpr VkQueueFlags kGraphicsAndCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        for (std::uint32_t family = 0; family < count; ++family) {
            if ((families[family].queueFlags & kGraphicsAndCompute) == kGraphicsAndCompute) {
                queueFamily = family;
                break;
            }
        }
    }

    [[nodiscard]] std::string name() const { return static_cast<const char*>(properties.properties.deviceName); }
};

// The minimum features of ADR-0005, and the descriptor capacities of ADR-0009 rule 10, that a
// device lacks, by name.
std::vector<std::string_view> missingFeatures(const DeviceInfo& info, const Limits& limits) {
    std::vector<std::string_view> missing;
    const auto require = [&](bool present, std::string_view name) {
        if (!present) {
            missing.push_back(name);
        }
    };
    require(info.properties.properties.apiVersion >= VK_API_VERSION_1_3, "Vulkan 1.3");
    require(info.queueFamily.has_value(), "a graphics and compute queue");
    const VkPhysicalDeviceFeatures& core = info.features.features;
    const VkPhysicalDeviceVulkan12Features& v12 = info.features12;
    const VkPhysicalDeviceVulkan13Features& v13 = info.features13;
    require(v13.dynamicRendering == VK_TRUE, "dynamic rendering");
    require(v13.synchronization2 == VK_TRUE, "synchronization2");
    require(v13.maintenance4 == VK_TRUE, "maintenance4");
    require(v12.timelineSemaphore == VK_TRUE, "timeline semaphores");
    require(v12.bufferDeviceAddress == VK_TRUE, "buffer device address");
    require(v12.drawIndirectCount == VK_TRUE, "drawIndirectCount");
    require(core.multiDrawIndirect == VK_TRUE, "multi-draw indirect");
    require(core.independentBlend == VK_TRUE, "independent blending");
    require(core.imageCubeArray == VK_TRUE, "cube map arrays");
    // ADR-0022: out-of-bounds buffer accesses stay inside the buffer, also through the bindless set.
    require(core.robustBufferAccess == VK_TRUE && info.properties12.robustBufferAccessUpdateAfterBind == VK_TRUE,
            "robust buffer access (with update-after-bind descriptors)");
    require(v12.runtimeDescriptorArray == VK_TRUE, "runtime descriptor arrays");
    require(v12.descriptorBindingPartiallyBound == VK_TRUE, "partially bound descriptors");
    require(v12.descriptorBindingUpdateUnusedWhilePending == VK_TRUE &&
                v12.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
                v12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
                v12.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE,
            "update-after-bind descriptors");
    require(v12.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
                v12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
                v12.shaderStorageImageArrayNonUniformIndexing == VK_TRUE,
            "non-uniform descriptor indexing");
    const VkPhysicalDeviceVulkan11Properties& p11 = info.properties11;
    constexpr VkSubgroupFeatureFlags kSubgroupOperations =
        VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    require((p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0 &&
                (p11.subgroupSupportedOperations & kSubgroupOperations) == kSubgroupOperations,
            "subgroup ballot and arithmetic in compute shaders");
    const VkPhysicalDeviceVulkan12Properties& p12 = info.properties12;
    const std::uint32_t resources = limits.resourceDescriptors;
    // Three bindings share the resource indices, plus a stage's color attachments.
    const std::uint64_t perStageResources = std::uint64_t{resources} * 3 + kMaxColorTargets;
    require(p12.maxDescriptorSetUpdateAfterBindStorageBuffers >= resources &&
                p12.maxDescriptorSetUpdateAfterBindSampledImages >= resources &&
                p12.maxDescriptorSetUpdateAfterBindStorageImages >= resources &&
                p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= resources &&
                p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= resources &&
                p12.maxPerStageDescriptorUpdateAfterBindStorageImages >= resources &&
                p12.maxPerStageUpdateAfterBindResources >= perStageResources,
            "262,144 update-after-bind resource descriptors per stage");
    require(p12.maxUpdateAfterBindDescriptorsInAllPools >= perStageResources + limits.samplerDescriptors,
            "262,144 update-after-bind resource descriptors and 2,048 samplers in all descriptor pools");
    require(p12.maxDescriptorSetUpdateAfterBindSamplers >= limits.samplerDescriptors &&
                p12.maxPerStageDescriptorUpdateAfterBindSamplers >= limits.samplerDescriptors &&
                info.properties.properties.limits.maxSamplerAllocationCount >= limits.samplerDescriptors,
            "2,048 update-after-bind samplers");
    require(info.properties.properties.limits.maxPushConstantsSize >= kMaxPushConstantSize,
            "128 bytes of push constants");
    return missing;
}

// Lower is preferred; std::nullopt for a device of the wrong kind.
std::optional<int> rank(VkPhysicalDeviceType type, AdapterPreference preference) {
    if (preference == AdapterPreference::Software) {
        return type == VK_PHYSICAL_DEVICE_TYPE_CPU ? std::optional<int>(0) : std::nullopt;
    }
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            return 3;
        default:
            return std::nullopt;
    }
}

} // namespace

// --- Formats ------------------------------------------------------------------------------

VkFormat vulkanFormat(Format format) {
    switch (format) {
        case Format::Unknown:
            return VK_FORMAT_UNDEFINED;
        case Format::R8Unorm:
            return VK_FORMAT_R8_UNORM;
        case Format::R8G8Unorm:
            return VK_FORMAT_R8G8_UNORM;
        case Format::R8G8B8A8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8Srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::B8G8R8A8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::B8G8R8A8Srgb:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R10G10B10A2Unorm:
            // The same bits as DXGI's R10G10B10A2: red in the lowest 10.
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::R11G11B10Float:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case Format::R16G16Float:
            return VK_FORMAT_R16G16_SFLOAT;
        case Format::R16G16B16A16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32Uint:
            return VK_FORMAT_R32_UINT;
        case Format::R32Float:
            return VK_FORMAT_R32_SFLOAT;
        case Format::R32G32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::R32G32B32A32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::D32Float:
            return VK_FORMAT_D32_SFLOAT;
        case Format::D32FloatS8Uint:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

// --- Creation -----------------------------------------------------------------------------

Expected<std::unique_ptr<VulkanBackend>, Error> VulkanBackend::create(const DeviceDesc& desc) {
    auto backend = std::make_unique<VulkanBackend>();
    Expected<VulkanLibrary, Error> library = VulkanLibrary::open(backend->m_vk);
    if (!library) {
        return makeUnexpected(std::move(library).error());
    }
    backend->m_library = std::move(*library);
    if (Expected<void, Error> created = backend->createInstance(desc); !created) {
        return makeUnexpected(std::move(created).error());
    }
    if (Expected<void, Error> picked = backend->pickPhysicalDevice(desc.adapter); !picked) {
        return makeUnexpected(std::move(picked).error());
    }
    if (Expected<void, Error> created = backend->createDevice(desc); !created) {
        return makeUnexpected(std::move(created).error());
    }
    if (Expected<void, Error> initialized = backend->initialize(desc); !initialized) {
        return makeUnexpected(std::move(initialized).error());
    }
    return backend;
}

Expected<void, Error> VulkanBackend::createInstance(const DeviceDesc& desc) {
    std::uint32_t count = 0;
    m_vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    m_vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    available.resize(count);

    // The validation layer, when asked for and installed (ADR-0009 rule 18), and its extensions.
    bool validation = false;
    std::vector<VkExtensionProperties> layerExtensions;
    if (desc.validation && kValidationAllowed) {
        m_vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        m_vk.vkEnumerateInstanceLayerProperties(&count, layers.data());
        layers.resize(count);
        validation = std::ranges::any_of(layers, [](const VkLayerProperties& layer) {
            return std::strcmp(static_cast<const char*>(layer.layerName), kValidationLayer) == 0;
        });
        if (validation) {
            m_vk.vkEnumerateInstanceExtensionProperties(kValidationLayer, &count, nullptr);
            layerExtensions.resize(count);
            m_vk.vkEnumerateInstanceExtensionProperties(kValidationLayer, &count, layerExtensions.data());
            layerExtensions.resize(count);
        } else {
            std::fprintf(stderr, "Vulkan: the validation layer (%s) is not installed; running without validation\n",
                         kValidationLayer);
        }
    }

    std::vector<const char*> extensions;
    const auto enable = [&](const char* name, bool fromLayer = false) {
        if (hasExtension(available, name) || (fromLayer && hasExtension(layerExtensions, name))) {
            extensions.push_back(name);
            return true;
        }
        return false;
    };
    // Presentation, where the window systems' extensions exist; headless drivers have none.
    if (enable(VK_KHR_SURFACE_EXTENSION_NAME)) {
#ifdef _WIN32
        enable(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
        enable(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        enable(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
    }
    m_debugUtils = enable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, validation);
    const bool layerSettings = validation && enable(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, true);

    const std::string applicationName(desc.applicationName);
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pApplicationName = applicationName.c_str(),
                                        .pEngineName = "Ironthorn",
                                        .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &application,
                              .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                              .ppEnabledExtensionNames = extensions.data()};
    if (validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &kValidationLayer;
    }

    // Synchronization validation is off in the layer by default.
    const VkBool32 on = VK_TRUE;
    const VkLayerSettingEXT syncValidation{.pLayerName = kValidationLayer,
                                           .pSettingName = "validate_sync",
                                           .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
                                           .valueCount = 1,
                                           .pValues = &on};
    const VkLayerSettingsCreateInfoEXT settings{
        .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT, .settingCount = 1, .pSettings = &syncValidation};
    VkDebugUtilsMessengerCreateInfoEXT messenger{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
#ifndef ITH_SHIPPING
    messenger.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger.pfnUserCallback = onDebugMessage;
#endif
    if (layerSettings) {
        messenger.pNext = &settings;
    } else if (validation) {
        std::fprintf(stderr, "Vulkan: the validation layer lacks %s; synchronization validation is off\n",
                     VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    }
    // The messenger also reports what happens while the instance is created and destroyed.
    if (validation && m_debugUtils) {
        info.pNext = &messenger;
    }

    if (const VkResult result = m_vk.vkCreateInstance(&info, nullptr, &m_instance); result != VK_SUCCESS) {
        m_instance = VK_NULL_HANDLE;
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
            return fail(ErrorCode::BackendUnavailable,
                        "No Vulkan driver is installed, or none supports Vulkan 1.3 (VK_ERROR_INCOMPATIBLE_DRIVER)");
        }
        return fail(ErrorCode::BackendUnavailable, "Creating the Vulkan instance failed (" + describe(result) + ")");
    }
    m_vk.loadInstance(m_instance);
    if (validation && m_debugUtils && m_vk.vkCreateDebugUtilsMessengerEXT != nullptr) {
        messenger.pNext = nullptr;
        m_vk.vkCreateDebugUtilsMessengerEXT(m_instance, &messenger, nullptr, &m_messenger);
    }
    return {};
}

Expected<void, Error> VulkanBackend::pickPhysicalDevice(AdapterPreference preference) {
    std::uint32_t count = 0;
    m_vk.vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    if (const VkResult result = m_vk.vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
        result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return fail(ErrorCode::BackendUnavailable, "Listing the Vulkan devices failed (" + describe(result) + ")");
    }
    devices.resize(count);
    if (devices.empty()) {
        return fail(ErrorCode::BackendUnavailable, "No Vulkan driver reports a device");
    }

    // The devices of the preferred kind, best first; the first that meets the minimum wins.
    struct Candidate {
        VkPhysicalDevice device = VK_NULL_HANDLE;
        int rank = 0;
    };

    std::vector<Candidate> candidates;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        m_vk.vkGetPhysicalDeviceProperties2(device, &properties);
        if (const std::optional<int> order = rank(properties.properties.deviceType, preference)) {
            candidates.push_back({.device = device, .rank = *order});
        }
    }
    std::ranges::stable_sort(candidates, {}, &Candidate::rank);
    if (candidates.empty()) {
        return fail(ErrorCode::UnsupportedDevice, preference == AdapterPreference::Software
                                                      ? "No software Vulkan device (such as lavapipe) is installed"
                                                      : "No hardware GPU supports Vulkan");
    }

    std::optional<Error> firstProblem;
    for (const Candidate& candidate : candidates) {
        const auto info = std::make_unique<DeviceInfo>();
        info->read(m_vk, candidate.device);
        const std::vector<std::string_view> missing = missingFeatures(*info, m_capabilities.limits);
        if (!missing.empty()) {
            if (!firstProblem) {
                firstProblem = detail::unsupportedDeviceError(info->name(), missing);
            }
            continue;
        }
        m_physicalDevice = candidate.device;
        m_queueFamily = info->queueFamily.value_or(0); // present: missingFeatures() checks it
        const VkPhysicalDeviceProperties& properties = info->properties.properties;
        const VkPhysicalDeviceLimits& limits = properties.limits;
        m_maxStorageBufferRange = limits.maxStorageBufferRange;
        m_maxAnisotropy = info->features.features.samplerAnisotropy == VK_TRUE ? limits.maxSamplerAnisotropy : 1.0f;
        m_deviceFault =
            hasExtension(info->extensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && info->fault.deviceFault == VK_TRUE;

        Capabilities& capabilities = m_capabilities;
        capabilities.backend = Backend::Vulkan;
        capabilities.shaderFormat = ShaderFormat::Spirv;
        const bool software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        capabilities.adapter = AdapterInfo{.name = info->name(),
                                           .vendorId = properties.vendorID,
                                           .deviceId = properties.deviceID,
                                           .software = software};
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            VkPhysicalDeviceMemoryProperties memory{};
            m_vk.vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
            for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
                if ((memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                    capabilities.adapter.dedicatedMemory =
                        std::max<std::uint64_t>(capabilities.adapter.dedicatedMemory, memory.memoryHeaps[heap].size);
                }
            }
        }
        capabilities.limits.maxTextureDimension2D = std::min(limits.maxImageDimension2D, limits.maxImageDimensionCube);
        capabilities.limits.maxTextureDimension3D = limits.maxImageDimension3D;
        capabilities.limits.maxTextureLayers = limits.maxImageArrayLayers;
        capabilities.limits.maxBufferSize = info->properties13.maxBufferSize;
        // Reported, not enabled: their commands come with the renderer paths that use them.
        capabilities.meshShaders = hasExtension(info->extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        capabilities.rayTracing = hasExtension(info->extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                                  hasExtension(info->extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        capabilities.variableRateShading = hasExtension(info->extensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        capabilities.samplerFeedback = false;
        m_swapchainExtension =
            m_vk.vkDestroySurfaceKHR != nullptr && hasExtension(info->extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        return {};
    }
    return makeUnexpected(
        std::move(firstProblem)
            .value_or(Error{.code = ErrorCode::UnsupportedDevice, .message = "No GPU meets the minimum"}));
}

Expected<void, Error> VulkanBackend::createDevice(const DeviceDesc& /*desc*/) {
    const auto info = std::make_unique<DeviceInfo>();
    info->read(m_vk, m_physicalDevice);
    const VkPhysicalDeviceFeatures& supported = info->features.features;

    // The minimum of ADR-0005 that the backend uses, and a few universal conveniences where present.
    VkPhysicalDeviceFaultFeaturesEXT fault{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
                                           .deviceFault = VK_TRUE};
    VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                         .pNext = m_deviceFault ? &fault : nullptr,
                                         .synchronization2 = VK_TRUE,
                                         .dynamicRendering = VK_TRUE,
                                         .maintenance4 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &v13;
    v12.drawIndirectCount = VK_TRUE;
    v12.descriptorIndexing = VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    // Required, though no shader uses addresses yet (ADR-0005; ADR-0009 rule 8).
    v12.bufferDeviceAddress = VK_TRUE;
    v12.vulkanMemoryModel = info->features12.vulkanMemoryModel;
    v12.vulkanMemoryModelDeviceScope = info->features12.vulkanMemoryModelDeviceScope;
    VkPhysicalDeviceVulkan11Features v11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
    v11.shaderDrawParameters = info->features11.shaderDrawParameters;
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
    features.features.multiDrawIndirect = VK_TRUE;
    features.features.independentBlend = VK_TRUE;
    features.features.imageCubeArray = VK_TRUE;
    // Out-of-bounds buffer accesses stay inside the buffer (ADR-0022).
    features.features.robustBufferAccess = VK_TRUE;
    features.features.samplerAnisotropy = supported.samplerAnisotropy;
    features.features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    features.features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;

    std::vector<const char*> extensions;
    if (m_swapchainExtension) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (m_deviceFault) {
        extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
#ifdef ITH_ENABLE_PROFILING
    // GPU zones (VulkanGpuTiming.h): what they need is enabled in profiling builds only, and only
    // where the GPU has it, so the minimum is the same in every build.
    GpuTimingSupport timing =
        readGpuTimingSupport(m_vk, m_physicalDevice, m_queueFamily, info->features12.hostQueryReset == VK_TRUE,
                             hasExtension(info->extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME));
    if (g_avoidCalibratedGpuTiming.load(std::memory_order_acquire)) {
        timing.calibratedTimestamps = false;
    }
    m_gpuTiming = chooseGpuTiming(timing);
    if (m_gpuTiming == VulkanGpuTiming::Calibrated) {
        v12.hostQueryReset = VK_TRUE;
        extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
#endif
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                        .queueFamilyIndex = m_queueFamily,
                                        .queueCount = 1,
                                        .pQueuePriorities = &priority};
    const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                        .pNext = &features,
                                        .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &queue,
                                        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
                                        .ppEnabledExtensionNames = extensions.data()};
    if (const VkResult result = m_vk.vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device);
        result != VK_SUCCESS) {
        m_device = VK_NULL_HANDLE;
        Error error = failure(result, "creating the device");
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::UnsupportedDevice;
        }
        return makeUnexpected(std::move(error));
    }
    m_vk.loadDevice(m_device);
    m_vk.vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
    return {};
}

Expected<void, Error> VulkanBackend::initialize(const DeviceDesc& desc) {
    const Limits& limits = m_capabilities.limits;
    setName(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<std::uint64_t>(m_device), // NOLINT(*-reinterpret-cast)
            desc.applicationName);
    setName(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<std::uint64_t>(m_queue), // NOLINT(*-reinterpret-cast)
            "Graphics queue");

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = m_vk.vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = m_vk.vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.pVulkanFunctions = &functions;
    allocatorInfo.instance = m_instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    if (const VkResult result = vmaCreateAllocator(&allocatorInfo, &m_allocator); result != VK_SUCCESS) {
        m_allocator = VK_NULL_HANDLE;
        return makeUnexpected(failure(result, "creating the GPU memory allocator"));
    }

    // The graphics timeline and the acquisition timeline.
    const VkSemaphoreTypeCreateInfo timelineType{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                 .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                                 .initialValue = 0};
    const VkSemaphoreCreateInfo timelineInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timelineType};
    for (VkSemaphore* semaphore : {&m_timeline, &m_acquireTimeline}) {
        if (const VkResult result = m_vk.vkCreateSemaphore(m_device, &timelineInfo, nullptr, semaphore);
            result != VK_SUCCESS) {
            *semaphore = VK_NULL_HANDLE;
            return makeUnexpected(failure(result, "creating a timeline semaphore"));
        }
    }
    setName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(m_timeline), // NOLINT(*-reinterpret-cast)
            "Graphics timeline");
    setName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(m_acquireTimeline), // NOLINT(*-reinterpret-cast)
            "Acquisition timeline");

    // The bindless set (ADR-0009 rule 7).
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
        {kBufferBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, limits.resourceDescriptors, kShaderStages, nullptr},
        {kSampledImageBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, limits.resourceDescriptors, kShaderStages, nullptr},
        {kStorageImageBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, limits.resourceDescriptors, kShaderStages, nullptr},
        {kSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, limits.samplerDescriptors, kShaderStages, nullptr},
    }};
    constexpr VkDescriptorBindingFlags kBindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                       VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                       VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const std::array<VkDescriptorBindingFlags, 4> bindingFlags{kBindingFlags, kBindingFlags, kBindingFlags,
                                                               kBindingFlags};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindingFlags.size()),
        .pBindingFlags = bindingFlags.data()};
    const VkDescriptorSetLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                     .pNext = &flagsInfo,
                                                     .flags =
                                                         VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                     .bindingCount = static_cast<std::uint32_t>(bindings.size()),
                                                     .pBindings = bindings.data()};
    if (const VkResult result = m_vk.vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout);
        result != VK_SUCCESS) {
        m_setLayout = VK_NULL_HANDLE;
        return makeUnexpected(failure(result, "creating the bindless descriptor set layout"));
    }
    const std::array<VkDescriptorPoolSize, 4> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, limits.resourceDescriptors},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, limits.resourceDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, limits.resourceDescriptors},
        {VK_DESCRIPTOR_TYPE_SAMPLER, limits.samplerDescriptors},
    }};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                              .maxSets = 1,
                                              .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
                                              .pPoolSizes = sizes.data()};
    if (const VkResult result = m_vk.vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
        result != VK_SUCCESS) {
        m_descriptorPool = VK_NULL_HANDLE;
        return makeUnexpected(failure(result, "creating the bindless descriptor pool"));
    }
    const VkDescriptorSetAllocateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = m_descriptorPool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &m_setLayout};
    if (const VkResult result = m_vk.vkAllocateDescriptorSets(m_device, &setInfo, &m_descriptorSet);
        result != VK_SUCCESS) {
        m_descriptorSet = VK_NULL_HANDLE;
        return makeUnexpected(failure(result, "allocating the bindless descriptor set"));
    }
    setName(VK_OBJECT_TYPE_DESCRIPTOR_SET,
            reinterpret_cast<std::uint64_t>(m_descriptorSet), // NOLINT(*-reinterpret-cast)
            "Bindless set");

    // The one pipeline layout: the bindless set and 128 bytes of push constants (rule 9).
    const VkPushConstantRange constants{.stageFlags = kShaderStages, .offset = 0, .size = kMaxPushConstantSize};
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                        .setLayoutCount = 1,
                                                        .pSetLayouts = &m_setLayout,
                                                        .pushConstantRangeCount = 1,
                                                        .pPushConstantRanges = &constants};
    if (const VkResult result = m_vk.vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
        result != VK_SUCCESS) {
        m_pipelineLayout = VK_NULL_HANDLE;
        return makeUnexpected(failure(result, "creating the bindless pipeline layout"));
    }

    m_buffers.resize(limits.buffers);
    m_textures.resize(limits.textures);
    m_samplers.resize(limits.samplers, VK_NULL_HANDLE);
    m_pipelines.resize(limits.pipelines);
    m_swapchains.resize(limits.swapchains);
#ifdef ITH_ENABLE_PROFILING
    // Last, with the queue still idle: the uncalibrated path submits to it and waits.
    m_gpuProfiler.start({.vk = &m_vk,
                         .instance = m_instance,
                         .physicalDevice = m_physicalDevice,
                         .device = m_device,
                         .queue = m_queue,
                         .queueFamily = m_queueFamily,
                         .timing = m_gpuTiming});
#endif
    return {};
}

VulkanBackend::~VulkanBackend() {
    // The device has destroyed every object and waited for the GPU (Backend.h); this also covers
    // a backend whose creation stopped half-way.
    if (m_device != VK_NULL_HANDLE) {
        m_vk.vkDeviceWaitIdle(m_device);
#ifdef ITH_ENABLE_PROFILING
        // Its query pool belongs to the device.
        m_gpuProfiler.stop();
#endif
        for (SwapchainRecord& swapchain : m_swapchains) {
            releaseSwapchain(swapchain);
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) {
            m_vk.vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        }
        if (m_descriptorPool != VK_NULL_HANDLE) {
            m_vk.vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        }
        if (m_setLayout != VK_NULL_HANDLE) {
            m_vk.vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        }
        for (VkSemaphore semaphore : {m_timeline, m_acquireTimeline}) {
            if (semaphore != VK_NULL_HANDLE) {
                m_vk.vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        if (m_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(m_allocator);
        }
        m_vk.vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
        if (m_messenger != VK_NULL_HANDLE) {
            m_vk.vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
        }
        m_vk.vkDestroyInstance(m_instance, nullptr);
    }
}

// --- Errors -------------------------------------------------------------------------------

Error VulkanBackend::failure(VkResult result, std::string_view what) {
    if (result == VK_ERROR_DEVICE_LOST || m_lost.load(std::memory_order_acquire)) {
        return lostError();
    }
    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        return Error{.code = ErrorCode::OutOfMemory, .message = "Out of memory " + std::string(what)};
    }
    return Error{.code = ErrorCode::BackendFailure,
                 .message = "Vulkan failed " + std::string(what) + " (" + describe(result) + ")"};
}

Error VulkanBackend::lostError() {
    m_lost.store(true, std::memory_order_release);
    const MutexLock lock(m_lostMutex);
    if (!m_lostMessage.empty()) {
        return Error{.code = ErrorCode::DeviceLost, .message = m_lostMessage};
    }
    m_lostMessage = "The Vulkan device was lost (VK_ERROR_DEVICE_LOST) on '" + m_capabilities.adapter.name + "'";
    if (m_simulatedLoss.load(std::memory_order_acquire)) {
        m_lostMessage += "; simulated by failNextVulkanSubmitForTesting()";
    } else if (m_deviceFault && m_vk.vkGetDeviceFaultInfoEXT != nullptr) {
        // VK_EXT_device_fault: the driver's description, the faulting addresses and vendor data.
        VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (m_vk.vkGetDeviceFaultInfoEXT(m_device, &counts, nullptr) == VK_SUCCESS) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            counts.vendorBinarySize = 0;
            VkDeviceFaultInfoEXT fault{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
                                       .pAddressInfos = addresses.data(),
                                       .pVendorInfos = vendors.data()};
            const VkResult result = m_vk.vkGetDeviceFaultInfoEXT(m_device, &counts, &fault);
            if (result == VK_SUCCESS || result == VK_INCOMPLETE) {
                m_lostMessage += "; device fault: " + std::string(static_cast<const char*>(fault.description));
                for (std::uint32_t index = 0; index < counts.addressInfoCount; ++index) {
                    std::array<char, 64> address{};
                    std::snprintf(address.data(), address.size(), "address 0x%llX (type %d)",
                                  static_cast<unsigned long long>(addresses[index].reportedAddress),
                                  static_cast<int>(addresses[index].addressType));
                    m_lostMessage += ", " + std::string(address.data());
                }
                for (std::uint32_t index = 0; index < counts.vendorInfoCount; ++index) {
                    m_lostMessage += ", " + std::string(static_cast<const char*>(vendors[index].description));
                }
            }
        }
    }
    return Error{.code = ErrorCode::DeviceLost, .message = m_lostMessage};
}

Error VulkanBackend::lostOnQueue() {
    Error error = lostError();
    // A lost device returns at once; after a simulated loss the GPU finishes its work first. The
    // wait has no limit (an exception to ADR-0009 rule 19): Vulkan has no bounded wait for an idle
    // device, and the driver reports a hung GPU as lost, which ends the wait.
    m_vk.vkDeviceWaitIdle(m_device);
    return error;
}

void VulkanBackend::setName(VkObjectType type, std::uint64_t handle, std::string_view name) const {
    if (!m_debugUtils || m_vk.vkSetDebugUtilsObjectNameEXT == nullptr || handle == 0) {
        return;
    }
    const std::string text(name);
    const VkDebugUtilsObjectNameInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                                             .objectType = type,
                                             .objectHandle = handle,
                                             .pObjectName = text.c_str()};
    m_vk.vkSetDebugUtilsObjectNameEXT(m_device, &info);
}

// --- Descriptors --------------------------------------------------------------------------

void VulkanBackend::writeDescriptor(const VkWriteDescriptorSet& write) {
    const MutexLock lock(m_descriptorMutex);
    m_vk.vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

Expected<VkImageView, Error> VulkanBackend::createView(VkImage image, VkImageViewType type, VkFormat format,
                                                       VkImageAspectFlags aspects, std::uint32_t mipCount,
                                                       std::uint32_t layerCount, std::string_view name) {
    const VkImageViewCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = image,
                                     .viewType = type,
                                     .format = format,
                                     .subresourceRange = {.aspectMask = aspects,
                                                          .baseMipLevel = 0,
                                                          .levelCount = mipCount,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = layerCount}};
    VkImageView view = VK_NULL_HANDLE;
    if (const VkResult result = m_vk.vkCreateImageView(m_device, &info, nullptr, &view); result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "creating a view of " + quoted(name)));
    }
    setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(view), name); // NOLINT(*-reinterpret-cast)
    return view;
}

// --- Buffers ------------------------------------------------------------------------------

Expected<std::byte*, Error> VulkanBackend::createBuffer(std::uint32_t slot, const BufferDesc& desc,
                                                        const detail::BufferDescriptors& descriptors) {
    // Shaders see buffers as byte-address buffers of 4-byte words, as on Direct3D 12, so the size
    // is rounded up to whole words.
    // Rounded up to whole words, unless that would pass the largest buffer size the device
    // allows (desc.size is within it, so the sum can't overflow).
    const std::uint64_t rounded = desc.size + (4 - desc.size % 4) % 4;
    const std::uint64_t size = rounded <= m_capabilities.limits.maxBufferSize ? rounded : desc.size;
    VkBufferUsageFlags usage = 0;
    if (hasAny(desc.usage, BufferUsage::ShaderRead | BufferUsage::ShaderWrite)) {
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (hasAny(desc.usage, BufferUsage::Index)) {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (hasAny(desc.usage, BufferUsage::Indirect)) {
        usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (hasAny(desc.usage, BufferUsage::CopySrc)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasAny(desc.usage, BufferUsage::CopyDst)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = size,
                                        .usage = usage,
                                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    // Upload and Readback memory is mapped for the buffer's lifetime and host-coherent (Device.h).
    VmaAllocationCreateInfo allocationInfo{};
    switch (desc.memory) {
        case MemoryType::GpuOnly:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            break;
        case MemoryType::Upload:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocationInfo.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryType::Readback:
            allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            allocationInfo.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }

    BufferRecord record;
    VmaAllocationInfo allocated{};
    if (const VkResult result =
            vmaCreateBuffer(m_allocator, &bufferInfo, &allocationInfo, &record.buffer, &record.allocation, &allocated);
        result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "creating buffer " + quoted(desc.debugName)));
    }
    record.size = desc.size;
    const std::string name(desc.debugName);
    setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(record.buffer), name); // NOLINT(*-reinterpret-cast)
    vmaSetAllocationName(m_allocator, record.allocation, name.c_str());

    // Storage buffers are limited to maxStorageBufferRange bytes in shaders.
    const VkDescriptorBufferInfo view{
        .buffer = record.buffer, .offset = 0, .range = std::min<std::uint64_t>(size, m_maxStorageBufferRange)};
    for (const std::uint32_t index : {descriptors.read, descriptors.write}) {
        if (index != kInvalidDescriptorIndex) {
            writeDescriptor(VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = m_descriptorSet,
                                                 .dstBinding = kBufferBinding,
                                                 .dstArrayElement = index,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                 .pBufferInfo = &view});
        }
    }
    m_buffers[slot] = record;
    return desc.memory == MemoryType::GpuOnly ? nullptr : static_cast<std::byte*>(allocated.pMappedData);
}

void VulkanBackend::destroyBuffer(std::uint32_t slot) {
    BufferRecord& record = m_buffers[slot];
    vmaDestroyBuffer(m_allocator, record.buffer, record.allocation);
    record = {};
}

// --- Textures -----------------------------------------------------------------------------

Expected<void, Error> VulkanBackend::createTexture(std::uint32_t slot, const TextureDesc& desc,
                                                   const detail::TextureDescriptors& descriptors) {
    // The device refuses sRGB storage textures (Device.cpp), as Direct3D 12 has none.
    ITH_ASSERT(!(hasAny(desc.usage, TextureUsage::Storage) && isSrgb(desc.format)));
    const bool volume = desc.dimension == TextureDimension::Texture3D;
    const bool cube = desc.dimension == TextureDimension::TextureCube;
    const VkFormat format = vulkanFormat(desc.format);
    VkImageUsageFlags usage = 0;
    if (hasAny(desc.usage, TextureUsage::Sampled)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (hasAny(desc.usage, TextureUsage::Storage)) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (hasAny(desc.usage, TextureUsage::RenderTarget)) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (hasAny(desc.usage, TextureUsage::DepthStencil)) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (hasAny(desc.usage, TextureUsage::CopySrc)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasAny(desc.usage, TextureUsage::CopyDst)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    VkImageCreateFlags flags = 0;
    if (cube) {
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (volume && hasAny(desc.usage, TextureUsage::RenderTarget)) {
        // Rendering into a 3D texture goes through a 2D view of its first slice.
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .flags = flags,
                                      .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                      .format = format,
                                      .extent = {.width = desc.width, .height = desc.height, .depth = desc.depth},
                                      .mipLevels = desc.mipLevels,
                                      .arrayLayers = volume ? 1 : desc.layers,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = usage,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    TextureRecord record;
    if (const VkResult result =
            vmaCreateImage(m_allocator, &imageInfo, &allocationInfo, &record.image, &record.allocation, nullptr);
        result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "creating texture " + quoted(desc.debugName)));
    }
    const std::string name(desc.debugName);
    setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(record.image), name); // NOLINT(*-reinterpret-cast)
    vmaSetAllocationName(m_allocator, record.allocation, name.c_str());
    record.aspects = aspectsOf(desc.format);
    record.texelSize = formatTexelSize(desc.format);
    record.width = desc.width;
    record.height = desc.height;
    record.depth = volume ? desc.depth : 1;
    record.layers = volume ? 1 : desc.layers;
    record.mipLevels = desc.mipLevels;
    record.volume = volume;

    // Views; on failure everything made so far is released.
    const auto release = [&] {
        for (VkImageView view : {record.sampledView, record.storageView, record.attachmentView}) {
            if (view != VK_NULL_HANDLE) {
                m_vk.vkDestroyImageView(m_device, view, nullptr);
            }
        }
        vmaDestroyImage(m_allocator, record.image, record.allocation);
    };
    if (descriptors.sampled != kInvalidDescriptorIndex) {
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (volume) {
            type = VK_IMAGE_VIEW_TYPE_3D;
        } else if (cube) {
            type = desc.layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        } else if (desc.layers == 1) {
            type = VK_IMAGE_VIEW_TYPE_2D;
        }
        // Shaders sample a depth texture's depth.
        const VkImageAspectFlags aspects =
            isDepthFormat(desc.format) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : record.aspects;
        Expected<VkImageView, Error> view =
            createView(record.image, type, format, aspects, record.mipLevels, record.layers, name);
        if (!view) {
            release();
            return makeUnexpected(std::move(view).error());
        }
        record.sampledView = *view;
        const VkDescriptorImageInfo image{.imageView = record.sampledView,
                                          .imageLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
        writeDescriptor(VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = m_descriptorSet,
                                             .dstBinding = kSampledImageBinding,
                                             .dstArrayElement = descriptors.sampled,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                             .pImageInfo = &image});
    }
    if (descriptors.storage != kInvalidDescriptorIndex) {
        // Mip 0, every layer (cubes as 2D arrays).
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (volume) {
            type = VK_IMAGE_VIEW_TYPE_3D;
        } else if (desc.layers == 1) {
            type = VK_IMAGE_VIEW_TYPE_2D;
        }
        Expected<VkImageView, Error> view =
            createView(record.image, type, format, record.aspects, 1, record.layers, name);
        if (!view) {
            release();
            return makeUnexpected(std::move(view).error());
        }
        record.storageView = *view;
        const VkDescriptorImageInfo image{.imageView = record.storageView, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        writeDescriptor(VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = m_descriptorSet,
                                             .dstBinding = kStorageImageBinding,
                                             .dstArrayElement = descriptors.storage,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &image});
    }
    if (hasAny(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil)) {
        // Mip 0, layer 0 (CommandList::beginRendering()).
        Expected<VkImageView, Error> view =
            createView(record.image, VK_IMAGE_VIEW_TYPE_2D, format, record.aspects, 1, 1, name);
        if (!view) {
            release();
            return makeUnexpected(std::move(view).error());
        }
        record.attachmentView = *view;
    }
    m_textures[slot] = record;
    return {};
}

void VulkanBackend::destroyTexture(std::uint32_t slot) {
    TextureRecord& record = m_textures[slot];
    for (VkImageView view : {record.sampledView, record.storageView, record.attachmentView}) {
        if (view != VK_NULL_HANDLE) {
            m_vk.vkDestroyImageView(m_device, view, nullptr);
        }
    }
    if (record.allocation != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, record.image, record.allocation);
    }
    record = {};
}

// --- Samplers -----------------------------------------------------------------------------

Expected<void, Error> VulkanBackend::createSampler(std::uint32_t slot, const SamplerDesc& desc,
                                                   std::uint32_t descriptor) {
    // Anisotropic filtering is linear in every direction, as on Direct3D 12.
    const bool anisotropic = desc.maxAnisotropy > 1.0f && m_maxAnisotropy > 1.0f;
    const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = anisotropic ? VK_FILTER_LINEAR : filter(desc.magFilter),
                                   .minFilter = anisotropic ? VK_FILTER_LINEAR : filter(desc.minFilter),
                                   .mipmapMode = anisotropic || desc.mipFilter == Filter::Linear
                                                     ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = addressMode(desc.addressU),
                                   .addressModeV = addressMode(desc.addressV),
                                   .addressModeW = addressMode(desc.addressW),
                                   .mipLodBias = desc.mipLodBias,
                                   .anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE,
                                   .maxAnisotropy = anisotropic ? std::min(desc.maxAnisotropy, m_maxAnisotropy) : 1.0f,
                                   .compareEnable = desc.compare ? VK_TRUE : VK_FALSE,
                                   .compareOp = desc.compare ? compareOp(desc.compareOp) : VK_COMPARE_OP_ALWAYS,
                                   .minLod = desc.minLod,
                                   .maxLod = desc.maxLod,
                                   // Direct3D 12's default border color.
                                   .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                   .unnormalizedCoordinates = VK_FALSE};
    VkSampler sampler = VK_NULL_HANDLE;
    if (const VkResult result = m_vk.vkCreateSampler(m_device, &info, nullptr, &sampler); result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "creating sampler " + quoted(desc.debugName)));
    }
    setName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<std::uint64_t>(sampler),
            desc.debugName); // NOLINT(*-reinterpret-cast)
    const VkDescriptorImageInfo image{.sampler = sampler};
    writeDescriptor(VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = m_descriptorSet,
                                         .dstBinding = kSamplerBinding,
                                         .dstArrayElement = descriptor,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                         .pImageInfo = &image});
    m_samplers[slot] = sampler;
    return {};
}

void VulkanBackend::destroySampler(std::uint32_t slot) {
    m_vk.vkDestroySampler(m_device, m_samplers[slot], nullptr);
    m_samplers[slot] = VK_NULL_HANDLE;
}

// --- Pipelines ----------------------------------------------------------------------------

Expected<VkShaderModule, Error> VulkanBackend::createShaderModule(const ShaderDesc& shader,
                                                                  std::string_view pipelineName) {
    const std::string which = "Shader " + quoted(shader.entryPoint) + " of pipeline " + quoted(pipelineName);
    if (!isSpirv(shader.code)) {
        return fail(ErrorCode::InvalidShader, which + " is not SPIR-V; Vulkan takes SPIR-V bytecode");
    }
    if (std::optional<std::string> problem =
            bindingProblem(shader.code, {.resources = m_capabilities.limits.resourceDescriptors,
                                         .samplers = m_capabilities.limits.samplerDescriptors})) {
        return fail(ErrorCode::InvalidShader, which + " is refused: " + *problem);
    }
    // SPIR-V is a sequence of 32-bit words; the bytes are copied so that they are aligned.
    std::vector<std::uint32_t> words(shader.code.size() / 4);
    std::memcpy(words.data(), shader.code.data(), shader.code.size());
    const VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shader.code.size(), .pCode = words.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult result = m_vk.vkCreateShaderModule(m_device, &info, nullptr, &module); result != VK_SUCCESS) {
        Error error = failure(result, "creating " + which);
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::InvalidShader;
        }
        return makeUnexpected(std::move(error));
    }
    return module;
}

Expected<void, Error> VulkanBackend::createGraphicsPipeline(std::uint32_t slot, const GraphicsPipelineDesc& desc) {
    std::vector<VkShaderModule> modules;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<std::string> entryPoints;
    // Reserved up front: the stages point at these strings, which must not move as more are added.
    entryPoints.reserve(desc.shaders.size());
    const auto destroyModules = [&] {
        for (VkShaderModule module : modules) {
            m_vk.vkDestroyShaderModule(m_device, module, nullptr);
        }
    };
    for (const ShaderDesc& shader : desc.shaders) {
        Expected<VkShaderModule, Error> module = createShaderModule(shader, desc.debugName);
        if (!module) {
            destroyModules();
            return makeUnexpected(std::move(module).error());
        }
        modules.push_back(*module);
        entryPoints.emplace_back(shader.entryPoint);
        stages.push_back(VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                         .stage = shaderStage(shader.stage),
                                                         .module = *module,
                                                         .pName = entryPoints.back().c_str()});
    }

    std::array<VkFormat, kMaxColorTargets> colorFormats{};
    std::array<VkPipelineColorBlendAttachmentState, kMaxColorTargets> blends{};
    const auto colorCount =
        static_cast<std::uint32_t>(std::min<std::size_t>(desc.colorTargets.size(), kMaxColorTargets));
    for (std::uint32_t target = 0; target < colorCount; ++target) {
        colorFormats[target] = vulkanFormat(desc.colorTargets[target].format);
        blends[target] = blendState(desc.colorTargets[target].blend);
    }
    const VkFormat depthFormat = vulkanFormat(desc.depthFormat);
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = colorCount,
        .pColorAttachmentFormats = colorFormats.data(),
        .depthAttachmentFormat = depthFormat,
        .stencilAttachmentFormat = desc.depthFormat == Format::D32FloatS8Uint ? depthFormat : VK_FORMAT_UNDEFINED};
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topology(desc.topology)};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        // The viewport's height is negative (ADR-0006), so the winding is the engine's.
        .frontFace =
            desc.frontFace == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f};
    if (desc.cullMode == CullMode::Front) {
        rasterization.cullMode = VK_CULL_MODE_FRONT_BIT;
    } else if (desc.cullMode == CullMode::Back) {
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    }
    const VkPipelineMultisampleStateCreateInfo multisample{.sType =
                                                               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    // Writing depth needs the depth test on; without a test it always passes.
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depth.test || desc.depth.write ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = desc.depth.test ? compareOp(desc.depth.compare) : VK_COMPARE_OP_ALWAYS,
        .maxDepthBounds = 1.0f};
    const VkPipelineColorBlendStateCreateInfo blend{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = colorCount,
                                                    .pAttachments = blends.data()};
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount =
                                                       static_cast<std::uint32_t>(dynamicStates.size()),
                                                   .pDynamicStates = dynamicStates.data()};
    const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .pNext = &rendering,
                                            .stageCount = static_cast<std::uint32_t>(stages.size()),
                                            .pStages = stages.data(),
                                            .pVertexInputState = &vertexInput,
                                            .pInputAssemblyState = &inputAssembly,
                                            .pViewportState = &viewport,
                                            .pRasterizationState = &rasterization,
                                            .pMultisampleState = &multisample,
                                            .pDepthStencilState = &depth,
                                            .pColorBlendState = &blend,
                                            .pDynamicState = &dynamic,
                                            .layout = m_pipelineLayout};
    PipelineRecord record{.bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    const VkResult result =
        m_vk.vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &record.pipeline);
    destroyModules();
    if (result != VK_SUCCESS) {
        Error error = failure(result, "creating pipeline " + quoted(desc.debugName));
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::InvalidShader;
        }
        return makeUnexpected(std::move(error));
    }
    setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(record.pipeline), // NOLINT(*-reinterpret-cast)
            desc.debugName);
    m_pipelines[slot] = record;
    return {};
}

Expected<void, Error> VulkanBackend::createComputePipeline(std::uint32_t slot, const ComputePipelineDesc& desc) {
    Expected<VkShaderModule, Error> module = createShaderModule(desc.shader, desc.debugName);
    if (!module) {
        return makeUnexpected(std::move(module).error());
    }
    const std::string entryPoint(desc.shader.entryPoint);
    const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                     .module = *module,
                                                     .pName = entryPoint.c_str()},
                                           .layout = m_pipelineLayout};
    PipelineRecord record{.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE};
    const VkResult result =
        m_vk.vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &record.pipeline);
    m_vk.vkDestroyShaderModule(m_device, *module, nullptr);
    if (result != VK_SUCCESS) {
        Error error = failure(result, "creating pipeline " + quoted(desc.debugName));
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::InvalidShader;
        }
        return makeUnexpected(std::move(error));
    }
    setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(record.pipeline), // NOLINT(*-reinterpret-cast)
            desc.debugName);
    m_pipelines[slot] = record;
    return {};
}

void VulkanBackend::destroyPipeline(std::uint32_t slot) {
    m_vk.vkDestroyPipeline(m_device, m_pipelines[slot].pipeline, nullptr);
    m_pipelines[slot] = {};
}

// --- Swapchains ---------------------------------------------------------------------------

void VulkanBackend::releaseImages(SwapchainRecord& swapchain) {
    for (std::uint32_t image = 0; image < swapchain.imageCount; ++image) {
        destroyTexture(swapchain.imageSlots[image]);
    }
    swapchain.imageCount = 0;
}

void VulkanBackend::releaseSwapchain(SwapchainRecord& swapchain) {
    if (swapchain.swapchain != VK_NULL_HANDLE) {
        // Presentation is done (buildSwapchain()); unbounded like vkDeviceWaitIdle (lostOnQueue()).
        m_vk.vkQueueWaitIdle(m_queue);
    }
    releaseImages(swapchain);
    if (swapchain.swapchain != VK_NULL_HANDLE) {
        m_vk.vkDestroySwapchainKHR(m_device, swapchain.swapchain, nullptr);
        swapchain.swapchain = VK_NULL_HANDLE;
    }
    for (VkSemaphore& semaphore : swapchain.acquired) {
        m_vk.vkDestroySemaphore(m_device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    for (VkSemaphore& semaphore : swapchain.rendered) {
        m_vk.vkDestroySemaphore(m_device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    if (swapchain.surface != VK_NULL_HANDLE) {
        m_vk.vkDestroySurfaceKHR(m_instance, swapchain.surface, nullptr);
        swapchain.surface = VK_NULL_HANDLE;
    }
}

Expected<detail::SwapchainImages, Error> VulkanBackend::buildSwapchain(SwapchainRecord& record, std::uint32_t width,
                                                                       std::uint32_t height) {
    // The GPU is idle (Backend.h), but presentation is queue work too: it must be done before the
    // swapchain and its semaphores go. vkQueueWaitIdle has no limit (see lostOnQueue()).
    if (record.swapchain != VK_NULL_HANDLE) {
        m_vk.vkQueueWaitIdle(m_queue);
    }
    releaseImages(record);
    VkSurfaceCapabilitiesKHR surface{};
    if (const VkResult result =
            m_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, record.surface, &surface);
        result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "reading the surface of swapchain " + quoted(record.name)));
    }
    // Where the window system fixes the size (Win32, X11), the images take the window's size.
    VkExtent2D extent = surface.currentExtent;
    if (extent.width == 0xFFFF'FFFFu) {
        extent.width = std::clamp(width, surface.minImageExtent.width, surface.maxImageExtent.width);
        extent.height = std::clamp(height, surface.minImageExtent.height, surface.maxImageExtent.height);
    }
    std::uint32_t imageCount = std::max(kSwapchainImageCount, surface.minImageCount);
    if (surface.maxImageCount != 0) {
        imageCount = std::min(imageCount, surface.maxImageCount);
    }
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    std::string problem;
    if (extent.width == 0 || extent.height == 0) {
        problem = "the window has no area";
    } else if (imageCount > kMaxSwapchainImages) {
        problem = "the surface needs " + std::to_string(imageCount) + " images";
    } else if ((surface.supportedUsageFlags & kUsage) != kUsage) {
        problem = "the surface's images can't be rendered and copied to";
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const VkCompositeAlphaFlagBitsKHR option :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if ((surface.supportedCompositeAlpha & option) != 0) {
            alpha = option;
            break;
        }
    }

    // The old swapchain is retired by the call even when it fails, and destroyed in any case.
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (problem.empty()) {
        const VkSwapchainCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                            .surface = record.surface,
                                            .minImageCount = imageCount,
                                            .imageFormat = record.surfaceFormat.format,
                                            .imageColorSpace = record.surfaceFormat.colorSpace,
                                            .imageExtent = extent,
                                            .imageArrayLayers = 1,
                                            .imageUsage = kUsage,
                                            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                            .preTransform = surface.currentTransform,
                                            .compositeAlpha = alpha,
                                            .presentMode = record.presentMode,
                                            .clipped = VK_TRUE,
                                            .oldSwapchain = record.swapchain};
        result = m_vk.vkCreateSwapchainKHR(m_device, &info, nullptr, &created);
    }
    if (record.swapchain != VK_NULL_HANDLE) {
        m_vk.vkDestroySwapchainKHR(m_device, record.swapchain, nullptr);
    }
    record.swapchain = result == VK_SUCCESS ? created : VK_NULL_HANDLE;
    // Fresh semaphores for every swapchain: none can be left signaled or waited for by the old one.
    for (VkSemaphore& semaphore : record.acquired) {
        m_vk.vkDestroySemaphore(m_device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    for (VkSemaphore& semaphore : record.rendered) {
        m_vk.vkDestroySemaphore(m_device, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
    }
    if (!problem.empty()) {
        return fail(ErrorCode::SurfaceFailed, "Swapchain " + quoted(record.name) + " can't be created: " + problem);
    }
    if (result != VK_SUCCESS) {
        Error error = failure(result, "creating swapchain " + quoted(record.name));
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::SurfaceFailed;
        }
        return makeUnexpected(std::move(error));
    }
    setName(VK_OBJECT_TYPE_SWAPCHAIN_KHR,
            reinterpret_cast<std::uint64_t>(record.swapchain), // NOLINT(*-reinterpret-cast)
            record.name);

    std::uint32_t count = 0;
    m_vk.vkGetSwapchainImagesKHR(m_device, record.swapchain, &count, nullptr);
    if (count > kMaxSwapchainImages) {
        return fail(ErrorCode::SurfaceFailed, "Swapchain " + quoted(record.name) + " has " + std::to_string(count) +
                                                  " images, more than " + std::to_string(kMaxSwapchainImages));
    }
    std::array<VkImage, kMaxSwapchainImages> images{};
    m_vk.vkGetSwapchainImagesKHR(m_device, record.swapchain, &count, images.data());

    const VkSemaphoreCreateInfo binary{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const std::span<VkSemaphore> rendered(record.rendered.data(), count);
    for (std::span<VkSemaphore> semaphores : {std::span<VkSemaphore>(record.acquired), rendered}) {
        for (VkSemaphore& semaphore : semaphores) {
            if (const VkResult created2 = m_vk.vkCreateSemaphore(m_device, &binary, nullptr, &semaphore);
                created2 != VK_SUCCESS) {
                semaphore = VK_NULL_HANDLE;
                return makeUnexpected(failure(created2, "creating the semaphores of swapchain " + quoted(record.name)));
            }
        }
    }
    for (std::uint32_t image = 0; image < count; ++image) {
        const std::string name = record.name + " image " + std::to_string(image);
        setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(images[image]),
                name); // NOLINT(*-reinterpret-cast)
        Expected<VkImageView, Error> view = createView(
            images[image], VK_IMAGE_VIEW_TYPE_2D, record.surfaceFormat.format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, name);
        if (!view) {
            return makeUnexpected(std::move(view).error());
        }
        m_textures[record.imageSlots[image]] = TextureRecord{.image = images[image],
                                                             .attachmentView = *view,
                                                             .texelSize = formatTexelSize(record.format),
                                                             .width = extent.width,
                                                             .height = extent.height};
        record.imageCount = image + 1;
    }
    record.suboptimal = false;
    record.presentFailed = false;
    return detail::SwapchainImages{
        .count = count, .format = record.format, .width = extent.width, .height = extent.height};
}

Expected<detail::SwapchainImages, Error> VulkanBackend::createSwapchain(std::uint32_t slot, const SwapchainDesc& desc,
                                                                        std::span<const std::uint32_t> imageSlots) {
    const std::string name(desc.debugName);
    if (!m_swapchainExtension) {
        return fail(ErrorCode::SurfaceFailed,
                    "Swapchain " + quoted(name) + " needs presentation, which this Vulkan device doesn't offer");
    }
    SwapchainRecord record;
    record.name = name;
    record.format = desc.format;
    ITH_ASSERT(imageSlots.size() >= kMaxSwapchainImages);
    std::copy_n(imageSlots.begin(), kMaxSwapchainImages, record.imageSlots.begin());

    // A surface from the window's native handles, through its window system's extension.
    VkResult result = VK_ERROR_EXTENSION_NOT_PRESENT;
#ifdef _WIN32
    if (const auto* window = std::get_if<Win32WindowHandle>(&desc.window);
        window != nullptr && window->hwnd != nullptr && m_vk.vkCreateWin32SurfaceKHR != nullptr) {
        const VkWin32SurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                               .hinstance = static_cast<HINSTANCE>(window->hinstance),
                                               .hwnd = static_cast<HWND>(window->hwnd)};
        result = m_vk.vkCreateWin32SurfaceKHR(m_instance, &info, nullptr, &record.surface);
    }
#else
    if (const auto* window = std::get_if<X11WindowHandle>(&desc.window);
        window != nullptr && window->display != nullptr && m_vk.vkCreateXlibSurfaceKHR != nullptr) {
        const VkXlibSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                                              .dpy = static_cast<::Display*>(window->display),
                                              .window = static_cast<::Window>(window->window)};
        result = m_vk.vkCreateXlibSurfaceKHR(m_instance, &info, nullptr, &record.surface);
    } else if (const auto* wayland = std::get_if<WaylandWindowHandle>(&desc.window);
               wayland != nullptr && wayland->surface != nullptr && m_vk.vkCreateWaylandSurfaceKHR != nullptr) {
        const VkWaylandSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                                                 .display = static_cast<wl_display*>(wayland->display),
                                                 .surface = static_cast<wl_surface*>(wayland->surface)};
        result = m_vk.vkCreateWaylandSurfaceKHR(m_instance, &info, nullptr, &record.surface);
    }
#endif
    if (result != VK_SUCCESS) {
        record.surface = VK_NULL_HANDLE;
        Error error = failure(result, "creating the surface of swapchain " + quoted(name) +
                                          " (it needs a window whose window system Vulkan presents to)");
        if (error.code == ErrorCode::BackendFailure) {
            error.code = ErrorCode::SurfaceFailed;
        }
        return makeUnexpected(std::move(error));
    }

    // The queue must present to the surface, in the requested format.
    std::string problem;
    VkBool32 presents = VK_FALSE;
    m_vk.vkGetPhysicalDeviceSurfaceSupportKHR(m_physicalDevice, m_queueFamily, record.surface, &presents);
    std::uint32_t count = 0;
    m_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, record.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    m_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, record.surface, &count, formats.data());
    formats.resize(count);
    const VkFormat wanted = vulkanFormat(desc.format);
    const auto format = std::ranges::find_if(formats, [&](const VkSurfaceFormatKHR& candidate) {
        return candidate.format == wanted && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (presents == VK_FALSE) {
        problem = "the graphics queue can't present to the window";
    } else if (format == formats.end()) {
        problem = "the window doesn't take the swapchain's format";
    } else {
        record.surfaceFormat = *format;
    }
    // Without vertical sync: tearing where the window system allows it, as on Direct3D 12.
    m_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, record.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    m_vk.vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, record.surface, &count, modes.data());
    modes.resize(count);
    record.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!desc.vsync) {
        for (const VkPresentModeKHR mode : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
            if (std::ranges::find(modes, mode) != modes.end()) {
                record.presentMode = mode;
                break;
            }
        }
    }
    if (!problem.empty()) {
        releaseSwapchain(record);
        return fail(ErrorCode::SurfaceFailed, "Swapchain " + quoted(name) + " can't be created: " + problem);
    }

    SwapchainRecord& stored = m_swapchains[slot];
    stored = std::move(record);
    Expected<detail::SwapchainImages, Error> images = buildSwapchain(stored, desc.width, desc.height);
    if (!images) {
        releaseSwapchain(stored);
        stored = {};
    }
    return images;
}

Expected<detail::SwapchainImages, Error> VulkanBackend::resizeSwapchain(std::uint32_t slot, std::uint32_t width,
                                                                        std::uint32_t height) {
    return buildSwapchain(m_swapchains[slot], width, height);
}

void VulkanBackend::destroySwapchain(std::uint32_t slot) {
    releaseSwapchain(m_swapchains[slot]);
    m_swapchains[slot] = {};
}

Expected<detail::AcquireResult, Error> VulkanBackend::acquireImage(std::uint32_t swapchainSlot,
                                                                   std::uint32_t frameSlot) {
    if (m_lost.load(std::memory_order_acquire)) {
        return makeUnexpected(lostError());
    }
    SwapchainRecord& swapchain = m_swapchains[swapchainSlot];
    // After a failed recreation or presentation the swapchain is rebuilt, with fresh semaphores.
    if (swapchain.swapchain == VK_NULL_HANDLE || swapchain.presentFailed) {
        return detail::AcquireResult{.status = detail::AcquireStatus::OutOfDate};
    }
    VkSemaphore acquired = swapchain.acquired[frameSlot];
    std::uint32_t image = 0;
    VkResult result = VK_TIMEOUT;
    // In steps, looking for device loss between them, and at most kMaxAcquireSteps of them: no
    // wait is without a limit (ADR-0009 rule 19).
    for (std::uint32_t step = 0; result == VK_TIMEOUT && step < kMaxAcquireSteps; ++step) {
        if (m_lost.load(std::memory_order_acquire)) {
            return makeUnexpected(lostError());
        }
        result =
            m_vk.vkAcquireNextImageKHR(m_device, swapchain.swapchain, kWaitStepNs, acquired, VK_NULL_HANDLE, &image);
    }
    if (result == VK_TIMEOUT) {
        return fail(ErrorCode::BackendFailure, "No image of swapchain " + quoted(swapchain.name) + " became free in " +
                                                   std::to_string(kMaxAcquireSteps) + " seconds");
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return detail::AcquireResult{.status = detail::AcquireStatus::OutOfDate};
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        return makeUnexpected(lostOnQueue());
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Error error = failure(result, "acquiring an image of swapchain " + quoted(swapchain.name));
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            error.code = ErrorCode::SurfaceFailed;
        }
        return makeUnexpected(std::move(error));
    }
    swapchain.suboptimal = result == VK_SUBOPTIMAL_KHR;
    swapchain.currentImage = image;

    // The image is ready once the acquisition semaphore is signaled. A batch waits for it and
    // signals the acquisition timeline, which every later batch of the frame waits for: a wait on
    // a binary semaphore would hold back only its own batch.
    ++m_acquireValue;
    const VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .semaphore = acquired,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = m_acquireTimeline,
                                       .value = m_acquireValue,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 batch{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = 1,
                              .pWaitSemaphoreInfos = &wait,
                              .signalSemaphoreInfoCount = 1,
                              .pSignalSemaphoreInfos = &signal};
    if (const VkResult submitted = submitBatch(batch); submitted != VK_SUCCESS) {
        // The frame's batches would wait for the acquisition forever: the device can't be used.
        Error error = lostOnQueue();
        error.message += "; submitting after acquiring a swapchain image failed (" + describe(submitted) + ")";
        return makeUnexpected(std::move(error));
    }
    m_acquiredThisFrame = true;
    return detail::AcquireResult{.status = detail::AcquireStatus::Acquired, .imageIndex = image};
}

// --- Command lists and submission ---------------------------------------------------------

Expected<std::unique_ptr<detail::CommandListBackend>, Error> VulkanBackend::createCommandList() {
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                           .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                           .queueFamilyIndex = m_queueFamily};
    VkCommandPool pool = VK_NULL_HANDLE;
    if (const VkResult result = m_vk.vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool); result != VK_SUCCESS) {
        return makeUnexpected(failure(result, "creating a command pool"));
    }
    const VkCommandBufferAllocateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                 .commandPool = pool,
                                                 .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                 .commandBufferCount = 1};
    VkCommandBuffer commands = VK_NULL_HANDLE;
    if (const VkResult result = m_vk.vkAllocateCommandBuffers(m_device, &bufferInfo, &commands); result != VK_SUCCESS) {
        m_vk.vkDestroyCommandPool(m_device, pool, nullptr);
        return makeUnexpected(failure(result, "creating a command buffer"));
    }
    return std::unique_ptr<detail::CommandListBackend>(std::make_unique<VulkanCommandList>(*this, pool, commands));
}

void VulkanBackend::beginFrame(std::uint32_t /*frameSlot*/) {
    // Each command list resets its own pool when it is begun (VulkanCommandList).
    m_acquiredThisFrame = false;
}

VkResult VulkanBackend::submitBatch(const VkSubmitInfo2& batch) {
    if (const VkResult failure = m_failNextSubmit.exchange(VK_SUCCESS, std::memory_order_acq_rel);
        failure != VK_SUCCESS) {
        if (failure == VK_ERROR_DEVICE_LOST) {
            m_simulatedLoss.store(true, std::memory_order_release);
        }
        return failure;
    }
    return m_vk.vkQueueSubmit2(m_queue, 1, &batch, VK_NULL_HANDLE);
}

std::optional<VkSemaphoreSubmitInfo> VulkanBackend::acquireWait() const {
    if (!m_acquiredThisFrame) {
        return std::nullopt;
    }
    return VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = m_acquireTimeline,
                                 .value = m_acquireValue,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
}

Expected<void, Error> VulkanBackend::submit(std::span<detail::CommandListBackend* const> lists,
                                            std::uint64_t signalValue) {
    if (m_lost.load(std::memory_order_acquire)) {
        return makeUnexpected(lostError());
    }
    m_submitLists.clear();
    VkResult closed = VK_SUCCESS;
    for (detail::CommandListBackend* list : lists) {
        auto* vulkanList = static_cast<VulkanCommandList*>(list);
        if (const VkResult result = vulkanList->close(); result != VK_SUCCESS && closed == VK_SUCCESS) {
            closed = result;
        }
        m_submitLists.push_back(VkCommandBufferSubmitInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                          .commandBuffer = vulkanList->native()});
    }
    if (closed != VK_SUCCESS) {
        return makeUnexpected(failure(closed, "recording a command list"));
    }
    const std::optional<VkSemaphoreSubmitInfo> wait = acquireWait();
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = m_timeline,
                                       .value = signalValue,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkSubmitInfo2 batch{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = wait ? 1u : 0u,
                              .pWaitSemaphoreInfos = wait ? &*wait : nullptr,
                              .commandBufferInfoCount = static_cast<std::uint32_t>(m_submitLists.size()),
                              .pCommandBufferInfos = m_submitLists.data(),
                              .signalSemaphoreInfoCount = 1,
                              .pSignalSemaphoreInfos = &signal};
    // A failed submission submits nothing and signals nothing (Backend.h).
    if (const VkResult result = submitBatch(batch); result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            return makeUnexpected(lostOnQueue());
        }
        return makeUnexpected(failure(result, "submitting command lists"));
    }
    return {};
}

Expected<detail::PresentStatus, Error> VulkanBackend::endFrame([[maybe_unused]] std::uint32_t frameSlot,
                                                               std::optional<std::uint32_t> swapchainSlot,
                                                               std::uint64_t signalValue) {
    if (m_lost.load(std::memory_order_acquire)) {
        return makeUnexpected(lostError());
    }
    // Once per frame, in profiling builds: the zones of earlier frames the GPU has finished, and a
    // command buffer that resets their queries after them, which this batch submits.
    VkCommandBufferSubmitInfo profilerCommands{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
#ifdef ITH_ENABLE_PROFILING
    profilerCommands.commandBuffer = m_gpuProfiler.collect(frameSlot);
#endif
    const bool collected = profilerCommands.commandBuffer != VK_NULL_HANDLE;
    SwapchainRecord* swapchain = swapchainSlot ? &m_swapchains[*swapchainSlot] : nullptr;
    // Signals the timeline and, when presenting, the image's rendering semaphore after all the
    // frame's work. Semaphore signals of a submission cover everything submitted before it.
    std::array<VkSemaphoreSubmitInfo, 2> signals{};
    std::uint32_t signalCount = 0;
    signals[signalCount++] = VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                   .semaphore = m_timeline,
                                                   .value = signalValue,
                                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (swapchain != nullptr) {
        signals[signalCount++] = VkSemaphoreSubmitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                                       .semaphore = swapchain->rendered[swapchain->currentImage],
                                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    }
    const std::optional<VkSemaphoreSubmitInfo> wait = acquireWait();
    const VkSubmitInfo2 batch{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = wait ? 1u : 0u,
                              .pWaitSemaphoreInfos = wait ? &*wait : nullptr,
                              .commandBufferInfoCount = collected ? 1u : 0u,
                              .pCommandBufferInfos = collected ? &profilerCommands : nullptr,
                              .signalSemaphoreInfoCount = signalCount,
                              .pSignalSemaphoreInfos = signals.data()};
    m_acquiredThisFrame = false;
    if (const VkResult result = submitBatch(batch); result != VK_SUCCESS) {
#ifdef ITH_ENABLE_PROFILING
        if (collected) {
            m_gpuProfiler.disable("submitting the profiler's collection failed");
        }
#endif
        // The frame's value would never be signaled, and waits for it would never end: the
        // device can't be used any more (Backend.h).
        Error error = lostOnQueue();
        if (result != VK_ERROR_DEVICE_LOST) {
            error.message += "; signaling the graphics timeline failed (" + describe(result) + ")";
        }
        return makeUnexpected(std::move(error));
    }
    if (swapchain == nullptr) {
        return detail::PresentStatus::Presented;
    }
    const VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &swapchain->rendered[swapchain->currentImage],
                                   .swapchainCount = 1,
                                   .pSwapchains = &swapchain->swapchain,
                                   .pImageIndices = &swapchain->currentImage};
    const VkResult presented = m_vk.vkQueuePresentKHR(m_queue, &present);
    if (presented == VK_ERROR_DEVICE_LOST) {
        return makeUnexpected(lostOnQueue());
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR || swapchain->suboptimal) {
        return detail::PresentStatus::OutOfDate;
    }
    if (presented != VK_SUCCESS) {
        swapchain->presentFailed = true;
        Error error = failure(presented, "presenting swapchain " + quoted(swapchain->name));
        if (presented == VK_ERROR_SURFACE_LOST_KHR) {
            error.code = ErrorCode::SurfaceFailed;
        }
        return makeUnexpected(std::move(error));
    }
    return detail::PresentStatus::Presented;
}

std::uint64_t VulkanBackend::completedValue() {
    // Everything counts as finished once the device is lost.
    if (m_lost.load(std::memory_order_acquire)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    std::uint64_t value = 0;
    if (const VkResult result = m_vk.vkGetSemaphoreCounterValue(m_device, m_timeline, &value); result != VK_SUCCESS) {
        static_cast<void>(lostOnQueue());
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value;
}

Expected<void, Error> VulkanBackend::waitForValue(std::uint64_t value) {
    const VkSemaphoreWaitInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &m_timeline,
                                   .pValues = &value};
    // In steps, so that no wait is without a limit while the device can still be lost.
    VkResult result = VK_TIMEOUT;
    while (result == VK_TIMEOUT && !m_lost.load(std::memory_order_acquire)) {
        result = m_vk.vkWaitSemaphores(m_device, &info, kWaitStepNs);
    }
    if (m_lost.load(std::memory_order_acquire)) {
        return makeUnexpected(lostError());
    }
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            return makeUnexpected(lostOnQueue());
        }
        return makeUnexpected(failure(result, "waiting for the GPU"));
    }
    return {};
}

VulkanGpuTiming VulkanBackend::gpuTiming() const {
#ifdef ITH_ENABLE_PROFILING
    // Off when the profiler couldn't start.
    return m_gpuProfiler.timing();
#else
    return m_gpuTiming;
#endif
}

} // namespace ith::rhi::vulkan

// --- Factory ------------------------------------------------------------------------------

namespace ith::rhi {

Expected<Device, Error> createVulkanDevice(const DeviceDesc& desc) {
    Expected<std::unique_ptr<vulkan::VulkanBackend>, Error> backend = vulkan::VulkanBackend::create(desc);
    if (!backend) {
        return makeUnexpected(std::move(backend).error());
    }
    return detail::makeDevice(std::move(*backend));
}

void failNextVulkanSubmitForTesting(Device& device, bool outOfMemory) {
    detail::DeviceBackend* backend = detail::backendOf(device);
    if (backend == nullptr || backend->capabilities().backend != Backend::Vulkan) {
        ITH_ASSERT_MSG(false, "failNextVulkanSubmitForTesting() needs a live Vulkan device");
        return;
    }
    static_cast<vulkan::VulkanBackend*>(backend)->failNextSubmit(outOfMemory ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                                                             : VK_ERROR_DEVICE_LOST);
}

VulkanGpuTiming vulkanGpuTimingForTesting(Device& device) {
    detail::DeviceBackend* backend = detail::backendOf(device);
    if (backend == nullptr || backend->capabilities().backend != Backend::Vulkan) {
        ITH_ASSERT_MSG(false, "vulkanGpuTimingForTesting() needs a live Vulkan device");
        return VulkanGpuTiming::Off;
    }
    return static_cast<vulkan::VulkanBackend*>(backend)->gpuTiming();
}

void avoidCalibratedVulkanGpuTimingForTesting(bool avoid) {
    vulkan::g_avoidCalibratedGpuTiming.store(avoid, std::memory_order_release);
}

bool vulkanGpuZonesDisabledForTesting(Device& device) {
    const detail::DeviceBackend* backend = detail::backendOf(device);
    if (backend == nullptr || backend->capabilities().backend != Backend::Vulkan) {
        ITH_ASSERT_MSG(false, "vulkanGpuZonesDisabledForTesting() needs a live Vulkan device");
        return false;
    }
#ifdef ITH_ENABLE_PROFILING
    return static_cast<const vulkan::VulkanBackend*>(backend)->gpuProfiler().disabled();
#else
    return false;
#endif
}

} // namespace ith::rhi
