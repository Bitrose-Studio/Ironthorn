#include "ith/foundation/Mutex.h"

#include <doctest/doctest.h>

#include <thread>
#include <vector>

namespace {

// A counter whose value is guarded by a mutex, as engine code guards shared state.
class Counter {
public:
    void increment() {
        ith::MutexLock lock(m_mutex);
        ++m_value;
    }

    int value() {
        ith::MutexLock lock(m_mutex);
        return m_value;
    }

private:
    ith::Mutex m_mutex;
    int m_value ITH_GUARDED_BY(m_mutex) = 0;
};

} // namespace

TEST_CASE("Mutex: MutexLock serializes increments from several threads") {
    constexpr int kThreads = 4;
    constexpr int kIncrements = 10000;
    Counter counter;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&counter] {
            for (int index = 0; index < kIncrements; ++index) {
                counter.increment();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(counter.value() == kThreads * kIncrements);
}

TEST_CASE("Mutex: heldMutexCount counts the locks the calling thread holds") {
    ith::Mutex first;
    ith::Mutex second;
    CHECK(ith::heldMutexCount() == 0);
    {
        ith::MutexLock outer(first);
        ith::MutexLock inner(second);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(ith::heldMutexCount() == 2);
        // Another thread holds nothing.
        int otherThreadCount = -1;
        std::thread([&otherThreadCount] { otherThreadCount = ith::heldMutexCount(); }).join();
        CHECK(otherThreadCount == 0);
#else
        CHECK(ith::heldMutexCount() == 0);
#endif
    }
    CHECK(ith::heldMutexCount() == 0);
}
