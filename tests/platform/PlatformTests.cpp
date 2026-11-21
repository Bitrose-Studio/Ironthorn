// The platform layer on SDL's headless "offscreen" video driver, so no display is needed. Events
// the offscreen driver can't produce (close requests, focus, lifecycle) are pushed into SDL's
// queue the way the operating system's driver would, to check their translation. The tests rely on
// ith::WindowId being SDL's window id, to address pushed events and to look windows up in SDL.

#include "ith/platform/Platform.h"

#include "AssertCapture.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

ith::Platform createHeadless() {
    ith::Expected<ith::Platform> platform = ith::Platform::create({.videoDriver = "offscreen"});
    REQUIRE_MESSAGE(platform.hasValue(), (platform ? "" : platform.error().message.c_str()));
    return std::move(*platform);
}

ith::Window createWindow(ith::Platform& platform, const ith::WindowDesc& desc = {}) {
    ith::Expected<ith::Window> window = platform.createWindow(desc);
    REQUIRE_MESSAGE(window.hasValue(), (window ? "" : window.error().message.c_str()));
    return std::move(*window);
}

void pushWindowEvent(SDL_EventType type, ith::WindowId window, int data1 = 0, int data2 = 0) {
    SDL_Event event{};
    event.window.type = type;
    event.window.windowID = window;
    event.window.data1 = data1;
    event.window.data2 = data2;
    REQUIRE(SDL_PushEvent(&event));
}

void pushApplicationEvent(SDL_EventType type) {
    SDL_Event event{};
    event.type = type;
    REQUIRE(SDL_PushEvent(&event));
}

std::vector<ith::Event> pump(ith::Platform& platform) {
    const std::span<const ith::Event> events = platform.pumpEvents();
    return {events.begin(), events.end()};
}

bool contains(const std::vector<ith::Event>& events, ith::EventType type, ith::WindowId window) {
    return std::ranges::any_of(events, [&](const ith::Event& e) { return e.type == type && e.window == window; });
}

std::vector<std::string> compiledVideoDrivers() {
    std::vector<std::string> drivers;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
        drivers.emplace_back(SDL_GetVideoDriver(i));
    }
    return drivers;
}

} // namespace

TEST_CASE("The platform's video drivers are compiled in") {
    const std::vector<std::string> drivers = compiledVideoDrivers();
    const auto has = [&](const char* name) { return std::ranges::find(drivers, name) != drivers.end(); };
    CHECK(has("offscreen"));
#if defined(_WIN32)
    CHECK(has("windows"));
#elif defined(__linux__)
    // SDL drops X11 or Wayland silently when their development packages were missing at
    // configure time (docs/development/building.md); a shipped build needs both.
    CHECK(has("x11"));
    CHECK(has("wayland"));
#endif
}

TEST_CASE("Platform initializes the requested video driver") {
    ith::Platform platform = createHeadless();
    CHECK(platform.videoDriver() == "offscreen");
}

TEST_CASE("Platform reports an unknown video driver with the library's message") {
    const ith::Expected<ith::Platform> platform = ith::Platform::create({.videoDriver = "no_such_driver"});
    REQUIRE_FALSE(platform.hasValue());
    CHECK(platform.error().code == "platform.init_failed");
    CHECK(platform.error().message.size() > std::string("the video subsystem could not be initialized: ").size());

    // A failed attempt leaves nothing behind: the next one succeeds.
    ith::Platform retry = createHeadless();
    CHECK(retry.videoDriver() == "offscreen");
}

TEST_CASE("A window reports its size, pixel size and display scale") {
    ith::Platform platform = createHeadless();
    {
        const ith::Window window = createWindow(platform, {.title = "Test", .width = 640, .height = 480});
        CHECK(window.id() != ith::kNoWindow);
        CHECK(window.size() == ith::WindowSize{.width = 640, .height = 480});
        const ith::WindowSize pixels = window.pixelSize();
        CHECK(pixels.width >= 640);
        CHECK(pixels.height >= 480);
        CHECK(window.displayScale() > 0.0f);
        // The offscreen driver has no operating system window.
        CHECK(std::holds_alternative<std::monostate>(window.nativeHandle()));
    }
    {
        const ith::Window fixed = createWindow(platform, {.width = 1, .height = 1, .resizable = false});
        CHECK(fixed.size() == ith::WindowSize{.width = 1, .height = 1});
    }
}

