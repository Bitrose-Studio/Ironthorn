# ADR-0046: Editor viewport horizontal field of view (amends 0042, 0045)

- **Status:** Accepted
- **Date:** 2026-11-26
- **Related:** amends [ADR-0042](0042-editor-viewport.md) rules 2, 4, 5 and 27, and
  [ADR-0045](0045-editor-viewport-amendments-after-the-viewport-panel.md) item 8 (the redraw key);
  [ADR-0006](0006-math-and-coordinate-conventions.md) rule 6 (the reversed-depth projection),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (an editor familiar to Unreal Engine
  developers)

## Context

ADR-0042 rule 2 gives `EditorCamera` a vertical field of view, `fovY`, of 90°. The viewport
panel is usually much wider than tall. At the default panel size, 1280x460, 90° vertical is about
140° horizontal. The edges stretch strongly, and the grid looks bent and fanned out.

Comparison renders at 1280x460, 640x400 and 640x960:

| Rule | 1280x460 | 640x400 | 640x960 |
|---|---|---|---|
| 90° vertical | ~140° horizontal | ~116° | ~67° |
| 60° vertical | ~116° horizontal | ~85° | ~42° |
| 90° horizontal | 39.5° vertical | 64° | 112.6° |

90° horizontal looked correct in wide panels. It is also what users of Unreal Engine's editor are
used to (ADR-0008). In a tall panel the vertical angle grows (112.6° at 2:3).

## Options considered

- **Keep 90° vertical** (rule 2 as written). Wide panels stretch. Rejected.
- **60° vertical.** Better, but a very wide panel still stretches: about 116° horizontal at the
  default size. Rejected.
- **90° horizontal, vertical derived from the aspect ratio (chosen).** Wide panels look right at
  any width; the familiar default.
- **90° horizontal, with the vertical angle clamped in tall panels.** Adds a second rule and a
  switch point for a rare layout. Rejected for KISS.

## Decision

1. **Rule 2, the camera's field.** `EditorCamera` holds `fovX`, the full horizontal field of view,
   default 90° (π/2), in (0, π). It replaces `fovY`. *Reason:* wide panels don't stretch.
2. **The derived vertical angle.** Wherever a vertical angle is needed, it is
   `fovY = 2 · atan(tan(fovX / 2) / aspect)`, with `aspect` the image's width over its height. It
   is computed per render and never stored. *Reason:* one stored value, no drift.
3. **Rule 5, the projection.** `perspectiveReversedInfinite` still takes a vertical angle; the
   viewport passes the derived one. The grid pass's ray basis (right times `tan(fovX / 2)`, up
   times `tan(fovY / 2)`) uses the same pair. *Reason:* the shapes and the grid must agree.
4. **Rule 27, Focus.** The framing distance uses `fovX` and the derived vertical angle, limited
   by the smaller of the two. *Reason:* the selection fits in both directions.
5. **Rule 4, saved cameras.** `Viewports.json` stores `fovX` in place of `fovY`; the document
   stays version 1. Unknown fields are ignored, as before, so an entry written with `fovY` (a
   development-only format from a single day) keeps its position, yaw and pitch and gets the default
   `fovX`; both defaults are 90°, so nothing is lost. *Reason:* no migration code for a format no
   user has, and a newer file stays readable by an older editor.
6. **ADR-0045 item 8, the redraw key.** The key holds `fovX` in place of `fovY`. A resize already
   re-renders, so a new derived vertical angle needs nothing more.
7. **Scope.** This rule is the editor camera's only. Game view (rule 20) still uses the editor
   camera in P2; the game camera's own field-of-view rule comes with the P3 renderer.

## Consequences

- Wide viewports look undistorted at the default size; the grid no longer fans out at the edges.
- A tall viewport shows a wide vertical angle (112.6° at 2:3) and stretches at the top and bottom.
  Accepted.
- Cameras saved with `fovY` load with the default horizontal angle; their old `fovY` is ignored.
- Add Shape (ADR-0045 item 9) places along the view direction only and is unaffected.
- ADR-0042 is marked as amended by this ADR in its header and in the index.
- **Revisit when:** tall viewports turn out to be common (clamp the vertical angle); a user setting
  for the field of view is added; the P3 game camera defines its own rule.
