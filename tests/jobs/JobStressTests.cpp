// Stress tests of the job system (ADR-0025 rule 19): random job trees with counters,
// continuations and nested waits, pool exhaustion, counter lifetimes, concurrent parallelFor,
// cancellation and shutdown with work in flight. They are sized to finish in a few seconds in
// Debug and in the sanitizer builds, where TSan and ASan check them; optimized builds run
// millions of jobs.

#include "ith/jobs/CancellationToken.h"
#include "ith/jobs/JobSystem.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define ITH_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define ITH_TEST_SANITIZED 1
#endif
#endif

namespace {

using ith::JobCounter;
using ith::JobLane;
using ith::JobSystem;
using ith::JobSystemConfig;

// How many times larger than the sanitizer runs the Debug and optimized runs are.
#if defined(ITH_TEST_SANITIZED)
constexpr int kScale = 1;
#elif defined(ITH_DEBUG)
constexpr int kScale = 4;
#else
constexpr int kScale = 20;
#endif

std::uint32_t mix(std::uint32_t value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

// A random tree of jobs. Each node, chosen by its seed, either waits for its children in a
// nested wait, adds them to the counter it was given, or adds them to a counter of its own and
// queues a continuation after it.
struct Tree {
    static constexpr std::uint32_t kMaxDepth = 6;
    static constexpr std::uint32_t kMaxNesting = 3;

    JobSystem* jobs = nullptr;
    std::atomic<std::uint64_t> visited{0};
    std::unique_ptr<JobCounter[]> stageCounters; // One per node that queues a continuation.
    std::atomic<std::size_t> nextStage{0};
    std::atomic<int> nestedWaitBudget{4};

    enum class Mode : std::uint8_t {
        NestedWait,
        SameCounter,
        Continuation
    };

    static std::uint32_t children(std::uint32_t seed, std::uint32_t depth) {
        return depth >= kMaxDepth ? 0 : mix(seed) % 5;
    }

    static Mode mode(std::uint32_t seed) { return static_cast<Mode>((mix(seed) >> 8u) % 3); }

    static std::uint32_t childSeed(std::uint32_t seed, std::uint32_t index) { return mix(seed * 31u + index + 1); }

    // Jobs the tree runs, continuations included, and the stage counters it needs.
    static void count(std::uint32_t seed, std::uint32_t depth, std::uint64_t& jobs, std::size_t& stages) {
        ++jobs;
        if (mode(seed) == Mode::Continuation) {
            ++jobs;
            ++stages;
        }
        for (std::uint32_t index = 0; index < children(seed, depth); ++index) {
            count(childSeed(seed, index), depth + 1, jobs, stages);
        }
    }

    void node(std::uint32_t seed, std::uint32_t depth, std::uint32_t nesting, JobCounter* counter) {
        visited.fetch_add(1);
        const std::uint32_t childCount = children(seed, depth);
        switch (mode(seed)) {
            case Mode::NestedWait: {
                // Nested waits stack on a thread that helps while it waits; a small budget per
                // tree keeps the stack within the helping depth (ADR-0025 rule 12).
                if (nesting < kMaxNesting && nestedWaitBudget.fetch_sub(1) > 0) {
                    JobCounter local;
                    submitChildren(seed, depth, nesting + 1, &local, childCount);
                    jobs->wait(local);
                } else {
                    submitChildren(seed, depth, nesting, counter, childCount);
                }
                break;
            }
            case Mode::SameCounter:
                submitChildren(seed, depth, nesting, counter, childCount);
                break;
            case Mode::Continuation: {
                JobCounter& stage = stageCounters[nextStage.fetch_add(1)];
                submitChildren(seed, depth, nesting, &stage, childCount);
                jobs->submitAfter(
                    stage, JobLane::Foreground, "Continuation", [this] { visited.fetch_add(1); }, counter);
                break;
            }
        }
    }

    void submitChildren(std::uint32_t seed, std::uint32_t depth, std::uint32_t nesting, JobCounter* counter,
                        std::uint32_t childCount) {
        for (std::uint32_t index = 0; index < childCount; ++index) {
            const std::uint32_t child = childSeed(seed, index);
            jobs->submit(
                JobLane::Foreground, "Node",
                [this, child, depth, nesting, counter] { node(child, depth + 1, nesting, counter); }, counter);
        }
    }

    // Runs the tree from `seed` and returns how many jobs ran and how many were expected.
    static std::pair<std::uint64_t, std::uint64_t> run(JobSystem& jobs, std::uint32_t seed) {
        std::uint64_t expected = 0;
        std::size_t stages = 0;
        count(seed, 0, expected, stages);
        // Parked continuations hold foreground records; the fixed seeds keep them well inside the
        // 4,096 records, so a full pool always has queued jobs to run.
        CHECK(stages < 2048);
        Tree tree;
        tree.jobs = &jobs;
        tree.stageCounters = std::make_unique<JobCounter[]>(stages);
        JobCounter root;
        jobs.submit(JobLane::Foreground, "Root", [&tree, seed, &root] { tree.node(seed, 0, 0, &root); }, &root);
        jobs.wait(root);
        return {tree.visited.load(), expected};
    }
};

} // namespace

TEST_CASE("Job stress: random trees with nested waits and continuations") {
    constexpr int kRounds = 150 * kScale;
    for (const JobSystemConfig config : {JobSystemConfig{.foregroundWorkers = 0, .backgroundWorkers = 0},
                                         JobSystemConfig{.foregroundWorkers = 1, .backgroundWorkers = 1},
                                         JobSystemConfig{.foregroundWorkers = 3, .backgroundWorkers = 2},
                                         JobSystemConfig{.foregroundWorkers = 8, .backgroundWorkers = 2}}) {
        CAPTURE(config.foregroundWorkers);
        JobSystem jobs(config);
        std::uint64_t ran = 0;
        std::uint64_t expected = 0;
        std::atomic<std::uint64_t> backgroundRan{0};
        std::atomic<std::uint64_t> backgroundExpected{0};
        JobCounter background;
        for (int round = 0; round < kRounds; ++round) {
            const auto seed = static_cast<std::uint32_t>(round);
            // A background job builds another tree at the same time and waits for it.
            jobs.submit(
                JobLane::Background, "Driver",
                [&jobs, seed, &backgroundRan, &backgroundExpected] {
                    const auto [treeRan, treeExpected] = Tree::run(jobs, seed ^ 0x5bd1e995u);
                    backgroundRan.fetch_add(treeRan);
                    backgroundExpected.fetch_add(treeExpected);
                },
                &background);
            const auto [treeRan, treeExpected] = Tree::run(jobs, seed);
            ran += treeRan;
            expected += treeExpected;
            jobs.wait(background);
        }
        CHECK(ran == expected);
        CHECK(backgroundRan.load() == backgroundExpected.load());
    }
}

TEST_CASE("Job stress: a tiny foreground pool shared by several submitting threads") {
    JobSystemConfig config{.foregroundWorkers = 3, .backgroundWorkers = 1};
    config.foregroundJobCapacity = 8;
    JobSystem jobs(config);
    constexpr int kPerThread = 5000 * kScale;
    std::atomic<std::int64_t> sum{0};
    auto submitMany = [&jobs, &sum] {
        JobCounter counter;
        for (int index = 0; index < kPerThread; ++index) {
            jobs.submit(
                JobLane::Foreground, "Add",
                [&jobs, &sum, &counter, index] {
                    sum.fetch_add(index);
                    if ((index % 16) == 0) {
                        jobs.submit(JobLane::Foreground, "Sub", [&sum] { sum.fetch_add(1); }, &counter);
                    }
                },
                &counter);
        }
        jobs.wait(counter);
    };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 3; ++thread) {
        threads.emplace_back(submitMany);
    }
    submitMany();
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::int64_t perThreadSum =
        static_cast<std::int64_t>(kPerThread) * (kPerThread - 1) / 2 + (kPerThread + 15) / 16;
    CHECK(sum.load() == 4 * perThreadSum);
}

