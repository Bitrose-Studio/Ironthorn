#include "ith/platform/Platform.h"

#include "ith/foundation/Assert.h"
#include "ith/platform/PlatformState.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace ith {
namespace detail {

PlatformState& platformState() {
    static PlatformState state;
    return state;
}

Unexpected<Error> sdlError(const char* code, const char* what) {
    return makeError(code, std::string(what) + ": " + SDL_GetError());
}

} // namespace detail

namespace {

using detail::sdlError;

// Key values are USB HID usage ids, and so are SDL scancodes; these pin the equivalence that
// keyFromScancode relies on.
static_assert(static_cast<int>(Key::A) == SDL_SCANCODE_A);
static_assert(static_cast<int>(Key::Digit0) == SDL_SCANCODE_0);
static_assert(static_cast<int>(Key::Application) == SDL_SCANCODE_APPLICATION);
static_assert(static_cast<int>(Key::KeypadEquals) == SDL_SCANCODE_KP_EQUALS);
static_assert(static_cast<int>(Key::F24) == SDL_SCANCODE_F24);
static_assert(static_cast<int>(Key::KeypadComma) == SDL_SCANCODE_KP_COMMA);
static_assert(static_cast<int>(Key::International1) == SDL_SCANCODE_INTERNATIONAL1);
static_assert(static_cast<int>(Key::International5) == SDL_SCANCODE_INTERNATIONAL5);
static_assert(static_cast<int>(Key::Lang1) == SDL_SCANCODE_LANG1);
static_assert(static_cast<int>(Key::Lang2) == SDL_SCANCODE_LANG2);
static_assert(static_cast<int>(Key::LeftControl) == SDL_SCANCODE_LCTRL);
static_assert(static_cast<int>(Key::RightSuper) == SDL_SCANCODE_RGUI);

// The engine key at `scancode`, or Key::None for keys the engine doesn't name (media and system
// keys).
Key keyFromScancode(SDL_Scancode scancode) {
    const int code = static_cast<int>(scancode);
    const bool known = (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_APPLICATION) ||
                       (code >= SDL_SCANCODE_KP_EQUALS && code <= SDL_SCANCODE_F24) || code == SDL_SCANCODE_KP_COMMA ||
                       (code >= SDL_SCANCODE_INTERNATIONAL1 && code <= SDL_SCANCODE_INTERNATIONAL5) ||
                       code == SDL_SCANCODE_LANG1 || code == SDL_SCANCODE_LANG2 ||
                       (code >= SDL_SCANCODE_LCTRL && code <= SDL_SCANCODE_RGUI);
    return known ? static_cast<Key>(code) : Key::None;
}

KeyModifiers modifiersFrom(SDL_Keymod mod) {
    KeyModifiers result = KeyModifiers::None;
    const auto add = [&](SDL_Keymod bits, KeyModifiers modifier) {
        if ((mod & bits) != 0) {
            result = result | modifier;
        }
    };
    add(SDL_KMOD_SHIFT, KeyModifiers::Shift);
    add(SDL_KMOD_CTRL, KeyModifiers::Control);
    add(SDL_KMOD_ALT, KeyModifiers::Alt);
    add(SDL_KMOD_GUI, KeyModifiers::Super);
    add(SDL_KMOD_CAPS, KeyModifiers::CapsLock);
    add(SDL_KMOD_NUM, KeyModifiers::NumLock);
    return result;
}

// The character a key event's keycode stands for, or 0 for keys that print nothing. SDL reports
// key events' keycodes without modifiers, with latin letters on non-latin layouts.
char32_t layoutKeyFrom(SDL_Keycode keycode) {
    if ((keycode & (SDLK_SCANCODE_MASK | SDLK_EXTENDED_MASK)) != 0) {
        return 0;
    }
    // Control characters (Enter, Escape, Backspace, Tab, Delete) print nothing.
    if (keycode < 0x20 || keycode == 0x7F) {
        return 0;
    }
    return static_cast<char32_t>(keycode);
}

MouseButton mouseButtonFrom(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return MouseButton::Left;
        case SDL_BUTTON_RIGHT:
            return MouseButton::Right;
        case SDL_BUTTON_MIDDLE:
            return MouseButton::Middle;
        case SDL_BUTTON_X1:
            return MouseButton::Back;
        case SDL_BUTTON_X2:
            return MouseButton::Forward;
        default:
            return MouseButton::None;
    }
}