TEST_CASE("Windows have distinct ids and moving one transfers it") {
    ith::Platform platform = createHeadless();
    ith::Window first = createWindow(platform);
    const ith::Window second = createWindow(platform);
    CHECK(first.id() != second.id());

    const ith::WindowId id = first.id();
    ith::Window moved = std::move(first);
    CHECK(moved.id() == id);
    CHECK(first.id() == ith::kNoWindow); // NOLINT(bugprone-use-after-move): the moved-from state is specified
    CHECK(moved.size() == ith::WindowSize{.width = 1280, .height = 720});

    // Assigning over a window closes the one it held.
    moved = createWindow(platform, {.width = 320, .height = 200});
    CHECK(moved.id() != id);
    CHECK(SDL_GetWindowFromID(id) == nullptr);
}

TEST_CASE("Resizing a window delivers resize events and updates its size") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform, {.width = 640, .height = 480});
    static_cast<void>(pump(platform));

    REQUIRE(SDL_SetWindowSize(SDL_GetWindowFromID(window.id()), 800, 600));
    REQUIRE(SDL_SyncWindow(SDL_GetWindowFromID(window.id())));
    const std::vector<ith::Event> events = pump(platform);

    const auto resized = std::ranges::find(events, ith::EventType::WindowResized, &ith::Event::type);
    REQUIRE(resized != events.end());
    CHECK(resized->window == window.id());
    CHECK(resized->width == 800);
    CHECK(resized->height == 600);
    CHECK(window.size() == ith::WindowSize{.width = 800, .height = 600});
}

TEST_CASE("The event pump translates window events in order, with timestamps") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, window.id(), 2560, 1440);
    pushWindowEvent(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_MINIMIZED, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_RESTORED, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_MAXIMIZED, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, window.id());
    pushWindowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED, window.id());
    // Events the engine doesn't use are dropped.
    pushWindowEvent(SDL_EVENT_WINDOW_MOUSE_ENTER, window.id());

    const std::vector<ith::Event> events = pump(platform);
    const std::vector<ith::EventType> expected{
        ith::EventType::WindowFocusGained,
        ith::EventType::WindowPixelSizeChanged,
        ith::EventType::WindowDisplayScaleChanged,
        ith::EventType::WindowMinimized,
        ith::EventType::WindowRestored,
        ith::EventType::WindowMaximized,
        ith::EventType::WindowFocusLost,
        ith::EventType::WindowCloseRequested,
    };
    REQUIRE(events.size() == expected.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        CHECK(events[i].type == expected[i]);
        CHECK(events[i].window == window.id());
        CHECK(events[i].timestampNs > 0);
        if (i > 0) {
            CHECK(events[i].timestampNs >= events[i - 1].timestampNs);
        }
    }
    CHECK(events[1].width == 2560);
    CHECK(events[1].height == 1440);
    CHECK(events[0].width == 0);

    // A close request leaves the window open, and the next pump starts empty.
    CHECK(window.size().width > 0);
    CHECK(pump(platform).empty());
}

TEST_CASE("The event pump translates application events") {
    ith::Platform platform = createHeadless();
    static_cast<void>(pump(platform));

    pushApplicationEvent(SDL_EVENT_LOW_MEMORY);
    pushApplicationEvent(SDL_EVENT_WILL_ENTER_BACKGROUND);
    pushApplicationEvent(SDL_EVENT_DID_ENTER_FOREGROUND);
    pushApplicationEvent(SDL_EVENT_DISPLAY_ADDED);
    pushApplicationEvent(SDL_EVENT_DISPLAY_REMOVED);
    pushApplicationEvent(SDL_EVENT_QUIT);

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 6);
    CHECK(events[0].type == ith::EventType::LowMemory);
    CHECK(events[1].type == ith::EventType::EnteringBackground);
    CHECK(events[2].type == ith::EventType::EnteredForeground);
    CHECK(events[3].type == ith::EventType::DisplayAdded);
    CHECK(events[4].type == ith::EventType::DisplayRemoved);
    CHECK(events[5].type == ith::EventType::QuitRequested);
    for (const ith::Event& event : events) {
        CHECK(event.window == ith::kNoWindow);
    }
}

TEST_CASE("Event timestamps are on std::chrono::steady_clock") {
    ith::Platform platform = createHeadless();
    static_cast<void>(pump(platform));

    const auto now = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    const std::uint64_t before = now();
    pushApplicationEvent(SDL_EVENT_LOW_MEMORY);
    const std::uint64_t after = now();

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 1);
    // The two clocks are matched once, at initialization, which takes a few microseconds.
    constexpr std::uint64_t kToleranceNs = 1'000'000;
    CHECK(events[0].timestampNs + kToleranceNs >= before);
    CHECK(events[0].timestampNs <= after + kToleranceNs);
}

