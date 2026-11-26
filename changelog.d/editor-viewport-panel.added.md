- The editor has a Viewport panel, open in the centre of the default layout. It shows the level's
  primitive shapes, lit by a fixed light from the camera, on an infinite grid with 1 m and 10 m
  lines and coloured X and Z axes that fade into the distance; selected shapes are tinted. It renders
  again only when its camera, the level, the selection, its size or its game view changed, so an
  idle editor does no scene work. A toolbar above it selects the transform tool, turns snapping on
  or off and sets its step, switches the gizmo space, toggles the game view (no grid, no tint) and
  shows the fly speed step. Mouse input reaches a viewport only where nothing is drawn over it, so a
  floating panel on top of it keeps its clicks.
- The Outliner has a context menu: Add Shape places a cube, sphere, plane or cylinder 2 m in front
  of the first viewport's camera, resting on the ground, and selects it.
