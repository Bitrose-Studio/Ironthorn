// The job system (ADR-0025): deterministic tests without workers, functional tests with
// workers, the wait rules, shutdown and the watchdog.

#include "ith/jobs/JobSystem.h"

#include "ith/foundation/Mutex.h"
#include "ith/jobs/Progress.h"
#include "ith/jobs/Watchdog.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using ith::JobCounter;
using ith::JobLane;
using ith::JobSystem;
using ith::JobSystemConfig;

constexpr JobSystemConfig kNoWorkers{.foregroundWorkers = 0, .backgroundWorkers = 0};

JobSystemConfig workers(std::uint32_t foreground, std::uint32_t background) {
    return JobSystemConfig{.foregroundWorkers = foreground, .backgroundWorkers = background};
}

// For the death tests: prints a failed assert or fatal error like the default handler, then ends the
// process at once with exit code 0. The CTests match the message; a process that dies by SIGABRT counts as
// failed whatever it printed.
[[noreturn]] ith::AssertAction printAndExit(const ith::AssertInfo& info) {
    const char* kind = info.expression != nullptr ? "assertion failed" : "fatal error";
    const char* message = info.message != nullptr ? info.message : "";
    std::fprintf(stderr, "%s(%d): %s: %s\n", info.file, info.line, kind, message);
    std::fflush(stderr);
#ifdef _WIN32
    // On Windows, _Exit still runs process-exit work, which crashed now and then (an access
    // violation) while other threads were mid-job. TerminateProcess ends the process outright.
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    std::_Exit(0);
}

// Submits a job that waits on a counter `levels` times nested, each level one job deeper.
void nest(JobSystem& jobs, int level, int levels, std::atomic<int>& deepest) {
    deepest.store(std::max(deepest.load(), level));
    if (level == levels) {
        return;
    }
    JobCounter counter;
    jobs.submit(
        JobLane::Foreground, "Nest", [&jobs, level, levels, &deepest] { nest(jobs, level + 1, levels, deepest); },
        &counter);
    jobs.wait(counter);
}

} // namespace

TEST_CASE("JobSystem: default worker counts follow the hardware") {
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    CHECK(ith::defaultForegroundWorkerCount() == threads - 1);
    CHECK(ith::defaultBackgroundWorkerCount() >= 1);
    CHECK(ith::defaultBackgroundWorkerCount() <= 4);

    const JobSystem jobs(workers(3, 2));
    CHECK(jobs.workerCount(JobLane::Foreground) == 3);
    CHECK(jobs.workerCount(JobLane::Background) == 2);
    CHECK(jobs.workerCount(JobLane::MainThread) == 0);
    CHECK(jobs.isMainThread());
    CHECK_FALSE(jobs.shutdownRequested());
}

