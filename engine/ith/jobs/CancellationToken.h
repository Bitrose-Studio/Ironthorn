#pragma once

// CancellationToken: cooperative cancellation of a group of jobs (ADR-0025 rule 14).
//
//   ith::CancellationToken cancel;
//   jobs.submit(ith::JobLane::Background, "Import", [&cancel] {
//       for (const Item& item : items) {
//           if (cancel.isCancelled()) {
//               return;
//           }
//           import(item);
//       }
//   });
//   ...
//   cancel.cancel(); // from any thread, e.g. the editor's cancel button

#include <atomic>

namespace ith {

// A flag that an operation's jobs check to stop early. Owned by the operation; jobs hold a
// pointer or reference to it, so it must outlive them. Cancelling removes no queued job: queued
// jobs still run, see the flag and return. All members are thread-safe.
class CancellationToken {
public:
    CancellationToken() = default;

    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;
    CancellationToken(CancellationToken&&) = delete;
    CancellationToken& operator=(CancellationToken&&) = delete;

    // Asks the jobs that check this token to stop.
    void cancel() { m_cancelled.store(true, std::memory_order_release); }

    // True once cancel() was called (and reset() wasn't called since).
    bool isCancelled() const { return m_cancelled.load(std::memory_order_acquire); }

    // Clears the flag, so the token can serve another operation. Call it only when no job of
    // the previous operation is still running.
    void reset() { m_cancelled.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_cancelled{false};
};

} // namespace ith
