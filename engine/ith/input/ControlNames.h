#pragma once

// Stable string names of input controls, for configuration files and bindings (ADR-0013 rule 12).
//
//   controlName(ith::Key::W)                     -> "Keyboard.W"
//   controlName(ith::MouseButton::Left)          -> "Mouse.Left"
//   controlName(ith::GamepadButton::FaceSouth)   -> "Gamepad.FaceSouth"
//   controlName(ith::Axis2D::GamepadLeftStick)   -> "Gamepad.LeftStick"
//   findControl("Keyboard.W")                    -> ith::Key::W
//
// A name is the device ("Keyboard", "Mouse", "Gamepad"), a dot and the control's enumerator,
// except the 2D controls, whose names drop the device prefix of their enumerator
// (Axis2D::MouseDelta is "Mouse.Delta"). Names are stable like the values: never renamed or
// reused. Lookup is case-sensitive.

#include "ith/platform_events/InputCodes.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace ith {

// One-dimensional mouse values, read from the per-frame sums of the input module's state.
enum class MouseAxis : std::uint8_t {
    None = 0,
    DeltaX = 1, // the frame's mouse motion, right positive
    DeltaY = 2, // down positive
    WheelX = 3, // the frame's wheel steps, right positive
    WheelY = 4, // away from the user positive
};

// Two-dimensional controls: two axes read together, x then y.
enum class Axis2D : std::uint8_t {
    None = 0,
    MouseDelta = 1,        // MouseAxis::DeltaX and DeltaY
    MouseWheel = 2,        // MouseAxis::WheelX and WheelY
    GamepadLeftStick = 3,  // GamepadAxis::LeftStickX and LeftStickY
    GamepadRightStick = 4, // GamepadAxis::RightStickX and RightStickY
};

// Any control a binding can name.
using Control = std::variant<Key, MouseButton, GamepadButton, GamepadAxis, MouseAxis, Axis2D>;

// The stable name of `control`; empty for the None values and for values without a name.
// Thread-safe.
[[nodiscard]] std::string_view controlName(Control control) noexcept;

// The control called `name`, or nullopt for an unknown name (a configuration file reports that as
// a warning, ADR-0013 rule 13). Thread-safe.
[[nodiscard]] std::optional<Control> findControl(std::string_view name) noexcept;

} // namespace ith
