// The main-thread wake-up (ADR-0035 rules 22 and 23) on SDL's offscreen video driver: waitEvents
// with a timeout, Platform::wake from other threads, and a JobSystem whose main-thread posts end
// the wait. Run through CTest only.

#include "ith/jobs/JobSystem.h"
#include "ith/platform/Platform.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

ith::Platform createHeadless() {
    ith::Expected<ith::Platform> platform = ith::Platform::create({.videoDriver = "offscreen"});
    REQUIRE_MESSAGE(platform.hasValue(), (platform ? "" : platform.error().message.c_str()));
    return std::move(*platform);
}

// Takes the events of start-up, so the waits below see only what the test causes.
void drain(ith::Platform& platform) {
    while (!platform.waitEvents(std::chrono::milliseconds(20)).empty()) {}
}

} // namespace

TEST_CASE("waitEvents returns after its timeout when nothing happens") {
    ith::Platform platform = createHeadless();
    drain(platform);
    const Clock::time_point start = Clock::now();
    CHECK(platform.waitEvents(std::chrono::milliseconds(60)).empty());
    const Clock::duration waited = Clock::now() - start;
    CHECK(waited >= std::chrono::milliseconds(50));
    CHECK(waited < std::chrono::seconds(1));
    // A zero or negative timeout doesn't wait.
    const Clock::time_point again = Clock::now();
    CHECK(platform.waitEvents(std::chrono::milliseconds(-5)).empty());
    CHECK(Clock::now() - again < std::chrono::milliseconds(50));
}

TEST_CASE("wake ends a wait, coalesces, and leaves no event behind") {
    ith::Platform platform = createHeadless();
    drain(platform);
    // Before the wait: the wait returns at once, without an event.
    for (int index = 0; index < 100; ++index) {
        ith::Platform::wake();
    }
    const Clock::time_point start = Clock::now();
    CHECK(platform.waitEvents(std::chrono::seconds(1)).empty());
    CHECK(Clock::now() - start < std::chrono::milliseconds(50));
    // The hundred wakes were one event: nothing is left to end the next wait early.
    const Clock::time_point quiet = Clock::now();
    CHECK(platform.waitEvents(std::chrono::milliseconds(60)).empty());
    CHECK(Clock::now() - quiet >= std::chrono::milliseconds(50));

    // From another thread, during the wait.
    std::thread waker([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ith::Platform::wake();
    });
    const Clock::time_point during = Clock::now();
    CHECK(platform.waitEvents(std::chrono::seconds(1)).empty());
    CHECK(Clock::now() - during < std::chrono::milliseconds(600));
    waker.join();
    // pumpEvents drops a wake too.
    ith::Platform::wake();
    CHECK(platform.pumpEvents().empty());
}

TEST_CASE("wake does nothing when no Platform exists") {
    ith::Platform::wake();
    ith::Platform platform = createHeadless();
    drain(platform);
    const Clock::time_point start = Clock::now();
    CHECK(platform.waitEvents(std::chrono::milliseconds(60)).empty());
    CHECK(Clock::now() - start >= std::chrono::milliseconds(50));
}

TEST_CASE("a post from a background job ends waitEvents within 50 ms") {
    ith::Platform platform = createHeadless();
    ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
    jobs.setMainThreadWake([](void* /*context*/) { ith::Platform::wake(); }, nullptr);
    drain(platform);

    std::atomic<Clock::rep> postedAt = 0;
    bool resultArrived = false;
    jobs.submit(ith::JobLane::Background, "Load", [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        postedAt.store(Clock::now().time_since_epoch().count());
        jobs.postToMainThread("Loaded", [&] { resultArrived = true; });
    });
    const Clock::time_point start = Clock::now();
    static_cast<void>(platform.waitEvents(std::chrono::seconds(1)));
    const Clock::time_point woke = Clock::now();
    REQUIRE(postedAt.load() != 0);
    const Clock::duration latency = woke - Clock::time_point(Clock::duration(postedAt.load()));
    CHECK(latency < std::chrono::milliseconds(50));
    CHECK(woke - start < std::chrono::milliseconds(900));
    // The frame order of rule 22: after the events, the main-thread jobs.
    CHECK(jobs.runMainThreadJobs() == 1);
    CHECK(resultArrived);
    // Removed before the Platform goes, as the application does.
    jobs.setMainThreadWake(nullptr, nullptr);
}