TEST_CASE("The clipboard round-trips UTF-8 text") {
    ith::Platform platform = createHeadless();
    const std::string text = "Ironthorn \xC3\xA9\xE2\x82\xAC";
    REQUIRE(platform.setClipboardText(text).hasValue());
    CHECK(platform.clipboardText() == text);

    // Empty text empties the clipboard.
    REQUIRE(platform.setClipboardText("").hasValue());
    CHECK(platform.clipboardText().empty());
}

TEST_CASE("A moved Platform keeps working") {
    ith::Platform first = createHeadless();
    ith::Platform second = std::move(first);
    const ith::Window window = createWindow(second);
    pushWindowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED, window.id());
    CHECK(contains(pump(second), ith::EventType::WindowCloseRequested, window.id()));

    // Move-assigning into a moved-from Platform hands the platform back.
    first = std::move(second);
    pushWindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, window.id());
    CHECK(contains(pump(first), ith::EventType::WindowFocusGained, window.id()));

    // Self-move-assignment keeps it.
    ith::Platform& alias = first;
    first = std::move(alias);
    CHECK(first.videoDriver() == "offscreen");
}

TEST_CASE("Self-move-assignment keeps a window open") {
    ith::Platform platform = createHeadless();
    ith::Window window = createWindow(platform, {.width = 320, .height = 200});
    const ith::WindowId id = window.id();
    ith::Window& alias = window;
    window = std::move(alias);
    CHECK(window.id() == id);
    CHECK(window.size() == ith::WindowSize{.width = 320, .height = 200});
}

TEST_CASE("Keyboard events carry the physical key, the layout key, modifiers and repeats") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    const auto pushKey = [&](bool down, SDL_Scancode scancode, SDL_Keycode keycode, SDL_Keymod mod, bool repeat) {
        SDL_Event event{};
        event.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = window.id();
        event.key.which = 5;
        event.key.scancode = scancode;
        event.key.key = keycode;
        event.key.mod = mod;
        event.key.down = down;
        event.key.repeat = repeat;
        REQUIRE(SDL_PushEvent(&event));
    };
    // The key left of S on AZERTY prints 'q'; its physical key is still Key::A.
    pushKey(true, SDL_SCANCODE_A, SDLK_Q, SDL_KMOD_LSHIFT | SDL_KMOD_RCTRL, false);
    pushKey(true, SDL_SCANCODE_A, SDLK_Q, SDL_KMOD_NONE, true);
    pushKey(false, SDL_SCANCODE_A, SDLK_Q, SDL_KMOD_NUM, false);
    pushKey(true, SDL_SCANCODE_RETURN, SDLK_RETURN, SDL_KMOD_NONE, false);
    pushKey(true, SDL_SCANCODE_F5, SDLK_F5, SDL_KMOD_NONE, false);
    // Keys the engine doesn't name (media keys) are dropped.
    pushKey(true, SDL_SCANCODE_MEDIA_PLAY, SDLK_MEDIA_PLAY, SDL_KMOD_NONE, false);

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 5);
    CHECK(events[0].type == ith::EventType::KeyDown);
    CHECK(events[0].window == window.id());
    CHECK(events[0].device == 5);
    CHECK(events[0].key == ith::Key::A);
    CHECK(events[0].layoutKey == U'q');
    CHECK(events[0].modifiers == (ith::KeyModifiers::Shift | ith::KeyModifiers::Control));
    CHECK_FALSE(events[0].repeat);
    CHECK(events[1].repeat);
    CHECK(events[2].type == ith::EventType::KeyUp);
    CHECK(events[2].modifiers == ith::KeyModifiers::NumLock);
    CHECK(events[3].key == ith::Key::Enter);
    CHECK(events[3].layoutKey == 0);
    CHECK(events[4].key == ith::Key::F5);
    CHECK(events[4].layoutKey == 0);
}

TEST_CASE("Key display names come from the keyboard layout") {
    ith::Platform platform = createHeadless();
    // The offscreen driver uses SDL's default US layout.
    CHECK(platform.keyDisplayName(ith::Key::W) == "W");
    CHECK_FALSE(platform.keyDisplayName(ith::Key::LeftShift).empty());
    CHECK(platform.keyDisplayName(ith::Key::None).empty());
}

