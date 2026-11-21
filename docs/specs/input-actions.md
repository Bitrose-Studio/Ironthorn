# Spec: input actions and contexts

- **Status:** Implemented in v1 (see section 8 for the requirements not in v1)
- **Date:** 2026-11-21
- **Builds on:** [ADR-0003](../decisions/0003-clean-room.md) (clean room),
  [ADR-0004](../decisions/0004-modules-and-layering.md) (layers),
  [ADR-0008](../decisions/0008-editor-familiar-to-unreal-developers.md) (editor conventions,
  layered configuration), [ADR-0010](../decisions/0010-serialization.md) (config and data
  format), [ADR-0013](../decisions/0013-input.md) (raw input, routing, the contract in rule 12),
  [ADR-0014](../decisions/0014-ecs-storage-and-world.md) (stages, resources),
  [ADR-0015](../decisions/0015-asset-identity-and-reimport.md) and
  [ADR-0024](../decisions/0024-asset-document-envelope-and-extension.md) (assets),
  [ADR-0016](../decisions/0016-gameplay-model-on-the-ecs.md) (ECS-native gameplay, players,
  control, events)

This is a clean-room behavioural spec (ADR-0003). It was written from public documentation and
observable behaviour only; the sources are listed at the end. The implementation is built from
this spec and Ironthorn's architecture, never from another engine's code.

## 1. Purpose and scope

`core/input` (ADR-0013) delivers raw device input: ordered, timestamped events and per-frame
state for keys, buttons, sticks and the mouse, routed through each window's consumer stack. Game
code should not ask "is the space bar down?". It should ask "did the player jump?", and the
answer should not change when the player rebinds Jump to a gamepad button.

The **input actions** plugin turns raw input into **actions**:

- designers declare what a player can do (**input actions**) and which keys do it in which
  situation (**input contexts**);
- each key binding can reshape its value (**modifiers**: dead zones, scaling, axis swaps) and
  decide when it counts (**triggers**: press, release, hold, tap, chords, combos);
- contexts are stacked per player by priority, and a higher context can take keys away from
  lower ones;
- players rebind keys, and their changes are saved in the user layer of the configuration;
- gameplay systems read the result as ECS data and events, with no callbacks.

### In scope for the first version

- Input action and input context assets, their editors and validation.
- Value kinds digital, 1D, 2D and 3D.
- The modifiers and triggers listed in sections 3.5 and 3.6.
- The per-player context stack with priorities and key consumption.
- The trigger state machine and the five action phases, with nanosecond timestamps.
- Rebinding by stable slot names, profiles, interactive key capture, conflict reporting.
- ECS integration: player components, an action event, an evaluation system in the `Input`
  stage.
- A debug overlay and debug console commands.

### Out of scope for the first version

| Out of scope | Why, or where it goes |
|---|---|
| Game-defined modifier and trigger types (C++ or visual graph) | The set is closed in v1; an extension point comes when a game needs one (decision 2) |
| Smoothing, custom curve assets, mouse acceleration curves | Power curve and scale cover the common cases; add when a game needs them |
| Composite bindings (four keys into one 2D value) | Multiple mappings with swizzle and negate do the same job |
| Keyboard modifiers as part of a key (`Shift+F` as one key) | Use a chord on a modifier action |
| Touch, gestures, pen, gyro, motion and touchpad | ADR-0013 defers them to Android and later |
| Steam's own action configurator | ADR-0013 rule 7 |
| Platform redirects of whole contexts | The platform configuration layer overrides bindings slot by slot |
| Network replication or prediction of input | Owned by the networking ADR; actions are evaluated only for local players |
| Recording and replay | ADR-0013 follow-up work; the event list makes it possible |
| Accessibility options such as hold-to-toggle | Planned for P9 (accessibility); the data model must not prevent it |
| Rumble and force-feedback assets | Separate feature on ADR-0013 rule 6 outputs |
| Editor command shortcuts | The editor's command bindings are a separate system (ADR-0008, ADR-0013 rule 3) |
| UI navigation (focus moves, back, confirm) | The game UI framework (P4) owns it |

## 2. Concepts and terminology

| Ironthorn name | What it is |
|---|---|
| **Input action** | An asset naming one thing a player can do (`Jump`, `Move`, `Look`), with a value kind and optional defaults for its mappings |
| **Value kind** | `Digital` (on or off), `Axis1` (one float), `Axis2` (two floats), `Axis3` (three floats) |
| **Input context** | An asset holding a list of mappings, for one situation (on foot, in a vehicle, in a menu overlay) |
| **Mapping** | One key bound to one action inside a context, with its own modifiers and triggers |
| **Key** | A stable `core/input` key name (`Keyboard.W`, `Mouse.Left`, `Mouse.Delta`, `Gamepad.LeftStick`), ADR-0013 rule 12 |
| **Raw kind** | What a key reports: digital (keys and buttons), 1D (a trigger, the wheel, one stick axis) or 2D (a stick, the mouse delta) |
| **Device family** | `KeyboardMouse` or `Gamepad`; keyboard and mouse are one family, as they are one device set in ADR-0013 |
| **Modifier** | A step that reshapes a value (dead zone, scale, negate, swizzle, curve) |
| **Trigger** | A rule that decides, from a value over time, whether a mapping is idle, in progress or firing |
| **Trigger kind** | `Explicit`, `Implicit` or `Blocker`: how a trigger combines with the others on the same mapping |
| **Trigger state** | Per trigger, mapping and action each frame: `None`, `Ongoing` or `Triggered` |
| **Phase** | What changed for an action this frame: `Started`, `Ongoing`, `Triggered`, `Completed`, `Canceled` |
| **Context stack** | The ordered list of active contexts of one player, with priorities |
| **Claim** | A key taken by a mapping in a higher context, hidden from lower contexts until released |
| **Binding slot** | A stable, project-unique name for one rebindable binding (`jump.keyboard`), used to store rebinds |
| **Binding profile** | A named set of rebinds and input settings (`default`, `left_handed`) |
| **Input setting** | A named per-profile value a player can tune (look sensitivity, invert look) that a modifier reads |
| **Suspension** | A state in which a player's actions see no input, for example while a blocking menu is open |

