# Input actions

The `input_actions` module turns raw input into **actions**: game code asks "did the player
jump?", not "is the space bar down?", and the answer doesn't change when the player rebinds Jump.
The behaviour is specified in the [input actions spec](../specs/input-actions.md); this page shows
how to use the first version. It builds on the [input module](input.md) (ADR-0013, ADR-0027) and
the [ECS](ecs.md) (ADR-0014, ADR-0016).

`input_actions` is a `runtime` module in the `framework` layer: a dedicated server links it and
evaluates nothing, because it has no local players.

## The parts

| Type | Header | What it is |
|---|---|---|
| `InputActionAsset`, `InputContextAsset` | `InputActionAssets.h` | The assets: actions with a value kind, contexts with mappings, modifiers and triggers |
| `InputAssetLibrary` | `InputAssetLibrary.h` | The loaded actions, contexts and input settings by asset id, and their validation |
| `InputBindingConfig`, `InputBindings` | `InputBindings.h` | Rebinding: profiles in four configuration layers and the `ith.input_bindings` document |
| `InputActions` | `InputActions.h` | The evaluation: reads `ith::Input`, writes action states and events for each local player |
| `InputPlayer`, `InputContexts`, `ProvidedInputContexts`, `InputProfile`, `ActionStates`, `ActionEvent` | `InputActionComponents.h` | The ECS data |

## Use

```cpp
#include "ith/input_actions/InputActions.h"

ith::InputAssetLibrary library;            // registered by the application (see Assets)
ith::InputBindingConfig bindings(library);
ith::InputActions actions(input, bindings);
actions.attachWindow(window.id());         // a Game-layer consumer in the window's stack

const ith::Entity player = world.spawn(
    ith::InputPlayer{.index = 0},
    ith::InputContexts{.contexts = {{.context = ith::AssetRef::to(onFootId), .priority = 0}}});

while (running) {
    input.processEvents(platform.pumpEvents(), platform.pumpTimeNs());
    actions.update(world); // once per frame, after Input
    // systems read the results:
    const ith::ActionStates& states = *world.get<ith::ActionStates>(player);
    if (states.has(jumpId, ith::ActionPhase::Started)) { requestJump(); }
    const ith::ActionValue move = states.value(moveId); // x and y for an Axis2 action
    for (const ith::ActionEvent& event : actions.events()) { ... }
}
```

- **Players** are entities with `InputPlayer`. Its `index` is the `ith::Input` player index: the
  player reads only the devices `Input` assigns to it, so local multiplayer and joining work as in
  the [input guide](input.md#players-and-devices). `update` adds `ActionStates` to each of them.
- **Contexts.** Gameplay changes a player's stack by writing `InputContexts`; the change takes
  effect at the next `update`. An entity can bring contexts with `ProvidedInputContexts`: while
  `InputPlayer::controlled` names it, they are part of the player's stack (a vehicle's controls).
  `controlled` stands in for the `Controls` relation of ADR-0016 until the ECS has relations.
- **Reading.** `ActionStates` has one `ActionState` per action of the player's table: value,
  state (`None`, `Ongoing`, `Triggered`), the phases of this frame (`Started`, `Ongoing`,
  `Triggered`, `Completed`, `Canceled`) with their timestamps, `startedAtNs` and
  `triggeredSinceNs`. An action not in the table reads as zero and `None`. `actions.events()` lists
  the frame's `ActionEvent`s (player, action, phase, value, timestamp) by player index, then table
  order, then phase order; it is replaced by each `update`, so a system that reads it once per frame
  sees every event once. There are no callbacks.
- **Suspension.** Setting `ActionStates::suspended` makes every mapping of the player read its key
  as not actuated, so its actions end at once (a pause menu, a binding capture). On resume, digital
  keys still held count only once pressed again; sticks are read at once.
