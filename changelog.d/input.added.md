- `ith/platform_events`: the engine's event type and the stable ids of keys, mouse buttons,
  positional gamepad buttons, raw gamepad axes and gamepad types. `ith::Event` moved here from
  `ith/platform`.
- `ith/platform`: keyboard (physical key, layout key, modifiers, repeats), text input with IME
  composition, mouse (buttons, position in pixels, motion, fractional wheel with the natural
  scrolling setting) and gamepad events (hotplug, name, type, GUID, Steam handle, buttons, sticks
  and triggers); gamepad rumble, trigger rumble and LED; key names in the user's layout; relative
  mouse mode, text input and the IME area on `ith::Window`; the pump time of each frame. A machine
  without gamepad support still gets windows, keyboard and mouse.
- `ith/input`: `ith::Input` builds per-frame input state from the events (taps within a frame,
  repeats that aren't presses, releases on focus loss) and routes them through per-window
  consumer stacks with consumption, focus, mouse capture, capture on click and per-player device
  assignment. The stable control names (`Keyboard.W`, `Mouse.Delta`, `Gamepad.LeftStick`) are
  found with `ith::findControl`. See `docs/development/input.md`.