TEST_CASE("Mouse events carry buttons, positions in pixels, motion and fractional wheel steps") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform, {.width = 400, .height = 300});
    static_cast<void>(pump(platform));
    const float density = static_cast<float>(window.pixelSize().width) / static_cast<float>(window.size().width);

    SDL_Event button{};
    button.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.windowID = window.id();
    button.button.button = SDL_BUTTON_X1;
    button.button.down = true;
    button.button.clicks = 2;
    button.button.x = 10.0f;
    button.button.y = 20.0f;
    REQUIRE(SDL_PushEvent(&button));
    button.button.type = SDL_EVENT_MOUSE_BUTTON_UP;
    button.button.button = SDL_BUTTON_RIGHT;
    button.button.down = false;
    REQUIRE(SDL_PushEvent(&button));

    SDL_Event motion{};
    motion.motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = window.id();
    motion.motion.x = 30.0f;
    motion.motion.y = 40.0f;
    motion.motion.xrel = 2.5f;
    motion.motion.yrel = -1.0f;
    REQUIRE(SDL_PushEvent(&motion));

    SDL_Event wheel{};
    wheel.wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.windowID = window.id();
    wheel.wheel.x = 0.0f;
    wheel.wheel.y = 0.25f;
    REQUIRE(SDL_PushEvent(&wheel));
    // A flipped wheel (natural scrolling) is reported in the same direction as a normal one.
    wheel.wheel.y = -0.5f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    REQUIRE(SDL_PushEvent(&wheel));

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 5);
    CHECK(events[0].type == ith::EventType::MouseButtonDown);
    CHECK(events[0].mouseButton == ith::MouseButton::Back);
    CHECK(events[0].clicks == 2);
    CHECK(events[0].x == doctest::Approx(10.0f * density));
    CHECK(events[0].y == doctest::Approx(20.0f * density));
    CHECK(events[1].type == ith::EventType::MouseButtonUp);
    CHECK(events[1].mouseButton == ith::MouseButton::Right);
    CHECK(events[1].clicks == 0);
    CHECK(events[2].type == ith::EventType::MouseMoved);
    CHECK(events[2].x == doctest::Approx(30.0f * density));
    CHECK(events[2].deltaX == doctest::Approx(2.5f * density));
    CHECK(events[2].deltaY == doctest::Approx(-1.0f * density));
    CHECK(events[3].type == ith::EventType::MouseWheel);
    CHECK(events[3].deltaY == 0.25f);
    CHECK_FALSE(events[3].wheelFlipped);
    CHECK(events[4].deltaY == 0.5f);
    CHECK(events[4].wheelFlipped);
}

TEST_CASE("Mouse events carry the modifiers of the key events before them in the queue") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    const auto pushCtrl = [&](bool down) {
        SDL_Event key{};
        key.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        key.key.windowID = window.id();
        key.key.scancode = SDL_SCANCODE_LCTRL;
        key.key.key = SDLK_LCTRL;
        key.key.mod = down ? SDL_KMOD_LCTRL : SDL_KMOD_NONE;
        key.key.down = down;
        REQUIRE(SDL_PushEvent(&key));
    };
    const auto pushClick = [&] {
        SDL_Event button{};
        button.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        button.button.windowID = window.id();
        button.button.button = SDL_BUTTON_LEFT;
        button.button.down = true;
        REQUIRE(SDL_PushEvent(&button));
    };
    pushCtrl(true);
    pushClick();
    pushCtrl(false);
    pushClick();

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 4);
    CHECK(events[1].modifiers == ith::KeyModifiers::Control);
    CHECK(events[3].modifiers == ith::KeyModifiers::None);
}

TEST_CASE("Text and IME composition events carry their UTF-8 text") {
    ith::Platform platform = createHeadless();
    const ith::Window window = createWindow(platform);
    static_cast<void>(pump(platform));

    // Long enough to make the platform's text storage grow while the frame is gathered.
    const std::string longText(200, 'x');
    SDL_Event text{};
    text.text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = window.id();
    text.text.text = "\xC3\xA9t\xC3\xA9";
    REQUIRE(SDL_PushEvent(&text));
    SDL_Event editing{};
    editing.edit.type = SDL_EVENT_TEXT_EDITING;
    editing.edit.windowID = window.id();
    editing.edit.text = "\xE3\x81\x8B";
    editing.edit.start = 1;
    editing.edit.length = -1;
    REQUIRE(SDL_PushEvent(&editing));
    text.text.text = longText.c_str();
    REQUIRE(SDL_PushEvent(&text));

    const std::vector<ith::Event> events = pump(platform);
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == ith::EventType::TextInput);
    CHECK(events[0].window == window.id());
    CHECK(events[0].text == "\xC3\xA9t\xC3\xA9");
    CHECK(events[1].type == ith::EventType::TextEditing);
    CHECK(events[1].text == "\xE3\x81\x8B");
    CHECK(events[1].textCursor == 1);
    CHECK(events[1].textSelectionLength == -1);
    CHECK(events[2].text == longText);
}

