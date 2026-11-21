# ADR-0012: Windowing and the platform event loop

- **Status:** Accepted
- **Amended by:** [ADR-0025](0025-job-system.md)
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, no RTTI),
  [ADR-0004](0004-modules-and-layering.md) (`platform` layer, no third-party types in public
  headers), [ADR-0005](0005-reference-hardware.md) (Windows, Linux, Steam Deck),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (multiple windows, docking),
  [ADR-0009](0009-rhi.md) (the RHI creates its own surface),
  [ADR-0013](0013-input.md) (input built on these events)

## Context

The engine needs windows and an event loop before it can draw anything. Requirements:

- Windows and Linux (X11 and Wayland, including Steam Deck) now; Android and consoles later,
  without changing callers.
- The editor (ADR-0008) needs several windows, popup windows for menus and tooltips, high-DPI
  scaling per monitor, clipboard, cursors and file dialogs.
- Games need keyboard, mouse with raw relative motion, gamepads (Xbox, PlayStation, Switch,
  Steam Deck, Steam Input), text input with IME, and app lifecycle events (focus, minimize,
  background and foreground on Android).
- The RHI creates its own Vulkan surface or DXGI swapchain from a native window handle and never
  includes the windowing library (ADR-0009).
- No exceptions, no RTTI, no library types in public headers.

Lessons from earlier engine prototypes by the same maintainer:

- A desktop windowing library behind a private implementation, with a headless mode for CI,
  worked. Two mistakes: the window object created the Vulkan surface, and the library's window
  pointer leaked out as `void*`. Key modifiers were dropped and DPI changes were not reported.
- Translating platform events into engine-owned types at the platform boundary, and keeping
  every window call on one thread, kept the rest of the engine independent of the library.

Facts below were checked on 2026-11-20 against the projects' release tags and sources. SDL was
built with the engine flags on Windows (MSVC and clang-cl) and run; Linux was checked from SDL's
sources and documentation only, and CI confirms it.

## Options considered

**A. SDL 3.4.20** (zlib, [release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20),
2026-11-03). Plain C. Covers every requirement above: multiple and popup windows, high DPI,
raw relative mouse, IME composition, gamepads through HIDAPI drivers (DualSense, Switch, Steam
Deck) with a gamepad type for button glyphs, rumble and LEDs, Steam virtual gamepads, clipboard,
cursors, file dialogs, Android. Official console ports exist for platform licensees (zlib, under
NDA). Monthly bugfix releases on the stable 3.4 line; large, active maintainer base.
- Built on Windows with `/W4 /WX /EHs-c- /GR-` (MSVC 14.38 and clang-cl): zero warnings; a test
  program created a high-DPI window, read its native handle, toggled text input and found a
  gamepad. About 1.5–1.7 MB in a game with the option set below.
- On Linux, X11 and Wayland support are loaded at run time, but are only compiled in when their
  development headers are present at configure time; otherwise SDL silently drops them.

**B. GLFW 3.5.1** (zlib). Small, clean and fast to build, but no IME composition, no gamepad
type, rumble, LEDs or sensors, no Android, no consoles and no file dialogs. Its last release
before 3.5.1 was in 2024, and development is slow. Fine for a desktop tool, short of the game
and editor requirements.

**C. Our own platform layer.** Windows alone is weeks of work; Wayland, X11, IME and HID
gamepads are months, and gamepad quirks are an endless tail (SDL's joystick and HIDAPI code is
about 69k lines). Players would see no difference. Still possible later, per platform, behind
the same API.

## Decision

### Backend and build

1. **SDL 3.4.20 is the windowing and input backend**, linked statically and privately by the
   `platform` module. SDL headers appear only in `platform` source files.
2. **Pin:** `https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz`,
   SHA-256 `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f` (the maintainers'
   signed release archive).
