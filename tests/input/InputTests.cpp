// Input: routing, consumption, focus, capture, focus loss and player assignment, with events
// injected directly (ADR-0013 rule 14); no display is needed.

#include "ith/input/Input.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <ostream> // doctest prints std::string_view operands through it
#include <span>
#include <vector>

namespace {

constexpr ith::WindowId kWindow = 1;
constexpr ith::WindowId kOtherWindow = 2;
constexpr ith::DeviceId kPad = 11;
constexpr ith::DeviceId kOtherPad = 12;

// Records what reaches it and replies with `reply` (NotConsumed when empty).
class Recorder final : public ith::InputConsumer {
public:
    ith::InputReply onInputEvent(const ith::Event& event) override {
        seen.push_back(event);
        return reply ? reply(event) : ith::InputReply::NotConsumed;
    }

    std::size_t count(ith::EventType type) const {
        return static_cast<std::size_t>(std::ranges::count(seen, type, &ith::Event::type));
    }

    std::vector<ith::Event> seen;
    std::function<ith::InputReply(const ith::Event&)> reply;
};

ith::InputReply consumeAll(const ith::Event& /*event*/) {
    return ith::InputReply::Consumed;
}

ith::Event keyDown(ith::Key key, bool repeat = false, ith::WindowId window = kWindow) {
    return ith::Event{
        .type = ith::EventType::KeyDown, .timestampNs = 100, .window = window, .key = key, .repeat = repeat};
}

ith::Event keyUp(ith::Key key, ith::WindowId window = kWindow) {
    return ith::Event{.type = ith::EventType::KeyUp, .timestampNs = 200, .window = window, .key = key};
}

ith::Event mouseDown(ith::MouseButton button, ith::WindowId window = kWindow) {
    return ith::Event{.type = ith::EventType::MouseButtonDown, .window = window, .mouseButton = button};
}

ith::Event mouseUp(ith::MouseButton button, ith::WindowId window = kWindow) {
    return ith::Event{.type = ith::EventType::MouseButtonUp, .window = window, .mouseButton = button};
}

ith::Event mouseMove(float dx, float dy, ith::WindowId window = kWindow) {
    return ith::Event{.type = ith::EventType::MouseMoved, .window = window, .deltaX = dx, .deltaY = dy};
}

ith::Event focus(ith::EventType type, ith::WindowId window = kWindow) {
    return ith::Event{.type = type, .timestampNs = 500, .window = window};
}

ith::Event padConnected(ith::DeviceId pad) {
    return ith::Event{.type = ith::EventType::GamepadConnected,
                      .device = pad,
                      .text = "Test Pad",
                      .gamepadType = ith::GamepadType::PlayStation5,
                      .steamHandle = 77};
}

ith::Event padDown(ith::DeviceId pad, ith::GamepadButton button) {
    return ith::Event{.type = ith::EventType::GamepadButtonDown, .device = pad, .gamepadButton = button};
}

ith::Event padUp(ith::DeviceId pad, ith::GamepadButton button) {
    return ith::Event{.type = ith::EventType::GamepadButtonUp, .device = pad, .gamepadButton = button};
}

ith::Event padAxis(ith::DeviceId pad, ith::GamepadAxis axis, float value) {
    return ith::Event{.type = ith::EventType::GamepadAxisMoved, .device = pad, .gamepadAxis = axis, .axisValue = value};
}

void frame(ith::Input& input, std::initializer_list<ith::Event> events, std::uint64_t timeNs = 1000) {
    input.processEvents(std::span<const ith::Event>(events.begin(), events.size()), timeNs);
}

} // namespace

TEST_CASE("Events go down the stack top first: editor, UI, game, newest first within a layer") {
    ith::Input input;
    Recorder game;
    Recorder hud;
    Recorder modal;
    Recorder editor;
    // Added in scrambled order on purpose.
    const ith::ConsumerId gameId =
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Game, .consumer = &game});
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &hud});
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &editor});
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &modal});

    std::vector<const Recorder*> order;
    for (Recorder* recorder : {&game, &hud, &modal, &editor}) {
        recorder->reply = [&order, recorder](const ith::Event&) {
            order.push_back(recorder);
            return ith::InputReply::NotConsumed;
        };
    }
    frame(input, {keyDown(ith::Key::W)});
    CHECK(order == std::vector<const Recorder*>{&editor, &modal, &hud, &game});
    CHECK(input.consumerState(gameId).pressed(ith::Key::W));

    // Another window's stack sees nothing.
    Recorder other;
    input.addConsumer({.window = kOtherWindow, .consumer = &other});
    frame(input, {keyDown(ith::Key::A)});
    CHECK(other.seen.empty());
}

