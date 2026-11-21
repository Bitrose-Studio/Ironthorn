#pragma once

// The ECS data of the input action layer (docs/specs/input-actions.md, sections 3.8 and 3.9):
// components on a local player's entity, the action states the evaluation writes, and the action
// events it sends.
//
//   const ith::Entity player = world.spawn(
//       ith::InputPlayer{.index = 0},
//       ith::InputContexts{.contexts = {{.context = ith::AssetRef::to(onFoot), .priority = 0}}});
//   ...
//   actions.update(world);
//   const ith::ActionStates& states = *world.get<ith::ActionStates>(player);
//   if (states.has(jump, ith::ActionPhase::Started)) { requestJump(); }
//   const ith::ActionValue move = states.value(moveAction);
//
// Gameplay changes the player's contexts by writing InputContexts; the evaluation notices the
// change at its next update. ActionStates is written by the evaluation only, except `suspended`.

#include "ith/assets/AssetId.h"
#include "ith/assets/AssetRef.h"
#include "ith/ecs/Entity.h"
#include "ith/foundation/Uuid.h"
#include "ith/input_actions/InputAssetLibrary.h"
#include "ith/reflection/Annotations.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith {

// One entry of a context stack: a context and its priority (higher first).
struct InputContextEntry {
    ITH_TYPE(Struct)
    ITH_FIELD()
    AssetRef context;
    ITH_FIELD()
    std::int32_t priority = 0;

    friend bool operator==(const InputContextEntry&, const InputContextEntry&) = default;
};

// Makes an entity a local player of the input action layer. Only local players are evaluated.
struct InputPlayer {
    ITH_TYPE(Component)
    // The local player index of ith::Input (0 is player 1): the player's devices are those Input
    // assigns to this index.
    ITH_FIELD()
    std::uint8_t index = 0;
    // The entity this player controls, whose ProvidedInputContexts join the player's stack; the null
    // handle for none. A runtime link, not saved: it stands in for the Controls relation of
    // ADR-0016 rule 8 until the ECS has relations.
    Entity controlled;
};

// The player's own context stack. Adding the same context twice keeps one entry, with the later
// priority. Equal priorities order the entry added most recently first; an entry counts as added
// when the evaluation first sees it.
struct InputContexts {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::vector<InputContextEntry> contexts;
};

// Contexts an entity brings to the player that controls it (a vehicle's driving controls).
struct ProvidedInputContexts {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::vector<InputContextEntry> contexts;
};

// The binding profile a player uses; the default profile when the component is absent.
struct InputProfile {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::string name = "default";
};

// A trigger's, mapping's or action's state in one frame.
enum class TriggerState : std::uint8_t {
    None,
    Ongoing,
    Triggered,
};
ITH_ENUM(TriggerState)

// What changed for an action this frame (section 3.8). A set is a bitwise OR.
enum class ActionPhase : std::uint8_t {
    None = 0,
    Started = 1u << 0,
    Ongoing = 1u << 1,
    Triggered = 1u << 2,
    Completed = 1u << 3,
    Canceled = 1u << 4,
};
ITH_ENUM(ActionPhase, Flags)

[[nodiscard]] constexpr ActionPhase operator|(ActionPhase a, ActionPhase b) noexcept {
    return static_cast<ActionPhase>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

[[nodiscard]] constexpr ActionPhase operator&(ActionPhase a, ActionPhase b) noexcept {
    return static_cast<ActionPhase>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

// The number of phases.
inline constexpr std::size_t kActionPhaseCount = 5;
// The index of a single phase in ActionState::phaseTimesNs, from 0 for Started to 4 for Canceled.
// kActionPhaseCount, an index no array has, for None or a set of several phases.
[[nodiscard]] std::size_t phaseIndex(ActionPhase phase) noexcept;

// An action's value: a digital action has 1 or 0 in x; Axis1 uses x, Axis2 x and y, Axis3 all.
// Mouse motion is per frame and must not be multiplied by frame time; stick values are positions.
struct ActionValue {
    ITH_TYPE(Struct)
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;

    friend bool operator==(const ActionValue&, const ActionValue&) = default;
};

// One action of one player this frame. Timestamps are nanoseconds on ith::Input's clock.
struct ActionState {
    ITH_TYPE(Struct)
    ITH_FIELD()
    Uuid action;
    ITH_FIELD()
    ActionValue value;
    ITH_FIELD()
    TriggerState state = TriggerState::None;
    ITH_FIELD()
    ActionPhase phases = ActionPhase::None;
    // The timestamp of the last Started phase.
    ITH_FIELD()
    std::uint64_t startedAtNs = 0;
    // When the current run of Triggered frames began; empty while not Triggered.
    ITH_FIELD()
    std::optional<std::uint64_t> triggeredSinceNs;
    // For each phase of this frame, its timestamp (by phaseIndex); 0 for phases that didn't occur.
    ITH_FIELD()
    std::array<std::uint64_t, kActionPhaseCount> phaseTimesNs{};
};

// The action states of a local player, one per action of its mapping table, in table order.
// Added to every entity with InputPlayer by InputActions::update.
struct ActionStates {
    ITH_TYPE(Component, Transient)
    ITH_FIELD()
    std::vector<ActionState> actions;
    // Suspension (section 3.10): while true every mapping reads its key as not actuated. Set by the
    // game UI framework while a layer that blocks gameplay is shown, or by game code until it
    // exists. On resume, digital keys still held count only once pressed again.
    ITH_FIELD()
    bool suspended = false;
    // The device family that last actuated a key (a press, or an analog value beyond 0.5), for
    // input prompts.
    ITH_FIELD()
    InputDeviceFamily lastFamily = InputDeviceFamily::KeyboardMouse;

    // The state of `action`, or null when it is not in the player's table.
    [[nodiscard]] const ActionState* find(AssetId action) const noexcept;
    // The value of `action`; zero when it is not in the table.
    [[nodiscard]] ActionValue value(AssetId action) const noexcept;
    // The state of `action`; None when it is not in the table.
    [[nodiscard]] TriggerState state(AssetId action) const noexcept;
    // Whether `action` had `phase` this frame; false when it is not in the table.
    [[nodiscard]] bool has(AssetId action, ActionPhase phase) const noexcept;
};

// One phase of one action of one player, sent by InputActions::update. Within a frame, events are
// ordered by player index, then by the player's mapping table, then by phase as in section 3.8.
struct ActionEvent {
    Entity player;
    AssetId action;
    ActionPhase phase = ActionPhase::None;
    ActionValue value;
    std::uint64_t timestampNs = 0;
};

} // namespace ith
