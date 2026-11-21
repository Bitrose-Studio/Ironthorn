// InputState: per-frame edges, repeats, sums and gamepad values built from injected events.

#include "ith/input/InputState.h"

#include <doctest/doctest.h>

#include <vector>

namespace {

ith::Event key(ith::EventType type, ith::Key k, bool repeat = false) {
    return ith::Event{.type = type, .window = 1, .key = k, .repeat = repeat};
}

ith::Event button(ith::EventType type, ith::MouseButton b) {
    return ith::Event{.type = type, .window = 1, .mouseButton = b};
}

ith::Event padButton(ith::EventType type, ith::DeviceId pad, ith::GamepadButton b) {
    return ith::Event{.type = type, .device = pad, .gamepadButton = b};
}

ith::Event padAxis(ith::DeviceId pad, ith::GamepadAxis axis, float value) {
    return ith::Event{.type = ith::EventType::GamepadAxisMoved, .device = pad, .gamepadAxis = axis, .axisValue = value};
}

} // namespace

TEST_CASE("A key press, hold and release report their edges once") {
    ith::InputState state;
    state.apply(key(ith::EventType::KeyDown, ith::Key::W));
    CHECK(state.pressed(ith::Key::W));
    CHECK(state.held(ith::Key::W));
    CHECK_FALSE(state.released(ith::Key::W));

    state.beginFrame();
    CHECK_FALSE(state.pressed(ith::Key::W));
    CHECK(state.held(ith::Key::W));

    state.beginFrame();
    state.apply(key(ith::EventType::KeyUp, ith::Key::W));
    CHECK(state.released(ith::Key::W));
    CHECK_FALSE(state.held(ith::Key::W));
    CHECK_FALSE(state.held(ith::Key::A));
}

TEST_CASE("A tap within one frame reports both the press and the release") {
    ith::InputState state;
    state.apply(key(ith::EventType::KeyDown, ith::Key::Space));
    state.apply(key(ith::EventType::KeyUp, ith::Key::Space));
    CHECK(state.pressed(ith::Key::Space));
    CHECK(state.released(ith::Key::Space));
    CHECK_FALSE(state.held(ith::Key::Space));

    // Release then press again in one frame: both edges, held at the end.
    state.beginFrame();
    state.apply(key(ith::EventType::KeyDown, ith::Key::Space));
    state.beginFrame();
    state.apply(key(ith::EventType::KeyUp, ith::Key::Space));
    state.apply(key(ith::EventType::KeyDown, ith::Key::Space));
    CHECK(state.pressed(ith::Key::Space));
    CHECK(state.released(ith::Key::Space));
    CHECK(state.held(ith::Key::Space));
}

TEST_CASE("Repeats are not presses, and stray edges are ignored") {
    ith::InputState state;
    state.apply(key(ith::EventType::KeyDown, ith::Key::A));
    state.beginFrame();
    state.apply(key(ith::EventType::KeyDown, ith::Key::A, true));
    state.apply(key(ith::EventType::KeyDown, ith::Key::A, true));
    CHECK_FALSE(state.pressed(ith::Key::A));
    CHECK(state.held(ith::Key::A));

    // A repeat of a key that isn't held doesn't press it.
    state.apply(key(ith::EventType::KeyDown, ith::Key::B, true));
    CHECK_FALSE(state.held(ith::Key::B));
    // A release of a key that isn't held reports nothing.
    state.apply(key(ith::EventType::KeyUp, ith::Key::C));
    CHECK_FALSE(state.released(ith::Key::C));
}