The phase names are plain English words also used by other engines; they are kept because they
are what users of those engines expect (ADR-0008).

## 3. Behaviour

### 3.1 Assets

**Input action** (`.ithasset`, format `ith.input_action`, version 1, ADR-0024):

| Field | Meaning | Default |
|---|---|---|
| `displayName`, `category`, `description` | Shown in settings menus and the editor | name of the asset, empty, empty |
| `valueKind` | `Digital`, `Axis1`, `Axis2`, `Axis3` | `Digital` |
| `combine` | How values from several active mappings combine: `Strongest` or `Sum` (3.4) | `Strongest` |
| `clampToUnit` | After combining, scale a value longer than 1 down to length 1 | `false` |
| `consumesKeys` | Mappings of this action claim their keys from lower contexts (3.7) | `true` |
| `modifiers` | Appended to every mapping's modifiers | empty |
| `triggers` | Appended to every mapping's triggers | empty |

**Input context** (`.ithasset`, format `ith.input_context`, version 1):

| Field | Meaning |
|---|---|
| `displayName`, `description` | Shown in the editor and the debug overlay |
| `mappings` | Ordered list of mappings |

**Mapping** (inside a context):

| Field | Meaning | Default |
|---|---|---|
| `action` | Asset reference to an input action (by id, ADR-0015) | required |
| `key` | Stable key name | required |
| `modifiers` | Ordered list | empty |
| `triggers` | List | empty |
| `slot` | Binding slot name; present means the mapping is rebindable | absent |
| `slotLabel` | Shown next to the action name in a settings menu (`Primary`, `Alternate`) | empty |

Assets store asset ids and key names, never numeric key ids. Unknown key names are validation
errors in the editor and errors when cooking (ADR-0010 rule 13).

### 3.2 From raw input to a mapping's input value

For each mapping, each frame, the plugin reads the key's sample for the player who owns the
device (3.9):

- **Digital keys** give 0 or 1.
- **1D keys** give a float: gamepad triggers 0..1, one stick axis −1..1, the wheel in steps
  (summed per frame, fractions kept), one mouse axis in counts (summed per frame).
- **2D keys** give two floats: a stick (−1..1 each), the mouse delta (counts, summed per frame).

The value is placed in a three-component vector: a digital or 1D value goes into X, a 2D value
into X and Y, the rest is 0. Modifiers work on that vector. After the last modifier, components
beyond the action's value kind are dropped. Values are raw (ADR-0013 rule 12): no dead zone is
applied unless a modifier asks for it. A digital action's value is true when the combined value
(3.4) is not zero.

**Short presses are never lost.** `core/input` reports a press and release in one frame
(ADR-0013 rule 9). The plugin keeps, per digital key and player, a short queue of edges with
their timestamps and applies **at most one edge per key per frame**. The queue holds copies
of the fields it needs (key, press or release, timestamp, device); it never keeps `core/input`
event objects across frames, because their text views are valid only for the frame they were
delivered in.

- A press and release within one frame make the key actuated in this frame and released in the
  next frame. Durations still use the real timestamps, so a 10 ms tap measures 10 ms.
- Further edges wait in the queue for the next frames. The queue holds at most 8 edges; beyond
  that, the oldest complete press-release pairs are dropped, with a warning in development
  builds.

Analog keys use their end-of-frame value (or the frame sum for mouse motion and wheel); they
have no queue.

**Actuation.** A value is *actuated* for a trigger when its magnitude (absolute value for one
component, vector length otherwise) is at least the trigger's `threshold` (default 0.5). For a
mapping without triggers, any non-zero value counts as actuated.

### 3.3 Order of evaluation each frame

The evaluation system runs once per frame in the `Input` stage (ADR-0014 rule 17), after
`core/input` routing. For each local player, in ascending player index:

1. **Read input.** Take the events that reached the game consumer slot of each window's stack
   (ADR-0013 rule 10) from the player's devices, in order. Text events are never read
   (ADR-0013 rule 4).
2. **Update samples.** Apply device events to key samples and edge queues (3.2); apply
   disconnects (3.10).
3. **Apply changes.** If the player's context stack, the contexts it uses, the bindings or the
   profile changed since the last frame, rebuild the player's mapping table (3.7). This is the
   only step that may allocate.
4. **Evaluate mappings** in table order (3.7). For each mapping: modifiers in order, then
   triggers, giving the mapping's value and trigger state (3.6). Claims are applied as mappings
   are evaluated.
5. **Combine** the mappings of each action into the action's value and state (3.4).
6. **Derive phases** from the action's previous and current state (3.8) and write them to the
   player's action state and as events.

`now` is the frame's input time: the monotonic timestamp, in nanoseconds, at which `core/input`
finished collecting the frame's events. All trigger timing uses this clock and the event
timestamps. Game time scale and pause (ADR-0016 rule 12) do not affect triggers; a menu that
pauses the game still needs holds and taps.

### 3.4 Combining mappings into an action

- Every mapping of the action that the table includes (3.7) is evaluated.
- The action's **state** is the highest mapping state: `Triggered` over `Ongoing` over `None`.
- The action's **value** combines the values of mappings whose state is not `None`:
  - `Strongest`: per component, the value with the largest absolute value. If the largest
    absolute values tie with opposite signs, that component is 0 (W and S held together give no
    forward motion).
  - `Sum`: per component, the sum, not clamped (two mice, or keyboard turning plus mouse look).
- Then `clampToUnit` applies, if set (W and D together give a unit diagonal).
- With no mapping in a state other than `None`, the value is zero.

Action-level modifiers and triggers are not a separate step: they are appended to every mapping
of the action (3.1), so each mapping evaluates its own modifiers, then the action's.

### 3.5 Modifiers

Modifiers run in list order, each taking the output of the one before. All are pure functions
of their input, their parameters and, for the last three, frame time or input settings.

