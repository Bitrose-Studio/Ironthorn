#include "ith/rhi_vulkan/VulkanProfiler.h"

#ifdef ITH_ENABLE_PROFILING

#include "ith/foundation/Assert.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_vulkan/VulkanApi.h"
#include "ith/rhi_vulkan/VulkanDevice.h"
#include "ith/rhi_vulkan/VulkanGpuTiming.h"
#include "ith/rhi_vulkan/VulkanLoader.h"

// Tracy's Vulkan helpers, after the Vulkan headers they need. The module is compiled with
// TRACY_VK_USE_SYMBOL_TABLE (CMakeLists.txt): with VK_NO_PROTOTYPES there are no Vulkan symbols to
// link, so Tracy fetches the functions it calls through vkGetInstanceProcAddr and
// vkGetDeviceProcAddr.
#include <tracy/TracyVulkan.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ith::rhi::vulkan {
namespace {

// Tracy numbers GPU contexts with 8 bits for the whole process (NextGpuContextId) and never reuses
// a number. Each backend takes at most 128, so that the two together stay within Tracy's 256; a
// process that creates more devices (a long test run) gets no GPU zones on the later ones.
constexpr std::uint32_t kMaxContexts = 128;
std::atomic<std::uint32_t> g_contexts{0};

constexpr std::string_view kContextName = "Vulkan graphics queue";
// The size of Tracy's query pool (VkCtx::QueryCount in 0.15.0). Tracy halves it while creating the
// pool fails, forever if it always fails, so start() creates one of this size first.
constexpr std::uint32_t kQueryCount = 64 * 1024;
// Tracy copies a zone's name into a 16-bit-sized source location; longer names are cut.
constexpr std::size_t kMaxNameSize = 1024;
// How long start() waits for its test submission before it leaves GPU zones off.
constexpr std::uint64_t kSubmitTimeoutNs = 5'000'000'000;

// Platform code: the host clock Tracy reads its CPU timeline from, under Tracy's own conditions
// (TracyVulkan.hpp, FindAvailableTimeDomains) (ADR-0012).
#ifdef _WIN32
constexpr std::optional<VkTimeDomainEXT> kHostTimeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT;
#elif defined(__linux__) && defined(CLOCK_MONOTONIC_RAW)
constexpr std::optional<VkTimeDomainEXT> kHostTimeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
#else
constexpr std::optional<VkTimeDomainEXT> kHostTimeDomain;
#endif
// Tracy looks only at the first four time domains a device reports.
constexpr std::size_t kTimeDomainsTracyReads = 4;

struct TracyContextDeleter {
    void operator()(tracy::VkCtx* context) const { TracyVkDestroy(context); }
};

// Whether the extension's calibration works on this device: Tracy's start-up calibration retries
// until a reading succeeds.
bool calibrationWorks(const GpuProfilerSetup& setup) {
    if (setup.vk->vkGetCalibratedTimestampsEXT == nullptr || !kHostTimeDomain) {
        return false;
    }
    const std::array<VkCalibratedTimestampInfoEXT, 2> domains{{
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = *kHostTimeDomain},
    }};
    std::array<std::uint64_t, 2> timestamps{};
    std::uint64_t deviation = 0;
    return setup.vk->vkGetCalibratedTimestampsEXT(setup.device, static_cast<std::uint32_t>(domains.size()),
                                                  domains.data(), timestamps.data(), &deviation) == VK_SUCCESS;
}

// Whether a pool of Tracy's size can be created.
bool queryPoolWorks(const GpuProfilerSetup& setup) {
    const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = kQueryCount};
    VkQueryPool pool = VK_NULL_HANDLE;
    if (setup.vk->vkCreateQueryPool(setup.device, &info, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    setup.vk->vkDestroyQueryPool(setup.device, pool, nullptr);
    return true;
}

// Whether `commands`, empty, runs on the queue within the time limit: Tracy's uncalibrated set-up
// ignores the results of its submissions and then waits for its queries without a limit.
bool submissionWorks(const GpuProfilerSetup& setup, VkCommandBuffer commands) {
    const VulkanFunctions& vk = *setup.vk;
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS || vk.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        return false;
    }
    const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.vkCreateFence(setup.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        return false;
    }
    const VkCommandBufferSubmitInfo buffer{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                           .commandBuffer = commands};
    const VkSubmitInfo2 batch{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &buffer};
    const bool ran = vk.vkQueueSubmit2(setup.queue, 1, &batch, fence) == VK_SUCCESS &&
                     vk.vkWaitForFences(setup.device, 1, &fence, VK_TRUE, kSubmitTimeoutNs) == VK_SUCCESS;
    if (!ran) {
        // The submission may still be pending, and the fence can't be destroyed until it is done:
        // this wait has no limit, by necessity. GPU zones stay off for the device.
        vk.vkQueueWaitIdle(setup.queue);
    }
    vk.vkDestroyFence(setup.device, fence, nullptr);
    return ran;
}

