# ADR-0045: Editor viewport amendments after the Viewport panel (amends 0042, 0043)

- **Status:** Accepted
- **Amended by:** [ADR-0046](0046-editor-viewport-horizontal-field-of-view.md)
- **Date:** 2026-11-26
- **Related:** amends [ADR-0042](0042-editor-viewport.md) rules 4, 9, 11, 13, 15, 16, 19, 27, 28
  and 29, and [ADR-0043](0043-editor-ui-user-texture-state.md) rule 5 (the graph-import phase);
  [ADR-0006](0006-math-and-coordinate-conventions.md) (reversed depth),
  [ADR-0013](0013-input.md) rule 5 (relative mouse mode),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) (enabled state,
  transform propagation), [ADR-0040](0040-editor-docking-panels-and-layouts.md) rule 12 (the
  session layout)

## Context

P2 step 11's first two slices landed the `shapes` module, the `Viewports` store and its commands,
the viewport input consumer and the Viewport panel (`editor_core/{Viewports, ViewportCommands,
ViewportInput, AddShape}`, `editor_ui/{ViewportPanel, ImGuiPlatform}`, `Window::warpMouse`).
Building them showed where ADR-0042's text was wrong, too loose or premature: a missing file
should not warn, routing by rectangle alone let a floating window lose its clicks, a fixed fly
clamp made speed depend on the frame rate, and the id target has nothing to write until picking.
Picking and the gizmo (rules 21 to 26) are later slices and are not changed here.

This ADR restates the affected rules as built. Rules not listed stand.

## Options considered

The depth target:

- **Kept per instance, like the colour target** (rule 13 as written). Survives frames for nothing:
  no pass reads depth from an earlier frame. Rejected.
- **A render-graph transient per frame (chosen).** The graph creates and recycles it; the panel
  keeps nothing.

Who sets relative mouse mode:

- **The panel asks for it** (rule 11 as written). The panel draws only when its tab is visible,
  and Play In Editor capture needs the same switch, so two owners would race. Rejected.
- **The UI platform backend, every frame, from capture or fly (chosen).** One owner for the
  window's mode; it also knows when fly ends, so it can put the cursor back.

Culling the shapes:

- **Back-face culling.** A negative scale flips the winding, so mirrored shapes vanish, and a
  plane disappears from below. Rejected for the preview.
- **No culling (chosen).** A handful of primitives; the cost is negligible.

## Decision

1. **Rule 4, the missing file.** A missing `Viewports.json` (a fresh project) means default
   cameras, silently, as for the session layout. Only an unreadable file (wrong format, invalid
   camera, read error) means default cameras and one warning. A level with no saved entry whose
   cameras are all the default gets no entry. *Reason:* a warning on every fresh project is noise.
   Pruning entries of deleted levels is follow-up.
2. **Rules 27 and 28, world bounds and supports.** An entity's world matrix is the one
   `WorldTransform` holds after propagation, computed from the local transforms
   (`World::worldMatrixOf`), so Focus and Snap to Floor don't depend on propagation having run.
   Disabled entities are never supports. A root whose parent's matrix is singular (a zero scale)
   is skipped, since no local translation gives the drop. A drop shorter than 10 µm counts as
   resting and is skipped. *Reason:* commands run between frames; rounding in rotated or scaled
   parents would otherwise make empty undo entries.
3. **Rule 29, game view.** `ith.viewport.game_view` takes the same optional `viewport` argument
   as `focus_selection` (default `ith.viewport.1`) and toggles that viewport's flag. *Reason:* the
   flag is per viewport (rules 3 and 20).
4. **Rule 9, mouse routing.** The consumer takes presses, motion and the wheel only over an image
   that is hovered in the panel's last frame (`ViewportState::imageHovered`, the panel's item
   hover over an invisible button covering the image), not merely inside its rectangle. While a
   drag is held, the UI platform backend forwards no presses, motion or wheel to ImGui, only
   releases, and tells ImGui the mouse is gone; when the drag ends ImGui gets the last position
   back. Orbit, pan and dolly deltas are in window coordinates (pixels divided by the pixel
   density); fly's look uses relative mode's raw motion. *Reason:* a floating window or popup over
   an image keeps its clicks, and gestures feel the same at any display scale.
