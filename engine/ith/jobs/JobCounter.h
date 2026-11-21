#pragma once

// JobCounter: counts unfinished jobs, for waiting and for continuations (ADR-0025 rules 9-10).
//
//   ith::JobCounter counter;
//   jobs.submit(ith::JobLane::Foreground, "Cull", [&] { cull(); }, &counter);
//   jobs.submitAfter(counter, ith::JobLane::Foreground, "Sort", [&] { sort(); });
//   jobs.wait(counter);

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Mutex.h"
#include "ith/jobs/Job.h"

#include <atomic>
#include <cstdint>

namespace ith {

// The number of unfinished jobs submitted with it. Owned by the caller, typically on the
// stack; neither copyable nor movable, because queued jobs point at it. A counter that reaches
// zero may be used again; it keeps remembering the lanes it was used with, which the wait rules
// check (JobSystem::wait). Destroying a counter while jobs are pending or continuations are
// parked on it asserts: wait for it (or check isDone()) first.
//
// All members are thread-safe.
class JobCounter {
public:
    JobCounter() = default;
    ~JobCounter();

    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;
    JobCounter(JobCounter&&) = delete;
    JobCounter& operator=(JobCounter&&) = delete;

    // True when no job submitted with the counter is unfinished. Once it returns true, the job
    // system no longer touches the counter (until it is used again), so the caller may destroy
    // it.
    bool isDone() const;

private:
    friend class JobSystem;

    // The number of unfinished jobs. The decrement to zero happens while m_mutex is held, so a
    // thread that sees zero under m_mutex knows the job system has finished with the counter.
    std::atomic<std::uint32_t> m_pending{0};

    // One bit per JobLane this counter was ever used with, for the wait rules.
    std::atomic<std::uint8_t> m_lanes{0};

    mutable Mutex m_mutex;
    detail::Job* m_continuations ITH_GUARDED_BY(m_mutex) = nullptr;
};

} // namespace ith
