#pragma once

// The engine events Platform::pumpEvents() delivers once per frame (ADR-0012, ADR-0013).
//
// The platform layer translates every event of the windowing library into one of these at the
// start of a frame. Events the engine doesn't use yet are dropped. Window and application events
// are read directly; device events (keyboard, text, mouse, gamepad) are usually read through the
// input module (ith/input), which builds per-frame state from them and routes them to consumers.

#include "ith/platform_events/InputCodes.h"

#include <cstdint>
#include <string_view>

namespace ith {

// Identifies a window for the lifetime of the process. Ids are never reused while the process
// runs, so an event for a window that was destroyed in the meantime matches no live window.
using WindowId = std::uint32_t;

// The window id of events that don't belong to a window.
inline constexpr WindowId kNoWindow = 0;

// New types are added at the end, so recorded values keep their meaning.
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
    // `window` got keyboard focus. At most one window has it.
    WindowFocusGained,
    // `window` lost keyboard focus. The input module releases every held key and button here.
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

    // A key went down in `window` (the window with keyboard focus): `key`, `layoutKey`,
    // `modifiers`, `repeat`. A held key sends more KeyDown events with `repeat` set at the
    // operating system's repeat rate.
    KeyDown,
    // A key went up: `key`, `layoutKey`, `modifiers`.
    KeyUp,
    // Text was committed in `window` while text input is on (Window::startTextInput): `text` is
    // UTF-8, one character or more (an IME commits whole words). Separate from key events: typing
    // reads this, never KeyDown.
    TextInput,
    // The IME's composition changed in `window`: `text` is the uncommitted text (empty when the
    // composition ended), `textCursor` the insertion point and `textSelectionLength` the length
    // of the selected part, both in characters (code points), or -1 when the IME doesn't say.
    TextEditing,
    // A mouse button went down over `window` (the window under the mouse, or the one that has
    // the mouse in relative mode): `mouseButton`, `clicks`, `x`, `y`, `modifiers`.
    MouseButtonDown,
    // A mouse button went up: `mouseButton`, `x`, `y`, `modifiers`.
    MouseButtonUp,
    // The mouse moved: the new position `x`, `y` and the motion `deltaX`, `deltaY`.
    MouseMoved,
    // The mouse wheel turned: `deltaX`, `deltaY` in wheel steps, fractional on smooth wheels and
    // touchpads; positive is right and away from the user (scroll up); `wheelFlipped`. `x`, `y`
    // is the position.
    MouseWheel,
    // A gamepad connected and was opened: `device`, `text` (its name, UTF-8), `gamepadType`,
    // `gamepadGuid`, `steamHandle`. Gamepads present at start-up send it in the first pump.
    GamepadConnected,
    // The gamepad `device` disconnected. Its id is never used again; a reconnected gamepad gets a
    // new one.
    GamepadDisconnected,
    // A button of gamepad `device` went down: `gamepadButton`. Gamepads have no repeats.
    GamepadButtonDown,
    // A button of gamepad `device` went up: `gamepadButton`.
    GamepadButtonUp,
    // An axis of gamepad `device` changed: `gamepadAxis`, `axisValue` (see GamepadAxis).
    GamepadAxisMoved,

    // A drag from another application entered `window` (kNoWindow when the operating system drops
    // onto the application itself). The drop's other events follow, then DropComplete. A drag of
    // something else than files (text) also sends DropBegin and DropComplete, with no DropFile.
    DropBegin,
    // The drag moved over `window`: `x`, `y` in the window's pixels, like a mouse position.
    DropPosition,
    // One file or folder was dropped onto `window`: its absolute path in `text` and the drop's
    // position in `x`, `y`, in the window's pixels. Several files send one each. The path is the
    // operating system's bytes: UTF-8 on Windows (build a std::filesystem::path from it as UTF-8,
    // std::u8string), and on Linux UTF-8 on normally configured systems, though a file name may
    // be any bytes. When Windows can't start its drag and drop service (OLE), SDL falls back to
    // the older drop message: no DropPosition, and DropFile at 0, 0.
    DropFile,
    // The drop on `window` ended, with files or without (the drag left, or carried no files).
    DropComplete,
    // The open-file dialog of Platform::showOpenFileDialog chose a file: its absolute path in
    // `text`, the operating system's bytes like DropFile's. `window` is the dialog's window. One
    // event per file, then FileDialogClosed in the same pump.
    FileDialogFile,
    // The open-file dialog of `window` closed. `text` is empty when files were chosen (the
    // FileDialogFile events before it) and when the user cancelled; otherwise it is the error
    // that kept the dialog from being shown or from finishing (with the desktop portal possibly a
    // stale message). Another dialog may be opened now.
    FileDialogClosed,
};

