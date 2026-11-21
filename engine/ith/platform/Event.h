#pragma once

// The engine events Platform::pumpEvents() delivers once per frame (ADR-0012).
//
// The platform layer translates every event of the windowing library into one of these at the
// start of a frame. Events the engine doesn't use yet are dropped. Keyboard, mouse, text and
// gamepad events come with the input layer (ADR-0013).

#include <cstdint>

namespace ith {

// Identifies a window for the lifetime of the process. Ids are never reused while the process
// runs, so an event for a window that was destroyed in the meantime matches no live window.
using WindowId = std::uint32_t;

// The window id of events that don't belong to a window.
inline constexpr WindowId kNoWindow = 0;

enum class EventType : std::uint8_t {
    // The operating system or the user asked the application to quit, for example with Ctrl+C
    // in a terminal or when the session ends. Closing the last window does not send it: that is
    // a WindowCloseRequested, and the application decides whether to quit.
    QuitRequested,
    // The user asked to close `window` (its close button, Alt+F4). The window stays open until
    // the application destroys it, so the editor can ask to save first.
    WindowCloseRequested,
    // `window` has a new size in window coordinates: `width` x `height`.
    WindowResized,
    // `window` has a new size in pixels, `width` x `height`: the size a swapchain needs.
    WindowPixelSizeChanged,
    // The display scale of `window` changed, for example because it moved to another monitor.
    // Read the new value with Window::displayScale().
    WindowDisplayScaleChanged,
    WindowFocusGained,
    WindowFocusLost,
    WindowMinimized,
    WindowMaximized,
    // `window` left the minimized or maximized state.
    WindowRestored,
    DisplayAdded,
    DisplayRemoved,
    // The operating system is low on memory; caches should be trimmed.
    LowMemory,
    // The application is about to go to the background (mobile platforms): stop rendering. The
    // swapchain is recreated after EnteredForeground. Not reliable yet: the operating system may
    // suspend the application before the next pump; it becomes reliable once the platform layer
    // handles it in an event watch, with Android support.
    EnteringBackground,
    // The application is in the foreground again and may render.
    EnteredForeground,
};

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): `type` has no default on purpose, see below
struct Event {
    // No default member initializer, so no event silently becomes QuitRequested: the platform
    // layer always sets it, and tests build events with designated initializers.
    EventType type;
    // When the event happened, in nanoseconds of std::chrono::steady_clock (its
    // time_since_epoch()), so it can be compared with frame times.
    std::uint64_t timestampNs = 0;
    // The window the event belongs to, or kNoWindow for application and display events.
    WindowId window = kNoWindow;
    // The new size for WindowResized and WindowPixelSizeChanged; 0 for every other type.
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const Event&, const Event&) = default;
};

} // namespace ith
