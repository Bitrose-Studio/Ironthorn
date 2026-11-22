# Platform: windows and the event loop

The `platform` module (`engine/ith/platform`) opens windows and turns the operating system's
events into engine events, on top of SDL 3
([ADR-0012](../decisions/0012-windowing-and-event-loop.md)). It is a `client` module: players
and the editor link it, a dedicated server doesn't
([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)). SDL is linked privately; no SDL
type or header appears in the module's API. The event types themselves (`ith::Event`, key and
button ids) live in the `runtime` module `platform_events`, so modules that only read events, like
`input`, don't depend on `platform`.

## Use

```cpp
#include "ith/platform/Platform.h"

ith::Expected<ith::Platform> platform = ith::Platform::create();
if (!platform) {
    return fail(platform.error()); // "platform.init_failed" with SDL's message
}
ith::Expected<ith::Window> window = platform->createWindow({.title = "Game", .width = 1280, .height = 720});
...
bool running = true;
while (running) {
    for (const ith::Event& event : platform->pumpEvents()) {
        switch (event.type) {
            case ith::EventType::WindowCloseRequested:
            case ith::EventType::QuitRequested:
                running = false;
                break;
            case ith::EventType::WindowPixelSizeChanged:
                // resize the swapchain to event.width x event.height
                break;
            default:
                break;
        }
    }
    jobs.runMainThreadJobs();   // results of background work (ADR-0035 rule 22)
    // simulate and render
}
```

`samples/window_player` is a complete example.

## Rules

- **One `Platform` per application**, created by the application on the thread that becomes the
  main thread. Engine and game code never create one; Play In Editor uses the editor's. Every
  `Platform` and `Window` call happens on that thread; an assert checks it.
- **Windows die before the `Platform`**, and a surface or swapchain dies before its `Window`.
  `~Platform` asserts that no window is left.
- **The pump runs once per frame**, at its start. It returns the frame's events in order, each
  with a timestamp in `std::chrono::steady_clock` nanoseconds, comparable with frame times, and
  the id of its window (`kNoWindow` for application events). The span is valid until the next
  pump. No callback reaches engine code.
