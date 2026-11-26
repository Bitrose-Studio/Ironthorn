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

// NOLINTNEXTLINE(readability-make-member-function-const): changes the window's state
Expected<void> Window::setRelativeMouseMode(bool enabled) {
    SDL_Window* window = sdlWindow(m_id);
    if (window == nullptr) {
        return makeError("platform.relative_mouse_failed", "the window no longer exists");
    }
    if (SDL_GetWindowRelativeMouseMode(window) == enabled) {
        return {};
    }
    if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
        return detail::sdlError("platform.relative_mouse_failed", "relative mouse mode could not be changed");
    }
    return {};
}

bool Window::relativeMouseMode() const {
    SDL_Window* window = sdlWindow(m_id);
    return window != nullptr && SDL_GetWindowRelativeMouseMode(window);
}

// NOLINTNEXTLINE(readability-make-member-function-const): changes the cursor's state
void Window::warpMouse(float x, float y) {
    SDL_Window* window = sdlWindow(m_id);
    if (window == nullptr) {
        return;
    }
    // The engine works in pixels; the library wants window coordinates.
    const float density = SDL_GetWindowPixelDensity(window);
    const float scale = density > 0.0f ? 1.0f / density : 1.0f;
    SDL_WarpMouseInWindow(window, x * scale, y * scale);
}

// NOLINTNEXTLINE(readability-make-member-function-const): changes the window's state
Expected<void> Window::startTextInput() {
    SDL_Window* window = sdlWindow(m_id);
    if (window == nullptr) {
        return makeError("platform.text_input_failed", "the window no longer exists");
    }
    if (SDL_TextInputActive(window)) {
        return {};
    }
    if (!SDL_StartTextInput(window)) {
        return detail::sdlError("platform.text_input_failed", "text input could not be started");
    }
    return {};
}

// NOLINTNEXTLINE(readability-make-member-function-const): changes the window's state
void Window::stopTextInput() {
    SDL_Window* window = sdlWindow(m_id);
    if (window != nullptr && SDL_TextInputActive(window)) {
        SDL_StopTextInput(window);
    }
}

bool Window::textInputActive() const {
    SDL_Window* window = sdlWindow(m_id);
    return window != nullptr && SDL_TextInputActive(window);
}

// NOLINTNEXTLINE(readability-make-member-function-const): changes the window's state
Expected<void> Window::setTextInputArea(const TextInputArea& area) {
    ITH_ASSERT_MSG(area.width >= 0 && area.height >= 0, "a text input area needs a non-negative size");
    SDL_Window* window = sdlWindow(m_id);
    if (window == nullptr) {
        return makeError("platform.text_input_failed", "the window no longer exists");
    }
    // The engine works in pixels; the library wants window coordinates.
    const float density = SDL_GetWindowPixelDensity(window);
    const auto toWindow = [&](std::int32_t pixels) {
        return density > 0.0f ? static_cast<int>(static_cast<float>(pixels) / density) : pixels;
    };
    const SDL_Rect rect{
        .x = toWindow(area.x), .y = toWindow(area.y), .w = toWindow(area.width), .h = toWindow(area.height)};
    if (!SDL_SetTextInputArea(window, &rect, toWindow(area.cursor))) {
        return detail::sdlError("platform.text_input_failed", "the text input area could not be set");
    }
    return {};
}

} // namespace ith
