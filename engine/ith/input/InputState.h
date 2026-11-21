#pragma once

// Per-frame input state built from device events (ADR-0013 rule 9).
//
//   state.beginFrame();
//   for (const ith::Event& event : events) {
//       state.apply(event);
//   }
//   if (state.pressed(ith::Key::Space)) { jump(); }
//   const ith::InputVector2 look = state.axis2D(ith::Axis2D::MouseDelta);
//
// Usually read through ith::Input, which keeps one state for all devices and one per consumer,
// built only from the events that reached that consumer.

#include "ith/input/ControlNames.h"
#include "ith/platform_events/Event.h"

#include <array>
#include <bitset>
#include <vector>

namespace ith {

struct InputVector2 {
    float x = 0.0f;
    float y = 0.0f;

    friend bool operator==(const InputVector2&, const InputVector2&) = default;
};

// For each key and button: held, pressed this frame and released this frame. A press and a
// release within one frame report both (held then says which came last). Key repeats are not
// presses. A press of a control already held and a release of a control not held are ignored,
// so the state never shows a release without its press. Mouse motion and wheel are summed over
// the frame; gamepad values are kept per device.
//
// A plain value, not thread-safe: one thread at a time.
class InputState {
public:
    // Starts a new frame: clears the pressed and released edges, the mouse motion and wheel sums,
    // and forgets gamepads that disconnected during the last frame. Held controls stay held.
    void beginFrame();

    // Applies one event. Non-device events and text events change nothing.
    void apply(const Event& event);

    [[nodiscard]] bool held(Key key) const;
    [[nodiscard]] bool pressed(Key key) const;
    [[nodiscard]] bool released(Key key) const;

    [[nodiscard]] bool held(MouseButton button) const;
    [[nodiscard]] bool pressed(MouseButton button) const;
    [[nodiscard]] bool released(MouseButton button) const;

    // Gamepad buttons of one gamepad. False for a gamepad this state has no value from.
    [[nodiscard]] bool held(DeviceId gamepad, GamepadButton button) const;
    [[nodiscard]] bool pressed(DeviceId gamepad, GamepadButton button) const;
    [[nodiscard]] bool released(DeviceId gamepad, GamepadButton button) const;
    // The same on any gamepad of this state; a consumer bound to a player sees only that
    // player's gamepads.
    [[nodiscard]] bool held(GamepadButton button) const;
    [[nodiscard]] bool pressed(GamepadButton button) const;
    [[nodiscard]] bool released(GamepadButton button) const;

    // The raw axis value of one gamepad (see GamepadAxis); 0 when unknown.
    [[nodiscard]] float axis(DeviceId gamepad, GamepadAxis axis) const;
    // The value with the largest magnitude over the gamepads of this state.
    [[nodiscard]] float axis(GamepadAxis axis) const;
    // The frame's sums of mouse motion and wheel steps.
    [[nodiscard]] float axis(MouseAxis axis) const;
    // A 2D control of one gamepad: its stick.
    [[nodiscard]] InputVector2 axis2D(DeviceId gamepad, Axis2D axis) const;
    // A 2D control: the mouse sums, or the longest stick over the gamepads of this state.
    [[nodiscard]] InputVector2 axis2D(Axis2D axis) const;

    // The modifiers of the latest keyboard or mouse event.
    [[nodiscard]] KeyModifiers modifiers() const { return m_modifiers; }

    // The latest mouse position, in pixels of mouseWindow().
    [[nodiscard]] InputVector2 mousePosition() const { return m_mousePosition; }

    [[nodiscard]] WindowId mouseWindow() const { return m_mouseWindow; }

    // Whether this state has values from `gamepad`.
    [[nodiscard]] bool hasGamepad(DeviceId gamepad) const { return findGamepad(gamepad) != nullptr; }

    // The gamepads this state has values from, in the order they first appeared.
    [[nodiscard]] std::vector<DeviceId> gamepads() const;

private:
    struct Gamepad {
        DeviceId id = 0;
        std::bitset<kGamepadButtonCount> held;
        std::bitset<kGamepadButtonCount> pressed;
        std::bitset<kGamepadButtonCount> released;
        std::array<float, kGamepadAxisCount> axes{};
        bool disconnected = false;
    };

    const Gamepad* findGamepad(DeviceId id) const;
    Gamepad& gamepad(DeviceId id);

    std::bitset<kKeyTableSize> m_keysHeld;
    std::bitset<kKeyTableSize> m_keysPressed;
    std::bitset<kKeyTableSize> m_keysReleased;
    std::bitset<8> m_buttonsHeld;
    std::bitset<8> m_buttonsPressed;
    std::bitset<8> m_buttonsReleased;
    KeyModifiers m_modifiers = KeyModifiers::None;
    InputVector2 m_mousePosition;
    WindowId m_mouseWindow = kNoWindow;
    InputVector2 m_mouseDelta;
    InputVector2 m_wheel;
    std::vector<Gamepad> m_gamepads;
};

} // namespace ith
