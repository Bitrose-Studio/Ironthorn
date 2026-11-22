#pragma once

// The platform layer: initialization, windows, the per-frame event pump with device input
// (keyboard, text, mouse, gamepads), gamepad output, the clipboard and the cursor (ADR-0012,
// ADR-0013).
//
//   ith::Expected<ith::Platform> platform = ith::Platform::create();
//   if (!platform) {
//       return report(platform.error());
//   }
//   ith::Expected<ith::Window> window = platform->createWindow({.title = "Game"});
//   ...
//   bool running = true;
//   while (running) {
//       for (const ith::Event& event : platform->pumpEvents()) {
//           if (event.type == ith::EventType::WindowCloseRequested) {
//               running = false;
//           }
//       }
//       jobs.runMainThreadJobs();     // results of background work (ADR-0035 rule 22)
//       // simulate, render
//   }
//
// An idle application calls waitEvents(timeout) instead of pumpEvents(); connecting
// Platform::wake() to JobSystem::setMainThreadWake ends the wait when a job posts a result.
//
// SDL is the backend; none of its types appear in this module's headers. An application creates
// one Platform; engine and game code never create one (Play In Editor uses the editor's).
//
// Threading: the thread that creates the Platform is the main thread. Every Platform and Window
// function must be called on it, which is asserted, because the operating systems' window and
// event APIs require it.

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Expected.h"
#include "ith/platform/Window.h"
#include "ith/platform_events/Event.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// The operating system's cursor shapes (ADR-0012 rule 8), for Platform::setCursor().
enum class CursorShape : std::uint8_t {
    Arrow,
    // An I-beam, over editable text.
    TextInput,
    // Moving something in every direction.
    ResizeAll,
    // Resizing along one axis or diagonal: up-down, left-right, up-right to down-left, up-left to
    // down-right.
    ResizeNorthSouth,
    ResizeEastWest,
    ResizeNorthEastSouthWest,
    ResizeNorthWestSouthEast,
    // A pointing hand, over links and buttons that act at once.
    Hand,
    NotAllowed,
    // Busy; Progress is busy while the user may still work.
    Wait,
    Progress,
    Crosshair,
};

// The number of CursorShape values.
inline constexpr std::size_t kCursorShapeCount = 12;

struct PlatformDesc {
    // The video driver to use: "windows", "wayland", "x11", or "offscreen", which needs no
    // display and is what tests and CI use. Empty picks the default: the SDL_VIDEO_DRIVER
    // environment variable when it is set, otherwise the platform's driver (on Linux, Wayland
    // where the compositor supports it, otherwise X11). A non-empty value wins over the
    // environment variable.
    std::string_view videoDriver;
};

// Owns the initialized platform layer. Only one Platform may exist at a time, and every Window
// must be destroyed before it. Move-only; a moved-from Platform owns nothing and may only be
// destroyed or assigned to.
class Platform {
public:
    // Initializes the windowing library (video and gamepads) on the calling thread, which becomes
    // the main thread.
    // Fails with "platform.init_failed" and the library's message when no usable video driver
    // is available (no display, an unknown driver name).
    //
    // Precondition (asserted): no other Platform exists. If asserts continue, or are compiled
    // out, a second call fails with "platform.already_created" and leaves the first untouched.
    static Expected<Platform> create(const PlatformDesc& desc = {});

    Platform(Platform&& other) noexcept;
    Platform& operator=(Platform&& other) noexcept;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    // Shuts the windowing library down, which closes any window still open.
    // Precondition (asserted): every Window was destroyed first.
    ~Platform();

    // Creates a window, shown, high-DPI aware. Fails with "platform.window_failed" and the
    // library's message. On Wayland the window appears only once something is presented to it.
    //
    // Preconditions (asserted): called on the main thread; desc.width and desc.height are
    // positive.
    Expected<Window> createWindow(const WindowDesc& desc);

    // Drains the operating system's event queue and returns this frame's events in the order
    // they happened. Call it once per frame, at its start. The span points into this Platform:
    // it is valid until the next call or until the Platform is destroyed or moved.
    // Gamepad events arrive only while one of the application's windows has focus.
    //
    // Precondition (asserted): called on the main thread.
    std::span<const Event> pumpEvents() ITH_LIFETIMEBOUND;

    // pumpEvents() that first waits until an event arrives, wake() is called or `timeout` passes,
    // for an application that idles (ADR-0029 rule 12): the editor waits instead of rendering
    // frames nobody looks at. Returns at once when events are already queued; events the platform
    // doesn't report (device and display bookkeeping) don't end the wait. The span may be
    // empty (a timeout, or only a wake). A negative timeout counts as zero; timeouts are rounded
    // down to milliseconds and capped at about 24 days.
    //
    // Precondition (asserted): called on the main thread.
    std::span<const Event> waitEvents(std::chrono::milliseconds timeout) ITH_LIFETIMEBOUND;

