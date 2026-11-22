#pragma once

// ith::IdlePolicy: when the editor draws a frame (ADR-0029 rule 12).
//
//   ith::IdlePolicy idle({.focused = preferences.idleFramesPerSecond,
//                         .unfocused = preferences.idleFramesPerSecondUnfocused});
//   while (running) {
//       const auto timeout = idle.waitTimeout(focused);
//       std::span<const ith::Event> events =
//           timeout.count() == 0 ? platform.pumpEvents() : platform.waitEvents(timeout);
//       if (!events.empty() || jobs.runMainThreadJobs() > 0 || changed) {
//           idle.requestFrame();
//       }
//       ... draw a frame ...
//       idle.frameFinished();
//   }
//
// The editor redraws on demand: something happened (events, a view model changed, a job reported,
// Play In Editor runs, a panel animates), so a frame runs at once and Dear ImGui gets
// kSettleFrames more to settle its layout. Otherwise the loop waits for events with a timeout and
// draws at the idle rate: Editor Preferences' rate while a window has focus, a lower one while
// none has. Platform::wake() ends the wait early when a job posts to the main thread.
//
// Pure logic, without a clock: the application measures time. Main thread only.

#include <chrono>
#include <cstdint>

namespace ith {

// Idle frame rates in frames per second; values below 1 count as 1.
struct IdleRates {
    std::int32_t focused = 10;
    std::int32_t unfocused = 2;
};

class IdlePolicy {
public:
    // The frames that follow the one a request asks for, so the UI settles.
    static constexpr std::uint32_t kSettleFrames = 2;

    explicit IdlePolicy(IdleRates rates = {}) noexcept : m_rates(rates) {}

    void setRates(IdleRates rates) noexcept { m_rates = rates; }

    [[nodiscard]] IdleRates rates() const noexcept { return m_rates; }

    // Asks for a frame now and kSettleFrames after it. A request while frames are pending starts
    // the count again.
    void requestFrame() noexcept { m_pendingFrames = 1 + kSettleFrames; }

    // Records that a frame was drawn.
    void frameFinished() noexcept {
        if (m_pendingFrames > 0) {
            --m_pendingFrames;
        }
    }

    // Frames still asked for.
    [[nodiscard]] std::uint32_t pendingFrames() const noexcept { return m_pendingFrames; }

    // How long the loop may wait for events before the next frame: zero while frames are asked
    // for, otherwise one frame at the idle rate (`focused`: whether an editor window has focus),
    // rounded down to milliseconds.
    [[nodiscard]] std::chrono::milliseconds waitTimeout(bool focused) const noexcept {
        if (m_pendingFrames > 0) {
            return std::chrono::milliseconds(0);
        }
        const std::int32_t rate = focused ? m_rates.focused : m_rates.unfocused;
        return std::chrono::milliseconds(1000 / (rate < 1 ? 1 : rate));
    }

private:
    IdleRates m_rates;
    std::uint32_t m_pendingFrames = 1 + kSettleFrames;
};

} // namespace ith
