#pragma once

// Job lanes and the job record of the job system (ADR-0025). Engine code uses JobSystem
// (JobSystem.h); the record is visible only because JobSystem's templates build it in place.

#include <array>
#include <cstddef>
#include <cstdint>

namespace ith {

class JobCounter;

// Where a job runs (ADR-0025 rules 4 and 7).
enum class JobLane : std::uint8_t {
    Foreground, // Frame work: short jobs that never block on files, sockets or OS waits.
    Background, // Long work: import, cooking, shader compilation, streaming. May block on files.
    MainThread, // Runs on the main thread in JobSystem::runMainThreadJobs().
};

// The largest callable a job can hold, captures included. A job that needs more captures a
// pointer to the caller's own data.
inline constexpr std::size_t kJobCaptureSize = 48;

namespace detail {

// One job: a name and an inline callable, in a record owned by the JobSystem's pools. `next`
// links the record into exactly one list at a time: a pool's free list, a lane's queue or a
// counter's continuations. A job leaves its record when it starts (its callable moves to the
// running thread's stack), so records are held only by queued and parked jobs.
struct Job {
    alignas(std::max_align_t) std::array<std::byte, kJobCaptureSize> storage{};
    void (*relocate)(void* from, void* to) = nullptr; // Moves the callable to `to`, destroying it at `from`.
    void (*run)(void* storage) = nullptr;             // Calls the callable at `storage`, then destroys it.
    const char* name = nullptr;
    JobCounter* counter = nullptr;
    Job* next = nullptr;
    JobLane lane = JobLane::Foreground;
};

} // namespace detail
} // namespace ith
