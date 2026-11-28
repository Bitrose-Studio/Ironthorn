- Play In Editor starts in the selected viewport, the one the last mouse press on a viewport image
  went to (`ith.viewport.1` at first), and `ith.query.play` reports it and whether the mouse is
  captured. That viewport's own camera is the play camera: the session saves it at the start and
  restores it bit for bit at Stop, so `Viewports.json` never holds a pose from a session.
- Play flies the play viewport's camera with a spectator view on the game input layer
  (`ith::SpectatorConsumer`): captured mouse motion turns it and W/A/S/D/Q/E move it at the
  camera's speed step. Eject leaves the camera where the spectator was and Possess resumes the
  spectator there; Simulate and an ejected session keep every editor camera control.
- `ith::PlayInputConsumer` on top of the editor window's input stack runs Escape (Stop), Shift + F1
  (Free the Mouse) and F8 (Possess or Eject) while a session runs. With the mouse free, Escape first
  goes to an open interaction, or to a text field or popup the UI reports (`reportUi`); with the
  mouse captured it always stops. `EditorSession::setPlayInput` binds the editor window's `Input`:
  Play turns capture on click on, a click into the play viewport captures the mouse, and Shift + F1,
  Eject, Stop and focus loss release it.
- While a session runs, the keymap consumer gives the game every key while the mouse is captured and
  runs no shortcut, and with the mouse free keeps all keyboard and mouse input from the game, except
  the press that captures; gamepads reach the game only while possessed.
- Picks are checked against the world each viewport draws and change the play selection while
  playing (`applyPick` takes the viewport); Focus frames the active selection in the world its
  viewport draws.
- `ith::Input`'s capture on click captures only if capture on click is still on once the layers
  above the game have seen the press, so a Stop or Eject run by that press doesn't capture.
