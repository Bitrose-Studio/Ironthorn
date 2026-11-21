#pragma once

// The input action layer's evaluation (docs/specs/input-actions.md, sections 3.2 to 3.10): turns
// the raw input of ith::Input into the action states and events of each local player.
//
//   ith::InputAssetLibrary library;     // the registered actions, contexts and input settings
//   ith::InputBindingConfig bindings(library);
//   ith::InputActions actions(input, bindings);
//   actions.attachWindow(window.id());   // a Game-layer consumer in that window's stack
//   while (running) {
//       input.processEvents(platform.pumpEvents(), platform.pumpTimeNs());
//       actions.update(world);           // the evaluation system of the Input stage
//       for (const ith::ActionEvent& event : actions.events()) { ... }
//   }
//
// Each update, for every entity with InputPlayer in ascending player index:
// 1. the device events that reached the plugin's consumers from the player's devices are applied
//    to the player's key samples; digital keys apply at most one edge per frame, so a press and a
//    release within one frame are spread over two frames and keep their real timestamps;
// 2. if the player's context stack (InputContexts and the ProvidedInputContexts of the entity it
//    controls), its InputProfile, the library or the bindings changed, the player's mapping table
//    is rebuilt; this is the only step that allocates in steady state;
// 3. every mapping is evaluated in table order (modifiers, then triggers), with claims taking keys
//    from lower contexts;
// 4. the mappings of each action combine into its value and state, and the phases of the frame are
//    written to the player's ActionStates and sent as ActionEvents.
//
// Timing uses ith::Input's clock only (event timestamps and frameTimeNs()): game time scale and
// pause never affect triggers. The consumer reports every event from a device assigned to a player
// as consumed and ignores the others; text events are never read. Focus loss needs nothing here:
// Input sends real releases. When the mouse capture ends, every held mouse button is released at
// the frame's time and counts again only after a new press.
//
// Problems with the data (a missing context or action, an unknown key, a dependency cycle formed
// by the contexts of a player, an overflowing edge queue in development builds) never assert: they
// are recorded once per cause and read with takeMessages().
//
// Not thread-safe: use it on the thread that owns the Input and the World.

#include "ith/ecs/World.h"
#include "ith/foundation/Expected.h"
#include "ith/input/Input.h"
#include "ith/input_actions/InputActionComponents.h"
#include "ith/input_actions/InputBindings.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ith {

class InputActions {
public:
    // `input` and `bindings` (with its library) must outlive this object.
    InputActions(Input& input, const InputBindingConfig& bindings);
    // Removes the consumers this object added.
    ~InputActions();
    InputActions(const InputActions&) = delete;
    InputActions& operator=(const InputActions&) = delete;
    InputActions(InputActions&&) = delete;
    InputActions& operator=(InputActions&&) = delete;

    // Adds a Game-layer consumer to `window`'s stack, so its events reach the actions. Adding a
    // window twice does nothing. Precondition (asserted): `window` is not kNoWindow.
    void attachWindow(WindowId window);
    // Removes the consumer of `window`; unknown windows are ignored.
    void detachWindow(WindowId window);

    // Evaluates every local player of `world` for the frame Input last processed: run it once per
    // frame, after Input::processEvents. Adds ActionStates to every entity with InputPlayer that
    // lacks it. Precondition: no query of `world` is iterating.
    void update(World& world);

    // The action events of the last update, in order. Valid until the next update.
    [[nodiscard]] std::span<const ActionEvent> events() const;

    // The problems recorded since the last call: errors (code "input_actions.missing_context",
    // "input_actions.dependency_cycle") and warnings ("input_actions.missing_action",
    // "input_actions.unknown_key", "input_actions.edge_queue_full").
    [[nodiscard]] std::vector<Error> takeMessages();

    // The keys that currently trigger `action` for `player` (an entity with InputPlayer that the
    // last update evaluated), in mapping-table order (context order), filtered by `family` when
    // given, for input prompts. Names for people come from the platform: the layout's name for a
    // keyboard key, the gamepad type's glyph for a gamepad control. Empty when the action isn't in
    // the player's table.
    [[nodiscard]] std::vector<Control> keysFor(Entity player, AssetId action,
                                               std::optional<InputDeviceFamily> family = std::nullopt) const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace ith
