#pragma once

// The platform layer: initialization, windows, the per-frame event pump and the clipboard
// (ADR-0012).
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
//       // simulate, render
//   }
//
// SDL is the backend; none of its types appear in this module's headers. An application creates
// one Platform; engine and game code never create one (Play In Editor uses the editor's).
//
// Threading: the thread that creates the Platform is the main thread. Every Platform and Window
// function must be called on it, which is asserted, because the operating systems' window and
// event APIs require it.

#include "ith/foundation/Compiler.h"
#include "ith/foundation/Expected.h"
#include "ith/platform/Event.h"
#include "ith/platform/Window.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

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
    // Initializes the windowing library on the calling thread, which becomes the main thread.
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
    //
    // Precondition (asserted): called on the main thread.
    std::span<const Event> pumpEvents() ITH_LIFETIMEBOUND;

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

private:
    Platform() = default;

    // Asserts that this Platform owns the platform layer and that the caller is on the main thread.
    void assertUsable() const;

    bool m_owner = false;
    // Added to the library's event timestamps to put them on std::chrono::steady_clock.
    std::int64_t m_clockOffsetNs = 0;
    std::vector<Event> m_events;
};

} // namespace ith
