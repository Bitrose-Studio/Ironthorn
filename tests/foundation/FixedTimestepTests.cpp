#include "ith/foundation/FixedTimestep.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

TEST_SUITE("foundation.fixed_timestep") {
    TEST_CASE("one step per step of frame time") {
        ith::FixedTimestep clock(1.0 / 60.0);
        std::uint32_t steps = 0;
        for (int frame = 0; frame < 600; ++frame) {
            steps += clock.advance(1.0 / 60.0);
        }
        CHECK(steps == 600);
        CHECK(clock.alpha() == doctest::Approx(0.0).epsilon(1e-3));
    }

    TEST_CASE("partial frames accumulate") {
        ith::FixedTimestep clock(0.1);
        CHECK(clock.advance(0.04) == 0);
        CHECK(clock.alpha() == doctest::Approx(0.4));
        CHECK(clock.advance(0.04) == 0);
        CHECK(clock.advance(0.04) == 1);
        CHECK(clock.alpha() == doctest::Approx(0.2));
    }

    TEST_CASE("a slow frame runs several steps") {
        ith::FixedTimestep clock(0.01, 8);
        CHECK(clock.advance(0.035) == 3);
        CHECK(clock.alpha() == doctest::Approx(0.5));
    }

    TEST_CASE("a frame at exactly the step limit keeps the remainder") {
        ith::FixedTimestep clock(0.01, 4);
        CHECK(clock.advance(0.045) == 4);
        CHECK(clock.alpha() == doctest::Approx(0.5));
    }

    TEST_CASE("surplus time is dropped instead of spiralling") {
        ith::FixedTimestep clock(0.01, 4);
        CHECK(clock.advance(1.0) == 4);
        CHECK(clock.alpha() == 0.0);
        // The backlog is gone: the next ordinary frame runs one step.
        CHECK(clock.advance(0.01) == 1);
    }

    TEST_CASE("a frame time too large to accumulate is survived") {
        ith::FixedTimestep clock(0.01, 4);
        CHECK(clock.advance(std::numeric_limits<double>::max()) == 4);
        CHECK(clock.advance(std::numeric_limits<double>::max()) == 4);
        CHECK(clock.alpha() == 0.0);
        CHECK(clock.advance(0.01) == 1);
    }

    TEST_CASE("the total step count does not depend on frame pacing") {
        ith::FixedTimestep steady(1.0 / 120.0, 64);
        ith::FixedTimestep jittery(1.0 / 120.0, 64);

        std::uint32_t steadySteps = 0;
        for (int frame = 0; frame < 1000; ++frame) {
            steadySteps += steady.advance(0.01);
        }

        std::uint32_t jitterySteps = 0;
        for (int frame = 0; frame < 500; ++frame) {
            jitterySteps += jittery.advance(0.003);
            jitterySteps += jittery.advance(0.017);
        }

        // Both clocks consumed 10 seconds of frame time.
        CHECK(steadySteps == 1200);
        CHECK(jitterySteps == 1200);
    }

    TEST_CASE("zero, negative and non-finite frame times are ignored") {
        ith::FixedTimestep clock(0.01);
        CHECK(clock.advance(-1.0) == 0);
        CHECK(clock.advance(0.0) == 0);
        CHECK(clock.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
        CHECK(clock.advance(std::numeric_limits<double>::infinity()) == 0);
        CHECK(clock.advance(-std::numeric_limits<double>::infinity()) == 0);
        CHECK(clock.alpha() == 0.0);
    }

    TEST_CASE("alpha stays in [0, 1)") {
        ith::FixedTimestep clock(0.01);
        for (int frame = 0; frame < 1000; ++frame) {
            clock.advance(0.0037);
            CHECK(clock.alpha() >= 0.0);
            CHECK(clock.alpha() < 1.0);
        }
    }

    TEST_CASE("reset discards the accumulated time") {
        ith::FixedTimestep clock(0.1);
        clock.advance(0.05);
        clock.reset();
        CHECK(clock.alpha() == 0.0);
        CHECK(clock.advance(0.05) == 0);
    }

    TEST_CASE("the defaults are 60 steps per second and at most 8 per frame") {
        const ith::FixedTimestep clock;
        CHECK(clock.stepSeconds() == doctest::Approx(1.0 / 60.0));
        CHECK(clock.maxStepsPerFrame() == 8);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an invalid step asserts and falls back to safe values") {
        ith::test::AssertCapture capture;
        const ith::FixedTimestep zeroStep(0.0, 0);
        CHECK(capture.count() == 2);
        CHECK(zeroStep.stepSeconds() == doctest::Approx(1.0 / 60.0));
        CHECK(zeroStep.maxStepsPerFrame() == 1);

        const ith::FixedTimestep nanStep(std::numeric_limits<double>::quiet_NaN());
        CHECK(capture.count() == 3);
        CHECK(nanStep.stepSeconds() == doctest::Approx(1.0 / 60.0));
    }
#endif
}
