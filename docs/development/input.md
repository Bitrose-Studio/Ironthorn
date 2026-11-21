# Input: state, routing and consumption

Two modules carry device input ([ADR-0013](../decisions/0013-input.md), completed by
[ADR-0027](../decisions/0027-input-rules-completed.md)):

- **`platform_events`** (layer `platform`, kind `runtime`, header-only): what the platform emits,
  the event type `ith::Event` and the stable ids of keys, buttons, axes and gamepad types. The
  `platform` module fills these events from SDL ([platform guide](platform.md#device-input)).
- **`input`** (layer `core`, kind `runtime`): `ith::Input`, which keeps the device registry and
  per-frame state, and routes events through per-window consumer stacks with consumption, focus,
  mouse capture and per-player device assignment; and the binding vocabulary: the stable control
  names, the 1D mouse axes and the 2D controls (`ith/input/ControlNames.h`).

Both are `runtime` modules: `input` never calls the platform layer, so gameplay modules and the
action and mapping plugin that read input also link into a dedicated server, where no events
arrive ([ADR-0017](../decisions/0017-runtime-and-editor-separation.md)). Only `platform`, which
produces the events, is `client`.

There are no actions, mappings, dead zones or rebinding here: that is the action and mapping
plugin, built on this module.

## Use

```cpp
#include "ith/input/Input.h"

ith::Input input;
GameInput gameInput; // an ith::InputConsumer, or nullptr to only read state
const ith::ConsumerId game =
    input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Game, .consumer = &gameInput});
input.setCaptureOnClick(window.id(), true); // a game window: a click into it captures the mouse

while (running) {
    input.processEvents(platform.pumpEvents(), platform.pumpTimeNs());
    // Input decides; the application applies it to its windows.
    static_cast<void>(window.setRelativeMouseMode(input.mouseCaptured(window.id())));
    if (input.textInputWanted(window.id())) {
        static_cast<void>(window.startTextInput());
    } else {
        window.stopTextInput();
    }

    const ith::InputState& state = input.consumerState(game);
    if (state.pressed(ith::Key::Space)) {
        jump();
    }
    const ith::InputVector2 look = state.axis2D(ith::Axis2D::MouseDelta);
    const ith::InputVector2 move = state.axis2D(ith::Axis2D::GamepadLeftStick);
}
```

A consumer either implements `InputConsumer::onInputEvent` and returns `Consumed` or
`NotConsumed` per event, or passes no callback and only reads its state. Both styles see the
same events: `consumerState(id)` is built from exactly the events that reached that consumer.
`Input::events()` lists the frame's events in order as Input saw them, and `Input::state()` is the
state of every device before routing. Read `events()` after `processEvents`, never from a consumer
during it, and never keep an `Event` across frames: its `text` points into the platform's frame
storage.

## Controls and names

| Control | Ids | Names |
|---|---|---|
| Physical keys | `ith::Key`, USB HID usage values (`Key::W` is 26) | `Keyboard.W`, `Keyboard.Digit1`, `Keyboard.LeftShift` |
| Mouse buttons | `ith::MouseButton` | `Mouse.Left`, `Mouse.Right`, `Mouse.Middle`, `Mouse.Back`, `Mouse.Forward` |
| Mouse axes | `ith::MouseAxis` (frame sums) | `Mouse.DeltaX`, `Mouse.DeltaY`, `Mouse.WheelX`, `Mouse.WheelY` |
| Gamepad buttons | `ith::GamepadButton`, positional | `Gamepad.FaceSouth`, `Gamepad.LeftShoulder`, `Gamepad.LeftStickButton` |
| Gamepad axes | `ith::GamepadAxis`, raw | `Gamepad.LeftStickX`, `Gamepad.RightTrigger` |
| 2D controls | `ith::Axis2D` | `Mouse.Delta`, `Mouse.Wheel`, `Gamepad.LeftStick`, `Gamepad.RightStick` |

- **Values and names never change.** Saved bindings and recordings hold them. New controls get
  new values and names; `tests/input/ControlNamesTests.cpp` pins existing ones.
- `ith::controlName(control)` gives the name, `ith::findControl(name)` the control (an
  `ith::Control` variant), or nothing for an unknown name, which a config loader reports as a
  warning.
- **Two keyboard identities.** `Event::key` is the physical key, for game bindings and viewport
  navigation; `Event::layoutKey` is the character the user's layout prints on it, for editor
  command shortcuts, so Ctrl + Z follows the Z letter. Names shown to users come from
  `Platform::keyDisplayName(key)`.
- Sticks are -1..1 with right and up positive, triggers 0..1, without dead zones. Mouse motion is
  in pixels (in relative mode, the mouse's own unaccelerated units), the wheel in steps, positive
  up and right, fractional on smooth wheels.

## State

`InputState` reports, per key and button, `held`, `pressed` and `released` this frame:

- a press and release within one frame report both, so short taps are never lost;
- key repeats are not presses;
- a press of a control already held and a release of a control not held are ignored, so no
  release is ever seen without its press;
- mouse motion and wheel are summed per frame; gamepad values are per device, and the overloads
  without a device id read any gamepad of that state (the largest value, the longest stick).

## Routing

Each window has a consumer stack, top first: `Editor` (console, debug overlays, editor keys),
`Ui` (focused widget, then UI layers from modal to HUD), `Game` (the players' input). Within a
layer the consumer added last is on top. A shipped game has no `Editor` entries.

- **Keyboard** events go to the stack of their window, **mouse** events to the stack of the window
  under the mouse, **gamepad** events to the stack of the window with focus (none without focus).
- An event goes down the stack until a consumer returns `Consumed`.
- A **consumed press hides that key or button** from every lower consumer until its release. The
  release, and the key's repeats, go to exactly the consumers that saw the press, even if the
  stack changed in between (a menu opened above the game).
- A consumed gamepad axis sets that axis to 0 for lower consumers that still show a value, so a
  menu that takes the stick doesn't leave the character running.
- **Text** (`TextInput`, `TextEditing`) goes only to the focused consumer of its window, while it
  accepts text (`setFocusedConsumer(id, true)`); `textInputWanted(window)` tells the application
  to turn the window's text input on.
- **Gamepad connects and disconnects** go to every consumer that sees the device, in order
  with the other events.
- Consumers may add and remove consumers, themselves included, capture or release the mouse and
  assign devices from `onInputEvent`.

## Focus loss

When a window loses focus, Input releases every held key, mouse button and gamepad button, and
sets every non-zero gamepad axis to 0, with real events at the focus event's timestamp, routed
like any release.

SDL's own order is not clean: it releases held keys itself just before it reports the focus loss,
X11 and Wayland report held modifiers as new presses right after the focus returns, and Windows
reports held mouse buttons again. So a key or mouse button that was held or released in the frame
of the focus loss is marked, and a press of it in the frame focus returns is dropped: such a
control counts only once pressed again. A key really pressed in that same frame is lost; the next
press counts. Repeats of keys that aren't held and late releases of controls already released are
dropped too.

Gamepad axis events are ignored entirely while no window has focus (SDL still sends axes moving
toward the centre); `Input::state()` doesn't change either.

## Mouse capture

`captureMouse(window)` (only for the window with focus) sends that window's mouse input only to
its `Game` layer; the application puts the window in relative mode while
`mouseCaptured(window)` is true. Capture ends when the window loses focus and on
`releaseMouse()`, which a UI layer calls when it needs the cursor and the editor calls on
Shift + F1. The keyboard keeps going through the whole stack, so editor keys always work.

**Capture on click** is a per-window setting, off by default: the application turns it on for a
game window or the Play In Editor viewport (`setCaptureOnClick(window, true)`). While it is on,
the window has focus and the mouse isn't captured, a mouse press goes to the `Editor` and `Ui`
layers only. If one of them consumes it (a click on a HUD button), nothing else happens. Otherwise
the press captures the mouse and is consumed by the capture: no `Game` consumer sees it or its
release, so the capturing click never fires a game action. `Editor` and `Ui` consumers that saw
the press without consuming it get its release as usual.

## Players and devices

`devices()` lists the keyboard and mouse set (`kKeyboardMouse`, always present) and the connected
gamepads with their name, type, GUID and Steam handle.

- **Single-player** (the default): player 0 owns the keyboard, the mouse and every gamepad,
  including gamepads that connect later.
- **Local multiplayer** (`setLocalMultiplayer(true)`): every device is unassigned; players join
  when a consumer not bound to a player sees a press from an unassigned device and calls
  `assignDevice(device, player)`.
- A consumer bound to a player (`ConsumerDesc::player`) sees only that player's devices; an
  unbound consumer sees every device, unassigned ones included.
- **Axes are reset for consumers that lose a gamepad.** An axis has no release, so whenever a
  consumer stops seeing a gamepad, each of its non-zero axes for that gamepad is set to 0 with an
  axis event: on disconnect and focus loss at once, and after `assignDevice` (or
  `setLocalMultiplayer`) at the start of the next `processEvents`, because those may be called
  from a consumer while events are being routed. Held buttons release to the consumers that saw
  their press, wherever the device went.
- A disconnected gamepad's buttons are released and its axes set to 0 before
  `GamepadDisconnected`, which reaches every consumer that sees the device or still has its
  values. A reconnected gamepad has a new id and must be assigned again in local multiplayer.

## Time

Every event has a monotonic timestamp in `std::chrono::steady_clock` nanoseconds.
`Input::frameTimeNs()` is the frame's input time: the time passed to `processEvents`
(`Platform::pumpTimeNs()`), raised to the latest event timestamp if needed, so holds and taps can
be measured against it.

## Threading

`Input` is used from one thread, the one that pumps events; it is not thread-safe. `InputState`
is a plain value. Nothing a consumer may call from `onInputEvent` routes events, so routing is
never re-entered.

## Testing

Tests build `ith::Event` values with designated initializers and pass them to
`Input::processEvents` or `InputState::apply`; no display is needed
(`tests/input`). The platform translation is tested on SDL's `offscreen` driver
(`tests/platform`).

## Not here yet

- The action and mapping plugin (actions, contexts, modifiers, triggers, rebinding storage).
- Input recording and replay, and the editor's Play In Editor consumer.
- Gyro, accelerometer, touchpad, touch and pen.