- **Key prompts.** `actions.keysFor(player, action, family)` lists the keys that currently trigger
  an action, in context order. Turn them into text with `Platform::keyDisplayName` (the keyboard
  layout's name) or a glyph for the gamepad type; `ActionStates::lastFamily` says which family the
  player used last.

## Assets

An input action and an input context are native assets: `.ithasset` documents of the formats
`ith.input_action` and `ith.input_context`, with the asset id in the envelope
([serialization guide](serialization.md#asset-documents)). Until the runtime asset manager
exists, the application reads them and registers them:

```cpp
ith::Uuid id;
ith::InputContextAsset onFoot;
if (const auto read = ith::serialization::readText(text, ith::kInputContextFormat, onFoot, {.id = &id})) {
    library.setContext(ith::AssetId(id), std::move(onFoot));
}
```

```json
{
  "format": "ith.input_context",
  "version": 1,
  "id": "6f1c2a90-3b8e-4d7a-9c15-2e4f6a8b0d13",
  "data": {
    "displayName": "On foot",
    "mappings": [
      {
        "action": {"asset": "0b8e7f6a-1c2d-4e3f-8a9b-0c1d2e3f4a5b", "subAsset": "00000000-0000-0000-0000-000000000000"},
        "key": "Keyboard.Space",
        "triggers": [{"type": "Pressed"}],
        "slot": "jump.keyboard",
        "slotLabel": "Primary"
      }
    ]
  }
}
```

- Keys are the stable control names of the input module (`Keyboard.W`, `Mouse.Delta`,
  `Gamepad.LeftStick`, `Gamepad.RightTrigger`), never numeric ids.
- A modifier or trigger names its `type` and keeps its parameters in the field named after that
  type (`"deadZone": {"lower": 0.25}`, `"hold": {"seconds": 1}`). A missing block takes the
  defaults of the spec's sections 3.5 and 3.6. A trigger's `kind` is empty for its type's default
  (`Implicit` for `Chord`, `Explicit` otherwise). The writer writes every field, so the blocks of
  the other types appear as `null`.
- A modifier or trigger type this build doesn't know fails the read
  (`serialization.unknown_enumerator`); cooking reads with `UnknownFields::Fail`.
- `InputAssetLibrary::validate()` reports what the editor and the cooker check: missing actions,
  unknown keys, modifiers naming undeclared settings, slots shared by mappings of another action,
  family or default key, chord and combo dependency cycles, chords without an action, combos without steps, and
  parameters out of range (negative times or thresholds, a `DeadZone` whose `lower` is not below
  `upper`, a negative `Curve` exponent, a `MultiTap` `count` below 2) (errors), and `consumesKeys` set on an
  action a chord or combo depends on, or a chord on an action no context maps (warnings).
- Input settings (look sensitivity, invert look) are declared with `library.setSettings` until
  Project Settings exist.

## How a frame is evaluated

For each local player, in ascending index:

1. The device events that reached the plugin's consumers from the player's devices are applied:
   keys and buttons get at most **one edge per frame**, so a press and a release inside one frame
   make the key actuated this frame and released the next; durations still use the real event
   timestamps, and a hold is measured only up to a release already waiting. Up to 8 edges wait per
   key; beyond that the oldest press and release are dropped, with an
   `input_actions.edge_queue_full` message in development builds, once per player and key. The
   queue holds copies of the fields it needs, never `ith::Event` objects. A player with several
   gamepads has one sample per gamepad key: buttons are ORed, a 1D axis takes the largest
   magnitude, a stick the longest vector of one gamepad; ties go to the gamepad first in Input's
   device set. Axis values are kept for every gamepad, assigned or not, and read by owner, so a
   gamepad assigned again never brings back a stale value.
2. If the stack, the profile, the library or the bindings changed, the **mapping table** is
   rebuilt: contexts by priority, the most recently added first among equals; each action takes its
   mappings only from the highest contexts that map it (equal priorities all contribute); actions
   that chords and combos depend on come first and never claim keys; within a context, mappings
   with a chord come first. This is the only step that allocates in steady state.
3. Each mapping reads its key, runs its modifiers and then its action's, and evaluates its triggers
   and its action's. A mapping of an action with `consumesKeys` that is not `None` **claims** its
   key: lower contexts read it as zero until the key is released ("lower" is later in table order,
   so of two contexts at the same priority the newer one wins a shared key). An analog key's claim
   ends when it rests below the lowest threshold of the claiming mapping's triggers, or below 0.2
   without triggers. A 2D control and its 1D axes are one control for claims (`Gamepad.LeftStick`
   and `Gamepad.LeftStickX`, `Mouse.Delta` and `Mouse.DeltaY`). Within one context there are no
   claims, except the chord rule: while a chord mapping on a key is not `None`, the other mappings
   on that key in the context read it as zero (Ctrl + S saves and doesn't also move back).
4. Each action combines its mappings (`Strongest` or `Sum`, then `clampToUnit`), and its phases
   follow from its previous and current state. Removing an action from the table ends it with
   `Completed` or `Canceled` at the frame's time.

**Timing** uses `Input`'s clock only: `Input::frameTimeNs()` and the event timestamps. A phase
caused by a key edge carries the edge's timestamp, one caused by a threshold (a hold, a pulse, a
combo timing out) the exact instant it was crossed, any other the frame's time. Game pause and
time dilation never affect triggers.

**"Already held".** A mapping that starts seeing a key that is already held (its context was just
added, a claim on the key ended, the key was rebound) treats it as held since that moment:
`Pressed`, `Tap` and `MultiTap` don't fire until it is pressed again, `Down` fires at once and
`Hold` counts from then. The rule covers digital keys; an analog key whose claim ends is read as
it is, so a deflected stick counts as a new actuation.

**Combo steps** with phase `Triggered` count only in the first frame of a run of `Triggered`
frames, so an action that stays `Triggered` while held (`Down`, or no trigger) works as a step.

**Pulses** shorter than a frame catch up: the frame reports the last interval that ended, up to the
`limit`.

**Implicit triggers need the key.** A mapping without explicit triggers is `Triggered` only while
its own value is non-zero, also when its implicit triggers (chords) are `Triggered`: a chord on Ctrl
needs its own key held too. While the chord is satisfied but the key isn't, the mapping is
`Ongoing`.

**Forced ends.** Suspension, a claim from a higher context, the chord rule, focus loss and the end
of the mouse capture end actions without the player releasing the key. The mapping reads its key as
not actuated, so the action ends with `Completed` or `Canceled` as usual, but `Released`,
`HoldRelease`, `Tap` and `MultiTap` never fire from it: they fire only on a real release of a key
the mapping can see. Focus loss ends actions at the timestamps of the releases `Input` sends (they
carry the focus event's timestamp, which is how they are told apart). When the mouse capture ends,
every held mouse button is released at the frame's time and counts again only after a new press. Without capture, mouse input that no consumer
above the game takes still drives actions. Input consumed above the game slot (UI, console, editor
keys) never reaches an action. The plugin's consumer reports every event from an assigned device as
consumed, and ignores events from unassigned devices and text events.

## Rebinding

A mapping with a `slot` is rebindable. `InputBindingConfig` keeps the overrides of four layers,
engine, project, platform and user; for each profile and slot the highest layer that overrides it
wins, otherwise the authored key applies. A player uses the profile its `InputProfile` names,
`default` otherwise.

```cpp
const auto conflicts = bindings.conflicts("default", "jump.keyboard", ith::Key::F);
if (conflicts.empty()) {
    if (const auto rebound = bindings.rebind("default", "jump.keyboard", ith::Key::F); !rebound) {
        show(rebound.error().message); // input_actions.wrong_device_family, input_actions.incompatible_key
    }
}
bindings.setSetting("default", "look_sensitivity", 1.4f);
const auto text = ith::writeInputBindings(bindings.layer(ith::InputBindingLayer::User));
```

- A rebind changes only the key; modifiers and triggers stay as authored. It applies at the next
  `update`, to every mapping that shares the slot.
- A key must be of the slot's device family, and of a compatible raw kind: a digital slot takes
  digital or 1D keys, a 1D slot 1D keys, a 2D slot 2D keys. `rebind(profile, slot, std::nullopt)`
  unbinds a slot.
- Player changes go to the user layer, only where they differ from the layers below.
  `resetProfile` removes a profile's user overrides; `copyProfile` and `deleteProfile` work on the
  user layer.
- `readInputBindings` and `writeInputBindings` read and write the content of an
  `ith.input_bindings` document (comments allowed). Unknown key names are warnings and the entries
  are ignored; `setLayer` drops unknown slots and settings with warnings that name the source.
  Where each layer's file lives is not decided yet (the spec's decision 6), so the application
  reads and writes the files.

## Problems at run time

Nothing in the evaluation asserts on data. A context that isn't loaded is left out of the stack, a
mapping whose action isn't loaded is dropped, a mapping with an unknown key reads as zero, and a
chord or combo cycle formed by a player's contexts makes the triggers in the cycle report `None`,
and a second entity with the same `InputPlayer::index` is not evaluated
(`input_actions.duplicate_player`). Each is recorded once per cause; read them with
`actions.takeMessages()`. A frame time earlier than the previous frame's events never wraps a
duration.

## Threading

`InputActions` runs on the thread that owns the `Input` and the `World`; nothing here is
thread-safe. `InputAssetLibrary` and `InputBindingConfig` are changed on that thread too.

## Performance

Steady-state evaluation allocates nothing; only a table rebuild does. The test
`input_actions_allocation` checks it with counting global allocation functions, in an executable of
its own, built only without sanitizers (their runtimes replace the same functions). The benchmark `tests/input_actions/InputActionsBenchmark.cpp` evaluates four local
players with 100 mappings each, the spec's requirement of at most 0.1 ms per frame on the Low tier
reference machine. Run it on a Release build like the [ECS benchmark](ecs.md#benchmark):

```powershell
./scripts/dev.ps1 build -Config Release
./build/windows/bin/Release/ith_input_actions_benchmark.exe
```

## Tests

`tests/input_actions` injects device events with explicit timestamps and frame times; no display
is needed. Each test names the spec requirement it covers; `RegressionTests.cpp` holds the cases
found in review. In Clang sanitizer builds, `input_actions_fuzz` runs a short libFuzzer session over
`readDocument`, asset documents with their id, the action and context assets with their
validation, and the bindings document applied to a configuration (seeds in
`tests/input_actions/fuzz_seeds`).

## Not here yet

- The editor parts: the asset editors, the key picker, validation in the message log, Project
  Settings and the debug overlay (`input_actions_editor`).
- Interactive key capture ("press a key") for settings menus, and the debug console commands.
- The ECS schedule, resources, events and relations: `update` is called by the application,
  `actions.events()` stands in for ADR-0016 events, `InputPlayer::controlled` for the `Controls`
  relation, and the bindings are not yet an app-scoped resource.
- Loading through the runtime asset manager, and the files of the configuration layers.