3. **Options.** On: `SDL_STATIC`, `SDL_VULKAN`, `SDL_DIALOG`, `SDL_POWER`, joystick and HIDAPI
   (defaults). Off: `SDL_SHARED`, `SDL_TEST_LIBRARY`, `SDL_TESTS`, `SDL_EXAMPLES`,
   `SDL_INSTALL`, `SDL_UNINSTALL`, `SDL_AUDIO` (audio has its own library), `SDL_RENDER`,
   `SDL_GPU` (the RHI renders), `SDL_CAMERA`, `SDL_TRAY`, `SDL_HAPTIC` (gamepad rumble doesn't
   need it), `SDL_SENSOR` (gamepad sensors don't need it), `SDL_OPENGL`, `SDL_OPENGLES`.
4. **Steam.** Static linking keeps SDL overridable through its Dynamic API
   (`SDL3_DYNAMIC_API`), so Steam can still supply a newer SDL to the game.
5. **Linux CI** installs the X11, Wayland, xkbcommon, libdecor, D-Bus, IBus and udev development
   packages that SDL's Linux documentation lists. A platform test asserts that both the `x11` and
   `wayland` video drivers are compiled in, so a missing package fails CI instead of shipping an
   X11-only build. At run time SDL's default applies: Wayland where the compositor supports it,
   otherwise X11; a config setting can override it. Both are tested on the Linux lab machines.
6. **Licenses.** SDL is zlib. Its bundled HIDAPI is used under its BSD-style license (it is
   offered under GPL-3, BSD-style or its original license, at the user's choice). Both get a line
   in `docs/development/third-party.md` and the shipped notices.
7. **Upgrades** stay within 3.4.x, at phase boundaries or for a needed fix. Moving to 3.6 is a
   decision of its own.

### API

8. **`platform` exposes engine types only:**
   - **Windows:** create and destroy, any number of them, plus popup windows owned by a parent
     (menus, tooltips). Title, size, position, windowed or borderless fullscreen, resizable,
     minimize, maximize and restore. Fullscreen is **borderless only**; exclusive fullscreen is
     added only if measurements show a gain, which flip-model presentation usually removes.
   - **High DPI:** every window is created high-DPI aware. The API reports both the size in
     pixels (for the swapchain) and the display scale (for UI), and sends events when either
     changes, including when a window moves to another monitor.
   - **Native handle:** a backend-neutral struct with one variant per platform: Win32 `HWND`
     and `HINSTANCE`; Wayland display and surface; X11 display and window; later the Android
     native window. The RHI creates its surface from it.
   - **Clipboard** (UTF-8 text), **cursors** (system shapes, custom images, show and hide,
     relative mode for ADR-0013), and **file dialogs** (open, save and folder, asynchronous,
     result delivered as an event). File dialogs are for the editor and tools only; games use
     their own UI.
9. **Main thread only.** Window, event and cursor calls happen on the thread that initialized
   `platform`, checked by an assert. Other threads post work to the main thread through a
   thread-safe queue.
10. **Event pump once per frame.** At the start of each frame `platform` drains SDL's queue and
    translates every event into an engine event with a timestamp and the window it belongs to.
    The frame's events are a plain list that core systems read; no SDL callback reaches engine or
    game code.
11. **Lifecycle events** are part of that list: quit requested, window close requested (the
    editor asks to save), focus gained and lost, minimized and restored, display added or removed,
    low memory, and entering or leaving the background. When the app goes to the background
    (Android), rendering stops and the swapchain is recreated on return. After any long gap, such
    as a Steam Deck sleep, the frame delta is clamped so simulation doesn't jump.
12. **Headless mode.** Tests and CI use SDL's offscreen driver; no display is needed.
13. **Memory.** SDL's allocations are routed to the engine allocator through
    `SDL_SetMemoryFunctions` once the engine's general-purpose allocator exists.

## Consequences

- One library covers windows, events, input devices, IME and dialogs on every current target,
  and console ports exist. The editor's windowing and file-dialog needs are met without
  further libraries.
- About 1.5–1.7 MB per game, and SDL's 260-odd source files build once per configuration.
- Linux CI images must carry the development packages; the driver test guards against
  forgetting them.
- Rendering pauses while the user drags a window border on Windows, because the pump runs once
  per frame. Rendering during a live resize is added with the editor shell (P2), through SDL's
  event watch.
- Android needs SDL's Java activity glue as its entry point; it is adopted when Android becomes
  a target.
- Follow-up work: the `platform` window and event API with tests on the offscreen driver; the
  native handle consumed by both RHI backends; the Linux driver test; the third-party entry.
- **Revisit when:**
  - SDL 3.6 is released (upgrade decision);
  - a console target is chosen (the official port or our own backend behind the same API);
  - SDL's maintenance changes, or a platform needs something SDL lacks.