| Modifier | Parameters (defaults) | Behaviour |
|---|---|---|
| `DeadZone` | `lower` (0.2), `upper` (1.0), `shape`: `Radial` or `Axial` (`Radial`) | Radial: on the vector length, lengths below `lower` become 0, lengths from `lower` to `upper` are remapped to 0..1, lengths above `upper` become 1; direction is kept. Axial: the same on each component separately |
| `Scale` | `x`, `y`, `z` (1, 1, 1) | Multiplies each component |
| `Negate` | `x`, `y`, `z` (true, true, true) | Flips the sign of the selected components |
| `Swizzle` | `order`: one of `YXZ`, `ZYX`, `XZY`, `YZX`, `ZXY` (`YXZ`) | Reorders components; `YXZ` moves a key's X value to Y, so W can drive forward on a 2D action |
| `Curve` | `exponent` per component (1, 1, 1) | Each component becomes sign × magnitude ^ exponent; exponents greater than 1 give finer control near the centre |
| `FrameTimeScale` | none | Multiplies by the unscaled frame delta in seconds, for keys that should act as rates (keyboard turning) |
| `UserScale` | `setting` (name), `x`, `y`, `z` (true, true, true) | Multiplies the selected components by a float input setting of the player's profile (look sensitivity) |
| `UserNegate` | `setting` (name), `x`, `y`, `z` (false, true, false) | Flips the selected components when a boolean input setting is true (invert look) |

Input settings are declared in the project's input settings with a name, a type (float or
boolean), a default, a range for floats and a display name (3.11). A modifier naming an unknown
setting is a validation error; at runtime it behaves as if the setting had its declared default,
or does nothing if it is not declared.

Mouse delta values are per frame and must not be multiplied by frame time in gameplay; stick
values are positions and usually are. The documentation of each value kind says so.

### 3.6 Triggers

Every trigger has a `kind` and a `threshold` (default 0.5) for actuation. Each frame it reports
`None`, `Ongoing` or `Triggered`.

| Trigger | Default kind | Parameters (defaults) | Behaviour |
|---|---|---|---|
| `Down` | Explicit | — | `Triggered` every frame the value is actuated |
| `Pressed` | Explicit | — | `Triggered` in the frame the value becomes actuated, then `None` until it is released and actuated again |
| `Released` | Explicit | — | `Ongoing` while actuated; `Triggered` in the frame it stops being actuated |
| `Hold` | Explicit | `seconds` (0.5), `once` (true) | `Ongoing` while actuated for less than `seconds`. Then `Triggered`: in one frame if `once`, followed by `None` until released; every frame while still held otherwise. Released early: `None` |
| `HoldRelease` | Explicit | `seconds` (0.5) | `Ongoing` while actuated. On release, `Triggered` if it was held at least `seconds`, otherwise `None` |
| `Tap` | Explicit | `seconds` (0.2) | `Ongoing` while actuated for at most `seconds`; `Triggered` on a release within `seconds`; `None` as soon as the hold exceeds `seconds` |
| `MultiTap` | Explicit | `count` (2), `tapSeconds` (0.2), `gapSeconds` (0.3) | `Ongoing` from the first press; `Triggered` on the release that completes `count` taps, each held at most `tapSeconds` with at most `gapSeconds` between a release and the next press; `None` when a limit is exceeded |
| `Pulse` | Explicit | `seconds` (1.0), `atStart` (true), `limit` (0 = none) | `Ongoing` while actuated; `Triggered` at actuation if `atStart`, then each time another `seconds` have passed; after `limit` pulses, `None` until released |
| `Chord` | Implicit | `action` | `Triggered` while the referenced action's state is `Triggered` for the same player this frame, otherwise `None` |
| `Combo` | Explicit | `steps`: list of {`action`, `phase`: `Triggered` or `Completed`, `maxSeconds` (0.5)}; `cancelActions` | See below |

**Timing is exact.** Durations are measured between event timestamps, or from a press to `now`
while a key is still held. A hold of 0.5 s reaches its threshold in the first frame whose `now`
is at least 0.5 s after the press timestamp. A release that comes from the edge queue (3.2)
uses its own timestamp, not `now`.

**Combo.** The combo advances when the action of its current step shows the step's phase for
this player, within `maxSeconds` of the previous step (the first step has no time limit). After
the first step it is `Ongoing`; on the last step it is `Triggered` for that frame and resets. It
resets to `None` when a step's time runs out, when any action in `cancelActions` is `Triggered`,
or when the action of a step other than the current one shows its phase out of order. A step
with phase `Triggered` advances the combo, or counts as out of order, **only in the first frame
of a run of `Triggered` frames**; later frames of the same run are ignored, so actions using
`Down` (or no trigger) work as steps. The combo's own key is usually the last step's key; the trigger ignores the key's value and its
`threshold`.

