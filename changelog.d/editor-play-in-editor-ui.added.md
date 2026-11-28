- The editor has a main toolbar under the menu bar with Play and Simulate, replaced while a session
  runs by Stop (where Play was), Pause or Resume, Step (enabled while paused) and Possess or Eject.
  A disabled control's tooltip says why; every tooltip names its shortcut.
- The play viewport draws the game world with a coloured border and a "Playing", "Simulating" or
  "Paused" label (theme tokens `playActive`, `playSimulate`, `playPaused`), and "Click to control
  the game · Shift+F1 frees the mouse" while the game can take the mouse. The status bar shows the
  same state with the game's elapsed time. The other viewports keep drawing the level and show what
  Keep Simulation Changes keeps at once; every viewport tints the play selection.
- `ironthorn_editor` ticks the session every frame while it runs and draws continuously then, Play
  captures the mouse on a click into the play viewport, Escape, Shift + F1 and F8 work on top of
  every panel, and closing the editor stops the session first. A session starting while the selected
  viewport is closed plays in Viewport 1, which opens.
- While the mouse is captured the UI takes no keys and types nothing, so the game gets the keyboard;
  with the mouse free, Escape first closes an open menu or dialog or leaves a text field, and only
  then stops play.
- While playing, the Outliner and Details say they show the game world, entities spawned by the game
  are greyed and can't be selected, renaming, reparenting, the Enabled check boxes and Add and Remove
  Component are disabled, and the Outliner's context menu and the Edit menu offer Keep Simulation
  Changes, whose result shows in the status bar and the Output Log.