TEST_CASE("A consumed press hides the key from lower consumers until its release") {
    ith::Input input;
    Recorder ui;
    Recorder game;
    const ith::ConsumerId uiId =
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &ui});
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});

    ui.reply = consumeAll;
    frame(input, {keyDown(ith::Key::Enter)});
    CHECK(ui.count(ith::EventType::KeyDown) == 1);
    CHECK(game.seen.empty());
    CHECK_FALSE(input.consumerState(gameId).held(ith::Key::Enter));
    CHECK(input.state().held(ith::Key::Enter));

    // The UI stops consuming, but the held key stays its own: repeats and the release go to it.
    ui.reply = nullptr;
    frame(input, {keyDown(ith::Key::Enter, true), keyUp(ith::Key::Enter)});
    CHECK(game.seen.empty());
    CHECK(ui.count(ith::EventType::KeyDown) == 2);
    CHECK(ui.count(ith::EventType::KeyUp) == 1);
    CHECK(input.consumerState(uiId).released(ith::Key::Enter));

    // The next press reaches both.
    frame(input, {keyDown(ith::Key::Enter)});
    CHECK(game.count(ith::EventType::KeyDown) == 1);
}

TEST_CASE("A release reaches every consumer that saw the press, even after the stack changed") {
    ith::Input input;
    Recorder game;
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});
    frame(input, {mouseDown(ith::MouseButton::Left)});

    // A menu opens above the game and consumes everything.
    Recorder menu;
    menu.reply = consumeAll;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &menu});
    frame(input, {mouseUp(ith::MouseButton::Left)});
    CHECK(game.count(ith::EventType::MouseButtonUp) == 1);
    CHECK(menu.seen.empty()); // it never saw the press
    CHECK(input.consumerState(gameId).released(ith::MouseButton::Left));
}

TEST_CASE("Consumed motion stops; unconsumed motion reaches everyone") {
    ith::Input input;
    Recorder ui;
    Recorder game;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &ui});
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});

    frame(input, {mouseMove(1, 2), mouseMove(3, 4)});
    CHECK(input.consumerState(gameId).axis2D(ith::Axis2D::MouseDelta) == ith::InputVector2{4, 6});

    ui.reply = consumeAll;
    frame(input, {mouseMove(1, 1)});
    CHECK(input.consumerState(gameId).axis2D(ith::Axis2D::MouseDelta) == ith::InputVector2{});
    CHECK(input.state().axis2D(ith::Axis2D::MouseDelta) == ith::InputVector2{1, 1});
}

TEST_CASE("Focus loss releases everything with real events; held keys count again only when pressed") {
    ith::Input input;
    Recorder game;
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});
    frame(input, {focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::W), keyDown(ith::Key::LeftShift),
                  mouseDown(ith::MouseButton::Right)});

    frame(input, {focus(ith::EventType::WindowFocusLost)});
    CHECK(input.focusedWindow() == ith::kNoWindow);
    const ith::InputState& state = input.consumerState(gameId);
    CHECK(state.released(ith::Key::W));
    CHECK(state.released(ith::Key::LeftShift));
    CHECK(state.released(ith::MouseButton::Right));
    CHECK_FALSE(state.held(ith::Key::W));
    CHECK(game.count(ith::EventType::KeyUp) == 2);
    CHECK(game.count(ith::EventType::MouseButtonUp) == 1);
    // The releases come before the focus event, at its timestamp.
    const std::span<const ith::Event> events = input.events();
    REQUIRE(events.size() == 4);
    CHECK(events[0].type == ith::EventType::KeyUp);
    CHECK(events[0].timestampNs == 500);
    CHECK(events.back().type == ith::EventType::WindowFocusLost);

    // The operating system's own late releases are dropped: nothing is released twice.
    frame(input, {keyUp(ith::Key::W)});
    CHECK(input.events().empty());
    CHECK(game.count(ith::EventType::KeyUp) == 2);

    // Focus returns while Shift is still held: the press reported with the focus event and the
    // repeats of a held key are no presses.
    frame(input, {focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::LeftShift),
                  keyDown(ith::Key::LeftShift, true), mouseDown(ith::MouseButton::Right)});
    CHECK_FALSE(state.held(ith::Key::LeftShift));
    CHECK_FALSE(state.held(ith::MouseButton::Right));
    CHECK(game.count(ith::EventType::KeyDown) == 2);

    // Released and pressed again: a press.
    frame(input, {keyUp(ith::Key::LeftShift), keyDown(ith::Key::LeftShift)});
    CHECK(state.pressed(ith::Key::LeftShift));
    // A key that wasn't held at focus loss presses normally even in the frame focus returns.
    frame(input,
          {focus(ith::EventType::WindowFocusLost), focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::Q)});
    CHECK(state.pressed(ith::Key::Q));
}

