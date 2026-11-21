#include "ith/platform/Platform.h"

#include "ith/foundation/Assert.h"
#include "ith/platform/PlatformState.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace ith {
namespace detail {

PlatformState& platformState() {
    static PlatformState state;
    return state;
}

} // namespace detail

namespace {

Unexpected<Error> sdlError(const char* code, const char* what) {
    return makeError(code, std::string(what) + ": " + SDL_GetError());
}

Event applicationEvent(EventType type, const SDL_Event& event) {
    return Event{.type = type, .timestampNs = event.common.timestamp};
}

Event windowEvent(EventType type, const SDL_WindowEvent& event) {
    return Event{.type = type, .timestampNs = event.timestamp, .window = event.windowID};
}

std::optional<Event> translate(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
            return applicationEvent(EventType::QuitRequested, event);
        case SDL_EVENT_LOW_MEMORY:
            return applicationEvent(EventType::LowMemory, event);
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            return applicationEvent(EventType::EnteringBackground, event);
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            return applicationEvent(EventType::EnteredForeground, event);
        case SDL_EVENT_DISPLAY_ADDED:
            return applicationEvent(EventType::DisplayAdded, event);
        case SDL_EVENT_DISPLAY_REMOVED:
            return applicationEvent(EventType::DisplayRemoved, event);
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return windowEvent(EventType::WindowCloseRequested, event.window);
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            Event resized = windowEvent(event.type == SDL_EVENT_WINDOW_RESIZED ? EventType::WindowResized
                                                                               : EventType::WindowPixelSizeChanged,
                                        event.window);
            resized.width = event.window.data1;
            resized.height = event.window.data2;
            return resized;
        }
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            return windowEvent(EventType::WindowDisplayScaleChanged, event.window);
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            return windowEvent(EventType::WindowFocusGained, event.window);
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            return windowEvent(EventType::WindowFocusLost, event.window);
        case SDL_EVENT_WINDOW_MINIMIZED:
            return windowEvent(EventType::WindowMinimized, event.window);
        case SDL_EVENT_WINDOW_MAXIMIZED:
            return windowEvent(EventType::WindowMaximized, event.window);
        case SDL_EVENT_WINDOW_RESTORED:
            return windowEvent(EventType::WindowRestored, event.window);
        default:
            return std::nullopt;
    }
}

// Nanoseconds of std::chrono::steady_clock now.
std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Expected<Platform> Platform::create(const PlatformDesc& desc) {
    detail::PlatformState& state = detail::platformState();
    std::thread::id none;
    const bool claimed = state.mainThread.compare_exchange_strong(none, std::this_thread::get_id());
    ITH_ASSERT_MSG(claimed, "only one Platform may exist at a time");
    if (!claimed) {
        return makeError("platform.already_created", "a Platform already exists; only one may exist at a time");
    }

    if (!desc.videoDriver.empty()) {
        const std::string driver(desc.videoDriver);
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, driver.c_str(), SDL_HINT_OVERRIDE);
    }
    // Closing the last window is a close request like any other; the application decides
    // whether to quit (the editor asks to save first).
    SDL_SetHintWithPriority(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0", SDL_HINT_OVERRIDE);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Unexpected<Error> error = sdlError("platform.init_failed", "the video subsystem could not be initialized");
        SDL_Quit();
        state.mainThread.store(std::thread::id());
        return error;
    }

    Platform platform;
    platform.m_owner = true;
    // SDL stamps events with its own monotonic clock, which starts at SDL_Init.
    platform.m_clockOffsetNs = steadyNowNs() - static_cast<std::int64_t>(SDL_GetTicksNS());
    return platform;
}

Platform::Platform(Platform&& other) noexcept
    : m_owner(std::exchange(other.m_owner, false)),
      m_clockOffsetNs(other.m_clockOffsetNs),
      m_events(std::move(other.m_events)) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        const Platform discarded(std::move(*this));
        m_owner = std::exchange(other.m_owner, false);
        m_clockOffsetNs = other.m_clockOffsetNs;
        m_events = std::move(other.m_events);
    }
    return *this;
}

Platform::~Platform() {
    if (!m_owner) {
        return;
    }
    detail::PlatformState& state = detail::platformState();
    ITH_ASSERT_MSG(detail::isMainThread(), detail::kNotMainThread);
    ITH_ASSERT_MSG(state.windowCount.load(std::memory_order_relaxed) == 0,
                   "every Window must be destroyed before the Platform");
    // SDL_Quit closes the windows that are left, so none is alive afterwards.
    SDL_Quit();
    state.windowCount.store(0, std::memory_order_relaxed);
    state.mainThread.store(std::thread::id());
}

// Not const although it changes no member: creating a window changes the platform's state.
// NOLINTNEXTLINE(readability-make-member-function-const)
Expected<Window> Platform::createWindow(const WindowDesc& desc) {
    assertUsable();
    ITH_ASSERT_MSG(desc.width > 0 && desc.height > 0, "a window needs a positive width and height");

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    const std::string title(desc.title);
    SDL_Window* window = SDL_CreateWindow(title.c_str(), desc.width, desc.height, flags);
    if (window == nullptr) {
        return sdlError("platform.window_failed", "the window could not be created");
    }
    const WindowId id = SDL_GetWindowID(window);
    if (id == kNoWindow) {
        Unexpected<Error> error = sdlError("platform.window_failed", "the window has no id");
        SDL_DestroyWindow(window);
        return error;
    }
    detail::platformState().windowCount.fetch_add(1, std::memory_order_relaxed);
    return Window(id);
}

std::span<const Event> Platform::pumpEvents() {
    assertUsable();

    m_events.clear();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (std::optional<Event> translated = translate(event)) {
            translated->timestampNs =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(translated->timestampNs) + m_clockOffsetNs);
            m_events.push_back(*translated);
        }
    }
    return m_events;
}

std::string_view Platform::videoDriver() const {
    assertUsable();
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr ? std::string_view(driver) : std::string_view();
}

std::string Platform::clipboardText() const {
    assertUsable();
    char* text = SDL_GetClipboardText();
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    SDL_free(text);
    return result;
}

// NOLINTNEXTLINE(readability-make-member-function-const): writing the clipboard is not a const operation
Expected<void> Platform::setClipboardText(std::string_view text) {
    assertUsable();
    ITH_ASSERT_MSG(text.find('\0') == std::string_view::npos, "clipboard text must not contain NUL characters");
    const std::string copy(text);
    if (!SDL_SetClipboardText(copy.c_str())) {
        return sdlError("platform.clipboard_failed", "the clipboard could not be written");
    }
    return {};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads m_owner when asserts are on
void Platform::assertUsable() const {
    ITH_ASSERT_MSG(m_owner, "Platform used after it was moved from");
    ITH_ASSERT_MSG(detail::isMainThread(), detail::kNotMainThread);
}

} // namespace ith
