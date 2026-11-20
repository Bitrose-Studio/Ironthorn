#include "ith/foundation/Assert.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ith {
namespace {

// Stops in an attached debugger. Foundation may not depend on the platform layer, so the
// compiler intrinsics are used directly here.
void breakIntoDebugger() {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__clang__)
    __builtin_debugtrap();
#else
    __builtin_trap();
#endif
}

AssertAction defaultAssertHandler(const AssertInfo& info) {
    if (info.expression != nullptr) {
        std::fprintf(stderr, "%s(%d): assertion failed: %s%s%s\n", info.file, info.line, info.expression,
                     info.message != nullptr ? " - " : "", info.message != nullptr ? info.message : "");
    } else {
        std::fprintf(stderr, "%s(%d): fatal error: %s\n", info.file, info.line,
                     info.message != nullptr ? info.message : "(no message)");
    }
    std::fflush(stderr);
    return AssertAction::Abort;
}

std::atomic<AssertHandler> g_assertHandler{&defaultAssertHandler};

AssertAction callHandler(const AssertInfo& info) {
    return g_assertHandler.load(std::memory_order_acquire)(info);
}

} // namespace

AssertHandler setAssertHandler(AssertHandler handler) {
    if (handler == nullptr) {
        handler = &defaultAssertHandler;
    }
    AssertHandler previous = g_assertHandler.exchange(handler, std::memory_order_acq_rel);
    return previous == &defaultAssertHandler ? nullptr : previous;
}

namespace detail {

void reportAssertFailure(const char* expression, const char* message, const char* file, int line) {
    const AssertInfo info{.expression = expression, .message = message, .file = file, .line = line};
    switch (callHandler(info)) {
        case AssertAction::Continue:
            return;
        case AssertAction::DebugBreak:
            breakIntoDebugger();
            return;
        case AssertAction::Abort:
            break;
    }
    std::abort();
}

void reportFatal(const char* message, const char* file, int line) {
    const AssertInfo info{.expression = nullptr, .message = message, .file = file, .line = line};
    if (callHandler(info) == AssertAction::DebugBreak) {
        breakIntoDebugger();
    }
    std::abort();
}

} // namespace detail
} // namespace ith