// SDL's own order: on focus loss it releases the held keys first, then reports the focus loss;
// on X11 and Wayland it reports held modifiers as presses right after the focus returns.
TEST_CASE("Keys the platform released just before focus loss count again only when pressed") {
    ith::Input input;
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow});
    frame(input, {focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::LeftShift)});
    frame(input, {keyUp(ith::Key::LeftShift), focus(ith::EventType::WindowFocusLost)});
    frame(input, {focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::LeftShift)});
    const ith::InputState& state = input.consumerState(gameId);
    CHECK_FALSE(state.pressed(ith::Key::LeftShift));
    CHECK_FALSE(state.held(ith::Key::LeftShift));
    frame(input, {keyUp(ith::Key::LeftShift), keyDown(ith::Key::LeftShift)});
    CHECK(state.pressed(ith::Key::LeftShift));
}

// Ids beyond the tables (a newer recording, a malformed injected event) are ignored, never
// folded onto another control.
TEST_CASE("Out-of-range control ids change nothing") {
    ith::Input input;
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow});
    frame(input, {focus(ith::EventType::WindowFocusGained), keyDown(ith::Key::A), mouseDown(ith::MouseButton::Left)});
    frame(input, {keyUp(static_cast<ith::Key>(260)), mouseUp(static_cast<ith::MouseButton>(9))});
    CHECK(input.consumerState(gameId).held(ith::Key::A));
    CHECK(input.consumerState(gameId).held(ith::MouseButton::Left));
    ith::InputState state;
    state.apply(keyDown(static_cast<ith::Key>(260)));
    CHECK_FALSE(state.held(ith::Key::A));
}

TEST_CASE("Text goes only to the focused consumer that accepts text") {
    ith::Input input;
    Recorder field;
    Recorder game;
    const ith::ConsumerId fieldId =
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &field});
    input.addConsumer({.window = kWindow, .consumer = &game});
    const ith::Event text{.type = ith::EventType::TextInput, .window = kWindow, .text = "h\xC3\xA9"};

    frame(input, {text});
    CHECK(field.seen.empty());
    CHECK_FALSE(input.textInputWanted(kWindow));

    input.setFocusedConsumer(fieldId, true);
    CHECK(input.focusedConsumer(kWindow) == fieldId);
    CHECK(input.textInputWanted(kWindow));
    const ith::Event editing{.type = ith::EventType::TextEditing, .window = kWindow, .text = "k", .textCursor = 1};
    frame(input, {editing, text});
    REQUIRE(field.seen.size() == 2);
    CHECK(field.seen[0].type == ith::EventType::TextEditing);
    CHECK(field.seen[1].text == "h\xC3\xA9");
    CHECK(game.seen.empty());

    // Focus without text, then none.
    input.setFocusedConsumer(fieldId, false);
    CHECK_FALSE(input.textInputWanted(kWindow));
    input.clearFocusedConsumer(kWindow);
    CHECK(input.focusedConsumer(kWindow) == ith::kNoConsumer);
}

