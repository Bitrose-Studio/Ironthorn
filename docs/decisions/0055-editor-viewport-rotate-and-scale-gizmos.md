# ADR-0055: Editor viewport rotate and scale gizmos (amends 0042, 0048)

- **Status:** Accepted
- **Date:** 2026-11-28
- **Related:** amends [ADR-0042](0042-editor-viewport.md) rules 3, 4, 26 and 29 and its follow-up
  list, and [ADR-0048](0048-editor-viewport-translate-gizmo-as-built.md) items 5, 6, 10 and 12;
  [ADR-0006](0006-math-and-coordinate-conventions.md) rules 4 and 5 (radians in code, quaternions,
  right-handed), [ADR-0008](0008-editor-familiar-to-unreal-developers.md) rule 4 (W/E/R, Space,
  Alt + drag), [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md)
  (`LocalTransform`, `WorldTransform`), [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md)
  rule 6 (one transaction per interaction), [ADR-0053](0053-play-in-editor-and-simulate.md) rule 16
  (no gizmo while playing), [ADR-0005](0005-reference-hardware.md) (the lowest tier)

## Context

ADR-0042 deferred the rotate and scale gizmos to after P2 (its rule 26: with those tools active no
gizmo is drawn; edits go through Details). On 2026-11-28 the owner moved them before the P2 exit
gate. This ADR decides how they look, map a drag to a change, snap and share the translate gizmo's
machinery. Rules of ADR-0042 and ADR-0048 not listed here stand.

What is already fixed, in the code and the ADRs:

- **Tools and keys.** `ViewportTool` has Translate, Rotate and Scale; `ith.viewport.translate_tool`,
  `rotate_tool`, `scale_tool` and `cycle_tool` exist and are bound to W, E, R and Space
  (`engine/config/Keymap.json`). The toolbar already has the three tool buttons.
- **Space.** The world/local toggle already exists for translate: `Viewports::gizmoSpace()` (default
  World), `ith.viewport.toggle_gizmo_space` and a toolbar button (ADR-0042 rules 26 and 29). It has
  **no key binding**. `gizmoFrame(world, Local)` takes the primary's rotation with scale and shear
  removed.
- **Snapping** is one editor-wide `snapEnabled` and `snapStep` (on, 0.1 m), set by
  `ith.viewport.set_snap {enabled, step}`, with toolbar presets 0.01 to 1 m. It is session state
  (ADR-0042 rule 4); persisting it is listed follow-up. It snaps the drag's delta, not the absolute
  value (ADR-0048 item 12).
- **The translate drag** (`GizmoDrag`): one transaction, previewed once per frame, Esc cancels and
  waits for the release, focus loss cancels, Alt at the press duplicates inside the transaction, a
  net-zero release records nothing, roots are the selected entities without a selected ancestor
  that have a `LocalTransform` and a non-singular parent. The gizmo is hidden in game view, while
  playing, while a level opens, and while another interaction's transaction is open. The pivot is
  the primary's world matrix (`World::worldMatrixOf`). No handle sits at the pivot: a press there
  picks.
- **Transforms.** `LocalTransform` holds a `Transform`: translation, a unit quaternion rotation and
  a per-axis scale (`math/Transform.h`). `WorldTransform` is a matrix, because a rotated parent with
  non-uniform scale shears its children; `decompose` drops shear and gives a mirrored matrix a
  negative z scale. Details shows rotation as X, Y, Z degrees (`eulerDegrees`, yaw * pitch * roll)
  and converts back on edit. Angles are radians in code; degrees only in the UI and human-edited
  files (ADR-0006 rule 4).

**Prior art** (public documentation and observed behaviour only, ADR-0003):

- Unreal's documented transform tools
  ([Transforming Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/transforming-actors-in-unreal-engine)):
  the rotate gizmo has one coloured arc per axis, an outer camera-facing arc and a free "arc ball"
  between them; while dragging only the dragged axis is shown and the amount updates live. The
  scale gizmo has axis handles ending in cubes, two-axis plane handles and a centre cube for
  uniform scale, and **the scale gizmo always works in local space**. The viewport toolbar
  ([Viewport Toolbar](https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-toolbar))
  has a world/local toggle on Ctrl + backtick and separate increment lists for location, rotation and
  scale snapping. Observed, not found in the documentation: a rotation drag follows the arc's
  tangent at the grab point rather than the angle around the centre; separate on/off toggles per
  snap kind; defaults of 10° and 0.25.
