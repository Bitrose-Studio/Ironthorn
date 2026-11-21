#include "ith/input/ControlNames.h"

#include <array>
#include <span>
#include <utility>

namespace ith {
namespace {

template <typename Enum>
struct NamedControl {
    Enum value;
    std::string_view name;
};

// The tables are the single source of the names. They are small, so lookups scan them; name
// lookups happen when configuration is loaded, not per frame.
constexpr std::array kKeyNames = std::to_array<NamedControl<Key>>({
    {Key::A, "Keyboard.A"},
    {Key::B, "Keyboard.B"},
    {Key::C, "Keyboard.C"},
    {Key::D, "Keyboard.D"},
    {Key::E, "Keyboard.E"},
    {Key::F, "Keyboard.F"},
    {Key::G, "Keyboard.G"},
    {Key::H, "Keyboard.H"},
    {Key::I, "Keyboard.I"},
    {Key::J, "Keyboard.J"},
    {Key::K, "Keyboard.K"},
    {Key::L, "Keyboard.L"},
    {Key::M, "Keyboard.M"},
    {Key::N, "Keyboard.N"},
    {Key::O, "Keyboard.O"},
    {Key::P, "Keyboard.P"},
    {Key::Q, "Keyboard.Q"},
    {Key::R, "Keyboard.R"},
    {Key::S, "Keyboard.S"},
    {Key::T, "Keyboard.T"},
    {Key::U, "Keyboard.U"},
    {Key::V, "Keyboard.V"},
    {Key::W, "Keyboard.W"},
    {Key::X, "Keyboard.X"},
    {Key::Y, "Keyboard.Y"},
    {Key::Z, "Keyboard.Z"},
    {Key::Digit1, "Keyboard.Digit1"},
    {Key::Digit2, "Keyboard.Digit2"},
    {Key::Digit3, "Keyboard.Digit3"},
    {Key::Digit4, "Keyboard.Digit4"},
    {Key::Digit5, "Keyboard.Digit5"},
    {Key::Digit6, "Keyboard.Digit6"},
    {Key::Digit7, "Keyboard.Digit7"},
    {Key::Digit8, "Keyboard.Digit8"},
    {Key::Digit9, "Keyboard.Digit9"},
    {Key::Digit0, "Keyboard.Digit0"},
    {Key::Enter, "Keyboard.Enter"},
    {Key::Escape, "Keyboard.Escape"},
    {Key::Backspace, "Keyboard.Backspace"},
    {Key::Tab, "Keyboard.Tab"},
    {Key::Space, "Keyboard.Space"},
    {Key::Minus, "Keyboard.Minus"},
    {Key::Equals, "Keyboard.Equals"},
    {Key::LeftBracket, "Keyboard.LeftBracket"},
    {Key::RightBracket, "Keyboard.RightBracket"},
    {Key::Backslash, "Keyboard.Backslash"},
    {Key::NonUsHash, "Keyboard.NonUsHash"},
    {Key::Semicolon, "Keyboard.Semicolon"},
    {Key::Apostrophe, "Keyboard.Apostrophe"},
    {Key::Grave, "Keyboard.Grave"},
    {Key::Comma, "Keyboard.Comma"},
    {Key::Period, "Keyboard.Period"},
    {Key::Slash, "Keyboard.Slash"},
    {Key::CapsLock, "Keyboard.CapsLock"},
    {Key::F1, "Keyboard.F1"},
    {Key::F2, "Keyboard.F2"},
    {Key::F3, "Keyboard.F3"},
    {Key::F4, "Keyboard.F4"},
    {Key::F5, "Keyboard.F5"},
    {Key::F6, "Keyboard.F6"},
    {Key::F7, "Keyboard.F7"},
    {Key::F8, "Keyboard.F8"},
    {Key::F9, "Keyboard.F9"},
    {Key::F10, "Keyboard.F10"},
    {Key::F11, "Keyboard.F11"},
    {Key::F12, "Keyboard.F12"},
    {Key::PrintScreen, "Keyboard.PrintScreen"},
    {Key::ScrollLock, "Keyboard.ScrollLock"},
    {Key::Pause, "Keyboard.Pause"},
    {Key::Insert, "Keyboard.Insert"},
    {Key::Home, "Keyboard.Home"},
    {Key::PageUp, "Keyboard.PageUp"},
    {Key::Delete, "Keyboard.Delete"},
    {Key::End, "Keyboard.End"},
    {Key::PageDown, "Keyboard.PageDown"},
    {Key::Right, "Keyboard.Right"},
    {Key::Left, "Keyboard.Left"},
    {Key::Down, "Keyboard.Down"},
    {Key::Up, "Keyboard.Up"},
    {Key::NumLock, "Keyboard.NumLock"},
    {Key::KeypadDivide, "Keyboard.KeypadDivide"},
    {Key::KeypadMultiply, "Keyboard.KeypadMultiply"},
    {Key::KeypadMinus, "Keyboard.KeypadMinus"},
    {Key::KeypadPlus, "Keyboard.KeypadPlus"},
    {Key::KeypadEnter, "Keyboard.KeypadEnter"},
    {Key::Keypad1, "Keyboard.Keypad1"},
    {Key::Keypad2, "Keyboard.Keypad2"},
    {Key::Keypad3, "Keyboard.Keypad3"},
    {Key::Keypad4, "Keyboard.Keypad4"},
    {Key::Keypad5, "Keyboard.Keypad5"},
    {Key::Keypad6, "Keyboard.Keypad6"},
    {Key::Keypad7, "Keyboard.Keypad7"},
    {Key::Keypad8, "Keyboard.Keypad8"},
    {Key::Keypad9, "Keyboard.Keypad9"},
    {Key::Keypad0, "Keyboard.Keypad0"},
    {Key::KeypadPeriod, "Keyboard.KeypadPeriod"},
    {Key::NonUsBackslash, "Keyboard.NonUsBackslash"},
    {Key::Application, "Keyboard.Application"},
    {Key::KeypadEquals, "Keyboard.KeypadEquals"},
    {Key::F13, "Keyboard.F13"},
    {Key::F14, "Keyboard.F14"},
    {Key::F15, "Keyboard.F15"},
    {Key::F16, "Keyboard.F16"},
    {Key::F17, "Keyboard.F17"},
    {Key::F18, "Keyboard.F18"},
    {Key::F19, "Keyboard.F19"},
    {Key::F20, "Keyboard.F20"},
    {Key::F21, "Keyboard.F21"},
    {Key::F22, "Keyboard.F22"},
    {Key::F23, "Keyboard.F23"},
    {Key::F24, "Keyboard.F24"},
    {Key::KeypadComma, "Keyboard.KeypadComma"},
    {Key::International1, "Keyboard.International1"},
    {Key::International2, "Keyboard.International2"},
    {Key::International3, "Keyboard.International3"},
    {Key::International4, "Keyboard.International4"},
    {Key::International5, "Keyboard.International5"},
    {Key::Lang1, "Keyboard.Lang1"},
    {Key::Lang2, "Keyboard.Lang2"},
    {Key::LeftControl, "Keyboard.LeftControl"},
    {Key::LeftShift, "Keyboard.LeftShift"},
    {Key::LeftAlt, "Keyboard.LeftAlt"},
    {Key::LeftSuper, "Keyboard.LeftSuper"},
    {Key::RightControl, "Keyboard.RightControl"},
    {Key::RightShift, "Keyboard.RightShift"},
    {Key::RightAlt, "Keyboard.RightAlt"},
    {Key::RightSuper, "Keyboard.RightSuper"},
});

constexpr std::array kMouseButtonNames = std::to_array<NamedControl<MouseButton>>({
    {MouseButton::Left, "Mouse.Left"},
    {MouseButton::Right, "Mouse.Right"},
    {MouseButton::Middle, "Mouse.Middle"},
    {MouseButton::Back, "Mouse.Back"},
    {MouseButton::Forward, "Mouse.Forward"},
});

constexpr std::array kGamepadButtonNames = std::to_array<NamedControl<GamepadButton>>({
    {GamepadButton::FaceSouth, "Gamepad.FaceSouth"},
    {GamepadButton::FaceEast, "Gamepad.FaceEast"},
    {GamepadButton::FaceWest, "Gamepad.FaceWest"},
    {GamepadButton::FaceNorth, "Gamepad.FaceNorth"},
    {GamepadButton::Back, "Gamepad.Back"},
    {GamepadButton::Guide, "Gamepad.Guide"},
    {GamepadButton::Start, "Gamepad.Start"},
    {GamepadButton::LeftStickButton, "Gamepad.LeftStickButton"},
    {GamepadButton::RightStickButton, "Gamepad.RightStickButton"},
    {GamepadButton::LeftShoulder, "Gamepad.LeftShoulder"},
    {GamepadButton::RightShoulder, "Gamepad.RightShoulder"},
    {GamepadButton::DpadUp, "Gamepad.DpadUp"},
    {GamepadButton::DpadDown, "Gamepad.DpadDown"},
    {GamepadButton::DpadLeft, "Gamepad.DpadLeft"},
    {GamepadButton::DpadRight, "Gamepad.DpadRight"},
    {GamepadButton::Misc1, "Gamepad.Misc1"},
    {GamepadButton::RightPaddle1, "Gamepad.RightPaddle1"},
    {GamepadButton::LeftPaddle1, "Gamepad.LeftPaddle1"},
    {GamepadButton::RightPaddle2, "Gamepad.RightPaddle2"},
    {GamepadButton::LeftPaddle2, "Gamepad.LeftPaddle2"},
    {GamepadButton::Touchpad, "Gamepad.Touchpad"},
});

constexpr std::array kGamepadAxisNames = std::to_array<NamedControl<GamepadAxis>>({
    {GamepadAxis::LeftStickX, "Gamepad.LeftStickX"},
    {GamepadAxis::LeftStickY, "Gamepad.LeftStickY"},
    {GamepadAxis::RightStickX, "Gamepad.RightStickX"},
    {GamepadAxis::RightStickY, "Gamepad.RightStickY"},
    {GamepadAxis::LeftTrigger, "Gamepad.LeftTrigger"},
    {GamepadAxis::RightTrigger, "Gamepad.RightTrigger"},
});

constexpr std::array kMouseAxisNames = std::to_array<NamedControl<MouseAxis>>({
    {MouseAxis::DeltaX, "Mouse.DeltaX"},
    {MouseAxis::DeltaY, "Mouse.DeltaY"},
    {MouseAxis::WheelX, "Mouse.WheelX"},
    {MouseAxis::WheelY, "Mouse.WheelY"},
});

constexpr std::array kAxis2DNames = std::to_array<NamedControl<Axis2D>>({
    {Axis2D::MouseDelta, "Mouse.Delta"},
    {Axis2D::MouseWheel, "Mouse.Wheel"},
    {Axis2D::GamepadLeftStick, "Gamepad.LeftStick"},
    {Axis2D::GamepadRightStick, "Gamepad.RightStick"},
});

template <typename Enum, std::size_t Size>
std::string_view nameIn(const std::array<NamedControl<Enum>, Size>& table, Enum value) noexcept {
    for (const NamedControl<Enum>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return {};
}

template <typename Enum, std::size_t Size>
std::optional<Control> findIn(const std::array<NamedControl<Enum>, Size>& table, std::string_view name) noexcept {
    for (const NamedControl<Enum>& entry : table) {
        if (entry.name == name) {
            return Control(entry.value);
        }
    }
    return std::nullopt;
}

struct NameOf {
    std::string_view operator()(Key value) const noexcept { return nameIn(kKeyNames, value); }

    std::string_view operator()(MouseButton value) const noexcept { return nameIn(kMouseButtonNames, value); }

    std::string_view operator()(GamepadButton value) const noexcept { return nameIn(kGamepadButtonNames, value); }

    std::string_view operator()(GamepadAxis value) const noexcept { return nameIn(kGamepadAxisNames, value); }

    std::string_view operator()(MouseAxis value) const noexcept { return nameIn(kMouseAxisNames, value); }

    std::string_view operator()(Axis2D value) const noexcept { return nameIn(kAxis2DNames, value); }
};

} // namespace

std::string_view controlName(Control control) noexcept {
    // std::visit can't meet a valueless variant here: Control holds only enums.
    return std::visit(NameOf{}, control);
}

std::optional<Control> findControl(std::string_view name) noexcept {
    if (name.starts_with("Keyboard.")) {
        return findIn(kKeyNames, name);
    }
    if (name.starts_with("Mouse.")) {
        if (std::optional<Control> button = findIn(kMouseButtonNames, name)) {
            return button;
        }
        if (std::optional<Control> axis = findIn(kMouseAxisNames, name)) {
            return axis;
        }
        return findIn(kAxis2DNames, name);
    }
    if (name.starts_with("Gamepad.")) {
        if (std::optional<Control> button = findIn(kGamepadButtonNames, name)) {
            return button;
        }
        if (std::optional<Control> axis = findIn(kGamepadAxisNames, name)) {
            return axis;
        }
        return findIn(kAxis2DNames, name);
    }
    return std::nullopt;
}

} // namespace ith
