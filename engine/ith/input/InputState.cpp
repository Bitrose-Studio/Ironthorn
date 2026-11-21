#include "ith/input/InputState.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ith {
namespace {

// Ids beyond a table come only from malformed or newer data: they are ignored, never folded onto
// another control.
std::size_t indexOf(Key key) {
    return static_cast<std::size_t>(key);
}

std::size_t indexOf(MouseButton button) {
    return static_cast<std::size_t>(button);
}

std::size_t indexOf(GamepadButton button) {
    return static_cast<std::size_t>(button);
}

std::size_t indexOf(GamepadAxis axis) {
    return static_cast<std::size_t>(axis);
}

// The bit at `index`; false beyond the table.
template <std::size_t Size>
bool bit(const std::bitset<Size>& bits, std::size_t index) {
    return index < Size && bits[index];
}

// Applies a press or release to one set of bitsets, ignoring a press of a held control, a
// release of a control that isn't held, and ids beyond the table.
template <std::size_t Size>
void applyEdge(bool down, std::size_t index, std::bitset<Size>& held, std::bitset<Size>& pressed,
               std::bitset<Size>& released) {
    if (index >= Size) {
        return;
    }
    if (down && !held[index]) {
        held[index] = true;
        pressed[index] = true;
    } else if (!down && held[index]) {
        held[index] = false;
        released[index] = true;
    }
}

// The two gamepad axes of a stick.
struct StickAxes {
    GamepadAxis x = GamepadAxis::None;
    GamepadAxis y = GamepadAxis::None;
};

StickAxes stickAxes(Axis2D axis) {
    switch (axis) {
        case Axis2D::GamepadLeftStick:
            return {GamepadAxis::LeftStickX, GamepadAxis::LeftStickY};
        case Axis2D::GamepadRightStick:
            return {GamepadAxis::RightStickX, GamepadAxis::RightStickY};
        default:
            return {};
    }
}

} // namespace

void InputState::beginFrame() {
    m_keysPressed.reset();
    m_keysReleased.reset();
    m_buttonsPressed.reset();
    m_buttonsReleased.reset();
    m_mouseDelta = {};
    m_wheel = {};
    std::erase_if(m_gamepads, [](const Gamepad& gamepad) { return gamepad.disconnected; });
    for (Gamepad& gamepad : m_gamepads) {
        gamepad.pressed.reset();
        gamepad.released.reset();
    }
}

void InputState::apply(const Event& event) {
    switch (event.type) {
        case EventType::KeyDown:
        case EventType::KeyUp:
            m_modifiers = event.modifiers;
            if (event.key != Key::None && !event.repeat) {
                applyEdge(event.type == EventType::KeyDown, indexOf(event.key), m_keysHeld, m_keysPressed,
                          m_keysReleased);
            }
            break;
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp:
            m_modifiers = event.modifiers;
            m_mousePosition = {event.x, event.y};
            m_mouseWindow = event.window;
            if (event.mouseButton != MouseButton::None) {
                applyEdge(event.type == EventType::MouseButtonDown, indexOf(event.mouseButton), m_buttonsHeld,
                          m_buttonsPressed, m_buttonsReleased);
            }
            break;
        case EventType::MouseMoved:
            m_mousePosition = {event.x, event.y};
            m_mouseWindow = event.window;
            m_mouseDelta.x += event.deltaX;
            m_mouseDelta.y += event.deltaY;
            break;
        case EventType::MouseWheel:
            m_modifiers = event.modifiers;
            m_wheel.x += event.deltaX;
            m_wheel.y += event.deltaY;
            break;
        case EventType::GamepadConnected:
            gamepad(event.device).disconnected = false;
            break;
        case EventType::GamepadDisconnected:
            // Kept until the next frame, so this frame's last edges stay readable.
            if (const auto known = std::ranges::find(m_gamepads, event.device, &Gamepad::id);
                known != m_gamepads.end()) {
                known->disconnected = true;
            }
            break;
        case EventType::GamepadButtonDown:
        case EventType::GamepadButtonUp:
            if (event.gamepadButton != GamepadButton::None) {
                Gamepad& pad = gamepad(event.device);
                applyEdge(event.type == EventType::GamepadButtonDown, indexOf(event.gamepadButton), pad.held,
                          pad.pressed, pad.released);
            }
            break;
        case EventType::GamepadAxisMoved:
            if (event.gamepadAxis != GamepadAxis::None && indexOf(event.gamepadAxis) < kGamepadAxisCount) {
                gamepad(event.device).axes[indexOf(event.gamepadAxis)] = event.axisValue;
            }
            break;
        default:
            break;
    }
}

