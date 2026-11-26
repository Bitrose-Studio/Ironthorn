#pragma once

// CPU profiling macros (ADR-0007 rules 10 to 13). Engine and game code profile only through
// these; the backend is Tracy, built in only when the CMake option IRONTHORN_PROFILING is on,
// which defines ITH_ENABLE_PROFILING (docs/development/logging.md, "Profiling").
//
//   void World::update() {
//       ITH_PROFILE_FUNCTION();
//       {
//           ITH_PROFILE_SCOPE("Propagate transforms");
//           ...
//       }
//       ITH_PROFILE_PLOT("Entities", entityCount);
//   }
//
//   // At the end of each frame of the main loop:
//   ITH_PROFILE_FRAME();
//
// Without profiling, every macro expands to static_cast<void>(0): its arguments are not
// evaluated, nothing is compiled in and Tracy is not included. Write each call as a statement
// with a trailing semicolon, so that the call sites compile the same either way. A variable used
// only as a macro argument is unused without profiling; mark it [[maybe_unused]].
//
// With profiling, this header includes Tracy's header: Tracy's zones keep their source location
// in static data at the call site. That is the one third-party include in a public engine header
// that ADR-0007 rule 11 allows; no Tracy type appears in an engine function signature. A
// translation unit that includes Tracy's header compiles about 0.7 to 2 seconds longer, so keep
// this header out of widely included headers and include it in the .cpp files that profile.
//
// Rules for the zone macros (ITH_PROFILE_SCOPE, ITH_PROFILE_SCOPE_DYNAMIC, ITH_PROFILE_FUNCTION):
// - At most one of them per block: each declares a local variable that lives to the end of the
//   block. Open an inner block for a second zone; zones nest.
// - Not in constexpr functions: an enabled zone declares static data.
// - A zone ends when its block ends, on the thread that began it, so zones on one thread always
//   nest (the job system runs nested jobs inside waits, never on another stack).

#ifdef ITH_ENABLE_PROFILING

#include <tracy/Tracy.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace ith::detail {

// The value types Tracy's plots take: an integer converts to std::int64_t (an unsigned value above
// INT64_MAX wraps), a float or double to double (long double is refused). Tracy's overloads make
// an int ambiguous.
template <typename T>
constexpr auto profilePlotValue(T value) noexcept {
    static_assert(std::is_arithmetic_v<T>, "ITH_PROFILE_PLOT takes an integer or floating-point value");
    // A long double outside double's range would make the conversion undefined.
    static_assert(!std::is_same_v<T, long double>, "ITH_PROFILE_PLOT takes float or double, not long double");
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<double>(value);
    } else {
        return static_cast<std::int64_t>(value);
    }
}

} // namespace ith::detail

// A zone named `name`, a string literal, from here to the end of the enclosing block.
#define ITH_PROFILE_SCOPE(name) ZoneScopedN(name)

// A zone whose name is a NUL-terminated string known only at run time, e.g. a job's name, from
// here to the end of the enclosing block. `name` must stay valid until the zone begins. Costs a
// strlen per call, and while a viewer is connected an allocation and copy of the name, so prefer
// ITH_PROFILE_SCOPE where the name is a literal. Added under ADR-0025 rule 18 for job zones.
#define ITH_PROFILE_SCOPE_DYNAMIC(name) SuppressVarShadowWarning(ZoneTransientN(ithProfileDynamicZone, (name), true))

// A zone named after the enclosing function, from here to the end of the enclosing block.
#define ITH_PROFILE_FUNCTION() ZoneScoped

// Marks the end of a frame on the frame timeline. Call once per frame, on the thread that runs
// the main loop.
#define ITH_PROFILE_FRAME() FrameMark

// Adds `value` (an integer or floating-point value) to the plot named `name`, a string literal.
// A plot holds one kind of value: always plot integers or always floating-point values under one
// name.
#define ITH_PROFILE_PLOT(name, value) ::tracy::Profiler::PlotData((name), ::ith::detail::profilePlotValue(value))

// Names the calling thread in the profiler (`name`, a NUL-terminated string, is copied). Call
// once, early on the thread itself; it also sets the thread's OS name.
#define ITH_PROFILE_THREAD_NAME(name) ::tracy::SetThreadName(name)

#else

#define ITH_PROFILE_SCOPE(name) static_cast<void>(0)
#define ITH_PROFILE_SCOPE_DYNAMIC(name) static_cast<void>(0)
#define ITH_PROFILE_FUNCTION() static_cast<void>(0)
#define ITH_PROFILE_FRAME() static_cast<void>(0)
#define ITH_PROFILE_PLOT(name, value) static_cast<void>(0)
#define ITH_PROFILE_THREAD_NAME(name) static_cast<void>(0)

#endif
