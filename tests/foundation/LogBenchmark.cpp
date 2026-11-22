// Logger benchmarks (ADR-0007). Run on a Release build, by hand or as the CTest `log_benchmark`
// (label "benchmark", excluded from normal test runs). It only prints numbers and always exits
// with 0.
//
//   cmake --build build/windows --config Release --target ith_log_benchmark
//   build/windows/bin/Release/ith_log_benchmark
//
// Each measurement runs 7 times after a warm-up run and the median is printed. Records go to a
// sink that does nothing or to a LogRingBuffer, never to stderr, so the numbers are the logger's
// own cost. Record results together with the machine they ran on.

#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/foundation/Stopwatch.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

ITH_LOG_CATEGORY(Bench);

namespace {

constexpr int kRuns = 7;
constexpr int kStatements = 200000;

void ignoreRecord(void* /*context*/, const ith::LogRecord& /*record*/) {}

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

// Nanoseconds per statement of `kStatements` statements on each of `threads` threads.
double statementNanoseconds(int threads) {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    const ith::Stopwatch stopwatch;
    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([thread] {
            for (int index = 0; index < kStatements; ++index) {
                ITH_LOG_INFO(Bench, "Frame {} on thread {}: {} entities, {:.2f} ms", index, thread, index * 3, 16.6);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    return stopwatch.elapsedMilliseconds() * 1e6 / (static_cast<double>(kStatements) * threads);
}

} // namespace

int main() {
    const bool hadStderr = ith::removeLogSink(ith::stderrLogSink());

    g_logCategoryBench.setLevel(ith::LogLevel::Warning);
    std::printf("disabled statement (below the runtime level):  %8.2f ns\n",
                median([] { return statementNanoseconds(1); }));

    g_logCategoryBench.setLevel(ith::LogLevel::Info);
    const ith::LogSink ignore{.write = &ignoreRecord};
    static_cast<void>(ith::addLogSink(ignore));
    std::printf("formatted record to an empty sink:             %8.2f ns\n",
                median([] { return statementNanoseconds(1); }));
    ith::removeLogSink(ignore);

    ith::LogRingBuffer buffer(4096);
    static_cast<void>(ith::addLogSink(buffer.sink()));
    std::printf("formatted record to a LogRingBuffer:           %8.2f ns\n",
                median([] { return statementNanoseconds(1); }));
    std::printf("the same, 4 threads logging (per statement):   %8.2f ns\n",
                median([] { return statementNanoseconds(4); }));
    ith::removeLogSink(buffer.sink());

    if (hadStderr) {
        static_cast<void>(ith::addLogSink(ith::stderrLogSink()));
    }
    return 0;
}
