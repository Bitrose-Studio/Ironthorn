#include "ith/jobs/JobCounter.h"

#include "ith/foundation/Assert.h"

namespace ith {

JobCounter::~JobCounter() {
    const MutexLock lock(m_mutex);
    ITH_ASSERT_MSG(m_pending.load() == 0, "JobCounter destroyed while jobs submitted with it are unfinished");
    ITH_ASSERT_MSG(m_continuations == nullptr, "JobCounter destroyed with continuations waiting on it");
}

bool JobCounter::isDone() const {
    if (m_pending.load() != 0) {
        return false;
    }
    // The last job decrements the counter while holding the mutex, so taking it here waits until
    // that job has let go of the counter.
    const MutexLock lock(m_mutex);
    return m_pending.load() == 0;
}

} // namespace ith