**Combining triggers on one mapping** (including the action's appended triggers):

1. No triggers: `Triggered` while the value is actuated (any non-zero value), otherwise `None`.
2. If any `Blocker` is `Triggered`, the mapping is `None`.
3. Otherwise the mapping is `Triggered` when every `Implicit` trigger is `Triggered` and, if
   there are `Explicit` triggers, at least one of them is `Triggered`. A mapping with **no
   `Explicit` triggers** must also have its own value non-zero: implicit triggers alone never
   fire a mapping whose key isn't held, so `Ctrl` alone never fires Save on `Ctrl + S`.
4. Otherwise it is `Ongoing` if any `Explicit` or `Implicit` trigger is `Ongoing` or
   `Triggered`. This includes a mapping without explicit triggers whose implicit triggers are
   satisfied while its own key isn't held (`Ctrl` held, S not yet pressed).
5. Otherwise `None`.

Any trigger's kind may be changed in the asset: a `Chord` set to `Blocker` means "not while that
action is triggered".

**A mapping that becomes active while its key is held** (its context was just added, a claim on
it ended, 3.7) starts with "already held" as its previous sample and starts its timers at
activation. `Pressed`, `Tap` and `MultiTap` therefore don't fire from a key held before the
mapping existed; `Down` fires at once; `Hold` counts from activation. This keeps movement
going when a context swap happens under a held key, without phantom presses.

### 3.7 The context stack, the mapping table and consumption

Each player has a **context stack**: a list of entries {context, priority (integer, higher
first)}. Entries come from two places (3.9): the player's own list, and the contexts provided
by the entity the player controls (ADR-0016 rule 8). Adding the same context twice keeps one
entry, with the later priority.

The **mapping table** is rebuilt when the stack, a context asset, the bindings or the profile
changes:

1. Order contexts by priority, highest first; equal priorities by most recently added first.
2. **One context per action:** an action's mappings come only from the highest-ordered
   contexts that map it. Contexts with the same priority as that context also contribute. A
   higher context can therefore redefine an action's keys entirely.
3. **Dependencies first:** actions referenced by `Chord` or `Combo` triggers of other actions
   are placed first, each after the actions it depends on itself. These actions never claim
   keys, whatever their `consumesKeys` says (a validation warning if it is set explicitly). A
   dependency cycle is a validation error; a cycle formed only at runtime by a combination of
   contexts is logged once as an error and the triggers in the cycle report `None`.
4. Then the remaining mappings in context order; within one context, mappings that have a
   `Chord` trigger come before mappings that don't, otherwise declaration order.
5. Each mapping's key is resolved through the bindings (3.11).

**Claims.** During evaluation, when a mapping of an action with `consumesKeys` reaches a state
other than `None`, it **claims** its key for that player. A claimed key reads as not actuated
(zero) for every mapping of a **different, lower context** evaluated after it. "Lower" means
later in table order (step 1), so between two contexts of the same priority, the more recently
added one hides a key it shares with the older one. The claim lasts
until the key is released, even if the claiming mapping goes back to `None` while the key is
still held (a `Pressed` mapping in a vehicle context keeps W from also moving the character on
foot). A claim ends early when its mapping leaves the table; the key then becomes visible to
lower mappings under the "already held" rule of 3.6. That rule covers digital keys only: an
**analog key** whose claim ends is read as it is, so a deflected stick or held trigger counts
as a new actuation for the lower mappings (a `Pressed` mapping on it can fire).

**Analog claims end on rest.** An analog key has no release edge, so its claim ends when it is no
longer actuated: its magnitude falls below the claiming mapping's actuation threshold (the
lowest `threshold` of its triggers), or below 0.2, the default dead zone, if the mapping has no
triggers.

**Overlapping controls.** A 2D control and its 1D axes are one control for claims: a claim on
`Gamepad.LeftStick` also hides `Gamepad.LeftStickX` and `Gamepad.LeftStickY`, and a claim on
either axis hides the stick; the same holds for `Mouse.Delta` and `Mouse.DeltaX` /
`Mouse.DeltaY`, and for every other stick.

**Within one context there are no claims**: one key may drive two actions in the same context.
The exception is the **chord rule**: when a mapping with a `Chord` trigger on key K is not
`None`, the mappings on K without a `Chord` trigger in the same context read K as not actuated
this frame. With `Ctrl` as a chord, `Ctrl + S` saves and S alone moves back, and saving never
also moves back.

Claims are internal to the plugin. Its `core/input` consumer reports every event it reads from
an assigned device as consumed; nothing sits below the game slot.

### 3.8 Action states, phases and events

The action's state in the previous frame and in this frame give its phases:

| Previous | Current | Phases this frame |
|---|---|---|
| `None` | `Ongoing` | `Started`, `Ongoing` |
| `None` | `Triggered` | `Started`, `Triggered` |
| `Ongoing` | `Ongoing` | `Ongoing` |
| `Ongoing` | `Triggered` | `Triggered` |
| `Triggered` | `Triggered` | `Triggered` |
| `Triggered` | `Ongoing` | `Ongoing` |
| `Ongoing` | `None` | `Canceled` |
| `Triggered` | `None` | `Completed` |
| `None` | `None` | none |

So a `Hold` released early gives `Started`, `Ongoing` … `Canceled`; a held key with `Down` gives
`Started` and `Triggered` in the first frame, `Triggered` while held, and `Completed` on
release.

Each action of each local player has an **action state** with:

- the value (in the action's value kind);
- the current state;
- the phases of this frame;
- `startedAt`: the timestamp of the last `Started`;
- `triggeredSince`: the timestamp at which the current run of `Triggered` frames began, or none;
- for each phase that occurred this frame, its timestamp (below).

**Timestamps** (nanoseconds, the `core/input` clock): a phase caused by a device edge carries
that edge's timestamp; a phase caused by time (a hold threshold, a pulse, a combo step timing
out) carries the exact instant the threshold was crossed (press timestamp plus the threshold);
a phase caused by anything else (a context removed, suspension, a mapping table change) carries
`now`.

**Events.** For every phase, the plugin sends one **action event** (ADR-0016 rule 3) with the
player entity, the action, the phase, the value and the timestamp. Within a frame, events are
sent by ascending player index, then mapping table order, then phase order as in the table
above. `Ongoing` and `Triggered` are sent every frame they occur, so a system can react to
"each frame while held" from events alone; systems that need a continuous value read the action
state instead.

**Removing an action from the table** (its context removed, rebinding) evaluates it as `None`
that frame, so it gives `Completed` or `Canceled` like any other end.

### 3.9 Players, devices and ECS data

- A **local player** is an entity with a `Player` component and a device set (ADR-0013 rule 12,
  ADR-0016 rule 7). The plugin evaluates only local players. Remote players and AI controllers
  have no action states.
- Each player reads only the devices assigned to it. Events from unassigned devices are not
  read; joining stays with the game, as ADR-0013 rule 12 describes.
- **Gamepads are merged per player.** A player with several gamepads has one sample per gamepad
  key, merged across all of that player's gamepads. Mappings name `Gamepad.*` keys, never a
  particular gamepad.
  - **Buttons** are a logical OR: the first gamepad to press a button gives the press, the last
    one to release it gives the release.
  - **A 1D axis** (a trigger, one stick axis) takes the value with the largest magnitude; on a
    tie, the first gamepad in the player's device set wins.
  - **A stick** takes the longest vector of any one gamepad, with both components from that
    gamepad; components are never mixed across gamepads.
- Two players may use the same context assets; every claim, queue, timer and state is per
  player.
- **Components on the player entity:**
  - **input contexts**: the player's own list of {context, priority}. Gameplay changes it by
    writing the component (declared write access); the plugin sees the change through the
    `Changed` filter (ADR-0014).
  - **action states**: one entry per action in the player's table, written only by the plugin.
    Lookups take an action asset handle; an action not in the table reads as zero and `None`.
  - **binding profile**: the name of the player's profile (3.11), `default` when absent.
