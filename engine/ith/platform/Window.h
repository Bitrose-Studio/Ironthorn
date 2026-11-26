#pragma once

// A window created by Platform::createWindow() (ADR-0012).
//
// Every window is high-DPI aware. Its size has two units: window coordinates, which the
// operating system uses for positions and which UI layout uses together with displayScale(),
// and pixels, which the swapchain uses. They differ on high-DPI displays when the platform
// scales windows (macOS, Wayland with scaling); on Windows they are the same, and the display
// scale says how much larger the UI should be drawn.

#include "ith/foundation/Expected.h"
#include "ith/platform/NativeWindowHandle.h"
#include "ith/platform_events/Event.h"

#include <cstdint>
#include <string_view>

namespace ith {

struct WindowDesc {
    // The title, UTF-8.
    std::string_view title = "Ironthorn";
    // The size in window coordinates. Both must be positive.
    std::int32_t width = 1280;
    std::int32_t height = 720;
    // Whether the user can resize the window.
    bool resizable = true;
};

struct WindowSize {
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const WindowSize&, const WindowSize&) = default;
};

// A text field's place in a window, for the IME (Window::setTextInputArea). Window pixels.
struct TextInputArea {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    // The text cursor's offset from the field's left edge.
    std::int32_t cursor = 0;
};

// Owns an operating system window and closes it when destroyed. Move-only; a moved-from Window
// owns nothing and may only be destroyed or assigned to.
//
// Every function must be called on the main thread (asserted), and the window must be
// destroyed before the Platform that created it (asserted; a Window that outlives its Platform
// only asserts and does nothing, because the Platform has closed it already).
class Window {
public:
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window();

    // The id that this window's events carry. Never kNoWindow for a live window.
    WindowId id() const { return m_id; }

    // The size in window coordinates.
    WindowSize size() const;

    // The size in pixels: what a swapchain for this window needs.
    WindowSize pixelSize() const;

    // How much larger than at 100% the UI should be drawn on this window's display, e.g. 1.5
    // at 150% scaling. Always positive.
    float displayScale() const;

    // The operating system's handles for this window; std::monostate when the video driver has
    // none (the "offscreen" driver). See NativeWindowHandle.h for how long each handle lives.
    NativeWindowHandle nativeHandle() const;

    // Relative mouse mode (ADR-0013 rule 5): while on and the window has focus, the cursor is
    // hidden and locked to the window, and MouseMoved reports the mouse's unaccelerated motion.
    // The input module decides when it should be on (mouse capture); the application applies
    // it: `window.setRelativeMouseMode(input.mouseCaptured(window.id()))`. Setting the current
    // mode again does nothing. Fails with "platform.relative_mouse_failed" and the library's
    // message when the video driver can't do it.
    Expected<void> setRelativeMouseMode(bool enabled);
    bool relativeMouseMode() const;

    // Moves the cursor to `x`, `y` in the window's pixels, e.g. back where a drag in relative
    // mode started, just before relative mode ends. Generates a MouseMoved event. Best effort: some
    // platforms don't move it (Wayland compositors may ignore it; so does Remote Desktop), and a
    // window that no longer exists is ignored.
    void warpMouse(float x, float y);

    // Text input (ADR-0013 rule 4): while on, the window receives TextInput and TextEditing
    // events and the IME may open; on touch platforms the on-screen keyboard shows. Turn it on
    // only while a text field has focus, because it lets the IME take key presses. Starting it
    // again, or stopping it when it is off, does nothing. Fails with "platform.text_input_failed"
    // and the library's message.
    Expected<void> startTextInput();
    void stopTextInput();
    bool textInputActive() const;

    // Tells the IME where the focused text field is, so its candidate window appears next to it:
    // the field's rectangle in window pixels and the cursor's offset from the field's left edge
    // in pixels. Fails with "platform.text_input_failed" and the library's message.
    //
    // Precondition (asserted): `area` has a non-negative width and height.
    Expected<void> setTextInputArea(const TextInputArea& area);

private:
    friend class Platform;

    explicit Window(WindowId id) : m_id(id) {}

    WindowId m_id = kNoWindow;
};

} // namespace ith