TEST_CASE("Mouse capture sends mouse input only to the game layer") {
    ith::Input input;
    Recorder ui;
    Recorder game;
    ui.reply = consumeAll;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &ui});
    input.addConsumer({.window = kWindow, .consumer = &game});

    // Capture needs focus.
    CHECK_FALSE(input.captureMouse(kWindow));
    frame(input, {focus(ith::EventType::WindowFocusGained)});
    CHECK(input.captureMouse(kWindow));
    CHECK(input.mouseCaptured(kWindow));
    CHECK_FALSE(input.mouseCaptured(kOtherWindow));

    frame(input, {mouseMove(5, 0), mouseDown(ith::MouseButton::Left), keyDown(ith::Key::E)});
    CHECK(ui.count(ith::EventType::MouseMoved) == 0);
    CHECK(ui.count(ith::EventType::MouseButtonDown) == 0);
    CHECK(game.count(ith::EventType::MouseMoved) == 1);
    CHECK(game.count(ith::EventType::MouseButtonDown) == 1);
    // The keyboard still goes through the whole stack.
    CHECK(ui.count(ith::EventType::KeyDown) == 1);
    CHECK(game.count(ith::EventType::KeyDown) == 0);

    // A UI layer asks for the cursor.
    input.releaseMouse();
    frame(input, {mouseMove(1, 0)});
    CHECK(ui.count(ith::EventType::MouseMoved) == 1);
    CHECK(game.count(ith::EventType::MouseMoved) == 1);

    // Focus loss ends capture.
    CHECK(input.captureMouse(kWindow));
    frame(input, {focus(ith::EventType::WindowFocusLost)});
    CHECK_FALSE(input.mouseCaptured(kWindow));
    CHECK(input.mouseCaptureWindow() == ith::kNoWindow);
}

TEST_CASE("Capture on click is off by default: the click reaches the game and captures nothing") {
    ith::Input input;
    Recorder game;
    input.addConsumer({.window = kWindow, .consumer = &game});
    CHECK_FALSE(input.captureOnClick(kWindow));
    frame(input, {focus(ith::EventType::WindowFocusGained), mouseDown(ith::MouseButton::Left)});
    CHECK_FALSE(input.mouseCaptured(kWindow));
    CHECK(game.count(ith::EventType::MouseButtonDown) == 1);
}

TEST_CASE("Capture on click: the capturing click is consumed and the game never sees it") {
    ith::Input input;
    Recorder hud; // sees the click, leaves it
    Recorder game;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &hud});
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});
    input.setCaptureOnClick(kWindow, true);
    CHECK(input.captureOnClick(kWindow));
    CHECK_FALSE(input.captureOnClick(kOtherWindow));

    // Without focus, a click doesn't capture and goes down the stack as usual.
    frame(input, {mouseDown(ith::MouseButton::Left), mouseUp(ith::MouseButton::Left)});
    CHECK_FALSE(input.mouseCaptured(kWindow));
    CHECK(game.count(ith::EventType::MouseButtonDown) == 1);
    game.seen.clear();
    hud.seen.clear();

    frame(input, {focus(ith::EventType::WindowFocusGained), mouseDown(ith::MouseButton::Left)});
    CHECK(input.mouseCaptured(kWindow));
    CHECK(hud.count(ith::EventType::MouseButtonDown) == 1);
    CHECK(game.seen.empty());
    CHECK(input.state().held(ith::MouseButton::Left));

    // Its release reaches the UI consumer that saw the press, never the game.
    frame(input, {mouseUp(ith::MouseButton::Left)});
    CHECK(hud.count(ith::EventType::MouseButtonUp) == 1);
    CHECK(game.seen.empty());
    CHECK_FALSE(input.consumerState(gameId).released(ith::MouseButton::Left));

    // Captured: the next click goes to the game only.
    frame(input, {mouseDown(ith::MouseButton::Left)});
    CHECK(game.count(ith::EventType::MouseButtonDown) == 1);
    CHECK(hud.count(ith::EventType::MouseButtonDown) == 1);

    // Turned off, a click after a release of the capture reaches everyone again.
    input.setCaptureOnClick(kWindow, false);
    input.releaseMouse();
    frame(input, {mouseUp(ith::MouseButton::Left), mouseDown(ith::MouseButton::Right)});
    CHECK_FALSE(input.mouseCaptured(kWindow));
    CHECK(game.count(ith::EventType::MouseButtonDown) == 2);
}

