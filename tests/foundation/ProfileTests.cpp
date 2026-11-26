// The profiling macros (ADR-0007 rules 10 and 11). Every build compiles each macro the way engine
// code uses it; builds without profiling also check that the macros compile to nothing, and
// profiling builds that they run without a viewer connected.

#include "ith/foundation/Profile.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

#define ITH_TEST_STRINGIFY_EXPANDED(...) ITH_TEST_STRINGIFY(__VA_ARGS__)
#define ITH_TEST_STRINGIFY(...) #__VA_ARGS__

int g_evaluations = 0;

// Called only from macro arguments, which builds without profiling never evaluate.
[[maybe_unused]] int evaluate() {
    return ++g_evaluations;
}

// Every macro as engine code writes it, zones nested in inner blocks.
int profiledWork([[maybe_unused]] const char* dynamicName) {
    ITH_PROFILE_FUNCTION();
    int sum = 0;
    {
        ITH_PROFILE_SCOPE("Profile test: outer");
        for (int i = 0; i < 3; ++i) {
            ITH_PROFILE_SCOPE("Profile test: inner");
            {
                ITH_PROFILE_SCOPE_DYNAMIC(dynamicName);
                sum += i;
                {
                    ITH_PROFILE_SCOPE_DYNAMIC(dynamicName);
                    ++sum;
                }
            }
        }
    }
    [[maybe_unused]] const int count = 7;
    [[maybe_unused]] const std::uint64_t bytes = 1ULL << 40U;
    [[maybe_unused]] const std::size_t size = 3;
    [[maybe_unused]] const float ratio = 0.5F;
    [[maybe_unused]] const double seconds = 0.016;
    ITH_PROFILE_PLOT("Profile test: int", count);
    ITH_PROFILE_PLOT("Profile test: uint64", bytes);
    ITH_PROFILE_PLOT("Profile test: size", size);
    ITH_PROFILE_PLOT("Profile test: float", ratio);
    ITH_PROFILE_PLOT("Profile test: double", seconds);
    ITH_PROFILE_FRAME();
    return sum;
}

} // namespace

TEST_SUITE("foundation.profile") {
    TEST_CASE("Profile: every macro compiles and runs, with or without a viewer") {
        CHECK(profiledWork("Profile test: dynamic") == 6);

        // A thread named from a run-time string, as the job system names its workers.
        std::thread thread([] {
            const std::string name = std::string("Profile test ") + std::to_string(1);
            ITH_PROFILE_THREAD_NAME(name.c_str());
            ITH_PROFILE_SCOPE("Profile test: on a named thread");
        });
        thread.join();
    }

#ifdef ITH_ENABLE_PROFILING
    TEST_CASE("Profile: a plot evaluates its value once") {
        g_evaluations = 0;
        ITH_PROFILE_PLOT("Profile test: evaluated", evaluate());
        CHECK(g_evaluations == 1);
    }

    TEST_CASE("Profile: plot values convert to Tracy's types") {
        static_assert(std::is_same_v<decltype(ith::detail::profilePlotValue(1)), std::int64_t>);
        static_assert(std::is_same_v<decltype(ith::detail::profilePlotValue(1U)), std::int64_t>);
        static_assert(std::is_same_v<decltype(ith::detail::profilePlotValue(std::size_t{1})), std::int64_t>);
        static_assert(std::is_same_v<decltype(ith::detail::profilePlotValue(1.0F)), double>);
        static_assert(std::is_same_v<decltype(ith::detail::profilePlotValue(1.0)), double>);
        CHECK(ith::detail::profilePlotValue(-3) == -3);
        CHECK(ith::detail::profilePlotValue(0.25F) == 0.25);
    }
#else
    TEST_CASE("Profile: without profiling every macro expands to nothing") {
        constexpr std::string_view nothing = "static_cast<void>(0)";
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_SCOPE("a"))) == nothing);
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_SCOPE_DYNAMIC(name))) == nothing);
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_FUNCTION())) == nothing);
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_FRAME())) == nothing);
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_PLOT("a", 1))) == nothing);
        CHECK(std::string_view(ITH_TEST_STRINGIFY_EXPANDED(ITH_PROFILE_THREAD_NAME("a"))) == nothing);
    }

    TEST_CASE("Profile: without profiling the arguments are not evaluated") {
        g_evaluations = 0;
        ITH_PROFILE_PLOT("Profile test: evaluated", evaluate());
        ITH_PROFILE_SCOPE_DYNAMIC(evaluate() == 0 ? "a" : "b");
        CHECK(g_evaluations == 0);
    }
#endif
}