    // Ends a waitEvents() in progress early, or makes the next one return at once, by queueing an
    // internal event that pumps drop. Thread-safe, never blocks, and does nothing when no Platform
    // exists. Wakes that come before the next pump are coalesced into one event. Connect it to
    // JobSystem::setMainThreadWake so main-thread jobs end the wait (ADR-0035 rule 23); remove the
    // hook, or destroy the JobSystem, before destroying the Platform, so no wake runs while it shuts
    // down.
    static void wake() noexcept;

    // When the last pumpEvents() finished collecting events, in std::chrono::steady_clock
    // nanoseconds: the frame's input time, later than every timestamp of its events. 0 before the
    // first pump.
    std::uint64_t pumpTimeNs() const { return m_pumpTimeNs; }

    // The name of the video driver in use, e.g. "windows", "wayland", "x11" or "offscreen".
    //
    // Precondition (asserted): called on the main thread.
    std::string_view videoDriver() const;

    // The clipboard's text, UTF-8. Empty when the clipboard holds no text, and also when it can't
    // be read: callers treat both as "nothing to paste".
    //
    // Precondition (asserted): called on the main thread.
    std::string clipboardText() const;

    // Puts UTF-8 text on the clipboard. Fails with "platform.clipboard_failed" and the library's
    // message.
    //
    // Preconditions (asserted): called on the main thread; `text` has no NUL character (the
    // clipboard holds C strings, so the text would be cut there).
    Expected<void> setClipboardText(std::string_view text);

    // Sets the mouse cursor's shape over every window of the application. Setting the current
    // shape again does nothing. Fails with "platform.cursor_failed" and the library's message
    // when the video driver has no system cursors (the "offscreen" driver); the cursor then keeps
    // its shape.
    //
    // Precondition (asserted): called on the main thread.
    Expected<void> setCursor(CursorShape shape);

    // The shape of the last setCursor() that succeeded; Arrow before the first.
    [[nodiscard]] CursorShape cursor() const { return m_cursor; }

    // Shows or hides the mouse cursor over the application's windows; relative mouse mode hides
    // it on its own (Window::setRelativeMouseMode). Fails with "platform.cursor_failed" and the
    // library's message.
    //
    // Precondition (asserted): called on the main thread.
    Expected<void> setCursorVisible(bool visible);
    [[nodiscard]] bool cursorVisible() const;

    // The name of a physical key in the user's keyboard layout, for showing bindings: "Z" for
    // Key::W on AZERTY, "Left Shift" for Key::LeftShift. Empty for Key::None and keys the layout
    // doesn't have. English for keys that print nothing; the UI may translate those by Key.
    //
    // Precondition (asserted): called on the main thread.
    std::string keyDisplayName(Key key) const;

    // Gamepad output (ADR-0013 rule 6). `gamepad` is the device id of a GamepadConnected event.
    // Each fails with "platform.gamepad_not_found" when no connected gamepad has that id, and with
    // "platform.gamepad_output_failed" and the library's message when the gamepad can't do it
    // (no motors, no trigger motors, no LED).
    //
    // Preconditions (asserted): called on the main thread; strengths are within 0..1.

    // Rumbles the low-frequency (left, heavy) and high-frequency (right, light) motors for
    // `durationMs`, replacing any rumble in progress; strengths 0 stop it.
    Expected<void> rumbleGamepad(DeviceId gamepad, float lowFrequency, float highFrequency, std::uint32_t durationMs);
    // Rumbles the trigger motors (Xbox One and Series) for `durationMs`, replacing any trigger
    // rumble in progress.
    Expected<void> rumbleGamepadTriggers(DeviceId gamepad, float left, float right, std::uint32_t durationMs);
    // Sets the light bar or LED colour (PlayStation 4 and 5, some others).
    Expected<void> setGamepadLed(DeviceId gamepad, std::uint8_t red, std::uint8_t green, std::uint8_t blue);

private:
    Platform() = default;

    // Asserts that this Platform owns the platform layer and that the caller is on the main thread.
    void assertUsable() const;

    // Collects this frame's events, starting with `first` (an SDL event) when it is not null, and
    // sets `*woke` when one of them was a wake.
    std::span<const Event> collectEvents(const void* first, bool* woke);

    bool m_owner = false;
    // Added to the library's event timestamps to put them on std::chrono::steady_clock.
    std::int64_t m_clockOffsetNs = 0;
    // When the last pump finished, std::chrono::steady_clock nanoseconds.
    std::uint64_t m_pumpTimeNs = 0;
    // The modifiers of the latest key event pumped, for the mouse events after it.
    KeyModifiers m_modifiers = KeyModifiers::None;
    CursorShape m_cursor = CursorShape::Arrow;
    std::vector<Event> m_events;
    // The text of this frame's events, which their `text` views point into.
    std::string m_text;
};

} // namespace ith