GamepadButton gamepadButtonFrom(Uint8 button) {
    switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return GamepadButton::FaceSouth;
        case SDL_GAMEPAD_BUTTON_EAST:
            return GamepadButton::FaceEast;
        case SDL_GAMEPAD_BUTTON_WEST:
            return GamepadButton::FaceWest;
        case SDL_GAMEPAD_BUTTON_NORTH:
            return GamepadButton::FaceNorth;
        case SDL_GAMEPAD_BUTTON_BACK:
            return GamepadButton::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE:
            return GamepadButton::Guide;
        case SDL_GAMEPAD_BUTTON_START:
            return GamepadButton::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return GamepadButton::LeftStickButton;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return GamepadButton::RightStickButton;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return GamepadButton::LeftShoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return GamepadButton::RightShoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return GamepadButton::DpadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return GamepadButton::DpadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return GamepadButton::DpadLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return GamepadButton::DpadRight;
        case SDL_GAMEPAD_BUTTON_MISC1:
            return GamepadButton::Misc1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
            return GamepadButton::RightPaddle1;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
            return GamepadButton::LeftPaddle1;
        case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
            return GamepadButton::RightPaddle2;
        case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
            return GamepadButton::LeftPaddle2;
        case SDL_GAMEPAD_BUTTON_TOUCHPAD:
            return GamepadButton::Touchpad;
        default:
            return GamepadButton::None;
    }
}

struct AxisValue {
    GamepadAxis axis = GamepadAxis::None;
    float value = 0.0f;
};

// The engine axis and its value in the engine's range: sticks -1..1 with up positive (SDL reports
// down positive), triggers 0..1.
AxisValue gamepadAxisFrom(Uint8 axis, Sint16 value) {
    const float stick = std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
    switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            return {GamepadAxis::LeftStickX, stick};
        case SDL_GAMEPAD_AXIS_LEFTY:
            return {GamepadAxis::LeftStickY, -stick};
        case SDL_GAMEPAD_AXIS_RIGHTX:
            return {GamepadAxis::RightStickX, stick};
        case SDL_GAMEPAD_AXIS_RIGHTY:
            return {GamepadAxis::RightStickY, -stick};
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            return {GamepadAxis::LeftTrigger, std::max(stick, 0.0f)};
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return {GamepadAxis::RightTrigger, std::max(stick, 0.0f)};
        default:
            return {};
    }
}

GamepadType gamepadTypeFrom(SDL_GamepadType type) {
    switch (type) {
        case SDL_GAMEPAD_TYPE_STANDARD:
            return GamepadType::Standard;
        case SDL_GAMEPAD_TYPE_XBOX360:
            return GamepadType::Xbox360;
        case SDL_GAMEPAD_TYPE_XBOXONE:
            return GamepadType::XboxOne;
        case SDL_GAMEPAD_TYPE_PS3:
            return GamepadType::PlayStation3;
        case SDL_GAMEPAD_TYPE_PS4:
            return GamepadType::PlayStation4;
        case SDL_GAMEPAD_TYPE_PS5:
            return GamepadType::PlayStation5;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            return GamepadType::SwitchPro;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            return GamepadType::SwitchJoyConLeft;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            return GamepadType::SwitchJoyConRight;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return GamepadType::SwitchJoyConPair;
        case SDL_GAMEPAD_TYPE_GAMECUBE:
            return GamepadType::GameCube;
        case SDL_GAMEPAD_TYPE_STEAM:
            return GamepadType::Steam;
        default:
            return GamepadType::Unknown;
    }
}

// Window pixels per window coordinate of `window`; 1 when the window is gone.
float pixelDensity(SDL_WindowID window) {
    SDL_Window* sdlWindow = SDL_GetWindowFromID(window);
    const float density = sdlWindow != nullptr ? SDL_GetWindowPixelDensity(sdlWindow) : 0.0f;
    return density > 0.0f ? density : 1.0f;
}

bool inRelativeMode(SDL_WindowID window) {
    SDL_Window* sdlWindow = SDL_GetWindowFromID(window);
    return sdlWindow != nullptr && SDL_GetWindowRelativeMouseMode(sdlWindow);
}

// An event translated from the library, with the library's text that belongs in `event.text`.
struct Translated {
    Event event;
    const char* text = nullptr;
};

