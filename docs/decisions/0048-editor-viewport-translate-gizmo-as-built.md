# ADR-0048: Editor viewport translate gizmo as built (amends 0037, 0042, 0047)

- **Status:** Accepted
- **Amended by:** [ADR-0055](0055-editor-viewport-rotate-and-scale-gizmos.md)
- **Date:** 2026-11-27
- **Related:** amends [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md)
  rule 6, [ADR-0042](0042-editor-viewport.md) rules 12 and 23 to 26, and
  [ADR-0047](0047-editor-viewport-picking-as-built.md) items 1 and 2;
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) rule 4 (Alt + drag duplicates),
  [ADR-0013](0013-input.md) and [ADR-0027](0027-input-rules-completed.md) (focus loss),
  [ADR-0029](0029-editor-ui-toolkit.md) (draw lists, theme tokens),
  [ADR-0046](0046-editor-viewport-horizontal-field-of-view.md) (the projection the pick ray uses)

## Context

P2 step 11's gizmo slice adds the translate gizmo: the pure math (`editor_core/Gizmo`), the drag as
one transaction (`editor_core/ViewportGizmo`), the press and the drag in the viewport consumer
(`editor_core/ViewportInput`) and the drawing in the Viewport panel (`editor_ui/ViewportPanel`).
ADR-0042 rules 23 to 26 left open what the gizmo looks like, what a near-parallel ray does, which
entities move, how a drag ends on focus loss, and how the Alt duplicate runs inside the drag's
transaction. Rule 24 put the gizmo on the foreground draw list, and rule 23 clamped near-parallel
rays; building it changed both. ADR-0047 left the gizmo's share of the LMB press to this slice.

This ADR restates the affected rules as built. Rules not listed stand.

## Options considered

Where the gizmo is drawn:

- **ImGui's foreground draw list** (rule 24 as written). Always visible, but it draws over menus,
  popups and floating windows that take the clicks: a handle shows where it can't be grabbed.
  Rejected.
- **The viewport window's draw list, after the image, clipped to it (chosen).** Still on top of the
  scene with no depth test and no scene render, and whatever covers the image covers the gizmo too.

A ray nearly parallel to the dragged axis or plane:

- **Clamp the closest point** (rule 23 as written). The entity still jumps to the clamp limit as the
  ray crosses parallel. Rejected.
- **No position; the drag keeps its last one (chosen),** plus a cap on the drag's reach, so a ray
  close to parallel but past the threshold can't throw the entity to the horizon either.

How the Alt duplicate runs inside the drag's transaction (the registry refuses to execute a command
that changes the world while a transaction is open):

- **A registry path for nested execution (`executeNested`).** The right shape once several
  interactions need it; with one caller it is API for a single use. Deferred.
- **Call the command's function directly, after `canExecute` (chosen).** No new API; the
  registry's checks still run first.

What losing focus does to a held drag:

- **Commit the preview.** An Alt+Tab would leave an undo entry the user never released. Rejected.
- **Cancel it (chosen),** as Esc does.

What snapping rounds:

- **The absolute position.** An entity off the grid jumps to it at the first preview. Rejected.
- **The drag's delta, along the gizmo frame's axes (chosen).** The entity moves in whole steps from
  where it was.

## Decision

1. **Rule 24, where it is drawn.** The Viewport panel draws the gizmo on its window's draw list,
   after the image and clipped to it. It is always on top of the scene and needs no scene render.
   Menus, popups and floating windows over the image cover it, and they take the clicks there too.
   *Reason:* a handle is visible exactly where it can be grabbed.
2. **Rule 24, size and handles.** Arrows are `kGizmoArrowLength` (100) logical points long when
   their axis is seen side-on, times the display scale in pixels. A press within
   `kGizmoGrabTolerance` (6) points of an arrow grabs it. The plane squares (XY, YZ, XZ) span 0.2 to
   0.45 of the arrow along their two axes, each in the colour of the axis normal to it. Axis colours
   are the theme tokens `gizmoAxisX`, `gizmoAxisY` and `gizmoAxisZ`. The handle a drag holds is
   drawn in `gizmoHovered`. Without one, the handle under the pointer is, hit-tested as a press is,
   but only while the image is hovered and ImGui has a valid mouse position. During any held
   viewport drag ImGui is told the mouse is gone, so an orbit, a pan or a fly over a handle
   highlights nothing. *Reason:* the same physical size and grab distance at every display scale;
   a highlight promises a grab.
3. **Rule 24, end-on handles fade.** An axis is hidden when the sine of its angle to the view
   direction is at most 0.1, and opaque from 0.2; a plane uses the cosine of its normal's angle to
   the view direction with the same thresholds. A handle at opacity 0 can't be grabbed. *Reason:* a
   drag never starts where a small mouse move gives an unbounded move in the world.
4. **Rule 23, rays that grab nothing.** A ray within about 0.57° of parallel to the dragged axis or
   plane (sine 0.01), or whose closest point lies behind the camera, gives no position: the drag
   keeps its last one, unclamped. A drag moves at most `kGizmoMaxReach` (50) times the gizmo's
   distance from the camera from where it began. *Reason:* nothing explodes near the horizon, and
   no clamp makes the entity jump. The parallel threshold is below the fade's, so a grabbable handle
   always grabs at its press.