// One event. Which fields mean something depends on `type` (see EventType); the others keep their
// defaults. A plain value: copy it freely, except that `text` points into the Platform's frame
// storage and is valid only as long as the span pumpEvents() returned: never keep an Event across
// frames.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): `type` has no default on purpose, see below
struct Event {
    // No default member initializer, so no event silently becomes QuitRequested: the platform
    // layer always sets it, and tests build events with designated initializers.
    EventType type;
    // When the event happened, in nanoseconds of std::chrono::steady_clock (its
    // time_since_epoch()), so it can be compared with frame times. Monotonic.
    std::uint64_t timestampNs = 0;
    // The window the event belongs to, or kNoWindow for application, display and gamepad events.
    WindowId window = kNoWindow;
    // The device of keyboard, mouse and gamepad events (see DeviceId); 0 for other events.
    DeviceId device = 0;
    // The new size for WindowResized and WindowPixelSizeChanged.
    std::int32_t width = 0;
    std::int32_t height = 0;

    // Keyboard. `key` is the physical key. `layoutKey` is the character the user's layout puts
    // on that key without modifiers (a Unicode code point, lowercase for letters; latin letters
    // on non-latin layouts), or 0 when the key prints nothing (F1, Shift, Enter, arrows). Game
    // bindings use `key`; editor command shortcuts (Ctrl + Z) use `layoutKey`, so a letter
    // shortcut follows its letter.
    Key key = Key::None;
    char32_t layoutKey = 0;
    // The modifiers held during KeyDown, KeyUp, MouseButtonDown, MouseButtonUp and MouseWheel.
    KeyModifiers modifiers = KeyModifiers::None;
    // KeyDown sent by the operating system's key repeat. Repeats are not presses.
    bool repeat = false;

    // Mouse, DropPosition and DropFile. `x`, `y` is the position in the window's pixels, from its
    // top-left corner.
    MouseButton mouseButton = MouseButton::None;
    // 1 for a single click, 2 for a double click, and so on (MouseButtonDown only).
    std::uint8_t clicks = 0;
    float x = 0.0f;
    float y = 0.0f;
    // MouseMoved: the motion, right and down positive. In relative mode
    // (Window::setRelativeMouseMode) it is the mouse's own unaccelerated motion; otherwise the
    // change of the position in window pixels. Which of the two is decided when the event is
    // pumped, as the library doesn't record it per event. MouseWheel: the wheel steps.
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    // MouseWheel: the user's wheel direction is inverted ("natural scrolling"). The deltas are
    // already turned back to the usual direction; a UI that scrolls content may use this to
    // follow the user's setting.
    bool wheelFlipped = false;

    // Gamepad.
    GamepadButton gamepadButton = GamepadButton::None;
    GamepadAxis gamepadAxis = GamepadAxis::None;
    float axisValue = 0.0f;

    // TextInput and TextEditing: the text; GamepadConnected: the gamepad's name; DropFile and
    // FileDialogFile: the path (see DropFile for its encoding); FileDialogClosed: the error, if
    // any. UTF-8 otherwise.
    std::string_view text;
    std::int32_t textCursor = 0;
    std::int32_t textSelectionLength = 0;

    // GamepadConnected.
    GamepadType gamepadType = GamepadType::Unknown;
    GamepadGuid gamepadGuid;
    // The Steam Input handle (an InputHandle_t) when Steam provides this gamepad, for Steam's
    // glyphs; 0 otherwise.
    std::uint64_t steamHandle = 0;

    friend bool operator==(const Event&, const Event&) = default;
};

} // namespace ith
