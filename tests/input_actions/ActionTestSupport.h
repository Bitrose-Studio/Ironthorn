#pragma once

// A test rig for the input action layer: an Input with one focused window, a library, bindings, the
// evaluation and a World, driven frame by frame with injected device events and explicit times
// (ADR-0013 rule 14). Times are in milliseconds on the events' clock.

#include "ith/ecs/World.h"
#include "ith/input/Input.h"
#include "ith/input_actions/InputActions.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ith::test {

inline constexpr WindowId kWindow = 1;
inline constexpr DeviceId kPad = 21;
inline constexpr DeviceId kOtherPad = 22;

[[nodiscard]] constexpr std::uint64_t ms(double milliseconds) {
    return static_cast<std::uint64_t>(milliseconds * 1'000'000.0);
}

inline Event keyDown(Key key, double atMs) {
    return Event{.type = EventType::KeyDown, .timestampNs = ms(atMs), .window = kWindow, .key = key};
}

inline Event keyUp(Key key, double atMs) {
    return Event{.type = EventType::KeyUp, .timestampNs = ms(atMs), .window = kWindow, .key = key};
}

inline Event mouseDown(MouseButton button, double atMs) {
    return Event{.type = EventType::MouseButtonDown, .timestampNs = ms(atMs), .window = kWindow, .mouseButton = button};
}

inline Event mouseUp(MouseButton button, double atMs) {
    return Event{.type = EventType::MouseButtonUp, .timestampNs = ms(atMs), .window = kWindow, .mouseButton = button};
}

inline Event mouseMove(float dx, float dy, double atMs) {
    return Event{.type = EventType::MouseMoved, .timestampNs = ms(atMs), .window = kWindow, .deltaX = dx, .deltaY = dy};
}

inline Event mouseWheel(float dx, float dy, double atMs) {
    return Event{.type = EventType::MouseWheel, .timestampNs = ms(atMs), .window = kWindow, .deltaX = dx, .deltaY = dy};
}

inline Event padConnected(DeviceId pad, double atMs) {
    return Event{.type = EventType::GamepadConnected, .timestampNs = ms(atMs), .device = pad, .text = "Pad"};
}

inline Event padDisconnected(DeviceId pad, double atMs) {
    return Event{.type = EventType::GamepadDisconnected, .timestampNs = ms(atMs), .device = pad};
}

inline Event padDown(DeviceId pad, GamepadButton button, double atMs) {
    return Event{.type = EventType::GamepadButtonDown, .timestampNs = ms(atMs), .device = pad, .gamepadButton = button};
}

inline Event padUp(DeviceId pad, GamepadButton button, double atMs) {
    return Event{.type = EventType::GamepadButtonUp, .timestampNs = ms(atMs), .device = pad, .gamepadButton = button};
}

inline Event padAxis(DeviceId pad, GamepadAxis axis, float value, double atMs) {
    return Event{.type = EventType::GamepadAxisMoved,
                 .timestampNs = ms(atMs),
                 .device = pad,
                 .gamepadAxis = axis,
                 .axisValue = value};
}

inline Event focusEvent(EventType type, double atMs) {
    return Event{.type = type, .timestampNs = ms(atMs), .window = kWindow};
}

// A mapping of `action` on `key`.
inline InputMapping mapping(AssetId action, std::string key, std::vector<InputTrigger> triggers = {},
                            std::vector<InputModifier> modifiers = {}) {
    return InputMapping{.action = AssetRef::to(action),
                        .key = std::move(key),
                        .modifiers = std::move(modifiers),
                        .triggers = std::move(triggers)};
}

inline InputTrigger trigger(InputTriggerType type) {
    return InputTrigger{.type = type};
}

inline InputModifier modifier(InputModifierType type) {
    return InputModifier{.type = type};
}

class Rig {
public:
    Rig() : bindings(library), actions(input, bindings) {
        actions.attachWindow(kWindow);
        frame(0, {focusEvent(EventType::WindowFocusGained, 0)});
    }

    AssetId addAction(InputActionAsset asset = {}) {
        const AssetId id = AssetId::generate();
        library.setAction(id, std::move(asset));
        return id;
    }

    AssetId addAction(ActionValueKind kind) { return addAction(InputActionAsset{.valueKind = kind}); }

    AssetId addContext(std::vector<InputMapping> mappings) {
        const AssetId id = AssetId::generate();
        library.setContext(id, InputContextAsset{.mappings = std::move(mappings)});
        return id;
    }

    // A local player with these contexts, in order.
    Entity addPlayer(std::initializer_list<std::pair<AssetId, std::int32_t>> contexts = {}, PlayerIndex index = 0) {
        InputContexts own;
        for (const auto& [context, priority] : contexts) {
            own.contexts.push_back({.context = AssetRef::to(context), .priority = priority});
        }
        return world.spawn(InputPlayer{.index = index}, std::move(own));
    }

    void pushContext(Entity player, AssetId context, std::int32_t priority = 0) {
        world.get<InputContexts>(player)->contexts.push_back({.context = AssetRef::to(context), .priority = priority});
    }

    void removeContext(Entity player, AssetId context) {
        std::erase_if(world.get<InputContexts>(player)->contexts,
                      [context](const InputContextEntry& entry) { return entry.context.assetId() == context; });
    }

    // Runs one frame at `atMs` with `events`.
    void frame(double atMs, std::vector<Event> events = {}) {
        input.processEvents(events, ms(atMs));
        actions.update(world);
    }

    [[nodiscard]] const ActionStates& states(Entity player) const { return *world.get<ActionStates>(player); }

    [[nodiscard]] ActionPhase phases(Entity player, AssetId action) const {
        const ActionState* state = states(player).find(action);
        return state != nullptr ? state->phases : ActionPhase::None;
    }

    [[nodiscard]] TriggerState state(Entity player, AssetId action) const { return states(player).state(action); }

    [[nodiscard]] ActionValue value(Entity player, AssetId action) const { return states(player).value(action); }

    // The timestamp of `phase` of `action` this frame.
    [[nodiscard]] std::uint64_t phaseTime(Entity player, AssetId action, ActionPhase phase) const {
        const ActionState* state = states(player).find(action);
        return state != nullptr ? state->phaseTimesNs[phaseIndex(phase)] : 0;
    }

    // The events of the last frame for `action`.
    [[nodiscard]] std::vector<ActionEvent> eventsOf(AssetId action) const {
        std::vector<ActionEvent> result;
        for (const ActionEvent& event : actions.events()) {
            if (event.action == action) {
                result.push_back(event);
            }
        }
        return result;
    }

    Input input;
    InputAssetLibrary library;
    InputBindingConfig bindings;
    InputActions actions;
    World world;
};

} // namespace ith::test