TEST_CASE("Capture on click: a click that a UI layer consumes doesn't capture") {
    ith::Input input;
    Recorder menu;
    Recorder game;
    menu.reply = consumeAll;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &menu});
    input.addConsumer({.window = kWindow, .consumer = &game});
    input.setCaptureOnClick(kWindow, true);
    frame(input, {focus(ith::EventType::WindowFocusGained), mouseDown(ith::MouseButton::Left)});
    CHECK_FALSE(input.mouseCaptured(kWindow));
    CHECK(menu.count(ith::EventType::MouseButtonDown) == 1);
    CHECK(game.seen.empty());
}

TEST_CASE("Single-player gives player 0 every device, including gamepads that connect later") {
    ith::Input input;
    REQUIRE(input.devices().size() == 1);
    CHECK(input.devices()[0].kind == ith::InputDeviceKind::KeyboardMouse);
    CHECK(input.playerOf(ith::kKeyboardMouse) == 0);

    frame(input, {padConnected(kPad)});
    const ith::InputDevice* pad = input.findDevice(kPad);
    REQUIRE(pad != nullptr);
    CHECK(pad->name == "Test Pad");
    CHECK(pad->gamepadType == ith::GamepadType::PlayStation5);
    CHECK(pad->steamHandle == 77);
    CHECK(pad->player == 0);
    CHECK(input.playerOf(42) == ith::kNoPlayer);
}

TEST_CASE("Gamepad events go to the focused window's stack, then to the device's player") {
    ith::Input input;
    Recorder player0;
    Recorder player1;
    Recorder lobby; // not bound to a player: sees every device, for joining
    const ith::ConsumerId p0 = input.addConsumer({.window = kWindow, .consumer = &player0, .player = 0});
    const ith::ConsumerId p1 = input.addConsumer({.window = kWindow, .consumer = &player1, .player = 1});
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &lobby});
    input.setLocalMultiplayer(true);
    CHECK(input.playerOf(ith::kKeyboardMouse) == ith::kNoPlayer);

    // No window has focus: gamepad input isn't routed, but the device state has it.
    frame(input, {padConnected(kPad), padConnected(kOtherPad), padDown(kPad, ith::GamepadButton::FaceSouth)});
    CHECK(lobby.count(ith::EventType::GamepadConnected) == 2);
    CHECK(lobby.count(ith::EventType::GamepadButtonDown) == 0);
    CHECK(input.state().held(kPad, ith::GamepadButton::FaceSouth));
    CHECK(input.playerOf(kPad) == ith::kNoPlayer);

    // Unassigned: only the unbound consumer sees the press, and assigns the pad to join.
    lobby.reply = [&](const ith::Event& event) {
        if (event.type == ith::EventType::GamepadButtonDown && input.playerOf(event.device) == ith::kNoPlayer) {
            input.assignDevice(event.device, event.device == kPad ? 0 : 1);
            return ith::InputReply::Consumed;
        }
        return ith::InputReply::NotConsumed;
    };
    frame(input, {focus(ith::EventType::WindowFocusGained), padDown(kOtherPad, ith::GamepadButton::Start)});
    CHECK(player0.seen.empty());
    CHECK(player1.seen.empty());
    CHECK(input.playerOf(kOtherPad) == 1);

    frame(input, {padUp(kOtherPad, ith::GamepadButton::Start), padDown(kOtherPad, ith::GamepadButton::FaceEast),
                  padAxis(kOtherPad, ith::GamepadAxis::LeftStickX, 0.5f)});
    CHECK(player1.count(ith::EventType::GamepadButtonDown) == 1);
    CHECK(player0.seen.empty());
    CHECK(input.consumerState(p1).axis(ith::GamepadAxis::LeftStickX) == 0.5f);
    CHECK(input.consumerState(p0).axis(ith::GamepadAxis::LeftStickX) == 0.0f);
    // The release of the joining press went to the consumer that took it.
    CHECK(lobby.count(ith::EventType::GamepadButtonUp) == 1);

    // Back to single-player: everything is player 0's.
    input.setLocalMultiplayer(false);
    CHECK(input.playerOf(kOtherPad) == 0);
    CHECK(input.playerOf(ith::kKeyboardMouse) == 0);
}

