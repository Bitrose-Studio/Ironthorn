- Keep Simulation Changes: during a Play In Editor or Simulate session,
  `ith.play.keep_simulation_changes` (K, while the mouse is free) copies the component values that
  differ between the game world and the level back to the level for the given stable ids, or the
  play selection, as one undo entry named "Keep Simulation Changes" that marks the level dirty and
  is undone in one step after Stop. Entity references follow their stable ids. Added or removed
  components, a changed parent (with the LocalTransform relative to it) and a changed enabled state
  are reported in the result, not applied; ids that only one world has are reported as skipped. The game world doesn't change.