TEST_CASE("Windows switch text input and relative mouse mode") {
    ith::Platform platform = createHeadless();
    ith::Window window = createWindow(platform);

    CHECK_FALSE(window.textInputActive());
    REQUIRE(window.startTextInput().hasValue());
    CHECK(window.textInputActive());
    REQUIRE(window.startTextInput().hasValue()); // again: nothing changes
    CHECK(window.setTextInputArea({.x = 10, .y = 20, .width = 200, .height = 30, .cursor = 5}).hasValue());
    window.stopTextInput();
    CHECK_FALSE(window.textInputActive());
    window.stopTextInput();

    // The offscreen driver may not support relative mode; either way the result is consistent.
    const ith::Expected<void> relative = window.setRelativeMouseMode(true);
    if (relative) {
        CHECK(window.relativeMouseMode());
        REQUIRE(window.setRelativeMouseMode(false).hasValue());
    } else {
        CHECK(relative.error().code == "platform.relative_mouse_failed");
    }
    CHECK_FALSE(window.relativeMouseMode());
}

namespace {

// What a virtual gamepad's output callbacks received.
struct VirtualPadOutput {
    Uint16 low = 0;
    Uint16 high = 0;
    Uint16 leftTrigger = 0;
    Uint16 rightTrigger = 0;
    Uint8 red = 0;
    Uint8 green = 0;
    Uint8 blue = 0;
};

SDL_JoystickID attachVirtualPad(VirtualPadOutput& output) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Ironthorn Test Pad";
    desc.userdata = &output;
    desc.Rumble = [](void* userdata, Uint16 low, Uint16 high) {
        auto* out = static_cast<VirtualPadOutput*>(userdata);
        out->low = low;
        out->high = high;
        return true;
    };
    desc.RumbleTriggers = [](void* userdata, Uint16 left, Uint16 right) {
        auto* out = static_cast<VirtualPadOutput*>(userdata);
        out->leftTrigger = left;
        out->rightTrigger = right;
        return true;
    };
    desc.SetLED = [](void* userdata, Uint8 red, Uint8 green, Uint8 blue) {
        auto* out = static_cast<VirtualPadOutput*>(userdata);
        out->red = red;
        out->green = green;
        out->blue = blue;
        return true;
    };
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    REQUIRE_MESSAGE(id != 0, SDL_GetError());
    return id;
}

} // namespace

TEST_CASE("Gamepads connect, report buttons and axes, take output and disconnect") {
    // No window exists, so SDL delivers gamepad events without focus.
    ith::Platform platform = createHeadless();
    static_cast<void>(pump(platform));
    VirtualPadOutput output;
    const SDL_JoystickID id = attachVirtualPad(output);

    std::vector<ith::Event> events = pump(platform);
    const auto connected = std::ranges::find(events, ith::EventType::GamepadConnected, &ith::Event::type);
    REQUIRE(connected != events.end());
    CHECK(connected->device == id);
    CHECK(connected->text == "Ironthorn Test Pad");
    CHECK(connected->timestampNs > 0);
    CHECK(connected->gamepadGuid != ith::GamepadGuid{});

    SDL_Joystick* joystick = SDL_GetJoystickFromID(id);
    REQUIRE(joystick != nullptr);
    REQUIRE(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN));
    REQUIRE(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX));
    events = pump(platform);
    const auto down = std::ranges::find(events, ith::EventType::GamepadButtonDown, &ith::Event::type);
    REQUIRE(down != events.end());
    CHECK(down->device == id);
    CHECK(down->gamepadButton == ith::GamepadButton::FaceSouth);
    const auto axisValue = [&](ith::GamepadAxis axis) -> std::optional<float> {
        for (const ith::Event& event : events) {
            if (event.type == ith::EventType::GamepadAxisMoved && event.gamepadAxis == axis) {
                return event.axisValue;
            }
        }
        return std::nullopt;
    };
    // Stick up is positive; the full range is clamped to -1..1, triggers to 0..1.
    CHECK(axisValue(ith::GamepadAxis::LeftStickY) == 1.0f);
    CHECK(axisValue(ith::GamepadAxis::RightTrigger) == 1.0f);

    REQUIRE(platform.rumbleGamepad(id, 1.0f, 0.5f, 100).hasValue());
    CHECK(output.low == 65535);
    CHECK(output.high == 32768);
    REQUIRE(platform.rumbleGamepadTriggers(id, 0.0f, 1.0f, 100).hasValue());
    CHECK(output.rightTrigger == 65535);
    REQUIRE(platform.setGamepadLed(id, 255, 128, 0).hasValue());
    CHECK(output.red == 255);
    CHECK(output.green == 128);

    REQUIRE(SDL_DetachVirtualJoystick(id));
    events = pump(platform);
    const auto disconnected = std::ranges::find(events, ith::EventType::GamepadDisconnected, &ith::Event::type);
    REQUIRE(disconnected != events.end());
    CHECK(disconnected->device == id);

    const ith::Expected<void> gone = platform.rumbleGamepad(id, 1.0f, 1.0f, 100);
    REQUIRE_FALSE(gone.hasValue());
    CHECK(gone.error().code == "platform.gamepad_not_found");
    CHECK(platform.setGamepadLed(12345, 1, 2, 3).error().code == "platform.gamepad_not_found");
}