Translated applicationEvent(EventType type, const SDL_Event& event) {
    return {.event = Event{.type = type, .timestampNs = event.common.timestamp}};
}

Translated windowEvent(EventType type, const SDL_WindowEvent& event) {
    return {.event = Event{.type = type, .timestampNs = event.timestamp, .window = event.windowID}};
}

std::optional<Translated> keyEvent(const SDL_KeyboardEvent& event) {
    const Key key = keyFromScancode(event.scancode);
    if (key == Key::None) {
        return std::nullopt;
    }
    return Translated{.event = Event{
                          .type = event.down ? EventType::KeyDown : EventType::KeyUp,
                          .timestampNs = event.timestamp,
                          .window = event.windowID,
                          .device = event.which,
                          .key = key,
                          .layoutKey = layoutKeyFrom(event.key),
                          .modifiers = modifiersFrom(event.mod),
                          .repeat = event.down && event.repeat,
                      }};
}

std::optional<Translated> mouseButtonEvent(const SDL_MouseButtonEvent& event, KeyModifiers modifiers) {
    const MouseButton button = mouseButtonFrom(event.button);
    if (button == MouseButton::None) {
        return std::nullopt;
    }
    const float density = pixelDensity(event.windowID);
    return Translated{.event = Event{
                          .type = event.down ? EventType::MouseButtonDown : EventType::MouseButtonUp,
                          .timestampNs = event.timestamp,
                          .window = event.windowID,
                          .device = event.which,
                          .modifiers = modifiers,
                          .mouseButton = button,
                          .clicks = event.down ? event.clicks : std::uint8_t{0},
                          .x = event.x * density,
                          .y = event.y * density,
                      }};
}

Translated mouseMotionEvent(const SDL_MouseMotionEvent& event) {
    const float density = pixelDensity(event.windowID);
    // In relative mode the motion is the mouse's own, not a distance in the window.
    const float motionScale = inRelativeMode(event.windowID) ? 1.0f : density;
    return {.event = Event{
                .type = EventType::MouseMoved,
                .timestampNs = event.timestamp,
                .window = event.windowID,
                .device = event.which,
                .x = event.x * density,
                .y = event.y * density,
                .deltaX = event.xrel * motionScale,
                .deltaY = event.yrel * motionScale,
            }};
}

Translated mouseWheelEvent(const SDL_MouseWheelEvent& event, KeyModifiers modifiers) {
    const float density = pixelDensity(event.windowID);
    const float direction = event.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
    return {.event = Event{
                .type = EventType::MouseWheel,
                .timestampNs = event.timestamp,
                .window = event.windowID,
                .device = event.which,
                .modifiers = modifiers,
                .x = event.mouse_x * density,
                .y = event.mouse_y * density,
                .deltaX = event.x * direction,
                .deltaY = event.y * direction,
                .wheelFlipped = event.direction == SDL_MOUSEWHEEL_FLIPPED,
            }};
}

// Opens the gamepad, so that its events arrive and its outputs work, and describes it. Nothing
// when it can't be opened.
std::optional<Translated> gamepadAdded(const SDL_GamepadDeviceEvent& event) {
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(event.which);
    if (gamepad == nullptr) {
        gamepad = SDL_OpenGamepad(event.which);
    }
    if (gamepad == nullptr) {
        return std::nullopt;
    }
    Translated translated{.event =
                              Event{
                                  .type = EventType::GamepadConnected,
                                  .timestampNs = event.timestamp,
                                  .device = event.which,
                                  .gamepadType = gamepadTypeFrom(SDL_GetGamepadType(gamepad)),
                                  .steamHandle = SDL_GetGamepadSteamHandle(gamepad),
                              },
                          .text = SDL_GetGamepadName(gamepad)};
    const SDL_GUID guid = SDL_GetGamepadGUIDForID(event.which);
    std::ranges::copy(guid.data, translated.event.gamepadGuid.bytes.begin());
    return translated;
}

// Closes the gamepad. Nothing for a gamepad that was never opened.
std::optional<Translated> gamepadRemoved(const SDL_GamepadDeviceEvent& event) {
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(event.which);
    if (gamepad == nullptr) {
        return std::nullopt;
    }
    SDL_CloseGamepad(gamepad);
    return Translated{.event = Event{
                          .type = EventType::GamepadDisconnected,
                          .timestampNs = event.timestamp,
                          .device = event.which,
                      }};
}