bool InputState::held(Key key) const {
    return bit(m_keysHeld, indexOf(key));
}

bool InputState::pressed(Key key) const {
    return bit(m_keysPressed, indexOf(key));
}

bool InputState::released(Key key) const {
    return bit(m_keysReleased, indexOf(key));
}

bool InputState::held(MouseButton button) const {
    return bit(m_buttonsHeld, indexOf(button));
}

bool InputState::pressed(MouseButton button) const {
    return bit(m_buttonsPressed, indexOf(button));
}

bool InputState::released(MouseButton button) const {
    return bit(m_buttonsReleased, indexOf(button));
}

bool InputState::held(DeviceId gamepad, GamepadButton button) const {
    const Gamepad* pad = findGamepad(gamepad);
    return pad != nullptr && bit(pad->held, indexOf(button));
}

bool InputState::pressed(DeviceId gamepad, GamepadButton button) const {
    const Gamepad* pad = findGamepad(gamepad);
    return pad != nullptr && bit(pad->pressed, indexOf(button));
}

bool InputState::released(DeviceId gamepad, GamepadButton button) const {
    const Gamepad* pad = findGamepad(gamepad);
    return pad != nullptr && bit(pad->released, indexOf(button));
}

bool InputState::held(GamepadButton button) const {
    return std::ranges::any_of(m_gamepads, [&](const Gamepad& pad) { return bit(pad.held, indexOf(button)); });
}

bool InputState::pressed(GamepadButton button) const {
    return std::ranges::any_of(m_gamepads, [&](const Gamepad& pad) { return bit(pad.pressed, indexOf(button)); });
}

bool InputState::released(GamepadButton button) const {
    return std::ranges::any_of(m_gamepads, [&](const Gamepad& pad) { return bit(pad.released, indexOf(button)); });
}

float InputState::axis(DeviceId gamepad, GamepadAxis axis) const {
    const Gamepad* pad = findGamepad(gamepad);
    return pad != nullptr && indexOf(axis) < kGamepadAxisCount ? pad->axes[indexOf(axis)] : 0.0f;
}

float InputState::axis(GamepadAxis axis) const {
    float result = 0.0f;
    if (indexOf(axis) >= kGamepadAxisCount) {
        return result;
    }
    for (const Gamepad& pad : m_gamepads) {
        const float value = pad.axes[indexOf(axis)];
        if (std::fabs(value) > std::fabs(result)) {
            result = value;
        }
    }
    return result;
}

float InputState::axis(MouseAxis axis) const {
    switch (axis) {
        case MouseAxis::DeltaX:
            return m_mouseDelta.x;
        case MouseAxis::DeltaY:
            return m_mouseDelta.y;
        case MouseAxis::WheelX:
            return m_wheel.x;
        case MouseAxis::WheelY:
            return m_wheel.y;
        default:
            return 0.0f;
    }
}

InputVector2 InputState::axis2D(DeviceId gamepad, Axis2D axis) const {
    const StickAxes stick = stickAxes(axis);
    if (stick.x == GamepadAxis::None) {
        return {};
    }
    return {this->axis(gamepad, stick.x), this->axis(gamepad, stick.y)};
}

InputVector2 InputState::axis2D(Axis2D axis) const {
    switch (axis) {
        case Axis2D::MouseDelta:
            return m_mouseDelta;
        case Axis2D::MouseWheel:
            return m_wheel;
        default:
            break;
    }
    InputVector2 result;
    float longest = 0.0f;
    for (const Gamepad& pad : m_gamepads) {
        const InputVector2 value = axis2D(pad.id, axis);
        const float length = (value.x * value.x) + (value.y * value.y);
        if (length > longest) {
            longest = length;
            result = value;
        }
    }
    return result;
}

std::vector<DeviceId> InputState::gamepads() const {
    std::vector<DeviceId> ids;
    ids.reserve(m_gamepads.size());
    for (const Gamepad& pad : m_gamepads) {
        ids.push_back(pad.id);
    }
    return ids;
}

const InputState::Gamepad* InputState::findGamepad(DeviceId id) const {
    const auto found = std::ranges::find(m_gamepads, id, &Gamepad::id);
    return found != m_gamepads.end() ? &*found : nullptr;
}

InputState::Gamepad& InputState::gamepad(DeviceId id) {
    const auto found = std::ranges::find(m_gamepads, id, &Gamepad::id);
    if (found != m_gamepads.end()) {
        return *found;
    }
    return m_gamepads.emplace_back(Gamepad{.id = id});
}

} // namespace ith