#ifdef ITH_ENABLE_ASSERTS
TEST_CASE("Platform asserts its preconditions") {
    ith::Platform platform = createHeadless();

    SUBCASE("a window needs a positive size") {
        const ith::test::AssertCapture capture;
        // What happens after the assert is up to SDL; the window, if any, is closed here.
        static_cast<void>(platform.createWindow({.width = 0, .height = 480}));
        CHECK(capture.count() == 1);
    }
    SUBCASE("window calls belong on the main thread") {
        const ith::Window window = createWindow(platform);
        const ith::test::AssertCapture capture;
        // SDL is called off the main thread here only because the main thread is blocked in
        // join() meanwhile, so nothing runs concurrently.
        std::thread([&] { static_cast<void>(window.size()); }).join();
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage().find("thread that created the Platform") != std::string::npos);
    }
    SUBCASE("only one Platform may exist") {
        const ith::test::AssertCapture capture;
        const ith::Expected<ith::Platform> second = ith::Platform::create({.videoDriver = "offscreen"});
        CHECK(capture.count() == 1);
        REQUIRE_FALSE(second.hasValue());
        CHECK(second.error().code == "platform.already_created");
        // The first is untouched.
        CHECK(platform.videoDriver() == "offscreen");
    }
    SUBCASE("clipboard text has no NUL character") {
        const ith::test::AssertCapture capture;
        static_cast<void>(platform.setClipboardText(std::string_view("a\0b", 3)));
        CHECK(capture.count() == 1);
    }
    SUBCASE("a moved-from Platform can't be used") {
        ith::Platform other = std::move(platform);
        const ith::test::AssertCapture capture;
        static_cast<void>(platform.videoDriver()); // NOLINT(bugprone-use-after-move): tested on purpose
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "Platform used after it was moved from");
        platform = std::move(other);
    }
    SUBCASE("a moved-from Window can't be used") {
        ith::Window window = createWindow(platform);
        const ith::Window other = std::move(window);
        const ith::test::AssertCapture capture;
        static_cast<void>(window.size()); // NOLINT(bugprone-use-after-move): tested on purpose
        CHECK(capture.count() == 1);
        CHECK(capture.lastMessage() == "Window used after it was moved from");
    }
}

TEST_CASE("A Window that outlives its Platform only asserts") {
    std::optional<ith::Window> window;
    {
        const ith::test::AssertCapture capture;
        {
            ith::Platform platform = createHeadless();
            window = createWindow(platform);
        } // ~Platform asserts, then shuts SDL down, which closes the window.
        CHECK(capture.count() == 1);
        CHECK(window->size() == ith::WindowSize{});
        CHECK(capture.count() == 2);
        CHECK(capture.lastMessage() == "Window used after its Platform was destroyed");
        window.reset();
        CHECK(capture.count() == 3);
    }
    // No window is counted any more: a new Platform shuts down without asserting.
    const ith::test::AssertCapture capture;
    { const ith::Platform platform = createHeadless(); }
    CHECK(capture.count() == 0);
}
#endif