- **Contexts provided by a controlled entity:** an entity may carry a **provided input
  contexts** component listing {context, priority}. While a player `Controls` it (ADR-0016
  rule 8), those contexts are part of the player's stack; possessing another entity or ejecting
  removes them. A vehicle brings its driving controls without game code adding and removing
  contexts.
- **Reading actions in gameplay.** A gameplay system queries player entities with action states
  and a `Controls` link, reads the values or phases it needs, and writes intent onto the
  controlled entity (a movement input component, an ability request event). Or it reads action
  events. Gameplay code references actions through asset-reference fields on its own
  components or settings resources, set in the editor; looking actions up by path is for tests
  and tools.
- **Last used device family:** each player records which family last actuated a key (a press,
  or an analog value beyond 0.5). Input prompts use it to show the right key or gamepad glyph.
- **Key prompts:** the plugin answers "which keys currently trigger action A for player P?",
  ordered by context order, filtered by device family, as stable key names. Turning them into
  layout key names for keyboards (ADR-0013 rule 3) and gamepad glyphs (rule 6) is done by the
  application or the UI, which can reach the platform layer; a `runtime` module cannot.
- **Networking:** evaluation happens only where a local player exists. The plugin replicates
  nothing; sending player intent to a server belongs to the networking ADR. A dedicated server
  evaluates nothing because it has no local players.

### 3.10 Edge cases

- **Focus loss.** `core/input` releases every held key with real release events (ADR-0013 rule
  9). The plugin treats them as forced ends (below): actions end with `Completed` or
  `Canceled`, timestamped with the release events, and release-based triggers don't fire. Keys still held when focus returns are ignored until
  pressed again, which `core/input` already guarantees.
- **Forced ends.** An action can end without its key being released by the player:
  suspension, a claim from a higher context, the chord rule (3.7), focus loss (including the
  release events `core/input` sends for it) and capture loss. A forced end makes the mapping
  read its key as not actuated, so the action ends with `Completed` or `Canceled` as usual, but
  it **never fires `Released`, `HoldRelease` or `Tap`**: those fire only on a real release edge
  of a key the mapping can see.
- **Mouse capture.** While the mouse is captured, mouse input reaches only the game slot
  (ADR-0013 rule 11). Without capture, mouse input still reaches the game slot when no consumer
  above it takes it, so cursor-driven games work. `core/input` exposes whether the mouse is
  captured but sends no releases when capture ends; the plugin watches that state itself, and
  when capture ends it treats every held mouse button of the keyboard-and-mouse set as released
  at that moment (`now`), so mouse actions end with `Completed` or `Canceled`. Mouse buttons
  still held then count only once pressed again.
- **Suspension.** A player can be **suspended**: the game UI framework sets it while a layer
  that blocks gameplay is shown (an inventory, a pause menu), and game code may set it until
  that framework exists. While suspended, every mapping of the player reads its key as not
  actuated, so actions end with `Completed` or `Canceled` at the moment of suspension. On
  resume, digital keys still held are ignored until pressed again; analog keys are read at once.
  Without suspension, a key held before a menu opened would keep its action running, because
  ADR-0013 sends the release to the consumer that took the press.
- **Device disconnect.** Every key and axis of the disconnected device reads as released at the
  disconnect timestamp: digital keys get a release edge, axes become zero. The device's player
  keeps its other devices. A reconnected device's held keys count only once pressed again.
- **Chords across contexts.** A `Chord` reads the referenced action's final state this frame,
  wherever that action is mapped. If the referenced action is not in the player's table, the
  chord is `None` (validation in the editor warns when a context uses a chord whose action is
  mapped in no context).
- **Context changes during a hold.** Removing a context ends its actions this frame (3.8);
  adding one follows the "already held" rule (3.6). Neither produces a phantom press or a stuck
  action.
- **Same key, several actions, same context:** all of them fire, except as the chord rule says.
- **Same action in two contexts:** only the higher context's mappings count (3.7 step 2).
- **Opposite keys held together** on a `Strongest` action cancel out on that component (3.4).
- **Very short taps** and **press-release-press within one frame** are spread over frames by the
  edge queue, never lost (3.2).
- **Play In Editor** runs the same evaluation as a shipped game. The editor's keys sit above the
  game slot (ADR-0013 rule 11), so a context can never take Esc, Shift + F1 or F8.
- **Ejecting** in Play In Editor removes the player's `Controls` link (ADR-0016 rule 9), which
  removes the provided contexts; possessing restores them.

### 3.11 Rebinding, profiles and storage

**Slots.** A mapping with a `slot` is rebindable. A slot name is stable text chosen in the
editor (generated as `<action>.<family>` with a number if needed, and editable), and is kept
when the action or context is renamed or moved. Several mappings may share a slot, which is how
one rebind changes Jump in both the on-foot and swimming contexts. Mappings that share a slot
must have the same action, the same device family and the same default key; otherwise it is a
validation error.

**What a rebind changes.** Only the key. Modifiers and triggers stay as authored. A replacement
key must:

- be in the slot's device family (a keyboard slot takes keyboard and mouse keys, a gamepad slot
  takes gamepad keys);
- have a compatible raw kind: a digital slot accepts digital or 1D keys, a 1D slot 1D keys, a
  2D slot 2D keys.

A slot can also be **unbound** (no key).

**Profiles.** A binding profile holds rebinds (slot → key or unbound) and input setting values.
Every player uses the profile named in its binding-profile component, `default` when none is
set. A game may create, copy, rename, reset and delete profiles; resetting a profile removes its
overrides so the authored keys and setting defaults apply again.

**Storage.** Bindings are configuration (ADR-0013 rule 13): a config document with format
`ith.input_bindings`, read through the layered configuration of ADR-0008 (engine defaults,
project, platform, user). A layer overrides individual slots and settings of a profile, never a
whole profile. A player's changes are written to the **user** layer, by stable names, and only
where they differ from the layers below. An unknown slot, key or setting name is a warning
naming the file and the name, and the entry is ignored (ADR-0010 rule 13). An example of the
data:

```json
{
  "profiles": {
    "default": {
      "slots": { "jump.keyboard": "Keyboard.Space", "crouch.gamepad": null },
      "settings": { "look_sensitivity": 1.4, "invert_look_y": true }
    }
  }
}
```

