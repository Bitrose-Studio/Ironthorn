# ADR-0013: Input: events, state, routing and consumption

- **Status:** Accepted
- **Amended by:** [ADR-0027](0027-input-rules-completed.md), [ADR-0053](0053-play-in-editor-and-simulate.md)
- **Date:** 2026-11-20
- **Related:** [ADR-0003](0003-clean-room.md) (clean room), [ADR-0004](0004-modules-and-layering.md)
  (layers), [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (remappable bindings, Play In
  Editor controls, layered configuration), [ADR-0010](0010-serialization.md) (config format),
  [ADR-0012](0012-windowing-and-event-loop.md) (SDL backend, event pump)

## Context

Every consumer of input (the editor, the debug console, game UI and gameplay) reads the same
devices. The engine needs one raw input layer and one rule for who gets an event, before the
first interactive viewport exists. Requirements:

- Keyboard bindings that stay in place on any layout (W/A/S/D on AZERTY), with key names and
  text in the user's layout.
- Mouse with unaccelerated relative motion for camera control.
- Gamepads (Xbox, PlayStation, Switch, Steam Deck), their type for button glyphs, and Steam
  Input.
- Text input with IME composition, separate from key presses.
- **Consumption:** an open menu or a focused text field must stop gameplay from seeing the same
  input.
- Play In Editor behaves like the shipped game, plus the editor's keys (ADR-0008): Esc stops,
  Shift + F1 frees the mouse, F8 possesses or ejects.
- Game UI lives outside the scene, in UI layers owned by the local player, and needs focus and
  gamepad navigation.
- An action and mapping layer (input actions, mapping contexts with priority, modifiers,
  triggers, player rebinding) comes later as a framework plugin, built clean-room
  (ADR-0003). This ADR fixes only what it builds on.

Lessons from earlier engine prototypes by the same maintainer:

- Input that could not be consumed let clicks on UI reach gameplay, and dropped key modifiers
  broke shortcuts.
- Building state from events, so that a press and release within one frame still counts, and
  releasing every held key on focus loss, removed stuck keys on every platform.
- A routing stack where a consumed press hides the key from lower consumers until its release
  never produced a release without its press.

## Options considered

- **Polling only** (each consumer reads device state). Simple, but loses taps shorter than a
  frame, gives no order of events for combos and text, and has no way to consume.
- **Callbacks per consumer** registered with the platform layer. Ordering between consumers
  becomes registration order, and callbacks would run inside the platform pump.
- **An event list per frame, a state built from it, and an ordered stack of consumers** that
  may consume events (chosen). Both styles of reading are available, order is explicit, and
  nothing runs inside the pump.
- **Putting actions and mapping contexts into core.** They are a large, optional feature with
  their own assets and editor; keeping them in a plugin keeps core small, as the layering asks.

## Decision

### Layers

1. **`platform`** translates SDL events into engine device events (ADR-0012). **`core/input`**
   owns the device registry, per-frame state, routing and consumption. The **action and mapping
   plugin** lives in the framework layer and consumes `core/input` through the contract in
   rule 12. No input type depends on SDL.

### Device events

2. Every event carries a **timestamp** (monotonic, nanoseconds), the **device id** and, for
   keyboard, mouse and text, the **window id**.
3. **Keyboard:** a **physical key** (scancode, named after its position on a US layout, e.g.
   `Keyboard.W`), the **layout key** (what the user's layout prints on it), modifiers and a
   repeat flag. Game bindings and viewport navigation use the physical key. Editor command
   shortcuts (Ctrl + Z, Ctrl + D) use the layout key, so a letter shortcut follows its letter.
   Key names shown to users come from the layout key.
4. **Text input** is a separate event stream: committed UTF-8 text, and IME composition (text,
   cursor, selection). Text input is on only while a text field has focus; the field reports its
   rectangle so the IME window appears next to it. Key presses used by the IME don't reach
   consumers as key events.
5. **Mouse:** buttons (left, right, middle, back, forward), position in window pixels, wheel
   (fractional steps kept), and relative motion. **Relative mode** hides and locks the cursor and
   reports unaccelerated motion; it is a window operation (ADR-0012).
6. **Gamepads:** connect and disconnect events; a session id plus a persistent GUID; name and
   **type** (Xbox, PlayStation, Switch, Steam and so on) for glyphs. Buttons use positional
   names (face south, east, west, north), sticks are −1..1 and triggers 0..1, **without dead
   zones** (the plugin applies them). Output: rumble, trigger rumble and LED colour. Gyro,
   accelerometer and touchpad are exposed when a feature needs them.
7. **Steam Input:** gamepads come through SDL, which handles Steam's virtual gamepads and the
   configuration Steam passes to games. The engine keeps the Steam handle of each gamepad, so
   Steam's glyphs can be used. Steamworks' own action API is not used; it is reconsidered only
   if a game wants Steam's action configurator. Behaviour is checked on a Steam Deck.
8. **Touch and pen** come with Android.

### State

9. **Per-frame state is built from the events.** For each key and button: held, pressed this
   frame and released this frame; a press and release in one frame report both. Repeats are not
   presses. Mouse motion and wheel are summed per frame; gamepad values are per device.
   **Focus loss releases everything** with real release events, and keys still held when focus
   returns count only once pressed again. Consumers can read the state, the ordered events, or
   both.

### Routing and consumption

10. **Each window has an ordered stack of consumers**, top first. In the editor:
    1. editor and debug: the console, debug overlays, and editor keys (Esc, Shift + F1, F8 in
       Play In Editor; editor shortcuts);
    2. UI: the focused widget, then UI layers from modal down to HUD;
    3. game: the player's input (the mapping plugin's contexts sit here and order themselves).

    A shipped game has the same stack without the editor's entries.
    - A consumer returns **consumed** or **not consumed** for each event.
    - A **consumed press hides that key or button from every lower consumer until its matching
      release**, and that release goes to the consumer that took the press. Lower consumers'
      state never shows input taken above them.
    - Text events go only to the focused text field.
    - Gamepad events go to the stack of the window that has focus, then to the player the device
      is assigned to.

### Focus and mouse capture

11. **One window has keyboard focus; within it, one consumer has focus** (a widget or the game
    viewport). **Mouse capture** puts the window in relative mode and sends all mouse input to the
    game consumer. Capture ends when the window loses focus, when a UI layer asks for the cursor
    (opening a menu), or on Shift + F1 in Play In Editor. Clicking into the game viewport
    captures again. The editor keys sit above the game in the stack, so a game can never take
    them. Play In Editor uses exactly these rules, so it behaves like the shipped game.

### Contract for the action and mapping plugin

12. `core/input` guarantees:
    - **stable key ids**: enum values never renumbered, plus stable string names
      (`Keyboard.W`, `Mouse.Left`, `Gamepad.FaceSouth`) used in config files;
    - **timestamps** on every event, for holds, taps, chords and combos;
    - **per-player device assignment**: local players own device sets. Keyboard and mouse
      count as one set. In single-player, player 1 gets keyboard, mouse and every gamepad; in
      local multiplayer, players join explicitly by pressing a button on a device. Events from
      unassigned devices stay visible for joining;
    - **a consumer slot** in each window's stack;
    - **raw values**, without dead zones or curves.
13. **Rebinding** is stored as config (ADR-0010 JSON) in the layered configuration of ADR-0008:
    engine defaults, project, platform, then user. A player's or editor user's changes go in the
    user layer, by the stable string names. An unknown name is a warning, as ADR-0010 rules
    for unknown fields.

### Testing

14. Tests inject engine device events directly: state edges, taps within a frame, repeats,
    focus loss, routing, consumption and capture, with no display. CI also runs the platform
    tests on SDL's offscreen driver. The timestamped event list allows recording and replay for
    tests and bug reports later.

## Consequences

- UI and editor input can never leak into gameplay, and no key is ever stuck, on any platform.
- Bindings work on every keyboard layout, editor letter shortcuts follow the user's layout, and
  gamepad glyphs follow the device type.
- The action and mapping plugin can be built later without changing core, against a fixed
  contract.
- Two keyboard identities (physical and layout) must be explained in the binding UI.
- Follow-up work: device events in `platform`; `core/input` with state, routing, capture and
  tests; the editor consumer for Play In Editor; the action and mapping plugin (P5); input
  recording and replay.
- **Revisit when:**
  - a game needs Steam's action configurator (Steamworks Input);
  - Android adds touch and pen;
  - gyro aiming or touchpad features are needed.
