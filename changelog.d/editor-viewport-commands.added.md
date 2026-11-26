- The editor core has a viewports store: a camera and game-view flag for each of the four viewport
  instances, and an editor-wide transform tool, gizmo space (world or local) and snapping (on, 0.1 m
  by default). Each level's cameras are remembered per user in the project's
  `Saved/Editor/Viewports.json`, written when a level closes and at exit, only when they changed.
- New viewport commands, which the default keymap's W, E, R, Space, F, End, G and F11 now run:
  select or cycle the translate, rotate and scale tools, focus the selection, snap the selection to
  the floor (onto the ground or the tops of shapes below it, as one undo step), toggle the gizmo
  space, set snapping and toggle the game view. Immersive mode is registered but not available yet.
- The editor registers the shapes module, so levels with shapes load in the editor and
  `ith.entity.create` places a shape with an `ith::ShapeRenderer` component.