TEST_CASE("Job stress: counters live only as long as their jobs") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 4, .backgroundWorkers = 2});
    constexpr int kRounds = 2000 * kScale;
    std::atomic<int> ran{0};
    auto churn = [&jobs, &ran](JobLane lane) {
        for (int round = 0; round < kRounds; ++round) {
            JobCounter counter;
            const int jobCount = 1 + (round % 3);
            for (int index = 0; index < jobCount; ++index) {
                jobs.submit(lane, "Churn", [&ran] { ran.fetch_add(1); }, &counter);
            }
            JobCounter after;
            jobs.submitAfter(counter, lane, "After", [&ran] { ran.fetch_add(1); }, &after);
            if ((round % 2) == 0) {
                jobs.wait(after);
                jobs.wait(counter);
            } else {
                while (!after.isDone() || !counter.isDone()) {
                    std::this_thread::yield();
                }
            }
        }
    };
    std::thread external(churn, JobLane::Foreground);
    std::thread backgroundUser(churn, JobLane::Background);
    churn(JobLane::Foreground);
    external.join();
    backgroundUser.join();
    int expected = 0;
    for (int round = 0; round < kRounds; ++round) {
        expected += 2 + (round % 3);
    }
    CHECK(ran.load() == 3 * expected);
}

TEST_CASE("Job stress: nested parallelFor from several threads at once") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 4, .backgroundWorkers = 1});
    constexpr int kRounds = 30 * kScale;
    std::atomic<std::int64_t> total{0};
    auto loops = [&jobs, &total] {
        for (int round = 0; round < kRounds; ++round) {
            jobs.parallelFor("Outer", 64, 4, [&jobs, &total](std::size_t begin, std::size_t end) {
                for (std::size_t index = begin; index < end; ++index) {
                    jobs.parallelFor("Inner", 256, 16, [&total](std::size_t innerBegin, std::size_t innerEnd) {
                        total.fetch_add(static_cast<std::int64_t>(innerEnd - innerBegin));
                    });
                }
            });
        }
    };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 2; ++thread) {
        threads.emplace_back(loops);
    }
    loops();
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(total.load() == 3LL * kRounds * 64 * 256);
}

