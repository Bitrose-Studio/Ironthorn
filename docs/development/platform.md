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
- **Cursor.** `Platform::setCursor(CursorShape)` sets one of the system shapes (arrow, text,
  resize, hand, not allowed, wait, progress, crosshair) over every window, and
  `setCursorVisible()` shows or hides it. Shapes are created on first use and kept until the
  `Platform` goes. The `offscreen` driver has no system cursors, so `setCursor` fails there with
  `platform.cursor_failed` and the shape stays; callers such as the editor UI ignore that.
- **Errors** from SDL are `ith::Expected` errors with stable codes (`platform.init_failed`,
  `platform.window_failed`, `platform.clipboard_failed`, `platform.relative_mouse_failed`,
  `platform.text_input_failed`, `platform.cursor_failed`) and SDL's message. Reading the clipboard
  returns an empty string when there is no text or it can't be read.

## Dropped files and the open-file dialog

For the editor and tools ([ADR-0049](../decisions/0049-editor-content-browser-v1.md) rules 24
and 25); games use their own UI.

- **Drops.** Files and folders dragged from the file manager onto a window arrive as
  `DropBegin`, `DropPosition` while the drag moves, one `DropFile` per path (in `text`), and
  `DropComplete`, all with the window's id (`kNoWindow` when the operating system drops onto the
  application rather than one of its windows). `DropPosition` and `DropFile` carry the position
  in window pixels, like mouse events; `DropFile` has the last position of the drag. Dropped text
  is not delivered, so a drag of text sends only `DropBegin` and `DropComplete`. When Windows
  can't start its drag and drop service (OLE), SDL falls back to the older drop message: there is
  no `DropPosition` then, and `DropFile` is at 0, 0. Not verified yet: drops from a non-elevated
  file manager into an editor run as administrator, which Windows is expected to block.
- **Paths** in `DropFile` and `FileDialogFile` are the operating system's bytes. On Windows they
  are always UTF-8: build a `std::filesystem::path` from them as UTF-8 (a `std::u8string`), never
  through the ANSI code page. On Linux they are UTF-8 on normally configured systems, but a file
  name may be any bytes, not valid UTF-8.