TEST_CASE("JobSystem without workers: wait runs the queued jobs in submission order") {
    JobSystem jobs(kNoWorkers);
    std::vector<int> order;
    JobCounter counter;
    for (int index = 0; index < 5; ++index) {
        jobs.submit(JobLane::Foreground, "Record", [&order, index] { order.push_back(index); }, &counter);
    }
    CHECK(order.empty());
    CHECK_FALSE(counter.isDone());
    jobs.wait(counter);
    CHECK(counter.isDone());
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("JobSystem without workers: jobs submitted by jobs into the same counter are waited for") {
    JobSystem jobs(kNoWorkers);
    std::vector<int> order;
    JobCounter counter;
    jobs.submit(
        JobLane::Foreground, "Parent",
        [&] {
            order.push_back(0);
            jobs.submit(JobLane::Foreground, "Child", [&order] { order.push_back(1); }, &counter);
        },
        &counter);
    jobs.wait(counter);
    CHECK(order == std::vector<int>{0, 1});
}

TEST_CASE("JobSystem without workers: a counter that is already done returns at once") {
    JobSystem jobs(kNoWorkers);
    JobCounter counter;
    CHECK(counter.isDone());
    jobs.wait(counter);
    CHECK(counter.isDone());
}

TEST_CASE("JobSystem without workers: continuations run after their counter, in submission order") {
    JobSystem jobs(kNoWorkers);
    std::vector<int> order;
    JobCounter first;
    JobCounter second;
    jobs.submit(JobLane::Foreground, "First", [&order] { order.push_back(0); }, &first);
    jobs.submitAfter(first, JobLane::Foreground, "Then A", [&order] { order.push_back(1); }, &second);
    jobs.submitAfter(first, JobLane::Foreground, "Then B", [&order] { order.push_back(2); }, &second);
    CHECK_FALSE(second.isDone()); // Counted from submitAfter on, before it is queued.
    jobs.wait(second);
    CHECK(order == std::vector<int>{0, 1, 2});

    // After a counter that is done: queued at once.
    jobs.submitAfter(first, JobLane::Foreground, "Late", [&order] { order.push_back(3); }, &second);
    jobs.wait(second);
    CHECK(order == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("JobSystem without workers: a chain across lanes ends on the main thread") {
    JobSystem jobs(kNoWorkers);
    std::vector<int> order;
    JobCounter imported;
    JobCounter written;
    JobCounter published;
    jobs.submit(JobLane::Background, "Import", [&order] { order.push_back(0); }, &imported);
    jobs.submitAfter(imported, JobLane::Background, "Write", [&order] { order.push_back(1); }, &written);
    jobs.submitAfter(written, JobLane::MainThread, "Publish", [&order] { order.push_back(2); }, &published);

    // The main thread waits outside the frame loop: it runs the jobs of lanes without workers
    // and the main-thread jobs the counter waits for.
    jobs.wait(written);
    CHECK(order == std::vector<int>{0, 1});
    CHECK_FALSE(published.isDone());
    CHECK(jobs.runMainThreadJobs() == 1);
    CHECK(order == std::vector<int>{0, 1, 2});
    CHECK(published.isDone());
}

TEST_CASE("JobSystem: runMainThreadJobs runs what was queued before the call") {
    JobSystem jobs(kNoWorkers);
    std::vector<int> order;
    jobs.postToMainThread("A", [&order] { order.push_back(0); });
    jobs.postToMainThread("B", [&] {
        order.push_back(1);
        jobs.postToMainThread("C", [&order] { order.push_back(2); });
    });
    CHECK(jobs.runMainThreadJobs() == 2);
    CHECK(order == std::vector<int>{0, 1});
    CHECK(jobs.runMainThreadJobs() == 1);
    CHECK(order == std::vector<int>{0, 1, 2});
    CHECK(jobs.runMainThreadJobs() == 0);
}

TEST_CASE("JobSystem: the main thread waits for main-thread jobs by running them") {
    JobSystem jobs(workers(2, 1));
    JobCounter counter;
    std::thread::id ranOn;
    jobs.postToMainThread("Here", [&ranOn] { ranOn = std::this_thread::get_id(); }, &counter);
    jobs.wait(counter);
    CHECK(ranOn == std::this_thread::get_id());
}

TEST_CASE("JobSystem without workers: a full foreground pool makes the submitter run queued jobs") {
    JobSystemConfig config = kNoWorkers;
    config.foregroundJobCapacity = 4;
    JobSystem jobs(config);
    std::vector<int> order;
    JobCounter counter;
    for (int index = 0; index < 10; ++index) {
        jobs.submit(JobLane::Foreground, "Record", [&order, index] { order.push_back(index); }, &counter);
    }
    // The 5th submit found the pool full and ran the oldest queued job, and so on.
    CHECK(order.size() == 6);
    jobs.wait(counter);
    std::vector<int> expected;
    for (int index = 0; index < 10; ++index) {
        expected.push_back(index);
    }
    CHECK(order == expected);
}

TEST_CASE("JobSystem without workers: parallelFor covers every item once, in fixed ranges") {
    JobSystem jobs(kNoWorkers);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    jobs.parallelFor("Ranges", 10, 4,
                     [&ranges](std::size_t begin, std::size_t end) { ranges.emplace_back(begin, end); });
    CHECK(ranges == std::vector<std::pair<std::size_t, std::size_t>>{{0, 4}, {4, 8}, {8, 10}});

    ranges.clear();
    jobs.parallelFor("Empty", 0, 4, [&ranges](std::size_t begin, std::size_t end) { ranges.emplace_back(begin, end); });
    CHECK(ranges.empty());
}

TEST_CASE("JobSystem: parallelFor gives the same ranges and results for any worker count") {
    constexpr std::size_t kCount = 100'003;
    constexpr std::size_t kBatch = 1000;
    constexpr std::size_t kRanges = (kCount + kBatch - 1) / kBatch;

    // A float sum depends on the order of additions, so a deterministic result needs per-range
    // sums merged in range order.
    auto run = [&](std::uint32_t foregroundWorkers) {
        JobSystem jobs(workers(foregroundWorkers, 0));
        std::vector<float> partial(kRanges, 0.0f);
        std::vector<std::size_t> ends(kRanges, 0);
        std::vector<std::atomic<int>> visits(kCount);
        jobs.parallelFor("Sum", kCount, kBatch, [&](std::size_t begin, std::size_t end) {
            float sum = 0.0f;
            for (std::size_t index = begin; index < end; ++index) {
                sum += 1.0f / static_cast<float>(index + 1);
                visits[index].fetch_add(1);
            }
            partial[begin / kBatch] = sum;
            ends[begin / kBatch] = end;
        });
        CHECK(std::ranges::all_of(visits, [](const std::atomic<int>& count) { return count.load() == 1; }));
        float total = 0.0f;
        for (const float sum : partial) {
            total += sum;
        }
        return std::pair{total, ends};
    };

    const auto [serialTotal, serialEnds] = run(0);
    CHECK(serialEnds.back() == kCount);
    for (const std::uint32_t foregroundWorkers : {1u, 2u, 4u, 8u}) {
        CAPTURE(foregroundWorkers);
        const auto [total, ends] = run(foregroundWorkers);
        CHECK(std::memcmp(&total, &serialTotal, sizeof(float)) == 0);
        CHECK(ends == serialEnds);
    }
}

TEST_CASE("JobSystem: workers run many jobs") {
    JobSystem jobs(workers(4, 2));
    std::atomic<int> foreground{0};
    std::atomic<int> background{0};
    JobCounter frame;
    JobCounter longWork;
    for (int index = 0; index < 10'000; ++index) {
        jobs.submit(JobLane::Foreground, "Count", [&foreground] { foreground.fetch_add(1); }, &frame);
    }
    for (int index = 0; index < 100; ++index) {
        jobs.submit(JobLane::Background, "Count", [&background] { background.fetch_add(1); }, &longWork);
    }
    jobs.wait(frame);
    CHECK(foreground.load() == 10'000);
    jobs.wait(longWork);
    CHECK(background.load() == 100);
}

TEST_CASE("JobSystem: background jobs run on background workers, not on a waiting main thread") {
    JobSystem jobs(workers(2, 1));
    JobCounter counter;
    std::thread::id ranOn;
    jobs.submit(JobLane::Background, "Where", [&ranOn] { ranOn = std::this_thread::get_id(); }, &counter);
    jobs.wait(counter);
    CHECK(ranOn != std::thread::id{});
    CHECK(ranOn != std::this_thread::get_id());
}

TEST_CASE("JobSystem: a counter may be destroyed as soon as it is done") {
    JobSystem jobs(workers(4, 1));
    for (int round = 0; round < 2000; ++round) {
        JobCounter counter;
        jobs.submit(JobLane::Foreground, "Tiny", [] {}, &counter);
        if ((round % 2) == 0) {
            jobs.wait(counter);
        } else {
            while (!counter.isDone()) {
                std::this_thread::yield();
            }
        }
    }
}

TEST_CASE("JobSystem: nested waits past the helping depth still finish") {
    JobSystem jobs(workers(2, 0));
    std::atomic<int> deepest{0};
    JobCounter counter;
    jobs.submit(JobLane::Foreground, "Nest", [&jobs, &deepest] { nest(jobs, 0, 12, deepest); }, &counter);
    jobs.wait(counter);
    CHECK(deepest.load() == 12);
}

TEST_CASE("JobSystem: background jobs may wait on foreground work") {
    JobSystem jobs(workers(2, 1));
    std::atomic<int> sum{0};
    JobCounter counter;
    jobs.submit(
        JobLane::Background, "Import",
        [&jobs, &sum] {
            jobs.parallelFor("Decode", 1000, 10, [&sum](std::size_t begin, std::size_t end) {
                sum.fetch_add(static_cast<int>(end - begin));
            });
        },
        &counter);
    jobs.wait(counter);
    CHECK(sum.load() == 1000);
}

TEST_CASE("JobSystem: external threads may submit and wait") {
    JobSystem jobs(workers(2, 1));
    std::atomic<int> count{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 3; ++thread) {
        threads.emplace_back([&jobs, &count] {
            JobCounter counter;
            for (int index = 0; index < 500; ++index) {
                jobs.submit(JobLane::Foreground, "External", [&count] { count.fetch_add(1); }, &counter);
            }
            jobs.wait(counter);
            jobs.parallelFor("External", 100, 7, [&count](std::size_t begin, std::size_t end) {
                count.fetch_add(static_cast<int>(end - begin));
            });
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(count.load() == 3 * 600);
}

TEST_CASE("JobSystem: shutdown runs every pending job of every lane") {
    std::atomic<int> ran{0};
    std::atomic<bool> sawShutdown{false};
    JobCounter gate;
    {
        JobSystem jobs(workers(2, 1));
        for (int index = 0; index < 200; ++index) {
            jobs.submit(JobLane::Foreground, "Pending", [&ran] { ran.fetch_add(1); });
            jobs.submit(JobLane::Background, "Pending", [&ran] { ran.fetch_add(1); });
            jobs.postToMainThread("Pending", [&ran] { ran.fetch_add(1); });
        }
        // Jobs that submit more work and post back to the main thread during shutdown.
        jobs.submit(JobLane::Background, "Spawner", [&jobs, &ran] {
            for (int index = 0; index < 50; ++index) {
                jobs.submit(JobLane::Foreground, "Late", [&ran] { ran.fetch_add(1); });
                jobs.postToMainThread("Late", [&ran] { ran.fetch_add(1); });
            }
        });
        // Long background work that stops when shutdown is requested.
        jobs.submit(JobLane::Background, "Long", [&jobs, &sawShutdown] {
            while (!jobs.shutdownRequested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sawShutdown.store(true);
        });
        // A continuation that is still parked when shutdown begins (the gate counter outlives the
        // job system).
        jobs.submit(
            JobLane::Background, "Gate", [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); }, &gate);
        jobs.submitAfter(gate, JobLane::Foreground, "After gate", [&ran] { ran.fetch_add(1); });
    }
    CHECK(ran.load() == 600 + 100 + 1);
    CHECK(sawShutdown.load());
}

TEST_CASE("JobSystem: a job system can be created and destroyed repeatedly") {
    for (int round = 0; round < 20; ++round) {
        JobSystem jobs(workers(3, 1));
        jobs.submit(JobLane::Foreground, "Work", [] {});
        jobs.submit(JobLane::Background, "Work", [] {});
    }
}

#ifdef ITH_ENABLE_ASSERTS
TEST_CASE("JobSystem: waiting while holding an ith::Mutex asserts") {
    JobSystem jobs(kNoWorkers);
    ith::Mutex mutex;
    JobCounter counter;
    const ith::test::AssertCapture capture;
    {
        const ith::MutexLock lock(mutex);
        jobs.wait(counter);
    }
    CHECK(capture.count() == 1);
    CHECK(capture.lastMessage().find("ith::Mutex") != std::string::npos);
}

TEST_CASE("JobSystem: a foreground job waiting on background jobs asserts") {
    JobSystem jobs(kNoWorkers);
    JobCounter background;
    jobs.submit(JobLane::Background, "Background", [] {}, &background);
    jobs.wait(background);

    const ith::test::AssertCapture capture;
    JobCounter frame;
    jobs.submit(JobLane::Foreground, "Frame", [&jobs, &background] { jobs.wait(background); }, &frame);
    jobs.wait(frame);
    CHECK(capture.count() == 1);
    CHECK(capture.lastMessage().find("background") != std::string::npos);
}

TEST_CASE("JobSystem: a job waiting on main-thread jobs asserts") {
    JobSystem jobs(kNoWorkers);
    JobCounter posted;
    jobs.postToMainThread("Main", [] {}, &posted);
    CHECK(jobs.runMainThreadJobs() == 1);

    const ith::test::AssertCapture capture;
    JobCounter frame;
    jobs.submit(JobLane::Foreground, "Frame", [&jobs, &posted] { jobs.wait(posted); }, &frame);
    jobs.wait(frame);
    CHECK(capture.count() == 1);
    CHECK(capture.lastMessage().find("main-thread") != std::string::npos);
}
#endif

TEST_CASE("Watchdog: a deadlock needs every worker and the main thread stuck, and queued jobs") {
    using ith::detail::ForegroundWaitState;
    using ith::detail::isForegroundDeadlock;
    constexpr ForegroundWaitState kStuck{.workers = 4, .stuckWorkers = 4, .mainThreadStuck = true, .queuedJobs = 2};
    CHECK(isForegroundDeadlock(kStuck));

    ForegroundWaitState state = kStuck;
    state.stuckWorkers = 3;
    CHECK_FALSE(isForegroundDeadlock(state));
    // A busy main thread (not waiting) will still run the queued jobs.
    state = kStuck;
    state.mainThreadStuck = false;
    CHECK_FALSE(isForegroundDeadlock(state));
    state = kStuck;
    state.queuedJobs = 0;
    CHECK_FALSE(isForegroundDeadlock(state));
    // Without workers the waiting threads run everything themselves.
    CHECK_FALSE(isForegroundDeadlock(
        ForegroundWaitState{.workers = 0, .stuckWorkers = 0, .mainThreadStuck = true, .queuedJobs = 1}));
}

TEST_CASE("Watchdog: a worker stuck past the helping depth while the main thread is busy is no deadlock") {
    JobSystem jobs(workers(1, 0));
    std::atomic<int> deepest{0};
    JobCounter counter;
    // The only worker nests past the helping depth and sleeps with a job queued; the main thread
    // is busy for longer than the watchdog's grace, then waits and runs the rest.
    jobs.submit(JobLane::Foreground, "Nest", [&jobs, &deepest] { nest(jobs, 0, 10, deepest); }, &counter);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    jobs.wait(counter);
    CHECK(deepest.load() == 10);
}

TEST_CASE("JobSystem: a waiting main thread runs main-thread jobs added to its counter meanwhile") {
    JobSystem jobs(workers(2, 1));
    JobCounter counter;
    std::thread::id ranOn;
    jobs.submit(
        JobLane::Foreground, "Parent",
        [&jobs, &counter, &ranOn] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // The main thread is asleep in wait.
            jobs.postToMainThread("Child", [&ranOn] { ranOn = std::this_thread::get_id(); }, &counter);
        },
        &counter);
    jobs.wait(counter);
    CHECK(ranOn == std::this_thread::get_id());
}

TEST_CASE("JobSystem: a waiter runs background jobs added to its counter when the lane has no workers") {
    JobSystem jobs(workers(2, 0));
    JobCounter counter;
    std::atomic<bool> childRan{false};
    jobs.submit(
        JobLane::Foreground, "Parent",
        [&jobs, &counter, &childRan] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            jobs.submit(JobLane::Background, "Child", [&childRan] { childRan.store(true); }, &counter);
        },
        &counter);
    jobs.wait(counter);
    CHECK(childRan.load());
}

TEST_CASE("JobSystem without workers: lanes a counter gains while waited on are run") {
    JobSystem jobs(kNoWorkers);
    JobCounter counter;
    std::vector<int> order;
    jobs.submit(
        JobLane::Foreground, "Parent",
        [&jobs, &counter, &order] {
            order.push_back(0);
            jobs.submit(JobLane::Background, "Child", [&order] { order.push_back(1); }, &counter);
        },
        &counter);
    jobs.wait(counter);
    CHECK(order == std::vector<int>{0, 1});
}

TEST_CASE("JobSystem without workers: a waiter runs jobs its counter depends on only indirectly") {
    JobSystem jobs(kNoWorkers);
    JobCounter imported;
    JobCounter done;
    std::vector<int> order;
    jobs.submit(JobLane::Background, "Import", [&order] { order.push_back(0); }, &imported);
    jobs.submitAfter(imported, JobLane::Foreground, "Use", [&order] { order.push_back(1); }, &done);
    // `done` counts only a foreground job, which waits for a background one.
    jobs.wait(done);
    CHECK(order == std::vector<int>{0, 1});
}

TEST_CASE("JobSystem: a continuation counted by the counter it follows asserts and runs at once") {
    JobSystem jobs(workers(1, 1));
    JobCounter counter;
    std::atomic<int> ran{0};
    const ith::test::AssertCapture capture;
    jobs.submit(JobLane::Foreground, "A", [&ran] { ran.fetch_add(1); }, &counter);
    jobs.submitAfter(counter, JobLane::Foreground, "B", [&ran] { ran.fetch_add(1); }, &counter);
    jobs.wait(counter);
    CHECK(ran.load() == 2);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
}

// Deadlocks on purpose and must end the process with the watchdog's fatal error. Skipped in the
// normal run; the CTest jobs_watchdog runs it alone and expects the message.
TEST_CASE("Watchdog: stops the process when nested waits deadlock the foreground lane" * doctest::skip()) {
    ith::setAssertHandler(&printAndExit);
    JobSystem jobs(workers(1, 0));
    std::atomic<int> deepest{0};
    JobCounter counter;
    // A chain of 30 nested waits: the worker and the main thread each help up to the helping
    // depth, then both sleep while the next level is queued.
    jobs.submit(JobLane::Foreground, "Nest", [&jobs, &deepest] { nest(jobs, 0, 30, deepest); }, &counter);
    jobs.wait(counter);
    FAIL("the watchdog did not stop the process");
}

// Fills the foreground pool with continuations parked on a counter whose running job then needs
// a record. Skipped in the normal run; the CTest jobs_pool_exhausted runs it alone and expects
// the fatal error.
TEST_CASE("JobSystem: stops the process when parked continuations exhaust the foreground pool" * doctest::skip()) {
    ith::setAssertHandler(&printAndExit);
    JobSystemConfig config = workers(1, 1);
    config.foregroundJobCapacity = 2;
    JobSystem jobs(config);
    JobCounter counter;
    JobCounter after;
    std::atomic<bool> started{false};
    std::atomic<bool> parked{false};
    jobs.submit(
        JobLane::Foreground, "Parent",
        [&jobs, &counter, &started, &parked] {
            started.store(true);
            while (!parked.load()) {
                std::this_thread::yield();
            }
            jobs.submit(JobLane::Foreground, "Child", [] {}, &counter);
        },
        &counter);
    // Once the parent runs on the worker, its record is free again. (Had the main thread run it
    // instead, it would spin there forever, waiting for itself to set `parked`.)
    while (!started.load()) {
        std::this_thread::yield();
    }
    jobs.submitAfter(counter, JobLane::Foreground, "Then", [] {}, &after);
    jobs.submitAfter(counter, JobLane::Foreground, "Then", [] {}, &after);
    parked.store(true);
    jobs.wait(after);
    FAIL("the pool-exhaustion check did not stop the process");
}

TEST_CASE("JobSystem: the main-thread wake hook runs after every main-thread post and progress update") {
    JobSystem jobs(workers(1, 1));
    std::atomic<int> wakes = 0;
    jobs.setMainThreadWake([](void* context) { ++*static_cast<std::atomic<int>*>(context); }, &wakes);

    // A post from the main thread, from a background job, and a continuation that lands on the
    // main thread each wake it once.
    jobs.postToMainThread("Direct", [] {});
    CHECK(wakes.load() == 1);
    JobCounter background;
    jobs.submit(JobLane::Background, "Post", [&] { jobs.postToMainThread("Result", [] {}); }, &background);
    jobs.wait(background);
    CHECK(wakes.load() == 2);
    JobCounter before;
    JobCounter after;
    jobs.submit(JobLane::Foreground, "Before", [] {}, &before);
    jobs.submitAfter(before, JobLane::MainThread, "After", [] {}, &after);
    jobs.wait(after);
    // The worker that finishes "Before" queues "After" and then calls the hook, so the main thread,
    // which may run "After" as soon as it is queued, can return from the wait before the hook ran.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (wakes.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(wakes.load() == 3);
    static_cast<void>(jobs.runMainThreadJobs()); // the waits may have run some of them already

    // Work on other lanes doesn't wake the main thread; progress made with the JobSystem does.
    JobCounter other;
    jobs.submit(JobLane::Background, "Quiet", [] {}, &other);
    jobs.wait(other);
    CHECK(wakes.load() == 3);
    ith::Progress progress(jobs);
    progress.addWork(2);
    progress.complete();
    CHECK(wakes.load() == 5);
    ith::Progress silent;
    silent.addWork(1);
    CHECK(wakes.load() == 5);

    // jobs.wakeMainThread() calls it directly; a null hook removes it.
    jobs.wakeMainThread();
    CHECK(wakes.load() == 6);
    jobs.setMainThreadWake(nullptr, nullptr);
    jobs.postToMainThread("Unwoken", [] {});
    CHECK(wakes.load() == 6);
    CHECK(jobs.runMainThreadJobs() == 1);
}
