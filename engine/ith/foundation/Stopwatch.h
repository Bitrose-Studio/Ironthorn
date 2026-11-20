#pragma once

// A monotonic stopwatch for measuring how long something takes.
//
//   ith::Stopwatch stopwatch;
//   importAsset(path);
//   const double milliseconds = stopwatch.elapsedMilliseconds();
//
// A Stopwatch is a value with no shared state. An instance is not synchronized: use it from one
// thread at a time.

#include <chrono>

namespace ith {

// Measures elapsed time with std::chrono::steady_clock, which never goes backwards, so the
// result is never negative and is not affected by changes to the system clock.
class Stopwatch {
public:
    using Clock = std::chrono::steady_clock;

    // Starts measuring now.
    Stopwatch() : m_start(Clock::now()) {}

    // Starts measuring again from now.
    void restart() { m_start = Clock::now(); }

    // Time since construction or the last restart(), at the clock's full resolution.
    [[nodiscard]] Clock::duration elapsed() const { return Clock::now() - m_start; }

    // elapsed() in seconds.
    [[nodiscard]] double elapsedSeconds() const { return std::chrono::duration<double>(elapsed()).count(); }

    // elapsed() in milliseconds.
    [[nodiscard]] double elapsedMilliseconds() const {
        return std::chrono::duration<double, std::milli>(elapsed()).count();
    }

private:
    Clock::time_point m_start;
};

} // namespace ith