TEST_CASE("A disconnect releases the gamepad's buttons and axes before it is reported") {
    ith::Input input;
    Recorder game;
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});
    frame(input, {focus(ith::EventType::WindowFocusGained), padConnected(kPad),
                  padDown(kPad, ith::GamepadButton::FaceNorth), padAxis(kPad, ith::GamepadAxis::RightTrigger, 0.75f)});

    frame(input, {ith::Event{.type = ith::EventType::GamepadDisconnected, .timestampNs = 900, .device = kPad}});
    const std::span<const ith::Event> events = input.events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == ith::EventType::GamepadButtonUp);
    CHECK(events[0].timestampNs == 900);
    CHECK(events[1].type == ith::EventType::GamepadAxisMoved);
    CHECK(events[1].axisValue == 0.0f);
    CHECK(events[2].type == ith::EventType::GamepadDisconnected);
    REQUIRE(game.seen.size() >= 3);
    CHECK(game.seen.back().type == ith::EventType::GamepadDisconnected);
    CHECK(input.consumerState(gameId).released(kPad, ith::GamepadButton::FaceNorth));
    CHECK(input.consumerState(gameId).axis(kPad, ith::GamepadAxis::RightTrigger) == 0.0f);
    CHECK(input.findDevice(kPad) == nullptr);

    // A second disconnect, or events of an unknown pad's disconnect, are dropped.
    frame(input, {ith::Event{.type = ith::EventType::GamepadDisconnected, .device = kPad}});
    CHECK(input.events().empty());
}

TEST_CASE("Gamepad axis events are ignored while no window has focus") {
    ith::Input input;
    Recorder game;
    input.addConsumer({.window = kWindow, .consumer = &game});
    frame(input, {padConnected(kPad), padAxis(kPad, ith::GamepadAxis::LeftStickX, 0.5f)});
    CHECK(input.state().axis(kPad, ith::GamepadAxis::LeftStickX) == 0.0f);
    CHECK(game.count(ith::EventType::GamepadAxisMoved) == 0);
    CHECK(input.events().size() == 1); // only the connect

    frame(input, {focus(ith::EventType::WindowFocusGained), padAxis(kPad, ith::GamepadAxis::LeftStickX, 0.5f)});
    CHECK(input.state().axis(kPad, ith::GamepadAxis::LeftStickX) == 0.5f);
    // Focus loss sets the axis to 0; SDL's later centring events without focus change nothing.
    frame(input, {focus(ith::EventType::WindowFocusLost), padAxis(kPad, ith::GamepadAxis::LeftStickX, 0.25f)});
    CHECK(input.state().axis(kPad, ith::GamepadAxis::LeftStickX) == 0.0f);
    CHECK(game.seen.back().type == ith::EventType::GamepadAxisMoved);
    CHECK(game.seen.back().axisValue == 0.0f);
}

TEST_CASE("Reassigning a gamepad resets its axes for consumers that lose it, at the next frame") {
    ith::Input input;
    Recorder player0;
    Recorder player1;
    const ith::ConsumerId p0 = input.addConsumer({.window = kWindow, .consumer = &player0, .player = 0});
    const ith::ConsumerId p1 = input.addConsumer({.window = kWindow, .consumer = &player1, .player = 1});
    frame(input,
          {focus(ith::EventType::WindowFocusGained), padConnected(kPad),
           padAxis(kPad, ith::GamepadAxis::LeftStickX, 1.0f), padAxis(kPad, ith::GamepadAxis::LeftTrigger, 0.5f)});
    REQUIRE(input.consumerState(p0).axis(kPad, ith::GamepadAxis::LeftStickX) == 1.0f);

    // Reassigned from a consumer, during routing: nothing is routed until the next frame.
    player0.reply = [&](const ith::Event&) {
        input.assignDevice(kPad, 1);
        return ith::InputReply::NotConsumed;
    };
    frame(input, {keyDown(ith::Key::J)});
    player0.reply = nullptr;
    CHECK(input.consumerState(p0).axis(kPad, ith::GamepadAxis::LeftStickX) == 1.0f);

    const std::size_t before = player0.seen.size();
    frame(input, {}, 4000);
    CHECK(input.consumerState(p0).axis(kPad, ith::GamepadAxis::LeftStickX) == 0.0f);
    CHECK(input.consumerState(p0).axis(kPad, ith::GamepadAxis::LeftTrigger) == 0.0f);
    REQUIRE(player0.seen.size() == before + 2);
    CHECK(player0.seen.back().type == ith::EventType::GamepadAxisMoved);
    CHECK(player0.seen.back().timestampNs == 4000);
    CHECK(player1.count(ith::EventType::GamepadAxisMoved) == 0);
    CHECK(input.consumerState(p1).axis(kPad, ith::GamepadAxis::LeftStickX) == 0.0f);

    // The disconnect still reaches the consumer that knew the gamepad, after the resets.
    frame(input, {ith::Event{.type = ith::EventType::GamepadDisconnected, .device = kPad}});
    CHECK(player0.seen.back().type == ith::EventType::GamepadDisconnected);
    CHECK(player1.seen.back().type == ith::EventType::GamepadDisconnected);
}

