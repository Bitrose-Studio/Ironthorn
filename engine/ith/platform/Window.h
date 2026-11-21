#pragma once

// A window created by Platform::createWindow() (ADR-0012).
//
// Every window is high-DPI aware. Its size has two units: window coordinates, which the
// operating system uses for positions and which UI layout uses together with displayScale(),
// and pixels, which the swapchain uses. They differ on high-DPI displays when the platform
// scales windows (macOS, Wayland with scaling); on Windows they are the same, and the display
// scale says how much larger the UI should be drawn.

#include "ith/platform/Event.h"
#include "ith/platform/NativeWindowHandle.h"

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

private:
    friend class Platform;

    explicit Window(WindowId id) : m_id(id) {}

    WindowId m_id = kNoWindow;
};

} // namespace ith
