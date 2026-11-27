#pragma once

// Process-wide state of the platform layer, shared by Platform.cpp and Window.cpp. Not part of
// the module's API.
//
// The windowing library itself is process-wide, so this only tracks what the asserts need: which
// thread is the main thread and how many windows are alive. Both are atomic because the asserts
// may read them from any thread.

#include "ith/foundation/Expected.h"
#include "ith/platform_events/Event.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace ith {
struct OpenFileDialogDesc;
} // namespace ith

namespace ith::detail {

struct PlatformState {
    // The thread that created the live Platform; a default id when none exists. Platform::create
    // claims it with a compare-exchange, so two Platforms can't both start.
    std::atomic<std::thread::id> mainThread;
    // Windows alive. Changed only on the main thread; ~Platform resets it, because shutting the
    // windowing library down closes every window.
    std::atomic<std::int32_t> windowCount = 0;
    // The library event type Platform::wake() pushes, registered by Platform::create; 0 when no
    // Platform exists.
    std::atomic<std::uint32_t> wakeEventType = 0;
    // True from a wake() that pushed the event until a pump takes it, so a burst of wakes pushes
    // one event.
    std::atomic<bool> wakePending = false;
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

// The start of an open-file dialog, the part of Platform::showOpenFileDialog before the library
// shows it: fails with "platform.dialog_busy" while a dialog is open, otherwise marks one open for
// `window` and keeps copies of `desc`'s strings for the library. Only showOpenFileDialog and tests
// may call it; tests use it to stand in for showing the operating system's dialog, which can't be
// done headless.
//
// Precondition (asserted): called on the main thread.
Expected<void> beginOpenFileDialog(WindowId window, const OpenFileDialogDesc& desc);

// `folder` as the default location of a file dialog: unchanged when empty, otherwise with a
// trailing separator added when it has none (on Windows after turning every '/' into '\'), because
// the library takes a location without one for a file in its parent folder.
std::string dialogFolder(std::string_view folder);

// The end of an open-file dialog, which the library's callback forwards to, on any thread: `files`
// is the library's list of paths ending in a null pointer (just the null pointer, or one empty
// name, when the user cancelled), or null on an error, whose message is then the calling thread's
// last library error. Copies the result and wakes the main thread; the next pump delivers it as
// FileDialogFile and FileDialogClosed events. Does nothing when no dialog is open, and drops the
// result of a dialog whose Platform was destroyed. Only the library's callback and tests may call
// it.
void finishOpenFileDialog(const char* const* files);

} // namespace ith::detail