std::optional<Translated> gamepadButtonEvent(const SDL_GamepadButtonEvent& event) {
    const GamepadButton button = gamepadButtonFrom(event.button);
    if (button == GamepadButton::None) {
        return std::nullopt;
    }
    return Translated{.event = Event{
                          .type = event.down ? EventType::GamepadButtonDown : EventType::GamepadButtonUp,
                          .timestampNs = event.timestamp,
                          .device = event.which,
                          .gamepadButton = button,
                      }};
}

std::optional<Translated> gamepadAxisEvent(const SDL_GamepadAxisEvent& event) {
    const AxisValue axis = gamepadAxisFrom(event.axis, event.value);
    if (axis.axis == GamepadAxis::None) {
        return std::nullopt;
    }
    return Translated{.event = Event{
                          .type = EventType::GamepadAxisMoved,
                          .timestampNs = event.timestamp,
                          .device = event.which,
                          .gamepadAxis = axis.axis,
                          .axisValue = axis.value,
                      }};
}

Translated textEvent(const SDL_TextInputEvent& event) {
    return {.event = Event{.type = EventType::TextInput, .timestampNs = event.timestamp, .window = event.windowID},
            .text = event.text};
}

Translated textEditingEvent(const SDL_TextEditingEvent& event) {
    return {.event = Event{.type = EventType::TextEditing,
                           .timestampNs = event.timestamp,
                           .window = event.windowID,
                           .textCursor = event.start,
                           .textSelectionLength = event.length},
            .text = event.text};
}

// `modifiers` follows the key events in queue order, so mouse events get the modifiers held when
// they happened, not those at the time of the pump (SDL's mouse events carry none).
std::optional<Translated> translate(const SDL_Event& event, KeyModifiers& modifiers) {
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
            Translated resized = windowEvent(event.type == SDL_EVENT_WINDOW_RESIZED ? EventType::WindowResized
                                                                                    : EventType::WindowPixelSizeChanged,
                                             event.window);
            resized.event.width = event.window.data1;
            resized.event.height = event.window.data2;
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
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            modifiers = modifiersFrom(event.key.mod);
            return keyEvent(event.key);
        case SDL_EVENT_TEXT_INPUT:
            return textEvent(event.text);
        case SDL_EVENT_TEXT_EDITING:
            return textEditingEvent(event.edit);
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return mouseButtonEvent(event.button, modifiers);
        case SDL_EVENT_MOUSE_MOTION:
            return mouseMotionEvent(event.motion);
        case SDL_EVENT_MOUSE_WHEEL:
            return mouseWheelEvent(event.wheel, modifiers);
        case SDL_EVENT_GAMEPAD_ADDED:
            return gamepadAdded(event.gdevice);
        case SDL_EVENT_GAMEPAD_REMOVED:
            return gamepadRemoved(event.gdevice);
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return gamepadButtonEvent(event.gbutton);
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return gamepadAxisEvent(event.gaxis);
        default:
            return std::nullopt;
    }
}

// Nanoseconds of std::chrono::steady_clock now.
std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The open, connected gamepad with session id `gamepad`.
Expected<SDL_Gamepad*> connectedGamepad(DeviceId gamepad) {
    SDL_Gamepad* result = SDL_GetGamepadFromID(gamepad);
    if (result == nullptr || !SDL_GamepadConnected(result)) {
        return makeError("platform.gamepad_not_found", "no connected gamepad has the id " + std::to_string(gamepad));
    }
    return result;
}

Uint16 motorStrength(float strength) {
    ITH_ASSERT_MSG(strength >= 0.0f && strength <= 1.0f, "rumble strengths are within 0..1");
    return static_cast<Uint16>(std::lround(std::clamp(strength, 0.0f, 1.0f) * 65535.0f));
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
    // Gamepads are optional: without them the application still gets its windows, keyboard and
    // mouse, and the gamepad functions report "platform.gamepad_not_found".
    static_cast<void>(SDL_InitSubSystem(SDL_INIT_GAMEPAD));

    Platform platform;
    platform.m_owner = true;
    // SDL stamps events with its own monotonic clock, which starts at SDL_Init.
    platform.m_clockOffsetNs = steadyNowNs() - static_cast<std::int64_t>(SDL_GetTicksNS());
    return platform;
}

