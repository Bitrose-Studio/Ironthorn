#include "ith/platform/Window.h"

#include "ith/foundation/Assert.h"
#include "ith/platform/PlatformState.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <utility>

namespace ith {
namespace {

// The SDL window for `id`, or nullptr (after an assert) when there is none.
SDL_Window* sdlWindow(WindowId id) {
    ITH_ASSERT_MSG(id != kNoWindow, "Window used after it was moved from");
    ITH_ASSERT_MSG(detail::platformExists(), "Window used after its Platform was destroyed");
    // Without a Platform, SDL is shut down and has closed the window already.
    if (id == kNoWindow || !detail::platformExists()) {
        return nullptr;
    }
    ITH_ASSERT_MSG(detail::isMainThread(), detail::kNotMainThread);
    SDL_Window* window = SDL_GetWindowFromID(id);
    ITH_ASSERT_MSG(window != nullptr, "the window no longer exists");
    return window;
}

} // namespace

Window::Window(Window&& other) noexcept : m_id(std::exchange(other.m_id, kNoWindow)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        const Window discarded(std::move(*this));
        m_id = std::exchange(other.m_id, kNoWindow);
    }
    return *this;
}

Window::~Window() {
    if (m_id == kNoWindow) {
        return;
    }
    // Without a Platform, SDL has already closed the window and finds no window for the id.
    if (SDL_Window* window = sdlWindow(m_id)) {
        SDL_DestroyWindow(window);
        detail::platformState().windowCount.fetch_sub(1, std::memory_order_relaxed);
    }
}

WindowSize Window::size() const {
    WindowSize size;
    SDL_GetWindowSize(sdlWindow(m_id), &size.width, &size.height);
    return size;
}

WindowSize Window::pixelSize() const {
    WindowSize size;
    SDL_GetWindowSizeInPixels(sdlWindow(m_id), &size.width, &size.height);
    return size;
}

float Window::displayScale() const {
    const float scale = SDL_GetWindowDisplayScale(sdlWindow(m_id));
    // SDL reports 0 only for an invalid window, which sdlWindow() already asserts.
    return scale > 0.0f ? scale : 1.0f;
}

NativeWindowHandle Window::nativeHandle() const {
    SDL_Window* window = sdlWindow(m_id);
    if (window == nullptr) {
        return {};
    }
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr) {
        return {};
    }
    if (std::strcmp(driver, "windows") == 0) {
        return Win32WindowHandle{
            .hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr),
            .hinstance = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr),
        };
    }
    if (std::strcmp(driver, "wayland") == 0) {
        return WaylandWindowHandle{
            .display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr),
            .surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr),
        };
    }
    if (std::strcmp(driver, "x11") == 0) {
        return X11WindowHandle{
            .display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr),
            .window =
                static_cast<std::uint64_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)),
        };
    }
    return {};
}

} // namespace ith
