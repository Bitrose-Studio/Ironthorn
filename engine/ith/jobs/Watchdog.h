#pragma once

// The deadlock rule of the job system's watchdog (ADR-0025 rule 12, ADR-0026 rule 4), separate
// so tests can check it without deadlocking a process. Internal to the jobs module and its tests.

#include <cstdint>

namespace ith::detail {

// What the foreground lane looks like when a thread is about to sleep in JobSystem::wait.
struct ForegroundWaitState {
    std::uint32_t workers = 0;      // Foreground worker threads.
    std::uint32_t stuckWorkers = 0; // Workers asleep in a wait past the helping depth, so they can't run jobs.
    bool mainThreadStuck = false;   // The main thread is asleep in a wait past the helping depth.
    std::uint32_t queuedJobs = 0;   // Foreground jobs in the queue.
};

// True when no thread can run the queued foreground jobs (ADR-0026 rule 4): every foreground
// worker and the main thread sleep in waits past the helping depth, and jobs are queued. A
// main thread that is busy, not waiting, will still run them. Never true without workers,
// where waiting threads run every foreground job themselves.
constexpr bool isForegroundDeadlock(const ForegroundWaitState& state) {
    return state.workers > 0 && state.stuckWorkers == state.workers && state.mainThreadStuck && state.queuedJobs > 0;
}

} // namespace ith::detail