TEST_CASE("A consumed axis resets lower consumers that still show a value") {
    ith::Input input;
    Recorder menu;
    Recorder game;
    frame(input, {focus(ith::EventType::WindowFocusGained), padConnected(kPad)});
    const ith::ConsumerId gameId = input.addConsumer({.window = kWindow, .consumer = &game});
    frame(input, {padAxis(kPad, ith::GamepadAxis::LeftStickY, 1.0f)});
    CHECK(input.consumerState(gameId).axis(ith::GamepadAxis::LeftStickY) == 1.0f);

    menu.reply = consumeAll;
    input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &menu});
    frame(input, {padAxis(kPad, ith::GamepadAxis::LeftStickY, 0.9f)});
    CHECK(input.consumerState(gameId).axis(ith::GamepadAxis::LeftStickY) == 0.0f);
    CHECK(game.seen.back().axisValue == 0.0f);
}

TEST_CASE("Consumers may add and remove consumers, themselves included, while events are routed") {
    ith::Input input;
    Recorder game;
    Recorder menu;
    menu.reply = consumeAll;
    Recorder once;
    ith::ConsumerId onceId = ith::kNoConsumer;
    once.reply = [&](const ith::Event&) {
        input.removeConsumer(onceId);
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Ui, .consumer = &menu});
        return ith::InputReply::NotConsumed;
    };
    input.addConsumer({.window = kWindow, .consumer = &game});
    onceId = input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &once});

    frame(input, {keyDown(ith::Key::Tab), keyDown(ith::Key::I)});
    CHECK(once.seen.size() == 1);
    // The menu joined during the first event and took the second.
    CHECK(game.count(ith::EventType::KeyDown) == 1);
    CHECK(menu.count(ith::EventType::KeyDown) == 1);
}

TEST_CASE("A consumer without a callback only builds its state") {
    ith::Input input;
    const ith::ConsumerId reader = input.addConsumer({.window = kWindow});
    frame(input, {keyDown(ith::Key::D), keyUp(ith::Key::D)});
    CHECK(input.consumerState(reader).pressed(ith::Key::D));
    CHECK(input.consumerState(reader).released(ith::Key::D));
    input.removeConsumer(reader);
    input.removeConsumer(reader); // unknown ids are ignored
}

TEST_CASE("The frame time is never earlier than the frame's events") {
    ith::Input input;
    frame(input, {}, 5000);
    CHECK(input.frameTimeNs() == 5000);
    frame(input, {ith::Event{.type = ith::EventType::LowMemory, .timestampNs = 7000}}, 6000);
    CHECK(input.frameTimeNs() == 7000);
    // Non-device events pass through to events() unchanged.
    REQUIRE(input.events().size() == 1);
    CHECK(input.events()[0].type == ith::EventType::LowMemory);
}

#ifdef ITH_ENABLE_ASSERTS
TEST_CASE("Input asserts its preconditions") {
    ith::Input input;
    const ith::test::AssertCapture capture;
    input.assignDevice(99, 0);
    CHECK(capture.count() == 1);
    static_cast<void>(input.consumerState(1234));
    CHECK(capture.count() == 2);
    static_cast<void>(input.addConsumer({.window = ith::kNoWindow}));
    CHECK(capture.count() == 3);
    input.setFocusedConsumer(1234);
    CHECK(capture.count() == 4);
}
#endif
