#pragma once

// The null backend: a device without a GPU (ADR-0009, ADR-0021).
//
// It keeps every object, handle, descriptor index, frame and swapchain the way a real backend
// does, and drops the commands. Tests use it to check the RHI's backend-neutral rules (handle
// lifetimes, deferred destruction, frame pacing, error paths) without a GPU, and the editor or
// player can run on it without a GPU. A NullGpu, when given, simulates the GPU's progress and
// injects faults:
//
//   ith::rhi::NullGpu gpu;
//   gpu.setAutoComplete(false); // submitted work completes only when the test says so
//   auto device = ith::rhi::createNullDevice({.gpu = &gpu});
//   ...
//   gpu.complete(gpu.signaledValue());
//   gpu.loseDevice("simulated hang"); // the next submit, wait or present reports DeviceLost

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Mutex.h"
#include "ith/rhi/Capabilities.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Error.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ith::rhi {

// The simulated GPU of a null device, shared by the test and the backend. Thread-safe.
class NullGpu {
public:
    NullGpu() = default;
    NullGpu(const NullGpu&) = delete;
    NullGpu& operator=(const NullGpu&) = delete;
    NullGpu(NullGpu&&) = delete;
    NullGpu& operator=(NullGpu&&) = delete;
    ~NullGpu() = default;

    // On (the default), work completes as soon as it is submitted. Off, it completes only
    // through complete(), or when the device has to wait for it (which counts in waitCount()).
    void setAutoComplete(bool autoComplete);
    // Completes the work up to `value`, at most up to signaledValue().
    void complete(std::uint64_t value);
    // The highest timeline value submitted, and the highest completed.
    [[nodiscard]] std::uint64_t signaledValue() const;
    [[nodiscard]] std::uint64_t completedValue() const;
    // How often the device blocked on work that had not completed.
    [[nodiscard]] std::uint64_t waitCount() const;

    // From now on every submit, wait, acquire and present fails with DeviceLost and `reason`.
    void loseDevice(std::string reason);
    // The next create call of a buffer, texture, sampler, pipeline or swapchain fails with
    // OutOfMemory.
    void failNextCreate();
    // The next command list fails to start with OutOfMemory, which its submit reports.
    void failNextCommandList();
    // The next acquire or present reports the swapchain as out of date.
    void outdateNextAcquire();
    void outdateNextPresent();
    // The next swapchain recreation fails with OutOfMemory, leaving the swapchain without images.
    void failNextResize();

    // Backend objects alive: buffers, textures (without swapchain images), samplers, pipelines
    // and swapchains.
    [[nodiscard]] std::uint32_t liveObjectCount() const;
    // Recording calls that reached the backend, i.e. that passed the device's checks.
    [[nodiscard]] std::uint64_t commandCount() const;
    // Successful submit and present calls, and swapchain creations and recreations.
    [[nodiscard]] std::uint64_t submitCount() const;
    [[nodiscard]] std::uint64_t presentCount() const;
    [[nodiscard]] std::uint64_t swapchainBuildCount() const;

private:
    friend class NullBackend;
    friend class NullCommandList;

    mutable Mutex m_mutex;
    bool m_autoComplete ITH_GUARDED_BY(m_mutex) = true;
    std::uint64_t m_signaled ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_completed ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_waits ITH_GUARDED_BY(m_mutex) = 0;
    bool m_lost ITH_GUARDED_BY(m_mutex) = false;
    std::string m_lostReason ITH_GUARDED_BY(m_mutex);
    bool m_failNextCreate ITH_GUARDED_BY(m_mutex) = false;
    bool m_failNextCommandList ITH_GUARDED_BY(m_mutex) = false;
    bool m_outdateNextAcquire ITH_GUARDED_BY(m_mutex) = false;
    bool m_outdateNextPresent ITH_GUARDED_BY(m_mutex) = false;
    bool m_failNextResize ITH_GUARDED_BY(m_mutex) = false;
    std::uint32_t m_liveObjects ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_commands ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_submits ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_presents ITH_GUARDED_BY(m_mutex) = 0;
    std::uint64_t m_swapchainBuilds ITH_GUARDED_BY(m_mutex) = 0;
};

struct NullDeviceDesc {
    // The optional features and limits the device reports. `backend`, `shaderFormat` and the
    // adapter are set by the backend (Null, None, "Null GPU").
    Capabilities capabilities;
    // Minimum features the simulated GPU lacks; when not empty, creation fails with
    // UnsupportedDevice naming them.
    std::span<const std::string_view> missingFeatures;
    // The simulated GPU, or nullptr for one owned by the device that completes work at once. Not
    // owned; must outlive the device.
    NullGpu* gpu = nullptr;
};

// The most bytes the null backend allocates for one Upload or Readback buffer's mapped memory;
// larger ones fail with OutOfMemory, as a real GPU would, instead of ending the process.
inline constexpr std::uint64_t kNullMaxMappedSize = 256ull << 20;

// Creates a null device. Fails with UnsupportedDevice when desc.missingFeatures is not empty.
Expected<Device, Error> createNullDevice(const NullDeviceDesc& desc = {});

} // namespace ith::rhi
