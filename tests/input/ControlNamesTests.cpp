// The stable ids and names of input controls (ADR-0013 rule 12). The pinned values and names below
// must never change: saved bindings and recordings hold them. If one of these checks fails, the
// change broke compatibility; add a new value or name instead.

#include "ith/input/ControlNames.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through it
#include <set>
#include <string>
#include <variant>

TEST_CASE("Control ids keep their values") {
    CHECK(static_cast<int>(ith::Key::A) == 4);
    CHECK(static_cast<int>(ith::Key::W) == 26);
    CHECK(static_cast<int>(ith::Key::Digit1) == 30);
    CHECK(static_cast<int>(ith::Key::Escape) == 41);
    CHECK(static_cast<int>(ith::Key::Space) == 44);
    CHECK(static_cast<int>(ith::Key::F1) == 58);
    CHECK(static_cast<int>(ith::Key::Up) == 82);
    CHECK(static_cast<int>(ith::Key::Keypad0) == 98);
    CHECK(static_cast<int>(ith::Key::Lang2) == 145);
    CHECK(static_cast<int>(ith::Key::LeftShift) == 225);
    CHECK(static_cast<int>(ith::Key::RightSuper) == 231);

    CHECK(static_cast<int>(ith::MouseButton::Left) == 1);
    CHECK(static_cast<int>(ith::MouseButton::Right) == 2);
    CHECK(static_cast<int>(ith::MouseButton::Middle) == 3);
    CHECK(static_cast<int>(ith::MouseButton::Forward) == 5);

    CHECK(static_cast<int>(ith::GamepadButton::FaceSouth) == 1);
    CHECK(static_cast<int>(ith::GamepadButton::Start) == 7);
    CHECK(static_cast<int>(ith::GamepadButton::DpadRight) == 15);
    CHECK(static_cast<int>(ith::GamepadButton::Touchpad) == 21);

    CHECK(static_cast<int>(ith::GamepadAxis::LeftStickX) == 1);
    CHECK(static_cast<int>(ith::GamepadAxis::RightTrigger) == 6);
    CHECK(static_cast<int>(ith::MouseAxis::DeltaX) == 1);
    CHECK(static_cast<int>(ith::MouseAxis::WheelY) == 4);
    CHECK(static_cast<int>(ith::Axis2D::MouseDelta) == 1);
    CHECK(static_cast<int>(ith::Axis2D::GamepadRightStick) == 4);

    CHECK(static_cast<int>(ith::GamepadType::XboxOne) == 3);
    CHECK(static_cast<int>(ith::GamepadType::PlayStation5) == 6);
    CHECK(static_cast<int>(ith::GamepadType::Steam) == 12);
}

TEST_CASE("Controls keep their names") {
    CHECK(ith::controlName(ith::Key::W) == "Keyboard.W");
    CHECK(ith::controlName(ith::Key::Digit1) == "Keyboard.Digit1");
    CHECK(ith::controlName(ith::Key::LeftShift) == "Keyboard.LeftShift");
    CHECK(ith::controlName(ith::Key::KeypadEnter) == "Keyboard.KeypadEnter");
    CHECK(ith::controlName(ith::MouseButton::Left) == "Mouse.Left");
    CHECK(ith::controlName(ith::MouseButton::Back) == "Mouse.Back");
    CHECK(ith::controlName(ith::GamepadButton::FaceSouth) == "Gamepad.FaceSouth");
    CHECK(ith::controlName(ith::GamepadButton::LeftStickButton) == "Gamepad.LeftStickButton");
    CHECK(ith::controlName(ith::GamepadAxis::LeftStickX) == "Gamepad.LeftStickX");
    CHECK(ith::controlName(ith::GamepadAxis::RightTrigger) == "Gamepad.RightTrigger");
    CHECK(ith::controlName(ith::MouseAxis::DeltaY) == "Mouse.DeltaY");
    CHECK(ith::controlName(ith::MouseAxis::WheelY) == "Mouse.WheelY");
    CHECK(ith::controlName(ith::Axis2D::MouseDelta) == "Mouse.Delta");
    CHECK(ith::controlName(ith::Axis2D::MouseWheel) == "Mouse.Wheel");
    CHECK(ith::controlName(ith::Axis2D::GamepadLeftStick) == "Gamepad.LeftStick");
    CHECK(ith::controlName(ith::Axis2D::GamepadRightStick) == "Gamepad.RightStick");
}

