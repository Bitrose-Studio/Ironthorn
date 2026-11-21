#pragma once

// A mutex that Clang's thread-safety analysis understands (ADR-0001).
//
//   ith::Mutex m_mutex;
//   std::vector<int> m_values ITH_GUARDED_BY(m_mutex);
//
//   void add(int value) {
//       ith::MutexLock lock(m_mutex);
//       m_values.push_back(value);
//   }
//
// Not every standard library annotates std::mutex, so shared state is guarded by
// ith::Mutex and locked through ith::MutexLock; with -Wthread-safety, Clang then reports
// access to guarded state without the lock. Both are thin wrappers over std::mutex: not
// recursive, not copyable and not movable.

#include "ith/foundation/Compiler.h"

#include <mutex>

namespace ith {

class ITH_CAPABILITY("mutex") Mutex {
public:
    Mutex() = default;
    ~Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    // Blocks until the mutex is held by the calling thread. Locking a mutex the thread already
    // holds is undefined, as for std::mutex.
    void lock() ITH_ACQUIRE() { m_mutex.lock(); }

    // Releases the mutex. Precondition: the calling thread holds it.
    void unlock() ITH_RELEASE() { m_mutex.unlock(); }

private:
    std::mutex m_mutex;
};

// Holds a Mutex from construction to destruction.
class ITH_SCOPED_CAPABILITY MutexLock {
public:
    explicit MutexLock(Mutex& mutex) ITH_ACQUIRE(mutex) : m_mutex(mutex) { m_mutex.lock(); }

    ~MutexLock() ITH_RELEASE() { m_mutex.unlock(); }

    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
    MutexLock(MutexLock&&) = delete;
    MutexLock& operator=(MutexLock&&) = delete;

private:
    Mutex& m_mutex;
};

} // namespace ith
