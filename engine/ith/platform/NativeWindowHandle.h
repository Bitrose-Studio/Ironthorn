#pragma once

// The operating system's handles of a window, for the RHI to create its surface or swapchain
// from (ADR-0009, ADR-0012). The RHI never includes the windowing library.
//
//   const ith::NativeWindowHandle handle = window.nativeHandle();
//   if (const auto* win32 = std::get_if<ith::Win32WindowHandle>(&handle)) {
//       // vkCreateWin32SurfaceKHR or a DXGI swapchain from win32->hwnd and win32->hinstance
//   }
//
// The handles are plain values without ownership. The window handles (hwnd, the Wayland surface,
// the X11 window) belong to the Window and stay valid while it exists; the connection handles
// (hinstance, the Wayland and X11 displays) belong to the Platform and stay valid while it exists.
// A surface or swapchain created from them must be destroyed before the Window.

#include <cstdint>
#include <variant>

namespace ith {

// A window of the Win32 video driver.
struct Win32WindowHandle {
    void* hwnd = nullptr;      // HWND; owned by the Window
    void* hinstance = nullptr; // HINSTANCE; the process module, valid while the Platform exists
};

// A window of the Wayland video driver.
struct WaylandWindowHandle {
    void* display = nullptr; // struct wl_display*; owned by the Platform
    void* surface = nullptr; // struct wl_surface*; owned by the Window
};

// A window of the X11 video driver.
struct X11WindowHandle {
    void* display = nullptr;  // Display*; owned by the Platform
    std::uint64_t window = 0; // Window (an XID); owned by the Window
};

// One alternative per platform; std::monostate when the video driver has no native window, as
// with the headless "offscreen" driver used by tests.
using NativeWindowHandle = std::variant<std::monostate, Win32WindowHandle, WaylandWindowHandle, X11WindowHandle>;

} // namespace ith