- **Waiting while idle.** `waitEvents(timeout)` is the pump after a wait: until an event the
  platform reports arrives, `Platform::wake()` is called, or the timeout passes (events it doesn't
  report, such as device bookkeeping, don't end the wait). The editor uses it to stop rendering
  frames nobody looks at (ADR-0029 rule 12). `Platform::wake()` is static and thread-safe: it
  queues one internal event, however often it is called before the next pump, and every pump
  drops it. It does nothing when no `Platform` exists. Connect it to
  `JobSystem::setMainThreadWake` ([jobs guide](jobs.md#the-main-thread-queue-in-the-frame)), and
  let the `JobSystem` go before the `Platform`.
- **Closing a window is a request.** `WindowCloseRequested` leaves the window open; the
  application destroys it, or asks to save first. Closing the last window does not quit by
  itself. `QuitRequested` comes from the operating system (Ctrl+C, the session ending).
- **Two sizes.** `Window::size()` is in window coordinates; `Window::pixelSize()` is what a
  swapchain needs; `Window::displayScale()` is the UI scale of the window's display. A window
  moving to another monitor sends `WindowDisplayScaleChanged` and usually
  `WindowPixelSizeChanged`.
- **Native handles** (`Window::nativeHandle()`) are a `std::variant` of Win32, Wayland and X11
  handles, for the RHI to create its surface. They are `std::monostate` on the headless driver.
  Window handles live as long as the `Window`, display connections as long as the `Platform`.
- **On Wayland a window appears only once something is presented to it**, so a window without a
  renderer (such as `window_player`'s today) stays invisible there.
- **Errors** from SDL are `ith::Expected` errors with stable codes (`platform.init_failed`,
  `platform.window_failed`, `platform.clipboard_failed`, `platform.relative_mouse_failed`,
  `platform.text_input_failed`) and SDL's message. Reading the clipboard
  returns an empty string when there is no text or it can't be read.

## Video drivers

`PlatformDesc::videoDriver` picks the driver: `windows`, `wayland`, `x11` or `offscreen`. Empty
uses the `SDL_VIDEO_DRIVER` environment variable when it is set, otherwise SDL's default: on
Linux, Wayland where the compositor supports it, otherwise X11.

`offscreen` needs no display. The tests use it, and CI runs `window_player` with
`SDL_VIDEO_DRIVER=offscreen`. On Linux, SDL compiles in X11 and Wayland only when their
development packages are installed ([building](building.md#linux)); the `platform` test checks
that both are there. The Linux build log shows `validity error ... deprecated-since` and `frozen`
lines from `wayland-scanner` while SDL generates its Wayland protocol code; they are expected and
harmless.

## Device input

The pump also translates keyboard, text, mouse and gamepad events
([ADR-0013](../decisions/0013-input.md)). Most code reads them through the `input` module
([guide](input.md)), which builds state and routes them; the fields of each event type are
documented in `ith/platform_events/Event.h`.

- **Keyboard:** `key` is the physical key (`ith::Key`, a USB HID value, named after the US
  layout), `layoutKey` the character the user's layout prints on it (0 for keys that print
  nothing), plus `modifiers` and `repeat`. Media and system keys the engine doesn't name are
  dropped. `Platform::keyDisplayName(key)` gives the name to show for a binding ("Z" for
  `Key::W` on AZERTY).
- **Text:** `TextInput` (committed UTF-8) and `TextEditing` (IME composition with cursor and
  selection) arrive only while `Window::startTextInput()` is on; `Window::setTextInputArea()`
  places the IME next to the field. An event's `text` view is valid until the next pump.
- **Mouse:** buttons (left, right, middle, back, forward) with click counts, the position in
  window pixels, motion, and fractional wheel steps. A flipped wheel ("natural scrolling") is
  reported like a normal one, positive up and right, with `wheelFlipped` set so a UI can follow
  the user's setting. `Window::setRelativeMouseMode(true)` hides and locks the cursor; motion is
  then the mouse's own, unaccelerated. Whether relative mode applies is read when the event is
  pumped (SDL doesn't record it per event). Mouse button and wheel events carry the modifiers of
  the key events before them in the queue, so a click keeps the modifiers held when it happened.
- **Gamepads:** `Platform::create` starts SDL's gamepad subsystem; if it can't, the platform still
  works without gamepads (no gamepad events; output calls fail with
  `platform.gamepad_not_found`). The pump opens a gamepad
  when it connects (gamepads present at start-up connect in the first pump) and closes it when it
  disconnects. `GamepadConnected` carries the session id (`device`), the name, the type for glyphs,
  the GUID and the Steam Input handle. Buttons are positional; sticks are -1..1 with up positive,
  triggers 0..1, raw, without dead zones. `rumbleGamepad`, `rumbleGamepadTriggers` and
  `setGamepadLed` drive outputs and fail with `platform.gamepad_not_found` or
  `platform.gamepad_output_failed`. While no window of the application has focus (and at least one
  window exists), SDL drops gamepad presses and axis motion away from the centre, but still sends
  button releases and axis motion toward the centre; `input` ignores those (its focus loss has
  released everything already).
- **Timestamps:** every event, devices included, carries a `std::chrono::steady_clock` timestamp;
  `Platform::pumpTimeNs()` is when the last pump finished, the frame's input time.

The `platform` tests push SDL events into the queue to check the translation, and drive a SDL
virtual gamepad for connect, buttons, axes, outputs and disconnect, all on the `offscreen`
driver. `tests/platform/WakeTests.cpp` checks that `waitEvents` times out, that `wake()`
ends a wait from another thread and coalesces, and that a main-thread post from a background job
ends `waitEvents(1 s)` within 50 ms; like every platform test it runs through CTest on the
offscreen driver, never on a desktop.

## Not here yet

- **Cursor shapes and file dialogs**: cursors with the editor shell's UI, file dialogs with the
  editor.
- **Gyro, accelerometer and touchpad** of gamepads, and **touch and pen** (with Android).
- **Popup windows, fullscreen, position, minimize and maximize calls**: with the editor shell.
- **Routing SDL's allocations** to the engine allocator, and handling mobile lifecycle events in
  an event watch (Android); until then `EnteringBackground` may arrive too late to act on.