TEST_CASE("Mouse buttons, position, motion and wheel") {
    ith::InputState state;
    // A button event moves the position too; the motion below comes after it.
    state.apply(button(ith::EventType::MouseButtonDown, ith::MouseButton::Right));
    state.apply(
        ith::Event{.type = ith::EventType::MouseMoved, .window = 3, .x = 10, .y = 20, .deltaX = 2, .deltaY = -1});
    state.apply(
        ith::Event{.type = ith::EventType::MouseMoved, .window = 3, .x = 13, .y = 21, .deltaX = 3, .deltaY = 1.5f});
    state.apply(ith::Event{.type = ith::EventType::MouseWheel, .window = 3, .deltaY = 0.25f});
    state.apply(ith::Event{.type = ith::EventType::MouseWheel, .window = 3, .deltaX = -1, .deltaY = 0.5f});

    CHECK(state.mousePosition() == ith::InputVector2{13, 21});
    CHECK(state.mouseWindow() == 3);
    CHECK(state.axis2D(ith::Axis2D::MouseDelta) == ith::InputVector2{5, 0.5f});
    CHECK(state.axis(ith::MouseAxis::DeltaX) == 5);
    CHECK(state.axis2D(ith::Axis2D::MouseWheel) == ith::InputVector2{-1, 0.75f});
    CHECK(state.axis(ith::MouseAxis::WheelY) == 0.75f);
    CHECK(state.pressed(ith::MouseButton::Right));
    CHECK(state.held(ith::MouseButton::Right));

    state.beginFrame();
    CHECK(state.axis2D(ith::Axis2D::MouseDelta) == ith::InputVector2{});
    CHECK(state.axis2D(ith::Axis2D::MouseWheel) == ith::InputVector2{});
    CHECK(state.mousePosition() == ith::InputVector2{13, 21});
    state.apply(button(ith::EventType::MouseButtonUp, ith::MouseButton::Right));
    CHECK(state.released(ith::MouseButton::Right));
}

TEST_CASE("Modifiers follow the latest keyboard or mouse event") {
    ith::InputState state;
    state.apply(ith::Event{
        .type = ith::EventType::KeyDown, .key = ith::Key::LeftControl, .modifiers = ith::KeyModifiers::Control});
    CHECK(state.modifiers() == ith::KeyModifiers::Control);
    state.apply(ith::Event{.type = ith::EventType::MouseButtonDown,
                           .modifiers = ith::KeyModifiers::Control | ith::KeyModifiers::Shift,
                           .mouseButton = ith::MouseButton::Left});
    CHECK(ith::hasModifiers(state.modifiers(), ith::KeyModifiers::Shift));
}

TEST_CASE("Gamepad values are kept per device") {
    ith::InputState state;
    state.apply(ith::Event{.type = ith::EventType::GamepadConnected, .device = 7});
    state.apply(padButton(ith::EventType::GamepadButtonDown, 7, ith::GamepadButton::FaceSouth));
    state.apply(padAxis(7, ith::GamepadAxis::LeftStickX, 0.5f));
    state.apply(padAxis(7, ith::GamepadAxis::LeftStickY, -0.25f));
    state.apply(padAxis(9, ith::GamepadAxis::LeftStickX, -0.75f));
    state.apply(padAxis(9, ith::GamepadAxis::RightTrigger, 1.0f));

    CHECK(state.pressed(7, ith::GamepadButton::FaceSouth));
    CHECK_FALSE(state.held(9, ith::GamepadButton::FaceSouth));
    CHECK(state.held(ith::GamepadButton::FaceSouth));
    CHECK(state.axis(7, ith::GamepadAxis::LeftStickX) == 0.5f);
    CHECK(state.axis(9, ith::GamepadAxis::LeftStickX) == -0.75f);
    // Any gamepad: the value with the largest magnitude; the longest stick.
    CHECK(state.axis(ith::GamepadAxis::LeftStickX) == -0.75f);
    CHECK(state.axis2D(7, ith::Axis2D::GamepadLeftStick) == ith::InputVector2{0.5f, -0.25f});
    CHECK(state.axis2D(ith::Axis2D::GamepadLeftStick) == ith::InputVector2{-0.75f, 0.0f});
    CHECK(state.axis(42, ith::GamepadAxis::LeftStickX) == 0.0f);
    CHECK(state.gamepads() == std::vector<ith::DeviceId>{7, 9});

    // A disconnected gamepad stays readable for the rest of the frame.
    state.apply(padButton(ith::EventType::GamepadButtonUp, 7, ith::GamepadButton::FaceSouth));
    state.apply(ith::Event{.type = ith::EventType::GamepadDisconnected, .device = 7});
    CHECK(state.released(7, ith::GamepadButton::FaceSouth));
    state.beginFrame();
    CHECK(state.gamepads() == std::vector<ith::DeviceId>{9});
}