**Applying changes.** A rebind or profile change takes effect at the next frame's step 3 (3.3).
Actions whose keys changed end as described in 3.8; the new key follows the "already held" rule.

**Conflicts.** Before applying a rebind, the plugin reports every other slot in the same device
family and profile that uses the same key, with its action and contexts. It never resolves a
conflict on its own; the settings UI offers to swap, to unbind the other slot or to keep both.

**Interactive capture** ("press a key"):

- Started by the settings UI for one player and one slot. While capturing, that player is
  suspended (3.10), so pressing a key to bind it never also fires its current action.
- Captures the first key of the slot's family and a compatible raw kind that is pressed (or,
  for analog keys, moved beyond 0.5) on the player's devices, after every key held at the start
  has been released. Mouse motion is ignored unless the slot is 2D.
- `Keyboard.Escape` cancels by default; the UI may pass other cancel keys and keys to ignore,
  and an optional timeout.
- The capture is a UI consumer: it sits at the focused-widget position of the stack and
  consumes the keys it reads, so neither UI nor gameplay sees them.
- It returns the key name, or a cancel or timeout result; it does not apply the rebind.

**Display.** Settings menus list slots grouped by the action's `category`, showing the action's
`displayName`, the slot's `slotLabel` and the key's layout name or gamepad glyph. The binding UI
states that keyboard bindings follow key position, not the printed letter (ADR-0013
consequences).

### 3.12 Failure behaviour

- An asset that fails to load leaves its context out of the stack with an error naming the
  asset; other contexts keep working.
- A mapping whose action is missing is dropped with a warning; the editor reports it as a
  validation problem.
- Raw kinds never make a mapping invalid: a 2D key on an `Axis1` action feeds X and its Y is
  dropped (3.2). The editor shows a note on such mappings so a missing swizzle is noticed.
- A trigger or modifier type the build doesn't know is a load error for that asset (cooking
  fails; in the editor the asset loads with the entry removed and a warning).
- Nothing in evaluation asserts on user data; errors are logged once per cause, not every frame.

### 3.13 Editor

Familiar to users of Unreal Engine's editor (ADR-0008), in Ironthorn's own look:

- **Content Browser:** an **Input** category in the create menu with *Input Action* and *Input
  Context*. Both open in an asset editor with a Details-style panel generated from reflection.
- **Input action editor:** value kind, combine rule, clamp, consumption, display fields, and
  ordered, collapsible lists of modifiers and triggers with add, remove and reorder.
- **Input context editor:** one row per action, with its key mappings nested beneath; a **+**
  on the action row adds a mapping and a **+** at the top adds an action. Each mapping row has:
  - a key picker with a search field over stable key names and a button that captures the next
    key pressed;
  - modifier and trigger lists like the action editor's;
  - a rebindable check box with the slot name and label.

  When a mapping is created on a gamepad stick or trigger, the editor adds a `DeadZone`
  modifier (radial for sticks, axial for triggers), visible and removable like any other.
- **Validation** runs on save and in the asset scan: missing actions, unknown keys, slot
  conflicts (3.11), dependency cycles (3.7), unknown input settings (3.5), chords whose
  action is mapped nowhere (3.10), a `Chord` without an action, a `Combo` without steps, and
  modifier or trigger parameters out of range (negative times or thresholds, a `DeadZone` with
  `lower` not below `upper`, a `MultiTap` `count` below 2). Problems appear in the editor's message log with links to the
  asset.
- **Project Settings → Input:** the input settings players can tune (3.5), the default profile's
  project-layer overrides, and the debug options below.
- **Debug overlay** (toggled with the console command `input.debug`, in development builds and
  the editor): per local player, the context stack with priorities, each action with its value,
  state, phase and time since `Started`, claimed keys, the edge queues and the last used device
  family. Values update live in Play In Editor.
- **Debug commands** (development builds): `input.press <key> [player]`,
  `input.release <key> [player]` and `input.axis <key> <x> [y] [player]` inject device events
  through `core/input`; `input.action <action> <value> [player]` forces an action's value and
  `Triggered` state for one frame. All of them go through the same evaluation as real input.

## 4. Requirements

Each requirement is an acceptance test. Unless stated otherwise, tests run without a display:
they inject `core/input` device events with explicit timestamps and a fixed frame time
(ADR-0013 rule 14), run the evaluation system in a world, and check action states and events.

### Assets and data

1. Input action and input context assets round-trip through the text archive with every field,
   and a missing field takes the default in section 3.1.
2. An asset referencing an unknown key name, an unknown modifier or trigger type, or an unknown
   input setting is reported as a validation problem in the editor and fails cooking.
3. Mappings that share a slot with different actions, device families or default keys are a
   validation error.
4. A `Chord` or `Combo` dependency cycle within the project's assets is a validation error.

### Values and modifiers

5. A digital key on an `Axis2` action with `Swizzle(YXZ)` yields (0, 1); with `Negate` after it,
   (0, −1).
6. `DeadZone` radial with (0.2, 1.0): stick (0.1, 0) yields (0, 0); (0.6, 0) yields (0.5, 0);
   (0.8, 0.8) keeps its direction and has length 1. Axial applies the same remapping to each
   component separately.
7. `Scale`, `Negate`, `Curve` (exponent 2 on −0.5 gives −0.25), `FrameTimeScale`, `UserScale`
   and `UserNegate` each give the documented result, and modifiers run in list order.
8. Action-level modifiers and triggers run after each mapping's own.
9. `Strongest`: W (0, 1) and S (0, −1) together give (0, 0); W and D give (1, 1); with
   `clampToUnit`, W and D give a vector of length 1. `Sum`: two mouse mappings add.
10. Mouse delta and wheel values are the per-frame sums from `core/input`; no dead zone is applied
    unless a modifier asks for it.

### Triggers and timing

11. A mapping without triggers is `Triggered` exactly in the frames its value is non-zero.
12. `Pressed` fires once per press; `Released` fires once per release; `Down` fires every frame
    while actuated.