// The device's vkGetDeviceProcAddr, for deviceProcAddrWithoutCalibration(), which Tracy calls only
// on the thread that starts the profiler, while it does.
thread_local PFN_vkGetDeviceProcAddr g_deviceProcAddr = nullptr;

// vkGetDeviceProcAddr for the uncalibrated context: it never hands Tracy a calibration function,
// so Tracy aligns the timelines once instead of calibrating, whatever a driver returns for an
// extension the device didn't enable.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProcAddrWithoutCalibration(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkGetCalibratedTimestampsEXT") == 0 ||
        std::strcmp(name, "vkGetCalibratedTimestampsKHR") == 0) {
        return nullptr;
    }
    return g_deviceProcAddr(device, name);
}

} // namespace

GpuTimingSupport readGpuTimingSupport(const VulkanFunctions& vk, VkPhysicalDevice physicalDevice,
                                      std::uint32_t queueFamily, bool hostQueryReset, bool calibratedTimestamps) {
    GpuTimingSupport support{.hostQueryReset = hostQueryReset, .calibratedTimestamps = calibratedTimestamps};
    std::uint32_t count = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, families.data());
    support.timestamps = queueFamily < count && families[queueFamily].timestampValidBits > 0;
    if (calibratedTimestamps && kHostTimeDomain && vk.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT != nullptr) {
        vk.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(physicalDevice, &count, nullptr);
        std::vector<VkTimeDomainEXT> domains(count);
        vk.vkGetPhysicalDeviceCalibrateableTimeDomainsEXT(physicalDevice, &count, domains.data());
        domains.resize(std::min<std::size_t>(count, kTimeDomainsTracyReads));
        support.hostTimeDomain = std::ranges::find(domains, *kHostTimeDomain) != domains.end();
    }
    return support;
}

// The Tracy context and a command buffer per frame slot for collection. Collection records its
// query resets into the slot's buffer: a reset from the host would race the GPU while the frame
// that wrote the queries is still running (VUID-vkResetQueryPool-firstQuery-02741), while
// vkCmdResetQueryPool is ordered after the earlier commands that use the queries.
struct GpuProfiler::Context {
    Context(const VulkanFunctions& functions, VkDevice owner) : vk(&functions), device(owner) {}

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ~Context() {
        tracy.reset();
        for (VkCommandPool pool : pools) {
            if (pool != VK_NULL_HANDLE) {
                vk->vkDestroyCommandPool(device, pool, nullptr);
            }
        }
    }

