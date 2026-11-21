// Job system benchmarks (ADR-0025 rule 19). Run on a Release build, by hand or as the CTest
// `jobs_benchmark` (label "benchmark", excluded from normal test runs; CI runs it in the
// linux-clang-release job). It only prints numbers and always exits with 0.
//
//   cmake --build build/windows --config Release --target ith_jobs_benchmark
//   build/windows/bin/Release/ith_jobs_benchmark
//
// Each measurement runs 7 times after a warm-up run and the median is printed. The ADR's
// initial targets: under 200 ns per empty job at 8 workers, and at least 5x speed-up of a
// 1M-item parallelFor on 8 cores. Record results together with the machine they ran on.

#include "ith/foundation/Stopwatch.h"
#include "ith/jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace {

using ith::JobCounter;
using ith::JobLane;
using ith::JobSystem;
using ith::JobSystemConfig;

constexpr int kRuns = 7;

// Results are summed into this, so the optimizer can't drop the work.
std::atomic<double> g_sink{0.0};

double median(const std::function<double()>& measurement) {
    std::vector<double> samples;
    for (int run = 0; run <= kRuns; ++run) {
        const double value = measurement();
        if (run > 0) {
            samples.push_back(value);
        }
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

JobSystem makeJobs(std::uint32_t foregroundWorkers) {
    return JobSystem(JobSystemConfig{.foregroundWorkers = foregroundWorkers, .backgroundWorkers = 1});
}

// Empty jobs submitted by the main thread in frame-sized batches of 1,024, each batch waited
// for: the cost per job of submitting, scheduling, running and counting.
double emptyJobNanoseconds(JobSystem& jobs) {
    constexpr int kBatches = 200;
    constexpr int kBatch = 1024;
    const ith::Stopwatch stopwatch;
    for (int batch = 0; batch < kBatches; ++batch) {
        JobCounter counter;
        for (int index = 0; index < kBatch; ++index) {
            jobs.submit(JobLane::Foreground, "Empty", [] {}, &counter);
        }
        jobs.wait(counter);
    }
    return stopwatch.elapsedMilliseconds() * 1e6 / (kBatches * kBatch);
}

// A parallelFor over 1M items with a little arithmetic per item, in milliseconds.
double parallelForMilliseconds(JobSystem& jobs, std::vector<float>& values) {
    const ith::Stopwatch stopwatch;
    jobs.parallelFor("Transform", values.size(), 4096, [&values](std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const float value = values[index];
            values[index] = std::sqrt(value * value + 1.0f) * 0.5f + std::sin(value) * 0.25f;
        }
    });
    const double elapsed = stopwatch.elapsedMilliseconds();
    g_sink.store(g_sink.load() + values[values.size() / 2]);
    return elapsed;
}

// A chain of continuations, each queued when the previous job finishes, in microseconds per link.
double continuationMicroseconds(JobSystem& jobs) {
    constexpr int kLinks = 2000;
    const auto counters = std::make_unique<JobCounter[]>(kLinks);
    const ith::Stopwatch stopwatch;
    jobs.submit(JobLane::Foreground, "Link", [] {}, &counters[0]);
    for (int link = 1; link < kLinks; ++link) {
        jobs.submitAfter(counters[link - 1], JobLane::Foreground, "Link", [] {}, &counters[link]);
    }
    for (int link = 0; link < kLinks; ++link) {
        jobs.wait(counters[link]);
    }
    return stopwatch.elapsedMilliseconds() * 1e3 / kLinks;
}

// One job submitted to sleeping workers by a thread that only waits (it can't help: it is not
// the main thread), in microseconds from submit to the end of the wait.
double wakeUpMicroseconds(JobSystem& jobs) {
    constexpr int kRounds = 50;
    double total = 0.0;
    std::thread outsider([&jobs, &total] {
        for (int round = 0; round < kRounds; ++round) {
            std::this_thread::sleep_for(std::chrono::microseconds(200)); // Let the workers fall asleep.
            JobCounter counter;
            const ith::Stopwatch stopwatch;
            jobs.submit(JobLane::Foreground, "Wake", [] {}, &counter);
            jobs.wait(counter);
            total += stopwatch.elapsedMilliseconds();
        }
    });
    outsider.join();
    return total * 1e3 / kRounds;
}

} // namespace

int main() {
    const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    std::printf("Job system benchmark: %u hardware threads, median of %d runs\n\n", hardwareThreads, kRuns);

    std::printf("%-46s %12s\n", "Empty job, main thread submits and waits", "ns per job");
    for (const std::uint32_t workers : {1u, 4u, 8u}) {
        JobSystem jobs = makeJobs(workers);
        const double nanoseconds = median([&jobs] { return emptyJobNanoseconds(jobs); });
        std::printf("  %2u foreground workers %33.1f%s\n", workers, nanoseconds,
                    workers == 8 ? (nanoseconds <= 200.0 ? "   within target (200)" : "   ABOVE TARGET (200)") : "");
    }

    std::printf("\n%-46s %12s %10s\n", "parallelFor, 1M items, batches of 4,096", "ms", "speed-up");
    std::vector<float> values(1'000'000);
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = static_cast<float>(index % 1000) * 0.001f;
    }
    double serial = 0.0;
    std::vector<std::uint32_t> workerCounts{0, 1, 2, 4, 7, 8};
    if (hardwareThreads - 1 > 8) {
        workerCounts.push_back(hardwareThreads - 1);
    }
    for (const std::uint32_t workers : workerCounts) {
        JobSystem jobs = makeJobs(workers);
        const double milliseconds = median([&] { return parallelForMilliseconds(jobs, values); });
        if (workers == 0) {
            serial = milliseconds;
        }
        const double speedUp = serial / milliseconds;
        std::printf("  %2u foreground workers (+ main thread) %16.3f %9.2fx%s\n", workers, milliseconds, speedUp,
                    workers == 7
                        ? (speedUp >= 5.0 ? "   within target (5x on 8 threads)" : "   ABOVE TARGET (5x on 8 threads)")
                        : "");
    }

    std::printf("\n%-46s %12s\n", "Latency", "us");
    {
        JobSystem jobs = makeJobs(4);
        std::printf("  continuation chain, per link, 4 workers %20.2f\n",
                    median([&jobs] { return continuationMicroseconds(jobs); }));
        std::printf("  wake-up of a sleeping worker, 4 workers %20.2f\n",
                    median([&jobs] { return wakeUpMicroseconds(jobs); }));
    }
    std::printf("\n(sink %.1f)\n", g_sink.load());
    return 0;
}