13. `Hold` 0.5 s, `once`: press at t, frames every 16.6 ms; `Triggered` in the first frame with
    `now ≥ t + 0.5 s`, phase timestamp exactly `t + 0.5 s`; release before gives `Canceled` with
    the release timestamp. With `once` false it fires every frame after the threshold.
14. `HoldRelease`, `Tap`, `MultiTap` and `Pulse` (including `atStart` and `limit`) follow
    section 3.6 at their boundaries: exactly at the limit counts as within it.
15. A press and release with timestamps 10 ms apart inside one frame produce `Started` and
    `Triggered` in that frame and `Completed` in the next for a `Down` mapping; a `Tap` mapping
    measures 10 ms and triggers.
16. Three presses inside one frame are spread over the next frames and each produces its
    phases; a ninth queued edge drops the oldest complete pair and logs a warning in development
    builds.
17. Trigger combination: blockers win; all implicit and one explicit are needed; `Ongoing` and
    `None` follow section 3.6 rules 1 to 5, tested with every combination of one implicit, one
    explicit and one blocker trigger.
18. `Chord` on a `Ctrl` action: `Ctrl + S` triggers Save and not Back (same context); S alone
    triggers Back; Save in a higher context than the `Ctrl` action still sees `Ctrl` in the same
    frame.
19. `Combo` of three steps triggers when the steps happen in order within their time limits,
    resets when a step is late, when a cancel action triggers, and when a step is out of order.

### Phases and events

20. Every row of the phase table in section 3.8 is produced by a test.
21. Phase timestamps follow section 3.8: device edges carry the event timestamp, time thresholds
    the computed instant, other causes `now`.
22. Events are sent in the order of section 3.8 and every reading system sees each one exactly
    once (ADR-0016 rule 3).
23. The same recorded device events and frame times produce identical action states and events
    on two runs (ADR-0016 rule 18).

### Contexts and consumption

24. With a vehicle context at priority 10 mapping W to Accelerate and an on-foot context at 0
    mapping W to Move, W triggers only Accelerate; removing the vehicle context while W is held
    ends Accelerate (`Completed`) and starts Move with `Triggered` in the same frame and no
    `Pressed` firing.
25. A `Pressed` mapping in a higher context keeps W claimed while W is held, so a lower `Down`
    mapping never fires during that press.
26. An action with `consumesKeys` false never hides keys from lower contexts.
27. An action mapped in two contexts of different priority uses only the higher context's
    mappings; at equal priority both contribute.
28. Equal priorities order by most recently added first.
29. Adding a context while a key is held: `Down` fires at once, `Pressed` and `Tap` don't fire,
    `Hold` counts from the moment of adding.
30. Contexts provided by a controlled entity join the player's stack on possess and leave it on
    eject, ending their actions with `Completed` or `Canceled`.

### Routing, focus and devices

31. Input consumed above the game slot (UI, console, editor keys) never reaches an action.
32. Focus loss ends every running action, using the release events' timestamps. Losing mouse
    capture ends every action driven by a held mouse button at `now`, and that button counts
    again only after a new press; uncaptured mouse input that no higher consumer takes still
    drives actions.
33. Suspension ends every action of that player at once; on resume a still-held digital key does
    nothing until pressed again, and a stick deflection is read immediately.
34. Disconnecting a gamepad ends its actions at the disconnect timestamp; the player's keyboard
    actions keep working.
35. Two local players with the same contexts: each reads only its own devices; a claim, queue or
    timer of one never affects the other.
36. Events from devices assigned to no player change no action state.

### Rebinding

37. A rebind in the user layer changes the effective key of every mapping sharing that slot,
    from the next frame; modifiers and triggers are unchanged.
38. Layering: a platform-layer override of a slot is replaced by a user-layer override; removing
    the user override restores the platform key.
39. Only differences from lower layers are written to the user layer; resetting a profile
    removes them.
40. Unknown slot, key or setting names in configuration are warnings and are ignored.
41. A rebind to a key of another device family or an incompatible raw kind is refused with an
    error naming the reason.
42. The conflict query lists every slot in the same family and profile using the key.
43. Interactive capture ignores keys held at its start, cancels on Escape, honours ignored keys
    and the timeout, suspends the player while active, and consumes what it reads.
44. Two players with different profiles use different keys for the same slot.

### ECS and performance

45. Gameplay systems read action states through declared read access and change contexts
    through declared write access; the plugin's system declares exactly what it touches
    (ADR-0014 rule 18).
46. An action not in a player's table reads as zero and `None`.
47. The key-prompt query returns the effective keys of an action for a player in context order,
    filtered by device family, as stable key names (display names and glyphs come from the
    application).
48. Steady-state evaluation allocates no memory; only a mapping table rebuild may allocate.
49. Four local players with 100 mappings each evaluate in at most 0.1 ms per frame on the Low
    tier reference machine (ADR-0005), measured by a benchmark.

### Editor

50. Creating, editing and saving both asset types in the editor round-trips; capturing a key in
    the key picker records the physical key name.
51. A new mapping on a gamepad stick or trigger gets a visible `DeadZone` modifier.
52. The debug overlay shows the data listed in section 3.13 for every local player in Play In
    Editor, and the debug commands produce the same phases as real device input.

## 5. Fit with Ironthorn

### Layer and modules (ADR-0004, ADR-0017)

- **`input_actions`**, layer `framework`: assets, evaluation system, components, events,
  bindings, capture, key prompts and debug commands. Depends on `core` modules only: input,
  ECS, assets, serialization, reflection, and the gameplay tags or config modules if used. It
  never depends on `platform` types; device events come through `core/input`.
- **`input_actions_editor`**, layer `editor`, kind `editor`: the asset editors, the key picker,
  validation reporting and the overlay's editor parts.
- The overlay draws through the debug drawing or UI facility available when it is built; it is
  a client-only part and must not be linked into a dedicated server.
- `input_actions` is of kind `runtime`, like `core/input` (decision 1).

### Mapping onto the ECS (ADR-0016)

| Concept | ECS form |
|---|---|
| Input action, input context | Assets, referenced by id |
| Player's own contexts | Component on the player entity |
| Contexts a controlled thing brings | Component on that entity, joined through `Controls` |
| Action values, states, phases | Component on the player entity, written by the plugin |
| Binding profile choice | Component on the player entity |
| What happened | Action event, read by systems with their own cursors |
| Evaluation | One system in the `Input` stage |
| Bindings and input settings | Configuration, cached in an app-scoped resource |
| Suspension | A flag in the player's action-state component, set through a function the UI framework calls |

