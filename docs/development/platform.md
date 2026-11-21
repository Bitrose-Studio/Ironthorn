# Platform: windows and the event loop

The `platform` module (`engine/ith/platform`) opens windows and turns the operating system's
events into engine events, on top of SDL 3
([ADR-0012](../decisions/0012-windowing-and-event-loop.md)). It is a `client` module: players
and the editor link it, a dedicated server doesn't
([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)). SDL is linked privately; no SDL
type or header appears in the module's API.

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
  `platform.window_failed`, `platform.clipboard_failed`) and SDL's message. Reading the clipboard
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

## Not here yet

- **Keyboard, mouse, text input and gamepads** come with the input slice: their engine events
  need the stable key ids and device model of `core/input`
  ([ADR-0013](../decisions/0013-input.md)). SDL already has them compiled in.
- **Cursors, relative mouse mode and file dialogs**: cursors and relative mode come with input;
  file dialogs with the editor.
- **Popup windows, fullscreen, position, minimize and maximize calls**: with the editor shell.
- **Posting work to the main thread** from other threads, routing SDL's allocations to the engine
  allocator, and handling mobile lifecycle events in an event watch (Android); until then
  `EnteringBackground` may arrive too late to act on.
