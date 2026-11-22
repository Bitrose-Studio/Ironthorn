# ADR-0027: Input: rules completed after the first implementation

- **Status:** Accepted
- **Amended by:** [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md)
- **Date:** 2026-11-21
- **Related:** amends [ADR-0012](0012-windowing-and-event-loop.md) (windowing and the event loop)
  and [ADR-0013](0013-input.md) (input); [ADR-0004](0004-modules-and-layering.md) (layers),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds, dedicated server),
  [input actions spec](../specs/input-actions.md) section 6 (requirements on input)

## Context

The first input slice (`platform_events`, `input`, and the device events in `platform`) and the
input actions spec found rules in ADR-0012 and ADR-0013 that can't be kept as written, or that
leave gaps:

- ADR-0012 rule 8 and ADR-0013 rule 1 put the engine's event types in `platform`. `platform` is a
  `client` module (ADR-0017): a dedicated server doesn't link it. A `runtime` input module, and the
  `runtime` gameplay modules that read input, can't depend on it.
- ADR-0013 rule 11 lets clicking into the game viewport capture the mouse, but not who does it or
  whether the click also reaches the game. The spec (decision 7) needs the capturing click to fire
  no action.
- ADR-0013 rule 9 ("keys still held when focus returns count only once pressed again") assumes
  the operating system reports focus changes cleanly. SDL doesn't: it sends key releases for held
  keys just before the focus loss, in the same pump, and on X11 and Wayland it reports held
  modifiers as new presses right after the focus gain. Held mouse buttons are reported again on
  Windows.
- Gamepads: machines without a gamepad subsystem, gamepad values while no window has focus, and
  values left behind when a consumer stops seeing a device are not covered.
- The spec needs the frame's input time and gamepad hotplug in the frame's event order.

This ADR changes only those rules. Everything else in ADR-0012 and ADR-0013 stands.

## Options considered

- **Amend ADR-0012 and ADR-0013 by this ADR** (chosen), as ADR-0020 did for serialization: the
  accepted records stay readable and each change carries its reason.
- **Supersede both with rewrites.** Repeats about thirty unchanged rules for six changes.

Where the event types live:

- **In `platform`** (as written). `input` would have to be `client`, so no gameplay module that
  reads input could link into a dedicated server; each would need a server-side stub.
- **In `input`** (layer `core`). `platform` (layer `platform`) would depend upwards, which
  ADR-0004 rule 3 forbids.
- **In a small `runtime` module in the `platform` layer** (chosen). `platform` produces the
  events, `input` consumes them, and neither is pulled into a process kind it doesn't belong to.

Who captures the mouse on a click:

- **The game consumer captures when a click reaches it** (the first implementation). Every game
  repeats the same code, and the click reaches the game, so it may fire an action.
- **`input` captures, per window, before the game sees the click** (chosen). One rule for every
  game and for Play In Editor; the click never reaches the game.

## Decision

1. **Event types in a `runtime` module** (changes ADR-0012 rule 8 and ADR-0013 rule 1).
   - **`platform_events`** (layer `platform`, kind `runtime`, standard library only) holds what
     the platform emits: `Event` and its types, window and device ids, and the device control
     ids: keys as USB HID usage values, mouse buttons, positional gamepad buttons, raw gamepad
     axes, gamepad types and GUIDs. The numeric values are stable (ADR-0013 rule 12).
   - `platform` (kind `client`) and `input` depend on it; `platform` fills it from SDL.
   - **The binding vocabulary lives in `input`**, as ADR-0013 rule 12 assigns it: the stable
     string names (`Keyboard.W`, `Mouse.Left`, `Gamepad.FaceSouth`), the 1D mouse axes, the 2D
     controls (`Gamepad.LeftStick`, `Gamepad.RightStick`, `Mouse.Delta`, `Mouse.Wheel`), and the
     `Control` type that holds any of them. They are names for bindings, not something the
     platform emits.
   - **Why:** `input` must be `runtime` (rule 2), and it can depend only on `runtime` modules
     at its layer or below.
