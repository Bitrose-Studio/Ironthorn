#pragma once

// Turns variable frame times into a whole number of fixed simulation steps.
//
//   ith::FixedTimestep physicsClock(1.0 / 60.0);
//   const std::uint32_t steps = physicsClock.advance(frameSeconds);
//   for (std::uint32_t step = 0; step < steps; ++step) {
//       physics.step(physicsClock.stepSeconds());
//   }
//   renderInterpolated(physicsClock.alpha());
//
// A FixedTimestep is a value with no shared state. An instance is not synchronized: use it from
// one thread at a time.

#include <cstdint>

namespace ith {

// A constant step keeps physics stable and reproducible however the frame rate fluctuates.
// When a frame is so long that more than maxStepsPerFrame() steps would be due, the surplus is
// dropped: the simulation slows down rather than entering a spiral where every frame takes
// longer than the last.
class FixedTimestep {
public:
    // Preconditions (asserted): `stepSeconds` is finite and positive, and `maxStepsPerFrame` is
    // at least 1. When they are violated, 1/60 s and 1 are used instead.
    explicit FixedTimestep(double stepSeconds = 1.0 / 60.0, std::uint32_t maxStepsPerFrame = 8);

    // Adds the duration of a frame and returns how many fixed steps to simulate now, at most
    // maxStepsPerFrame(). Zero, negative and non-finite durations add nothing and return 0:
    // they come from clocks and debuggers, not from programming errors.
    std::uint32_t advance(double frameSeconds);

    // The fraction of a step accumulated but not simulated yet, in [0, 1). Rendering uses it
    // to interpolate between the previous and the current simulation state.
    [[nodiscard]] double alpha() const;

    // The length of one step in seconds.
    [[nodiscard]] double stepSeconds() const { return m_stepSeconds; }

    // The most steps advance() returns for one frame.
    [[nodiscard]] std::uint32_t maxStepsPerFrame() const { return m_maxStepsPerFrame; }

    // Discards the accumulated time, e.g. after loading a level or resuming from a pause.
    void reset() { m_accumulator = 0.0; }

private:
    double m_stepSeconds;
    std::uint32_t m_maxStepsPerFrame;
    double m_accumulator = 0.0;
};

} // namespace ith