5. **Rule 11, relative mouse mode.** The UI platform backend sets it each frame through
   `UiHost::setRelativeMouseMode` while the mouse is captured or a viewport flies, calling only on
   change; the panel requests nothing. When fly ends, the cursor is first warped back to where the
   drag began (`Window::warpMouse`, best effort: Wayland compositors and remote desktops may ignore
   it), then relative mode turns off. A failed switch is logged once. Fly movement clamps only the
   first update of a movement (after an idle wait) to 1/30 s; later frames move for their whole
   time. *Reason:* one owner for the window's mode; the cursor doesn't jump; speed is constant at
   any frame rate.
6. **Rule 13, targets.** Each instance has two targets for now: the colour target, kept by the
   panel, and depth (`D32Float`, cleared to 0) as a render-graph transient per frame. The id target
   arrives with picking (rule 21). An instance not drawn in a frame (hidden tab, closed panel)
   releases its colour target at the start of the next frame. Each side is clamped to the device's
   `maxTextureDimension2D` (logged once), and a size whose target failed to create is not retried
   until the size changes. *Reason:* nothing keeps depth across frames; hidden viewports hold no
   memory; a failing size doesn't fail every frame.

   **Rule 14, drawing the image.** The panel covers the image with an invisible button and draws
   the token with the window's draw list (`AddImage`) rather than `ImGui::Image`. *Reason:* the
   button gives the image an item for hover and keeps an LMB drag from moving the window.
7. **Rule 15, transforms and drawing.** `propagateTransforms` runs inside the panel's
   `addRenderPasses`, at most once per graph, before the first instance's draw list. When a level
   opens, the editor gives every entity with a `LocalTransform` and no `WorldTransform` an identity
   one, outside the history, the document staying clean, so loaded shapes are drawn (it is
   Transient and not saved). The shapes pass draws enabled entities only, without back-face culling,
   so mirrored (negative-scale) shapes render and a plane shows from both sides. It writes colour
   and depth (the id target per item 6). *Reason:* the panel knows when a render happens; a loaded
   level must look like a freshly built one.
8. **Rule 16, the redraw key.** An instance re-renders when any of these differ from its last
   render: the camera's position, yaw, pitch and `fovY` (not the speed step or pivot, which draw
   nothing); the document's structure, value and level counters (`EditorChanges`); the selection
   counter (ignored in game view); the game-view flag; or its target (a new target forgets the last
   render). *Reason:* render only for what changes the picture.
9. **Rule 19, placing shapes.** The Outliner's context menu has **Add Shape ▸ Cube / Sphere / Plane
   / Cylinder**. It issues `ith.entity.create` with a `LocalTransform` and a `ShapeRenderer`, 2 m
   in front of `ith.viewport.1`'s camera along its view direction, with the shape's bottom at
   y = 0. *Reason:* a menu needs no new command; the Outliner has a context menu, the viewport has
   no Place menu yet.
10. **Teardown (amends ADR-0043 rule 5).** The graph-import phase (`EditorUi::endFrame()` to
    `addRenderPasses()`) is also closed by `EditorUi::abandonFrame()`, for a frame whose render is
    skipped (no device frame, a failed import or render, exit), and by the next `newFrame()`. The
    application calls `abandonFrame()` before destroying a viewport panel after an unrendered
    frame, since unregistering inside the phase asserts. *Reason:* teardown and skipped frames
    must not trip the phase assertion.

## Consequences

- ADR-0042's rules now match the code and the guides (`docs/development/editor-core.md`,
  `editor-ui.md`); the tests of rule 30 that exist cover these rules.
- An unreadable `Viewports.json` is replaced by the next save; the cameras it held are lost.
- Fly can't jump the camera after an idle wait, and its speed no longer depends on the frame rate.
- The warp back is best effort; on a compositor that ignores it the cursor shows where it was
  locked.
- ADR-0042 and ADR-0043 are marked as amended by this ADR in their headers and in
  the index.
- Follow-up: pruning camera entries of deleted levels; the id target with picking; a Place menu in
  the viewport.
- **Revisit when:** picking lands (the third target, rule 21); a renderer feature reads depth
  across frames; the P3 renderer replaces the shapes pass and its culling choice.