- **The open-file dialog.** `Platform::showOpenFileDialog(window, {filters, defaultFolder,
  allowMany})` shows the operating system's dialog and returns at once. A later pump delivers one
  `FileDialogFile` per chosen file (the path in `text`) and then `FileDialogClosed`, whose `text`
  is empty when files were chosen or the user cancelled, and the error otherwise. One dialog at a
  time: until its `FileDialogClosed` has been pumped, another call fails with
  `platform.dialog_busy`.
  - **Filters** are a name and extensions without dots separated by semicolons
    (`{"Models", "gltf;glb"}`), or `"*"` for every file. A pattern is never empty (asserted); an
    invalid one is reported through `FileDialogClosed`. zenity matches extensions
    case-sensitively, the Windows dialog and the desktop portal don't, so list both cases
    (`"png;PNG"`) where it matters.
  - **The default folder** should exist: on Windows SDL may otherwise fall back to the older
    dialog, which is limited to `MAX_PATH`. The platform adds a trailing separator (and on
    Windows turns `/` into `\`), because SDL takes the location as a folder or a file and would
    otherwise treat the last name as a file.
  - **Releases without asserts:** an unknown window gives a dialog without a parent, and an empty
    pattern a `FileDialogClosed` with an error.

  ```cpp
  const ith::FileDialogFilter filters[] = {{.name = "Models", .patterns = "gltf;glb"},
                                           {.name = "All files", .patterns = "*"}};
  if (auto shown = platform.showOpenFileDialog(window, {.filters = filters, .allowMany = true}); !shown) {
      log(shown.error()); // platform.dialog_busy
  }
  // in a later frame
  for (const ith::Event& event : platform.pumpEvents()) {
      if (event.type == ith::EventType::FileDialogFile) {
          paths.emplace_back(event.text); // copy: the view is valid until the next pump
      } else if (event.type == ith::EventType::FileDialogClosed && !event.text.empty()) {
          log(event.text); // no dialog available, for example
      }
  }
  ```

  As for every event, the span and its `text` views are valid until the next pump or until the
  `Platform` is moved or destroyed.

  On Windows SDL shows the system dialog on a thread of its own. On Linux it needs, at run time,
  `xdg-desktop-portal` with a backend, or `zenity`; SDL tries the portal over D-Bus first, and the
  first dialog call blocks while it checks which is there. Without either, `FileDialogClosed`
  reports the error and the editor imports by drop only. Some portal failures leave a stale or
  unrelated SDL message in `FileDialogClosed`'s text.

  **How the result reaches the main thread.** SDL's callback may run on any thread. It copies the
  paths, or SDL's error message, into a slot of the platform guarded by a mutex, and calls
  `Platform::wake()`, which puts an event without data into SDL's thread-safe queue so that a
  `waitEvents()` returns. Every pump takes the slot at its end and turns it into the events,
  stamped when the pump takes them (SDL's clock and `std::chrono::steady_clock` may drift apart, so
  their order against the frame's other timestamps is close, not exact). The paths are owned by
  the slot, never by SDL's queue, so nothing leaks when the queue is cleared at shutdown, and a
  full queue only delays the result to the next pump. The platform keeps its copies of the filters
  and the default folder until the callback has run, as SDL requires. The slot is never
  destroyed, because SDL's dialog thread (Windows, zenity) is detached and may call back during
  static destruction at exit.
  - **The desktop portal** calls back only inside SDL's pump, and `waitEvents()` doesn't wake for
    it, so while a dialog is open `waitEvents()` waits at most 50 ms.
  - **A window destroyed while its dialog is open:** the dialog should still end with
    `FileDialogClosed` and the old window id; not verified.
  - **A `Platform` destroyed while a dialog is open** drops its result: the callback that comes
    later delivers nothing. With the Windows dialog and zenity, a later `Platform` in the same
    process can open a dialog once the old one has closed. With the desktop portal the callback
    never arrives after shutdown, so later `Platform`s in the process stay `platform.dialog_busy`.
    If that ever matters, the remedy is storage per request, with a request id passed to SDL as
    the callback's userdata.

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
  then the mouse's own, unaccelerated. `Window::warpMouse(x, y)` moves the cursor to a point in
  window pixels, e.g. back where a drag started before relative mode ends; it is best effort
  (Wayland compositors may ignore it). Whether relative mode applies is read when the event is
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
ends a long wait soon after the post. The waits are 10 s long and the latency is measured from
the post, so a late background job (its workers run below normal priority) or an unrelated event
doesn't fail the test. The latency is printed; ADR-0035 rule 26 asks for 50 ms, which the test
allows up to 500 ms for shared CI machines, far below the 10 s a missed wake would take. Like every
platform test it runs through CTest on the offscreen driver, never on a desktop.
`tests/platform/DropAndFileDialogTests.cpp` pushes SDL's drop events to check their translation.
The real file dialog can't be shown headless: the tests open it with an invalid filter, which SDL
rejects before showing anything (for `platform.dialog_busy` and the error path), or stand in for
showing it and for SDL's callback with `ith::detail::beginOpenFileDialog` and
`ith::detail::finishOpenFileDialog` (chosen files, both kinds of cancel, errors, a result from
another thread ending `waitEvents`, the 50 ms wait cap, and a callback that comes after its
`Platform` is gone).

## Not here yet

- **Custom cursor images, save and folder dialogs**: custom images when a tool needs one, the
  dialogs when a feature needs them.
- **Gyro, accelerometer and touchpad** of gamepads, and **touch and pen** (with Android).
- **Popup windows, fullscreen, position, minimize and maximize calls**: with the editor shell.
- **Routing SDL's allocations** to the engine allocator, and handling mobile lifecycle events in
  an event watch (Android); until then `EnteringBackground` may arrive too late to act on.
