#include "ith/foundation/Stopwatch.h"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

TEST_SUITE("foundation.stopwatch") {
    TEST_CASE("Stopwatch measures elapsed time and restarts") {
        const ith::Stopwatch stopwatch;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double first = stopwatch.elapsedSeconds();
        CHECK(first >= 0.015);
        CHECK(stopwatch.elapsedMilliseconds() >= first * 1000.0);
        CHECK(stopwatch.elapsed() >= std::chrono::milliseconds(15));

        ith::Stopwatch restarted = stopwatch;
        restarted.restart();
        CHECK(restarted.elapsedSeconds() < first);
    }

    TEST_CASE("Stopwatch never reports negative time") {
        const ith::Stopwatch stopwatch;
        for (int sample = 0; sample < 1000; ++sample) {
            CHECK(stopwatch.elapsed().count() >= 0);
        }
    }
}
