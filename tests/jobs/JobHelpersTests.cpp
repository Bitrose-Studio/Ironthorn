// The job system's helpers: cancellation, progress and the first error of a group of jobs
// (ADR-0025 rules 14-16).

#include "ith/foundation/Expected.h"
#include "ith/jobs/CancellationToken.h"
#include "ith/jobs/FirstError.h"
#include "ith/jobs/JobSystem.h"
#include "ith/jobs/Progress.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace {

using ith::JobCounter;
using ith::JobLane;
using ith::JobSystem;
using ith::JobSystemConfig;

ith::Expected<int> parse(std::size_t index) {
    if (index % 7 == 3) {
        return ith::makeError("test.bad_item", "item " + std::to_string(index));
    }
    return static_cast<int>(index);
}

} // namespace

TEST_CASE("CancellationToken: cancel, check and reset") {
    ith::CancellationToken token;
    CHECK_FALSE(token.isCancelled());
    token.cancel();
    CHECK(token.isCancelled());
    token.reset();
    CHECK_FALSE(token.isCancelled());
}

TEST_CASE("CancellationToken: queued jobs still run and return early") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 0, .backgroundWorkers = 0});
    ith::CancellationToken token;
    std::atomic<int> started{0};
    std::atomic<int> worked{0};
    JobCounter counter;
    for (int index = 0; index < 10; ++index) {
        jobs.submit(
            JobLane::Background, "Cancellable",
            [&token, &started, &worked, index] {
                started.fetch_add(1);
                if (index == 3) {
                    token.cancel();
                }
                if (token.isCancelled()) {
                    return;
                }
                worked.fetch_add(1);
            },
            &counter);
    }
    jobs.wait(counter);
    CHECK(started.load() == 10);
    CHECK(worked.load() == 3); // Jobs run in order without workers: 0, 1 and 2 worked.
}

TEST_CASE("CancellationToken: workers see a cancel from the main thread") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 2, .backgroundWorkers = 2});
    ith::CancellationToken token;
    JobCounter counter;
    for (int index = 0; index < 4; ++index) {
        jobs.submit(
            JobLane::Background, "Spin",
            [&token] {
                while (!token.isCancelled()) {
                    std::this_thread::yield();
                }
            },
            &counter);
    }
    token.cancel();
    jobs.wait(counter);
    CHECK(counter.isDone());
}

TEST_CASE("Progress: counts work and completed items") {
    ith::Progress progress;
    CHECK(progress.snapshot().total == 0);
    CHECK(progress.snapshot().fraction() == 0.0f);
    progress.addWork(4);
    progress.complete();
    progress.complete(2);
    const ith::Progress::Snapshot snapshot = progress.snapshot();
    CHECK(snapshot.done == 3);
    CHECK(snapshot.total == 4);
    CHECK(snapshot.fraction() == doctest::Approx(0.75f));
    progress.reset();
    CHECK(progress.snapshot().done == 0);
    CHECK(progress.snapshot().total == 0);
}

TEST_CASE("Progress: jobs update it from several threads") {
    JobSystem jobs(JobSystemConfig{.foregroundWorkers = 4, .backgroundWorkers = 0});
    ith::Progress progress;
    progress.addWork(1000);
    jobs.parallelFor("Work", 1000, 10, [&progress](std::size_t begin, std::size_t end) {
        progress.complete(static_cast<std::uint32_t>(end - begin));
    });
    CHECK(progress.snapshot().done == 1000);
    CHECK(progress.snapshot().fraction() == 1.0f);
}

TEST_CASE("FirstError: success when nothing failed") {
    ith::FirstError<> errors;
    CHECK_FALSE(errors.failed());
    CHECK(errors.check(0, parse(0)));
    CHECK(errors.result().hasValue());
}

TEST_CASE("FirstError: keeps the error of the lowest order") {
    ith::FirstError<> errors;
    errors.record(5, ith::Error{.code = "test.five", .message = "five"});
    errors.record(2, ith::Error{.code = "test.two", .message = "two"});
    errors.record(9, ith::Error{.code = "test.nine", .message = "nine"});
    errors.record(2, ith::Error{.code = "test.two_again", .message = "two again"});
    CHECK(errors.failed());
    const ith::Expected<void> result = errors.result();
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == "test.two");
}

TEST_CASE("FirstError: the kept error is the same for any worker count") {
    for (const std::uint32_t workers : {0u, 1u, 4u}) {
        CAPTURE(workers);
        JobSystem jobs(JobSystemConfig{.foregroundWorkers = workers, .backgroundWorkers = 0});
        ith::FirstError<> errors;
        std::atomic<int> parsed{0};
        jobs.parallelFor("Parse", 100, 3, [&errors, &parsed](std::size_t begin, std::size_t end) {
            for (std::size_t index = begin; index < end; ++index) {
                if (errors.check(index, parse(index))) {
                    parsed.fetch_add(1);
                }
            }
        });
        const ith::Expected<void> result = errors.result();
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().message == "item 3");
        CHECK(parsed.load() == 100 - 14);
    }
}
