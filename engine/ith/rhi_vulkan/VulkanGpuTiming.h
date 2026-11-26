#pragma once

// How the Vulkan backend times GPU zones on a device (docs/development/rhi.md, "GPU zones").
// Internal to the rhi_vulkan module, and its tests: no Vulkan type appears here, so the choice is
// tested without a GPU.
//
// Tracy's Vulkan context has two ways to put GPU timestamps on the CPU timeline:
// - Calibrated (TracyVkContextHostCalibrated): reads matching GPU and host clocks through
//   VK_EXT_calibrated_timestamps whenever it collects, and resets its query pool from the host
//   once, when it is created. Tracy only asserts what it needs, and its calibration loops forever
//   without a host time domain, so every requirement is checked first. Tracy looks up the
//   extension's EXT function names only: a driver with VK_KHR_calibrated_timestamps alone takes
//   the other path.
// - Uncalibrated (TracyVkContextCalibrated without the extension): writes one timestamp on the
//   queue when the context is created and aligns the timelines once.
// Both collect through a command buffer (VulkanProfiler.h). Without timestamps on the graphics
// queue the device has no GPU zones.

#include "ith/rhi_vulkan/VulkanDevice.h"

namespace ith::rhi::vulkan {

// What a physical device reports about timing GPU zones.
struct GpuTimingSupport {
    // The graphics queue family writes timestamps (timestampValidBits > 0).
    bool timestamps = false;
    // VkPhysicalDeviceVulkan12Features::hostQueryReset.
    bool hostQueryReset = false;
    // VK_EXT_calibrated_timestamps is supported (the EXT extension, not only the KHR one).
    bool calibratedTimestamps = false;
    // vkGetPhysicalDeviceCalibrateableTimeDomainsEXT reports, among its first four domains (all
    // that Tracy reads), the host clock Tracy reads: VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT
    // on Windows, CLOCK_MONOTONIC_RAW on Linux.
    bool hostTimeDomain = false;
};

// The way to time GPU zones on a device that reports `support`.
[[nodiscard]] constexpr VulkanGpuTiming chooseGpuTiming(const GpuTimingSupport& support) noexcept {
    if (!support.timestamps) {
        return VulkanGpuTiming::Off;
    }
    if (support.calibratedTimestamps && support.hostTimeDomain && support.hostQueryReset) {
        return VulkanGpuTiming::Calibrated;
    }
    return VulkanGpuTiming::Uncalibrated;
}

} // namespace ith::rhi::vulkan
