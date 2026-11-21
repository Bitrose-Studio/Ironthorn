#pragma once

// Process-wide state of the platform layer, shared by Platform.cpp and Window.cpp. Not part of
// the module's API.
//
// The windowing library itself is process-wide, so this only tracks what the asserts need: which
// thread is the main thread and how many windows are alive. Both are atomic because the asserts
// may read them from any thread.

#include "ith/foundation/Expected.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace ith::detail {

struct PlatformState {
    // The thread that created the live Platform; a default id when none exists. Platform::create
    // claims it with a compare-exchange, so two Platforms can't both start.
    std::atomic<std::thread::id> mainThread;
    // Windows alive. Changed only on the main thread; ~Platform resets it, because shutting the
    // windowing library down closes every window.
    std::atomic<std::int32_t> windowCount = 0;
};

PlatformState& platformState();

inline constexpr const char* kNotMainThread =
    "platform functions must be called on the thread that created the Platform";

// True when a Platform exists.
inline bool platformExists() {
    return platformState().mainThread.load(std::memory_order_relaxed) != std::thread::id();
}

// True when a Platform exists and the calling thread created it.
inline bool isMainThread() {
    return platformState().mainThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

// An error with `code` whose message is `what`, a colon and the windowing library's last error.
Unexpected<Error> sdlError(const char* code, const char* what);

} // namespace ith::detail
