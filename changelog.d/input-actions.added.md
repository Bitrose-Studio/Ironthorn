- `ith/input_actions`: the input action layer. Input action and input context assets
  (`ith.input_action`, `ith.input_context`) with digital, 1D, 2D and 3D values, dead zone, scale,
  negate, swizzle, curve, frame-time and user-setting modifiers, and Down, Pressed, Released, Hold,
  HoldRelease, Tap, MultiTap, Pulse, Chord and Combo triggers; validation of missing actions,
  unknown keys and settings, slot conflicts and dependency cycles. `ith::InputActions` evaluates
  every local player each frame: context stacks with priorities and key claims, presses shorter
  than a frame kept, exact timestamps, focus loss, mouse capture loss and suspension, and writes
  `ith::ActionStates` and per-frame `ith::ActionEvent`s for systems to read. Players rebind keys by
  stable slot names in profiles stored in layered configuration (`ith.input_bindings`). See
  `docs/development/input-actions.md`.