Platform::Platform(Platform&& other) noexcept
    : m_owner(std::exchange(other.m_owner, false)),
      m_clockOffsetNs(other.m_clockOffsetNs),
      m_pumpTimeNs(other.m_pumpTimeNs),
      m_modifiers(other.m_modifiers),
      m_events(std::move(other.m_events)),
      m_text(std::move(other.m_text)) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        const Platform discarded(std::move(*this));
        m_owner = std::exchange(other.m_owner, false);
        m_clockOffsetNs = other.m_clockOffsetNs;
        m_pumpTimeNs = other.m_pumpTimeNs;
        m_modifiers = other.m_modifiers;
        m_events = std::move(other.m_events);
        m_text = std::move(other.m_text);
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
    // SDL_Quit closes the windows and gamepads that are left, so none is alive afterwards.
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
    m_text.clear();

    // Text is gathered first and the views are set at the end, because m_text may move while it
    // grows.
    struct TextRange {
        std::size_t event = 0;
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    std::vector<TextRange> textRanges;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        std::optional<Translated> translated = translate(event, m_modifiers);
        if (!translated) {
            continue;
        }
        translated->event.timestampNs =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(translated->event.timestampNs) + m_clockOffsetNs);
        if (translated->text != nullptr) {
            const std::string_view text(translated->text);
            textRanges.push_back({.event = m_events.size(), .offset = m_text.size(), .size = text.size()});
            m_text.append(text);
        }
        m_events.push_back(translated->event);
    }
    for (const TextRange& range : textRanges) {
        m_events[range.event].text = std::string_view(m_text).substr(range.offset, range.size);
    }
    m_pumpTimeNs = static_cast<std::uint64_t>(steadyNowNs());
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

std::string Platform::keyDisplayName(Key key) const {
    assertUsable();
    if (key == Key::None) {
        return {};
    }
    const SDL_Keycode keycode = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(key), SDL_KMOD_NONE, false);
    if (keycode == SDLK_UNKNOWN) {
        return {};
    }
    // The name lives in temporary memory: copy it at once.
    const char* name = SDL_GetKeyName(keycode);
    return name != nullptr ? std::string(name) : std::string();
}

// NOLINTNEXTLINE(readability-make-member-function-const): drives a device
Expected<void> Platform::rumbleGamepad(DeviceId gamepad, float lowFrequency, float highFrequency,
                                       std::uint32_t durationMs) {
    assertUsable();
    const Uint16 low = motorStrength(lowFrequency);
    const Uint16 high = motorStrength(highFrequency);
    Expected<SDL_Gamepad*> device = connectedGamepad(gamepad);
    if (!device) {
        return makeUnexpected(std::move(device).error());
    }
    if (!SDL_RumbleGamepad(*device, low, high, durationMs)) {
        return sdlError("platform.gamepad_output_failed", "the gamepad could not rumble");
    }
    return {};
}

// NOLINTNEXTLINE(readability-make-member-function-const): drives a device
Expected<void> Platform::rumbleGamepadTriggers(DeviceId gamepad, float left, float right, std::uint32_t durationMs) {
    assertUsable();
    const Uint16 leftStrength = motorStrength(left);
    const Uint16 rightStrength = motorStrength(right);
    Expected<SDL_Gamepad*> device = connectedGamepad(gamepad);
    if (!device) {
        return makeUnexpected(std::move(device).error());
    }
    if (!SDL_RumbleGamepadTriggers(*device, leftStrength, rightStrength, durationMs)) {
        return sdlError("platform.gamepad_output_failed", "the gamepad's triggers could not rumble");
    }
    return {};
}

// NOLINTNEXTLINE(readability-make-member-function-const): drives a device
Expected<void> Platform::setGamepadLed(DeviceId gamepad, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    assertUsable();
    Expected<SDL_Gamepad*> device = connectedGamepad(gamepad);
    if (!device) {
        return makeUnexpected(std::move(device).error());
    }
    if (!SDL_SetGamepadLED(*device, red, green, blue)) {
        return sdlError("platform.gamepad_output_failed", "the gamepad's LED could not be set");
    }
    return {};
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads m_owner when asserts are on
void Platform::assertUsable() const {
    ITH_ASSERT_MSG(m_owner, "Platform used after it was moved from");
    ITH_ASSERT_MSG(detail::isMainThread(), detail::kNotMainThread);
}

} // namespace ith