    // Creates the pools and their command buffers; false when that fails.
    bool createCommandBuffers(std::uint32_t queueFamily) {
        // Tracy's uncalibrated set-up begins one buffer several times, which RESET_COMMAND_BUFFER
        // allows; collection resets the whole pool.
        const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                               .queueFamilyIndex = queueFamily};
        for (std::uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (vk->vkCreateCommandPool(device, &poolInfo, nullptr, &pools[slot]) != VK_SUCCESS) {
                pools[slot] = VK_NULL_HANDLE;
                return false;
            }
            const VkCommandBufferAllocateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                         .commandPool = pools[slot],
                                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                         .commandBufferCount = 1};
            if (vk->vkAllocateCommandBuffers(device, &bufferInfo, &buffers[slot]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    const VulkanFunctions* vk;
    VkDevice device;
    std::array<VkCommandPool, kFramesInFlight> pools{};
    std::array<VkCommandBuffer, kFramesInFlight> buffers{};
    std::unique_ptr<tracy::VkCtx, TracyContextDeleter> tracy;
    VulkanGpuTiming timing = VulkanGpuTiming::Off;
    // Set for good once a query may never be written or reset (GpuProfiler::disable()); read by
    // every thread that opens a zone.
    std::atomic<bool> disabled{false};
};

GpuProfiler::GpuProfiler() = default;

GpuProfiler::~GpuProfiler() {
    stop();
}

void GpuProfiler::start(const GpuProfilerSetup& setup) {
    stop();
    if (setup.timing == VulkanGpuTiming::Off) {
        std::fprintf(stderr, "Vulkan: no GPU zones: the graphics queue writes no timestamps\n");
        return;
    }
    if (g_contexts.fetch_add(1, std::memory_order_relaxed) >= kMaxContexts) {
        std::fprintf(stderr,
                     "Vulkan: no GPU zones: this process has created %u Vulkan devices, the most the "
                     "profiler numbers\n",
                     kMaxContexts);
        return;
    }
    auto context = std::make_unique<Context>(*setup.vk, setup.device);
    if (!context->createCommandBuffers(setup.queueFamily) || !queryPoolWorks(setup)) {
        std::fprintf(stderr, "Vulkan: no GPU zones: creating the profiler's command buffers or queries failed\n");
        return;
    }
    if (setup.timing == VulkanGpuTiming::Calibrated) {
        if (!calibrationWorks(setup)) {
            std::fprintf(stderr, "Vulkan: no GPU zones: reading calibrated timestamps failed\n");
            return;
        }
        context->tracy.reset(TracyVkContextHostCalibrated(setup.instance, setup.physicalDevice, setup.device,
                                                          setup.vk->vkGetInstanceProcAddr,
                                                          setup.vk->vkGetDeviceProcAddr));
    } else {
        if (!submissionWorks(setup, context->buffers[0])) {
            std::fprintf(stderr, "Vulkan: no GPU zones: a test submission to the graphics queue failed\n");
            return;
        }
        std::fprintf(stderr, "Vulkan: GPU zones are aligned with the CPU timeline once, at start-up: the device "
                             "doesn't use VK_EXT_calibrated_timestamps with a host clock\n");
        // The calibrated constructor, given no calibration function (TracyVkContext would leave
        // that function pointer uninitialized in 0.15.0 with TRACY_VK_USE_SYMBOL_TABLE): it writes
        // a timestamp on the queue, waits for it and aligns the timelines once. The buffer is idle
        // again when it returns.
        g_deviceProcAddr = setup.vk->vkGetDeviceProcAddr;
        context->tracy.reset(TracyVkContextCalibrated(setup.instance, setup.physicalDevice, setup.device, setup.queue,
                                                      context->buffers[0], setup.vk->vkGetInstanceProcAddr,
                                                      deviceProcAddrWithoutCalibration));
    }
    if (context->tracy->GetId() == tracy::InvalidGpuContextId) {
        std::fprintf(stderr, "Vulkan: no GPU zones: the profiler has no context number left\n");
        return;
    }
    context->timing = setup.timing;
    TracyVkContextName(context->tracy.get(), kContextName.data(), static_cast<std::uint16_t>(kContextName.size()));
    m_context = std::move(context);
}

void GpuProfiler::stop() {
    m_context.reset();
}

bool GpuProfiler::disabled() const {
    return m_context != nullptr && m_context->disabled.load(std::memory_order_acquire);
}

void GpuProfiler::disable(std::string_view reason) {
    if (m_context != nullptr && !m_context->disabled.exchange(true, std::memory_order_acq_rel)) {
        std::fprintf(stderr, "Vulkan: GPU zones are off for this device from now on: %.*s\n",
                     static_cast<int>(reason.size()), reason.data());
    }
}

VkCommandBuffer GpuProfiler::collect(std::uint32_t frameSlot) {
    ITH_ASSERT(frameSlot < kFramesInFlight);
    if (m_context == nullptr || frameSlot >= kFramesInFlight) {
        return VK_NULL_HANDLE;
    }
    Context& context = *m_context;
    const VulkanFunctions& vk = *context.vk;
    const VkCommandBufferBeginInfo info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBuffer commands = context.buffers[frameSlot];
    if (vk.vkResetCommandPool(context.device, context.pools[frameSlot], 0) != VK_SUCCESS ||
        vk.vkBeginCommandBuffer(commands, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    TracyVkCollect(context.tracy.get(), commands);
    if (vk.vkEndCommandBuffer(commands) != VK_SUCCESS) {
        // Tracy counts the queries it read as reset, but the resets will never run.
        disable("recording the profiler's collection failed");
        return VK_NULL_HANDLE;
    }
    return commands;
}

VulkanGpuTiming GpuProfiler::timing() const {
    return m_context != nullptr ? m_context->timing : VulkanGpuTiming::Off;
}

// The engine's zone scope, in place of Tracy's VkCtxScope: it ends the zone with the thread that
// began it, whichever thread closes it, and tells whether the recording wrote queries. The begin
// and end items follow VkCtxScope's (the transient-name constructor and the destructor) in the
// pinned Tracy 0.15.0; check them when Tracy is upgraded.
struct GpuZones::Scopes {
    struct Zone {
        // Whether the zone writes timestamps: the profiler runs and isn't disabled and, when
        // Tracy collects on demand, a viewer was connected when it began.
        bool active = false;
        tracy::VkCtx* context = nullptr;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::uint32_t thread = 0;
        std::uint64_t connection = 0;
    };

    std::array<Zone, kMaxGpuZoneDepth> open{};
    std::uint32_t depth = 0;
    const VulkanFunctions* vk = nullptr;
    // Whether the current recording wrote queries.
    bool wrote = false;
};

GpuZones::GpuZones() : m_scopes(std::make_unique<Scopes>()) {}

GpuZones::~GpuZones() {
    ITH_ASSERT_MSG(m_scopes->depth == 0, "a command list was destroyed with GPU zones open");
}

void GpuZones::begin(GpuProfiler& profiler, VkCommandBuffer commands, std::string_view name) {
    Scopes& scopes = *m_scopes;
    if (scopes.depth >= kMaxGpuZoneDepth) {
        ITH_ASSERT_MSG(false, "GPU zones nest deeper than kMaxGpuZoneDepth"); // CommandList checks it
        return;
    }
    Scopes::Zone& zone = scopes.open[scopes.depth++];
    zone = {};
    if (profiler.m_context == nullptr || profiler.m_context->disabled.load(std::memory_order_acquire)) {
        return;
    }
#ifdef TRACY_ON_DEMAND
    zone.active = tracy::GetProfiler().IsConnected();
    zone.connection = tracy::GetProfiler().ConnectionId();
#else
    zone.active = true;
#endif
    if (!zone.active) {
        return;
    }
    tracy::VkCtx* context = profiler.m_context->tracy.get();
    zone.context = context;
    zone.commands = commands;
    zone.thread = tracy::GetThreadHandle();
    scopes.vk = profiler.m_context->vk;
    scopes.wrote = true;

    const std::uint32_t query = context->NextQueryId();
    scopes.vk->vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, context->GetQueryPool(), query);

    const std::size_t nameSize = std::min(name.size(), kMaxNameSize);
    const std::uint64_t location =
        tracy::Profiler::AllocSourceLocation(TracyLine, TracyFile, std::strlen(TracyFile), TracyFunction,
                                             std::strlen(TracyFunction), name.empty() ? "" : name.data(), nameSize);
    tracy::QueueItem* item = tracy::Profiler::QueueSerial();
    tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneBeginAllocSrcLocSerial);
    tracy::MemWrite(&item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime());
    tracy::MemWrite(&item->gpuZoneBegin.srcloc, location);
    tracy::MemWrite(&item->gpuZoneBegin.thread, zone.thread);
    tracy::MemWrite(&item->gpuZoneBegin.queryId, static_cast<std::uint16_t>(query));
    tracy::MemWrite(&item->gpuZoneBegin.context, static_cast<std::uint8_t>(context->GetId()));
    tracy::Profiler::QueueSerialFinish();
}

void GpuZones::end() {
    Scopes& scopes = *m_scopes;
    if (scopes.depth == 0) {
        ITH_ASSERT_MSG(false, "endGpuZone() without beginGpuZone()"); // CommandList checks it
        return;
    }
    const Scopes::Zone zone = scopes.open[--scopes.depth];
    if (!zone.active) {
        return;
    }
    // Written even when the profiler was disabled since the zone began, so that the zone's
    // queries come in pairs.
    const std::uint32_t query = zone.context->NextQueryId();
    scopes.vk->vkCmdWriteTimestamp(zone.commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, zone.context->GetQueryPool(),
                                   query);
#ifdef TRACY_ON_DEMAND
    // The viewer that saw the zone begin is gone: the new one never heard of it.
    if (tracy::GetProfiler().ConnectionId() != zone.connection) {
        return;
    }
#endif
    tracy::QueueItem* item = tracy::Profiler::QueueSerial();
    tracy::MemWrite(&item->hdr.type, tracy::QueueType::GpuZoneEndSerial);
    tracy::MemWrite(&item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime());
    tracy::MemWrite(&item->gpuZoneEnd.thread, zone.thread);
    tracy::MemWrite(&item->gpuZoneEnd.queryId, static_cast<std::uint16_t>(query));
    tracy::MemWrite(&item->gpuZoneEnd.context, static_cast<std::uint8_t>(zone.context->GetId()));
    tracy::Profiler::QueueSerialFinish();
}

void GpuZones::endAll() {
    while (m_scopes->depth > 0) {
        end();
    }
}

void GpuZones::restart() {
    m_scopes->wrote = false;
}

void GpuZones::abandoned(GpuProfiler& profiler) {
    if (m_scopes->wrote) {
        // Tracy collects queries in order and would wait for these for good, and the queries
        // after them would never be reset again: no zone may write a query from now on.
        profiler.disable("a command list that recorded GPU zones never ran (discarded, or its "
                         "submission failed or was skipped); their times are lost");
    }
}

} // namespace ith::rhi::vulkan

#endif
