#pragma once

// Progress: how much of an operation is done, updated by its jobs and read by the editor once
// per frame (ADR-0025 rule 15).
//
//   ith::Progress progress;
//   progress.addWork(static_cast<std::uint32_t>(files.size()));
//   // in each job, after finishing one file:
//   progress.complete();
//   // on the main thread:
//   const ith::Progress::Snapshot now = progress.snapshot();
//   drawBar(now.fraction());
//
// A Progress made with a JobSystem calls its main-thread wake hook on every update, so an editor
// that waits for events while idle redraws the bar (ADR-0035 rule 23):
//
//   ith::Progress progress(jobs);

#include <atomic>
#include <cstdint>

namespace ith {

class JobSystem;

namespace detail {
// Calls jobs.wakeMainThread(); defined in JobSystem.cpp, so this header needs no JobSystem.
void wakeMainThread(const JobSystem& jobs);
} // namespace detail

// An atomic pair of counts: items done and items in total. Both live in one 64-bit atomic, so
// a snapshot never shows more done than the total that was added before it. Owned by the
// operation; it must outlive the jobs that update it. All members are thread-safe.
class Progress {
public:
    struct Snapshot {
        std::uint32_t done = 0;
        std::uint32_t total = 0;

        // done / total, or 0 when the total is 0.
        float fraction() const { return total == 0 ? 0.0f : static_cast<float>(done) / static_cast<float>(total); }
    };

    Progress() = default;

    // A Progress whose updates call the main-thread wake hook of `jobs`, which must outlive it.
    explicit Progress(const JobSystem& jobs) : m_jobs(&jobs) {}

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    Progress(Progress&&) = delete;
    Progress& operator=(Progress&&) = delete;
    ~Progress() = default;

    // Adds `items` to the total. The total must stay below 2^32.
    void addWork(std::uint32_t items) {
        m_state.fetch_add(static_cast<std::uint64_t>(items) << 32u);
        wake();
    }

    // Adds `items` to the done count. The done count must not exceed the total.
    void complete(std::uint32_t items = 1) {
        m_state.fetch_add(items);
        wake();
    }

    Snapshot snapshot() const {
        const std::uint64_t state = m_state.load();
        return Snapshot{.done = static_cast<std::uint32_t>(state & 0xffff'ffffu),
                        .total = static_cast<std::uint32_t>(state >> 32u)};
    }

    // Sets both counts to 0. Call it only when no job updates the progress.
    void reset() { m_state.store(0); }

private:
    void wake() const {
        if (m_jobs != nullptr) {
            detail::wakeMainThread(*m_jobs);
        }
    }

    const JobSystem* m_jobs = nullptr;
    std::atomic<std::uint64_t> m_state{0}; // Total in the high 32 bits, done in the low 32.
};

} // namespace ith
