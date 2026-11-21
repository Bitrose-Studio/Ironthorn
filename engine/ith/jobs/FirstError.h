#pragma once

// FirstError: keeps the first error of a group of jobs (ADR-0025 rule 16). Jobs return
// nothing; a job that fails records its error here, and the caller reads the result once the
// jobs are done.
//
//   ith::FirstError<> errors;
//   ith::JobCounter counter;
//   for (std::size_t index = 0; index < files.size(); ++index) {
//       jobs.submit(ith::JobLane::Background, "Import", [&, index] {
//           errors.check(index, importFile(files[index]));
//       }, &counter);
//   }
//   jobs.wait(counter);
//   ith::Expected<void> result = errors.result(); // the error of the lowest failing index

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Mutex.h"

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace ith {

// Keeps the error with the lowest order of all the errors recorded, so the result is the same
// whatever order the jobs ran in (ADR-0025 rule 17). Owned by the caller; it must outlive the
// jobs that record into it. All members are thread-safe.
template <typename E = Error>
class FirstError {
public:
    FirstError() = default;

    FirstError(const FirstError&) = delete;
    FirstError& operator=(const FirstError&) = delete;
    FirstError(FirstError&&) = delete;
    FirstError& operator=(FirstError&&) = delete;

    // Records `error` for the item `order` (e.g. its index). Kept if no error with a lower
    // order was recorded; of two errors with the same order, the first recorded is kept.
    void record(std::size_t order, E error) {
        const MutexLock lock(m_mutex);
        if (!m_error.has_value() || order < m_order) {
            m_error = std::move(error);
            m_order = order;
        }
        m_failed.store(true, std::memory_order_release);
    }

    // Records the error of `result`, if it holds one. Returns true when it holds a value.
    template <typename T>
    bool check(std::size_t order, Expected<T, E> result) {
        if (result) {
            return true;
        }
        record(order, std::move(result).error());
        return false;
    }

    // True once any error was recorded. Cheap: other jobs of the group may poll it to stop early.
    bool failed() const { return m_failed.load(std::memory_order_acquire); }

    // Success, or a copy of the kept error.
    Expected<void, E> result() const {
        const MutexLock lock(m_mutex);
        if (m_error.has_value()) {
            return makeUnexpected(*m_error);
        }
        return {};
    }

private:
    mutable Mutex m_mutex;
    std::optional<E> m_error ITH_GUARDED_BY(m_mutex);
    std::size_t m_order ITH_GUARDED_BY(m_mutex) = 0;
    std::atomic<bool> m_failed{false};
};

} // namespace ith