- Unity's snap settings API
  ([EditorSnapSettings](https://docs.unity.com/en-us/engine/current/script-reference/unityeditor/editorsnapsettings))
  has separate move, rotate and scale increments with separate enable flags for angle and scale
  snapping; its manual
  ([Snap increments](https://docs.unity3d.com/6000.2/Documentation/Manual/SnapIncrements.html))
  states no defaults.
- Godot's 3D introduction
  ([Introduction to 3D](https://docs.godotengine.org/en/stable/tutorials/3d/introduction_to_3d.html)):
  arcs to rotate, cubes to scale, a local-space toggle on T, rotate snap in degrees and scale snap
  in percent, Ctrl to snap temporarily. Defaults not stated.
- Blender's precision controls snap rotation to 5° with Ctrl (manual page
  `scene_layout/object/editing/transform/control/precision`; seen in search results only, the page
  refused the fetch).
- **Not verified:** none of the defaults above (10°, 15°, 0.25, 10%) is stated in a primary
  source.

Lessons from an earlier engine experiment: a rotation taken as the screen angle between the press
and the pointer around the centre wraps at ±180° (one atan2 of the press and current vectors) and
needs a facing-dependent sign flip; scale kept along the entity's own axes stayed decomposable; a
minimum scale factor kept a drag from reaching zero, which no later drag can undo.

## Options considered

### Rotate handles

- **Three axis rings (chosen).** A circle in the plane normal to each axis, a constant size on
  screen. Covers every precise rotation, snaps meaningfully, and leaves the inside of the rings to
  picking.
- **Plus a camera-facing ring and a free trackball** (Unreal's full set). The view ring rotates
  about an axis that is neither a world nor a local axis, so snapped results are rarely round; the
  trackball has no snapping at all and needs its own mapping. Deferred (settled question 1).

### Drag to angle

- **The screen angle around the centre.** A dial: natural on a ring facing the camera, but
  degenerate on a ring seen edge-on, which is the yaw ring at eye level, the most common rotation;
  it needs a sign flip by facing and accumulation past ±180°. Rejected.
- **The pick ray against the ring's plane.** Exact while the plane faces the camera; explodes
  edge-on, like a translate plane, so the yaw ring would fade at eye level. Rejected.
- **Along the ring's tangent at the grab point (chosen).** The tangent `axis × (grab − origin)` is
  projected to the screen at the press; the angle is the pointer's travel along it divided by the
  ring's radius in pixels. Linear, unbounded (no wrap), the right-handed sign comes from the cross
  product, and it works on an edge-on ring. It degenerates only where the tangent points at the
  camera, and those parts of the ring aren't grabbable.

### Drag to scale factor

- **The ratio of distances along the axis from the pivot** (the ray against the axis line). Exact
  under the cube, but unstable near the pivot and near parallel. Rejected.
- **Exponential, 2^(travel / arrow).** Never reaches zero, symmetric, but its snapped steps aren't
  evenly spaced. Rejected.
- **Linear screen travel (chosen).** factor = 1 + travel / arrow length, the travel measured along
  the handle's screen direction. Predictable, even snap steps, clamped above zero.

### Zero and negative scale

- **Allow mirroring by dragging through zero.** Passes through a zero scale, which collapses the
  entity and its children's transforms (the translate gizmo then skips them). Rejected.
- **Clamp the factor to a positive minimum (chosen).** The gizmo never changes a scale's sign: a
  mirrored entity stays mirrored, mirroring is a Details edit.

### Scale space and non-uniform scale under rotated parents

- **Follow the world/local toggle.** A world-axis scale of a rotated entity is a shear in its local
  space, which a `Transform` can't hold; it would be decomposed and lose the shear, so the result
  wouldn't match the drag. Rejected.
- **Always along each entity's own axes (chosen;** Unreal documents the same). The drag writes only
  `LocalTransform.scale`, component-wise, so the stored transform is always decomposable. Under a
  rotated, non-uniformly scaled parent the world result is skewed by the parent, exactly as Details
  would make it.

### Snapping settings

- **One toggle and step shared by the three tools.** A step in metres means nothing to a rotation.
  Rejected.
- **A toggle and a step per tool, editor-wide (chosen).** As Unreal and Unity do; it extends the
  existing state and command.
- **Per viewport.** ADR-0042 rule 3 already made the tool, space and snapping editor-wide;
  rejected.

Where they live: session state as today (chosen for P2, settled question 4), Editor Preferences (the
user config layer, settings that travel across projects) when they are persisted, or
`Viewports.json` (rejected: it holds per-level cameras, and snapping isn't per level or per project).

### Build or reuse

- **Our own math and drawing, extending `Gizmo` and `GizmoDrag` (chosen).** The translate gizmo's
  layout, hit test, fade, transaction and input routing are already ours and tested.
- **ImGuizmo (MIT, no tagged releases).** ADR-0042 rejected it for translate; adopting it now would
  mean two gizmo implementations or rewriting the translate one. Rejected again.

### Settled questions

The owner settled these on 2026-11-28, as recommended:

1. **View ring and trackball:** not in P2; axis rings only. The camera-facing ring is the first
   follow-up, the trackball after it.
2. **Rotation snap default:** on at 10° (Unreal muscle memory, ADR-0008, and consistent with
   translate being on by default); presets 1, 5, 10, 15, 30, 45 and 90°. 15° was the alternative.
3. **Scale snap default:** on at 0.25 (factor steps 0.75, 1, 1.25, …); presets 0.05, 0.1, 0.25,
   0.5 and 1. 0.1, or off by default, were the alternatives.
4. **Persisting tool, space and snapping:** session state in P2 (ADR-0042 rule 4); persisted later
   in Editor Preferences, not in `Viewports.json`.
5. **Keys:** `ith.viewport.toggle_gizmo_space` on **Ctrl + Keyboard.Grave** (Unreal's documented
   chord; Grave is a physical key, the one left of 1 on every layout); no snapping keys in P2 (the
   toolbar and the command suffice). Follow-up: `[` and `]` stepping the current tool's snap step.

## Decision

### Shared

1. **Every tool has a gizmo** (amends ADR-0042 rule 26 and ADR-0048 item 6). The gizmo shows the
   current tool's handles under the same conditions as translate's: outside game view, on an image
   with area, with a live primary in front of the camera, not while playing or opening, not while
   another interaction's transaction is open. One drag is one transaction named "Translate",
   "Rotate" or "Scale", with everything ADR-0048 items 5, 8, 9, 10, 11 and 13 give translate:
   press precedence over clicks and Alt + LMB orbit, once-per-frame preview plus the release's own
   position, Esc cancels and waits for the release, focus loss cancels, Alt at the press duplicates
   inside the transaction and the copies are what changes, and a release whose last applied change
   is the identity (delta zero, angle zero, factor one in every axis) records nothing.
2. **One drag type.** `GizmoDrag` keeps its tool from the press and previews it; the tool-specific
   math (layout, hit test, grab, drag, snap) is pure in `Gizmo.h`. Roots are chosen as ADR-0048
   item 7 says; rotate needs the non-singular parent (it moves translations), scale doesn't (it
   writes only local scale) and takes every selected root with a `LocalTransform`. Changing the
   tool or the space during a drag doesn't affect it: the drag keeps its press frame.
3. **Values come from the press.** Every preview computes each root's new value from its value at
   the press and the drag's total change, never incrementally, so nothing drifts over a long drag.
   A frame whose result isn't finite for any root is skipped whole, as for translate.

### Rotate

4. **Handles.** Three rings, X red, Y green, Z blue (the existing axis tokens), each the circle of
   radius `kGizmoArrowLength` (100 points, times the display scale) in the plane normal to its axis,
   sampled at 64 points. The half of a ring on the camera's side of the plane through the pivot
   facing the camera is the **front**: drawn opaque and grabbable. The back half is drawn at 25 %
   opacity and is never grabbable. A ring point is also not grabbable where the sine of its
   tangent's angle to the view direction is at most `kGizmoHiddenFacing` (0.1), the ends of a ring
   seen edge-on; rings don't fade otherwise, so the yaw ring works at eye level.
5. **Hit test.** A press within `kGizmoGrabTolerance` (6 points) of a grabbable ring segment grabs
   the nearest such ring. A press anywhere else, the pivot and the rings' inside included, is a
   click candidate or an orbit, as without a gizmo.
6. **Drag to angle.** At the press, the tangent `axis × (grab − pivot)` at the grabbed ring point is
   projected to the screen; the angle in radians is the pointer's travel from the press along that
   unit screen direction divided by the ring's radius in pixels. It is unbounded and signed, positive
   right-handed about the axis. A ring facing the camera follows the pointer exactly at the press.
7. **Space.** The axes are the world's or the primary's at the press (the existing toggle,
   ADR-0042 rule 26). The rotation `Q` (angle about the axis, in world space) turns every root about
   the pivot: its world position orbits it, mapped into the parent's space as translate maps a delta
   (the parent's inverse 3x3), and its local rotation becomes `normalize(qp⁻¹ · Q · qp · q)`, where `q`
   is its rotation at the press and `qp` its parent's world rotation (`decompose` of the parent's
   world matrix, identity for a root of the world). Exact under parents without non-uniform scale;
   under one with it, the closest rotation (the parent's shear is the parent's). Children follow.
8. **Snapping** rounds the angle to a multiple of the rotation step (relative to the press, as for
   translate), read at each preview. Default on at 10°, stored in radians (settled question 2).
9. **Feedback.** While dragging, only the dragged ring is drawn, in `gizmoHovered`, with the swept
   arc filled from the grab point (`gizmoHovered` at 25 % opacity; past a full turn it shows the
   angle modulo 360°) and the signed angle in degrees, one decimal, next to the pointer (`+37.5°`).
   No new theme tokens.

### Scale

10. **Handles.** Three axis lines as long as the arrows, each ending in a filled square of
    `kGizmoCubeHalfSize` (5 points) half-side (the "cube"); the three plane squares of translate
    (ADR-0048 item 2); and a **centre handle**, a square of `kGizmoCenterHalfSize` (8 points) at the
    pivot, in the theme's text colour. Axes and planes fade end-on exactly as translate's (ADR-0048
    item 3); the centre never fades.
11. **Hit test.** The centre square first, then the nearest visible axis (within the tolerance of
    the line or inside its cube), then the first plane square containing the pixel. **In the Scale
    tool a press on the pivot scales instead of picking** (amends ADR-0048 item 5 for Scale; Rotate
    and Translate still pick there).
12. **Drag to factor.** The travel is the pointer's motion from the press along a unit screen
    direction: the axis's projected direction for an axis, the projected direction from the pivot to
    the plane square's centre for a plane, and up-and-right, (1, −1)/√2 in image pixels, for the
    centre. factor = 1 + travel / (`kGizmoArrowLength` × display scale): one arrow length of travel
    doubles. An axis changes its component of the factor, a plane its two, the centre all three.
13. **Space.** Always each root's own axes: the scale gizmo's frame is the primary's local frame
    whatever the toggle says, and the toolbar shows the space button disabled with the tooltip
    "Scale is always along the selection's own axes". Each root's `LocalTransform.scale` becomes its
    scale at the press times the factor, component-wise; translations don't change (each root scales
    about its own origin); children follow.
14. **Zero and negative.** The factor is clamped to at least `kGizmoMinScaleFactor` (0.01) per drag,
    and with snapping to at least one step. A scale's sign never changes: a mirrored entity stays
    mirrored, and mirroring is a Details edit.
15. **Snapping** rounds the factor to a multiple of the scale step (relative to the press), read at
    each preview. Default on at 0.25 (settled question 3).
16. **Feedback.** While dragging, the dragged handle is drawn in `gizmoHovered` and the factor next
    to the pointer, two decimals (`×1.25`); for a single axis, plane or the centre alike.

### Snapping settings and commands

17. **A toggle and a step per tool**, editor-wide in the `Viewports` store, session state in P2
    (amends ADR-0042 rules 3 and 4; settled question 4): translate on at 0.1 m, rotate on at 10°,
    scale on at 0.25.
18. **`ith.viewport.set_snap`** gains an optional `"tool"` (`"translate"`, the default so existing
    scripts keep their meaning, `"rotate"` or `"scale"`). `"step"` is in metres, degrees (greater
    than 0, at most 360) or a factor (positive) respectively; the result is
    `{"tool", "enabled", "step"}`. No new command (amends ADR-0042 rule 29).
19. **The toolbar** shows the current tool's snap toggle and step, with that tool's presets (settled
    questions 2 and 3; translate's 0.01 to 1 m as today).
20. **Keys** (settled question 5): `ith.viewport.toggle_gizmo_space` is bound to
    `Ctrl+Keyboard.Grave`; no snapping keys in P2.

### Requirements, tests and slices

21. **Performance.** The layout and hit test run on the CPU at most once per viewport per frame,
    with fixed-size arrays and no heap allocation per frame; hovering never re-renders the scene; a
    preview is one patch per root (two fields for rotate), the same cost class as translate. A
    report-only benchmark times layout plus hit test for each tool; its expected cost is
    microseconds, far below a frame at the lowest tier (ADR-0005), where the gizmo must stay usable
    at a 1280 × 800 image.
22. **Tests.**
    - `editor_core` math, pure: ring layout (radius in pixels at display scale 1 and 2, front and
      back halves, the non-grabbable ends of an edge-on ring); ring hit test (nearest ring, back half
      refused, the pivot not a handle); the tangent drag (a face-on ring follows an arc exactly at the
      press, the sign right-handed for each axis, an edge-on yaw ring dragged sideways, angles past
      360°); rotation snapping (relative, negative angles); scale layout and hit order (centre
      first, cube ends); factors for axis, plane and centre; the clamp with and without snapping; a
      mirrored scale keeping its sign.
    - `editor_core` session, through a real `Input` with the keymap and viewport consumers: one
      "Rotate" and one "Scale" entry each; world and local rotation; two roots orbiting the primary;
      a root under a rotated parent (world result exact) and under a non-uniformly scaled one
      (translation exact, rotation the closest); a singular parent skipped for rotate, not for scale;
      scale writing only scale, children following, translations unchanged; the Alt duplicate; the
      identity release recording nothing; Esc and focus loss; hidden in the ADR-0048 item 6 cases
      for every tool; the pivot press scaling in Scale and picking in Rotate; undo restoring the
      quaternion exactly; `set_snap` per tool with its units, defaults and invalid arguments;
      `Ctrl+Grave` toggling the space.
    - `editor_ui`, null RHI: `drawnGizmo` reporting the tool and the highlighted handle; the
      feedback drawn only during a drag; the space button disabled in Scale.
    - `editor_ui_gpu`, WARP and lavapipe: one golden image per backend for the rotate gizmo and one
      for the scale gizmo over the selected cube.
    - Automation: a script runs `set_snap` with each tool and the three tool commands.
23. **Slices**, each committed after `verify`:
    1. Snapping per tool: the `Viewports` state, `set_snap`'s `"tool"`, the toolbar's per-tool
       presets, the `Ctrl+Grave` binding; tests.
    2. Rotate in `editor_core`: ring layout, hit test, tangent drag, snapping, `GizmoDrag` for
       Rotate, the consumer; tests.
    3. Rotate in `editor_ui`: rings, the swept arc and the angle text; golden images.
    4. Scale in `editor_core`: layout with the centre handle, hit order, factors, clamp, snapping,
       `GizmoDrag` for Scale; tests.
    5. Scale in `editor_ui`: cubes, centre, the space button disabled, the factor text; golden
       images.
    6. The guides (`editor-core.md` "Translate gizmo" becomes "Gizmos", `editor-ui.md` "Viewport"),
       a changelog fragment, and an as-built ADR if the build changes any rule here.

## Consequences

- The three tools of ADR-0008 rule 4 are usable in the viewport before the P2 exit gate; Details
  stays the way to type exact values, mirror and set absolute angles.
- The translate gizmo is unchanged apart from sharing its drag type and the per-tool snapping
  state; its behaviour and tests stand.
- The gizmo writes the quaternion; Details re-derives X, Y, Z degrees from it, so after a rotation
  past ±90° pitch Details can show a folded but equivalent triple (`eulerDegrees`). Accepted.
- Under a parent with non-uniform scale a rotation drag gives the closest rotation, not an exact
  one; the scale drag never shears. Both keep every stored transform decomposable.
- In the Scale tool the pivot is a handle, so a click there no longer selects; the other tools keep
  ADR-0048 item 5.
- The quadratic `Transactions` merge noted in ADR-0048 applies to rotate (two fields per root) as it
  does to translate; same follow-up.
- Follow-up: the camera-facing ring and the trackball; translate's centre (screen-plane) handle;
  `[` and `]` for snap steps; persisting tool, space and snapping in Editor Preferences; a
  temporary snap modifier.
- ADR-0042 and ADR-0048 are marked as amended by this ADR in their headers and in the index.
- **Revisit when:** users ask for the view ring, the trackball or a temporary snap modifier;
  component selection arrives (ADR-0041: gizmos on components and sockets need their own pivots);
  orthographic viewports arrive (the tangent drag holds, but the front/back test needs the view
  direction instead of the camera position); the P3 renderer replaces the preview (the gizmo math
  and drawing stay).