TEST_CASE("Names find their controls") {
    CHECK(ith::findControl("Keyboard.W") == ith::Control(ith::Key::W));
    CHECK(ith::findControl("Mouse.Left") == ith::Control(ith::MouseButton::Left));
    CHECK(ith::findControl("Mouse.Delta") == ith::Control(ith::Axis2D::MouseDelta));
    CHECK(ith::findControl("Mouse.WheelY") == ith::Control(ith::MouseAxis::WheelY));
    CHECK(ith::findControl("Gamepad.FaceSouth") == ith::Control(ith::GamepadButton::FaceSouth));
    CHECK(ith::findControl("Gamepad.LeftStick") == ith::Control(ith::Axis2D::GamepadLeftStick));
    CHECK(ith::findControl("Gamepad.LeftStickX") == ith::Control(ith::GamepadAxis::LeftStickX));

    // Unknown names, wrong case and the None values have no control.
    CHECK_FALSE(ith::findControl("Keyboard.w").has_value());
    CHECK_FALSE(ith::findControl("Keyboard.").has_value());
    CHECK_FALSE(ith::findControl("Keyboard.None").has_value());
    CHECK_FALSE(ith::findControl("Joystick.FaceSouth").has_value());
    CHECK_FALSE(ith::findControl("").has_value());
    CHECK(ith::controlName(ith::Key::None).empty());
    CHECK(ith::controlName(ith::GamepadAxis::None).empty());
    CHECK(ith::controlName(static_cast<ith::Key>(200)).empty());
}

TEST_CASE("Every named key round-trips and names are unique") {
    std::set<std::string, std::less<>> names;
    int named = 0;
    for (int value = 0; value < static_cast<int>(ith::kKeyTableSize); ++value) {
        const auto key = static_cast<ith::Key>(value);
        const std::string_view name = ith::controlName(key);
        if (name.empty()) {
            continue;
        }
        ++named;
        CHECK(name.starts_with("Keyboard."));
        CHECK(names.insert(std::string(name)).second);
        CHECK(ith::findControl(name) == ith::Control(key));
    }
    CHECK(named == 127);
    for (int value = 1; value < static_cast<int>(ith::kGamepadButtonCount); ++value) {
        const auto button = static_cast<ith::GamepadButton>(value);
        const std::string_view name = ith::controlName(button);
        REQUIRE_FALSE(name.empty());
        CHECK(names.insert(std::string(name)).second);
        CHECK(ith::findControl(name) == ith::Control(button));
    }
    for (int value = 1; value < static_cast<int>(ith::kGamepadAxisCount); ++value) {
        const auto axis = static_cast<ith::GamepadAxis>(value);
        const std::string_view name = ith::controlName(axis);
        REQUIRE_FALSE(name.empty());
        CHECK(names.insert(std::string(name)).second);
        CHECK(ith::findControl(name) == ith::Control(axis));
    }
}

TEST_CASE("Key modifiers combine as flags") {
    const ith::KeyModifiers set = ith::KeyModifiers::Control | ith::KeyModifiers::Shift;
    CHECK(ith::hasModifiers(set, ith::KeyModifiers::Control));
    CHECK(ith::hasModifiers(set, ith::KeyModifiers::Control | ith::KeyModifiers::Shift));
    CHECK_FALSE(ith::hasModifiers(set, ith::KeyModifiers::Alt));
    CHECK(static_cast<int>(ith::KeyModifiers::Shift) == 1);
    CHECK(static_cast<int>(ith::KeyModifiers::NumLock) == 32);
}
