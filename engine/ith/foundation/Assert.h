#pragma once

// Assertions (ADR-0001).
//
//   ITH_ASSERT(condition)               checked in Debug and development builds, compiled out
//   ITH_ASSERT_MSG(condition, message)  in shipping builds (the condition is not evaluated)
//   ITH_VERIFY(condition)               always evaluated; checked like ITH_ASSERT
//   ITH_FATAL(message)                  always fails, in every build, and never returns
//
// Asserts state programmer errors. Expected failures (a missing file, bad user data) are
// reported with ith::Expected instead. A failing assert calls the installed handler, which
// decides whether to break, abort or continue (tests continue and record the failure).

#include <cstdint>

namespace ith {

struct AssertInfo {
    const char* expression = nullptr; // The failed condition, or nullptr for ITH_FATAL.
    const char* message = nullptr;    // Optional explanation; may be nullptr.
    const char* file = nullptr;
    int line = 0;
};

enum class AssertAction : std::uint8_t {
    Continue,   // Resume after the assert (only honoured for ITH_ASSERT and ITH_VERIFY).
    DebugBreak, // Stop in the debugger, then continue if resumed.
    Abort,      // Terminate the process.
};

using AssertHandler = AssertAction (*)(const AssertInfo& info);

// Installs a handler for failed asserts and returns the previous one. Passing nullptr
// restores the default handler. It logs the failure as a Fatal record in the Assert category
// (Log.h), which every log sink and stderr receive, and aborts.
// Thread-safe; the handler itself may be called from any thread.
AssertHandler setAssertHandler(AssertHandler handler);

namespace detail {

// Reports a failed assert. Returns only when the handler chose to continue.
void reportAssertFailure(const char* expression, const char* message, const char* file, int line);

// Reports a fatal error. Never returns, whatever the handler chooses.
[[noreturn]] void reportFatal(const char* message, const char* file, int line);

} // namespace detail
} // namespace ith

#define ITH_FATAL(message) ::ith::detail::reportFatal((message), __FILE__, __LINE__)

#ifdef ITH_ENABLE_ASSERTS
#define ITH_ASSERT_MSG(condition, message)                                                                             \
    do {                                                                                                               \
        if (!(condition)) [[unlikely]] {                                                                               \
            ::ith::detail::reportAssertFailure(#condition, (message), __FILE__, __LINE__);                             \
        }                                                                                                              \
    } while (false)
#define ITH_VERIFY(condition) ITH_ASSERT_MSG(condition, nullptr)
#else
#define ITH_ASSERT_MSG(condition, message)                                                                             \
    do {                                                                                                               \
    } while (false)
#define ITH_VERIFY(condition) static_cast<void>(condition)
#endif

#define ITH_ASSERT(condition) ITH_ASSERT_MSG(condition, nullptr)