There are no callbacks, no per-entity input components with virtual functions and no input
"owner" objects. Unreal developers looking for where input is bound find it in the input
context assets and in the systems that read action states (to be added to the "If you know
Unreal" table of ADR-0016 when the plugin lands).

### Why this design is the simple one

- One evaluation pass per player per frame, in a fixed order, with one table rebuilt only on
  change.
- One state machine (three trigger states, five phases from a fixed table) shared by every
  trigger.
- Action-level modifiers and triggers are just appended to mappings, so there is one evaluation
  path.
- Consumption reuses the rule `core/input` already uses (a claim lasts until release) instead of
  a second model.
- Rebinding changes only keys, by stable slot names, in the existing layered configuration.
- Composites, custom trigger types and smoothing are left out until a game needs them.

## 6. Decisions

The owner settled the questions this spec raised (decided 2026-11-21).

1. **Module kind.** `input_actions` is a `runtime` module, and `core/input` is `runtime` too.
   `runtime` gameplay modules can read action states; on a dedicated server there are no local
   players, so the plugin evaluates nothing.
2. **Modifiers and triggers are a closed set in v1.** An extension point for game-defined types
   is added later, when a game needs one.
3. **Trigger clock.** Triggers use unscaled real time (`core/input` timestamps); pause and time
   dilation don't affect holds, taps or pulses (3.3).
4. **The highest context that maps an action fully replaces lower contexts' keys for that
   action** (3.7 step 2). Contexts at the same priority as that context still contribute.
5. **Event volume.** `Ongoing` and `Triggered` events are sent every frame they occur (3.8).
6. **Config file location and name** for the `ith.input_bindings` document in each layer stay
   open until a configuration-layout ADR exists. Only the format name and content are fixed by
   this spec.
7. **The click that captures the mouse** in a game viewport (ADR-0013 rule 11) is consumed by
   the capture and never fires a game action.
8. **Requirements on `core/input`**, accepted and built into it:
   - the frame's input timestamp (`now`, 3.3);
   - gamepad connect and disconnect events in the frame's event order (3.10);
   - stable key names for sticks, mouse motion and the wheel next to their 1D axes, such as
     `Gamepad.LeftStick`, `Mouse.Delta` and `Mouse.Wheel` (3.2).

## 7. Sources

Public documentation only. Epic Games' feature is named here only to identify the inspiration;
"Enhanced Input" is Epic's name and is not used for any Ironthorn feature.

**Unreal Engine (Epic Games), Enhanced Input documentation:**

- [Enhanced Input in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/enhanced-input-in-unreal-engine):
  action value types, mapping contexts and priority, modifiers, triggers and trigger kinds,
  trigger states and events, mappable configurations, debug commands, input injection.
- [Enhanced Input in the Parrot sample](https://dev.epicgames.com/documentation/unreal-engine/enhanced-input-in-parrot-for-unreal-engine):
  user settings, mappable keys and saving in a sample game.
- API reference pages (behaviour and parameter names of triggers and user settings only):
  [Tap trigger](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/EnhancedInput/UInputTriggerTap),
  [Hold trigger](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/EnhancedInput/UInputTriggerHold),
  [Hold-and-release trigger](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/EnhancedInput/UInputTriggerHoldAndRelease),
  [Pulse trigger](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/InputTriggerPulse),
  [Combo trigger](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/InputTriggerCombo),
  [Repeated tap trigger](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/EnhancedInput/UInputTriggerRepeatedTap),
  [User settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/EnhancedInput/UEnhancedInputUserSettings),
  [Player-mappable key settings](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/EnhancedInput/UPlayerMappableKeySettings).

**Unity Input System (Unity Technologies), version 1.11 manual:**

- [Actions](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.11/manual/Actions.html):
  action maps, enabling and disabling.
- [Interactions](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.11/manual/Interactions.html):
  phases, press, hold, tap, slow tap and multi-tap, default behaviour per action type, several
  interactions on one binding.
- [Processors](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.11/manual/Processors.html):
  dead zones, scale, invert, normalize, clamp, and the order of binding and action processors.
- [Action bindings](https://docs.unity3d.com/Packages/com.unity.inputsystem@1.11/manual/ActionBindings.html):
  composites, conflict resolution between controls and overlapping bindings (more complex
  binding first), interactive rebinding, saving overrides separately from the asset, binding
  groups.

**Godot Engine documentation:**

- [InputMap](https://docs.godotengine.org/en/stable/classes/class_inputmap.html): named actions,
  per-action dead zone (0.2 default), bindings stored in project settings.
- [Input](https://docs.godotengine.org/en/stable/classes/class_input.html): just-pressed
  semantics including sub-frame taps, action strength, axis and vector helpers with a circular
  dead zone, simulated action presses.

**Ironthorn:** ADR-0003, ADR-0004, ADR-0005, ADR-0008, ADR-0010, ADR-0013, ADR-0014,
ADR-0015, ADR-0016, ADR-0017 and ADR-0024 in [docs/decisions](../decisions/README.md).

## 8. Implementation status

Version 1 of the plugin implements this spec, including the corrections made during
implementation and review (3.6 rules 3 and 4 for mappings without explicit triggers, combo
steps on the first frame of a `Triggered` run in 3.6, equal-priority claims, analog claims and
overlapping controls in 3.7, gamepads merged per player in 3.9, key prompts as stable names in
3.9, forced ends in 3.10, the added validation checks in 3.13). These requirements are **not in v1**:

| Requirement | Why | Comes with |
|---|---|---|
| 2 and 4: failing cooking | There is no cooker yet | The cooker |
| 43: interactive key capture | Needs the game UI consumer it runs in | The game UI framework (P4) |
| 45: declared system access | The ECS schedule with declared access doesn't exist yet | The ECS schedule (ADR-0014 rule 18) |
| 50 to 52: asset editors, key picker, debug overlay and console commands | No editor shell, overlay or console yet | The editor shell (P2) and the debug console |
