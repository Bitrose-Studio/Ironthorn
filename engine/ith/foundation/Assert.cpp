#include "ith/foundation/Assert.h"

#include "ith/foundation/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ith {
namespace {

// Stops in an attached debugger. Foundation may not depend on the platform layer, so the
// compiler intrinsics are used directly here.
void breakIntoDebugger() {
#ifdef _MSC_VER
    __debugbreak();
#elif defined(__clang__)
    __builtin_debugtrap();
#else
    __builtin_trap();
#endif
}

// Sends a Fatal record to every log sink (ADR-0007 rule 9). The logger needs no initialization
// and writes Fatal records to stderr even when no sink does, so this works at any time.
AssertAction defaultAssertHandler(const AssertInfo& info) {
    std::array<char, kLogMessageCapacity> message{};
    int size = 0;
    if (info.expression != nullptr) {
        size = std::snprintf(message.data(), message.size(), "assertion failed: %s%s%s", info.expression,
                             info.message != nullptr ? " - " : "", info.message != nullptr ? info.message : "");
    } else {
        size = std::snprintf(message.data(), message.size(), "fatal error: %s",
                             info.message != nullptr ? info.message : "(no message)");
    }
    const std::size_t length = size < 0 ? 0 : std::min(static_cast<std::size_t>(size), message.size() - 1);
    logMessage(LogLevel::Fatal, g_logCategoryAssert, info.file, info.line, std::string_view(message.data(), length));
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
