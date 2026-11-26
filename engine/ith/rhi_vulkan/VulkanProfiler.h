#pragma once

// GPU zones of the Vulkan backend on Tracy's Vulkan context (ADR-0007 rule 12, ADR-0009 rule 15;
// docs/development/rhi.md, "GPU zones"). Internal to the rhi_vulkan module, and only in profiling
// builds (ITH_ENABLE_PROFILING). VulkanProfiler.cpp is the module's one source that includes Tracy
// (the tracy_confined test); no Tracy type appears here.
//
// One Tracy context times the graphics queue. Command lists open zones on it from any thread,
// through the engine's own zone scopes (GpuZones). The render thread collects the finished zones
// once per frame, in endFrame(), into a small command buffer that the frame's last batch submits:
// it resets the collected queries on the GPU after the commands that wrote them. Tracy collects
// queries in order, so a list with zones that never runs leaves queries it would wait for for
// good, and queries after them would never be reset; the profiler then turns GPU zones off for the
// device (disable()), and the zones still collected are those before it. The context owns a query
// pool of 65,536 timestamps (512 KiB of GPU memory), 1 MiB of host memory for the results, and a
// command pool per frame slot.

#ifdef ITH_ENABLE_PROFILING

#include "ith/rhi_vulkan/VulkanApi.h"
#include "ith/rhi_vulkan/VulkanDevice.h"
#include "ith/rhi_vulkan/VulkanGpuTiming.h"
#include "ith/rhi_vulkan/VulkanLoader.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace ith::rhi::vulkan {

// Reads what `physicalDevice` reports about timing GPU zones; `hostQueryReset` and
// `calibratedTimestamps` come from the caller's feature and extension queries.
[[nodiscard]] GpuTimingSupport readGpuTimingSupport(const VulkanFunctions& vk, VkPhysicalDevice physicalDevice,
                                                    std::uint32_t queueFamily, bool hostQueryReset,
                                                    bool calibratedTimestamps);

// What starting the profiler needs. The device was created with what `timing` requires: for
// VulkanGpuTiming::Calibrated, VK_EXT_calibrated_timestamps and the hostQueryReset feature.
struct GpuProfilerSetup {
    const VulkanFunctions* vk = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VulkanGpuTiming timing = VulkanGpuTiming::Off;
};

// The device's Tracy context for its graphics queue.
class GpuProfiler {
public:
    GpuProfiler();
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;
    GpuProfiler(GpuProfiler&&) = delete;
    GpuProfiler& operator=(GpuProfiler&&) = delete;
    // Stops the profiler if it still runs.
    ~GpuProfiler();

    // Creates the context, on the render thread while no other work is on the queue (the
    // uncalibrated path submits to it and waits). Leaves the profiler off, saying why on stderr,
    // when setup.timing is Off, when the process already has too many Tracy GPU contexts (Tracy
    // numbers them with 8 bits; each backend takes at most 128), or when a check that would
    // otherwise make Tracy wait forever fails: creating the query pool, a submission to the
    // queue, or reading calibrated timestamps.
    void start(const GpuProfilerSetup& setup);
    // Destroys the context: once the GPU is idle and before the device is destroyed. Zones of the
    // last frame or two, not collected yet, are lost.
    void stop();
    // Collects the zones the GPU has finished: the render thread, once per frame, in endFrame()
    // of `frameSlot`, whose earlier frame the GPU has finished. Returns the command buffer that
    // resets the collected queries, which the frame's last batch must submit; VK_NULL_HANDLE when
    // the profiler doesn't run or recording the buffer failed (which disables it).
    [[nodiscard]] VkCommandBuffer collect(std::uint32_t frameSlot);
    // Turns GPU zones off for the device for good, saying why on stderr the first time: a query
    // may never be written or reset. Zones opened later record nothing. Thread-safe.
    void disable(std::string_view reason);
    // Whether disable() was called (tests).
    [[nodiscard]] bool disabled() const;

    // How zones are timed; Off when the profiler doesn't run.
    [[nodiscard]] VulkanGpuTiming timing() const;

private:
    friend class GpuZones;
    struct Context;

    // Created by start() and destroyed by stop() on the render thread; command lists use it while
    // the device exists.
    std::unique_ptr<Context> m_context;
};

// The open GPU zones of one command list, innermost last, and whether its recording wrote queries.
// Used by the thread recording the list.
class GpuZones {
public:
    GpuZones();
    GpuZones(const GpuZones&) = delete;
    GpuZones& operator=(const GpuZones&) = delete;
    GpuZones(GpuZones&&) = delete;
    GpuZones& operator=(GpuZones&&) = delete;
    ~GpuZones();

    // Opens a zone in `commands`, which is recording. The zone records nothing when the profiler
    // doesn't run or is disabled, or when Tracy collects on demand and no viewer is connected.
    // Precondition: fewer than kMaxGpuZoneDepth are open.
    void begin(GpuProfiler& profiler, VkCommandBuffer commands, std::string_view name);
    // Closes the innermost zone, in the command buffer it was opened in, which is still recording.
    // The profiler sees it end on the thread that began it. Precondition: one is open.
    void end();
    // Closes every open zone, innermost first.
    void endAll();
    // A new recording begins, which has written no queries yet.
    void restart();
    // The current recording will never run: if it wrote queries, the profiler is disabled.
    void abandoned(GpuProfiler& profiler);

private:
    struct Scopes;

    std::unique_ptr<Scopes> m_scopes;
};

} // namespace ith::rhi::vulkan

#endif