TEST_CASE("Job stress: cancelling long background work at random moments") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 2, .backgroundWorkers = 3});
    constexpr int kRounds = 50 * kScale;
    for (int round = 0; round < kRounds; ++round) {
        ith::CancellationToken token;
        std::atomic<int> steps{0};
        JobCounter counter;
        for (int index = 0; index < 6; ++index) {
            jobs.submit(
                JobLane::Background, "Long",
                [&token, &steps] {
                    for (int step = 0; step < 100'000 && !token.isCancelled(); ++step) {
                        steps.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                &counter);
        }
        for (int spin = 0; spin < round % 50; ++spin) {
            std::this_thread::yield();
        }
        token.cancel();
        jobs.wait(counter);
        CHECK(steps.load() <= 600'000);
    }
}

TEST_CASE("Job stress: shutdown with work in flight on every lane") {
    constexpr int kRounds = 20 * kScale;
    for (int round = 0; round < kRounds; ++round) {
        std::atomic<int> ran{0};
        JobCounter gate;
        {
            JobSystem jobs(JobSystemConfig{.foregroundWorkers = 1 + static_cast<std::uint32_t>(round % 4),
                                           .backgroundWorkers = static_cast<std::uint32_t>(round % 3)});
            for (int index = 0; index < 100; ++index) {
                jobs.submit(
                    JobLane::Foreground, "Spawn",
                    [&jobs, &ran] {
                        ran.fetch_add(1);
                        jobs.submit(JobLane::Background, "Child", [&ran] { ran.fetch_add(1); });
                        jobs.postToMainThread("Back", [&ran] { ran.fetch_add(1); });
                    },
                    &gate);
            }
            jobs.submitAfter(gate, JobLane::Background, "After", [&ran] { ran.fetch_add(1); });
        }
        CHECK(ran.load() == 301);
    }
}