2. **`input` is a `runtime` module and never calls `platform`** (ADR-0013 rule 1 called it
   `core/input`; it is the module `input` in layer `core`).
   - Gameplay modules and the action plugin that read input link into a dedicated server, where no
     events arrive and every state stays empty.
   - `input` decides; the application applies. Each frame the application sets each window's
     relative mouse mode from `input`'s capture state and turns text input on or off from
     `input`'s text-input request (ADR-0013 rules 4 and 11).
   - **Why:** a dedicated server must link the same gameplay modules (ADR-0017), and routing stays
     testable without a display (ADR-0013 rule 14).
3. **Capture on click** (completes ADR-0013 rule 11; owner's decision of 2026-11-21).
   - It is a **per-window setting in `input`, off by default**. The application turns it on for
     game windows and the Play In Editor viewport.
   - In a window with the setting on, the window has focus and the mouse is not captured: a mouse
     press that the `Editor` and `Ui` layers leave unconsumed **captures the mouse and is
     consumed** by the capture. It never reaches the `Game` layer, so it fires no action.
   - Its release reaches no `Game` consumer either. `Editor` and `Ui` consumers that saw the press
     without consuming it get its release, as for any consumed press (ADR-0013 rule 10), so no
     button sticks.
   - The game consumer no longer captures by itself; `captureMouse` and `releaseMouse` stay for
     explicit control (a UI layer asking for the cursor, Shift + F1 in Play In Editor).
4. **Focus loss against SDL's real event order** (completes ADR-0013 rule 9).
   - On focus loss, every key and button still held is released with a real release event at the
     focus event's timestamp, routed like any release.
   - A key or mouse button that was **held or released in the frame of the focus loss** is marked.
     After focus returns, a press of a marked control in the frame of the focus gain is not a
     press: such a control counts only once pressed again. This covers SDL's releases just before
     the focus loss, the modifiers X11 and Wayland report again after the focus gain, and the
     mouse buttons Windows reports again.
   - Repeats of keys that aren't held, and releases of controls that aren't held, are dropped, so
     no consumer sees a release without its press.
5. **Gamepads.**
   - **Start-up is non-fatal.** If SDL's gamepad subsystem fails to start, `platform` still
     creates windows, keyboard and mouse; there are simply no gamepad events, and gamepad output
     calls fail with `platform.gamepad_not_found`.
   - **Gamepad axis events are ignored entirely while no window has focus**: they update neither
     `input`'s all-device state nor any consumer. Focus loss sets every non-zero axis to 0
     (rule 4), and SDL stops sending gamepad events without focus, so values would otherwise go
     stale.
   - **Axes are reset for consumers that lose a device.** Whenever a consumer stops seeing a
     gamepad, each of its axes for that gamepad that isn't 0 is set to 0 with an axis event:
     - on disconnect and on focus loss, at once;
     - when a consumer above consumes that axis, at once;
     - when the gamepad is assigned to another player, for the consumers that no longer see it,
       at the start of the next `processEvents`.

     Buttons are released as in rule 4. **Why:** an axis has no release, so a character would
     otherwise keep running.
6. **Timing and hotplug** (adds to ADR-0013 rule 12, from the spec's section 6).
   - **Frame input time:** `input` reports the time of each frame's input, the platform's pump
     time, raised to the latest event timestamp if that is later. Triggers measure holds and taps
     against it.
   - **Gamepad connect and disconnect are ordered events** in the frame's event list, delivered to
     every consumer that sees that device. A disconnect comes after the releases and axis resets
     of that gamepad.

## Consequences

- Gameplay and the action plugin read input in every process kind; a dedicated server links
  them unchanged.
- One module more (`platform_events`), with no code beyond plain types.
- A game gets capture on click by turning on one window setting, and the capturing click can't
  fire a game action.
- No key stays stuck and no stale modifier counts as a press after an Alt + Tab, on Windows, X11
  and Wayland. A key really pressed in the same frame as the focus gain is lost; the next press
  counts.
- Machines without gamepad support run, without gamepads.
- Follow-up work: mark ADR-0012 and ADR-0013 amended when this ADR is accepted.
- **Revisit when:**
  - a platform reports focus changes in yet another order (a console or Android);
  - a game needs gamepad input without window focus (background play);
  - a game needs a click that both captures and acts.