5. **Rule 12 and ADR-0047 item 1, the press.** An LMB press on a handle, with any modifiers
   including Alt, is a gizmo drag. It is tested first and takes precedence over the click candidate
   and the Alt+LMB orbit. A drag never clicks. When it begins, every viewport's waiting click is
   dropped, and a pick already copied is dropped when it is read while a transaction is open
   (`ViewportPanel::readPicks`). A press on a handle whose ray grabs no point
   (`editor.gizmo.no_grab`) quietly starts the gesture it would have started without the gizmo;
   other refusals of `GizmoDrag::begin` do the same and are logged as warnings in the `Gizmo`
   category. There is no handle at the pivot in P2 (no centre handle): a press within the grab
   tolerance of it still picks, or orbits with Alt. *Reason:* the handle under the pointer is what
   the user aimed at; an older click must not change the selection under a drag, and selecting
   would be refused during its transaction anyway.
6. **Rule 25, when the gizmo shows.** The gizmo shows with the Translate tool, outside game view,
   on an image with area (`hasImageArea()`, shared by the math and the consumer), with a live
   primary selected entity in front of the camera. It is hidden while playing, while a level is
   opening, and while a transaction is open that no viewport's `gizmoDrag` names: another
   interaction, such as a Details field open for typing, which would refuse the drag. Its pivot is the primary's world matrix computed from
   the local transforms (`World::worldMatrixOf`), so it follows a drag in the same frame. *Reason:*
   no handle is offered that a press couldn't start; the gizmo never lags the entity.
7. **Rule 25, what moves.** The moved roots are the selected entities with no selected ancestor
   that have a `LocalTransform` and whose parent's 3x3 is not singular. The test is scale-relative,
   in double: the absolute determinant of the parent's 3x3 with its columns made unit length must be
   at least 1e-6, and the inverse must be finite. Roots skipped for it are counted and logged once
   per drag, at Info, in the `Gizmo` category. The world-space delta
   is mapped into each root's parent space (the inverse of the parent's world 3x3) and added to its
   translation at the press; children follow. *Reason:* each root moves the same in the world under
   rotated and scaled parents; a collapsed parent has no local move that gives a world move.
8. **Rule 25, preview and release.** The drag previews the pointer's latest position once per frame,
   and again at the release's own position before it commits one "Translate" undo entry. If any
   root's translation at the press or its moved translation is not finite, the whole frame's
   preview is skipped and the last position kept. *Reason:* one transaction update per frame however many motion events arrive; the
   release lands where the user let go; the roots never move apart.
9. **Rule 25, Esc.** Esc cancels the transaction (ADR-0037 rule 6, the keymap's `cancelOpen`): the
   world and the selection come back, Alt copies removed. The drag stays held, moving nothing, until the
   release. It no longer names a held handle (`ViewportState::gizmoDrag`), and no handle is
   highlighted while it waits (item 2). *Reason:* the release must not start a new gesture.
10. **Rule 25, the Alt duplicate.** Alt at the press duplicates the selection inside the drag's
    transaction, and the copies are dragged; the undo entry is one "Translate" holding the copies and
    their move. A release whose last applied delta is zero cancels instead of committing, for every
    drag: one that never moved, one that snapping held at zero, or one moved back to where it began.
    A drag with no movable root (an Alt drag of a primary without a `LocalTransform`, say) previews
    nothing and records nothing either. So an Alt press leaves no duplicate-only entry and no copies
    stacked on the originals. *Reason:* one gesture, one entry; a stray Alt+click leaves no hidden
    duplicate.
11. **ADR-0037 rule 6, nested command functions.** An interaction may run an Edit command's function
    inside its own open transaction after the registry's `canExecute` for it; the command's changes
    join the open transaction. The Alt duplicate does this with `ith.edit.duplicate`. On this path the
    function must not run registry commands: it runs outside the registry's nesting depth, so a
    nested command would be refused as an open interaction. A sanctioned registry path (`executeNested`) is added
    only when a second caller appears. *Reason:* KISS for one caller; the registry's checks still run.
12. **Rule 26, snapping.** Snapping rounds the drag's delta along each gizmo-frame axis to the step:
    relative, not the absolute position. In Local space the axes are the primary's at the press. The
    step is read at each preview, so a change during a drag applies at once. *Reason:* the entity
    moves in whole steps from where it was, along the axes the user sees.
13. **ADR-0047 item 2, focus loss.** Losing window focus cancels a held gizmo drag, Alt copies
    included, and drops every viewport's waiting click. When a batch of events holds
    `WindowFocusLost` for the window, the application calls `ViewportConsumer::focusLost()` before
    Input routes the batch (`input.processEvents`), so Input's synthetic release finds the
    transaction ended and neither commits nor picks. The batch counts as a whole: a real release
    just before the focus loss in the same batch is cancelled too; accepted. `focusLost()` replaces the
    picking slice's `cancelClicks()`. *Reason:* an Alt+Tab must not leave an undo entry or change the
    selection.

## Consequences

- ADR-0042 rules 23 to 26 and ADR-0047 items 1 and 2 match the code and the guides
  (`docs/development/editor-core.md` "Translate gizmo", `editor-ui.md` "Viewport").
- The theme gains `gizmoAxisX`, `gizmoAxisY`, `gizmoAxisZ` and `gizmoHovered`.
- `ViewportConsumer` is made from the session (it needs the document for the gizmo); its
  `Viewports`-only constructor is gone.
- A golden image per backend shows the gizmo over the selected cube.
- The Alt duplicate bypasses the registry's execute path; a second such caller adds
  `executeNested` instead of another direct call.
- Follow-up: `Transactions` merging is quadratic in the patches per preview frame, so a drag of many
  roots costs more per frame. Fine at P2's selection sizes; fix before large selections are common.
- ADR-0037, ADR-0042 and ADR-0047 are marked as amended by this ADR in their headers and in the
  index when it is accepted.
- **Revisit when:** a centre handle or screen-plane drag is added (the pivot press stops picking);
  the rotate and scale gizmos land (after P2); a second caller needs nested command execution.
