# ADR-0047: Editor viewport picking as built (amends 0042, 0045)

- **Status:** Accepted
- **Date:** 2026-11-27
- **Related:** amends [ADR-0042](0042-editor-viewport.md) rules 12, 13, 15 and 22, and
  [ADR-0045](0045-editor-viewport-amendments-after-the-viewport-panel.md) items 6 and 7;
  [ADR-0009](0009-rhi.md) (frames in flight, copies), [ADR-0013](0013-input.md) and
  [ADR-0027](0027-input-rules-completed.md) (focus loss),
  [ADR-0028](0028-render-graph-v1.md) (passes and culling),
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rule 5 (only entities
  with a stable id are selectable), [ADR-0046](0046-editor-viewport-horizontal-field-of-view.md)

## Context

P2 step 11's picking slice landed the id target, the click in the viewport consumer and the
read-back in the Viewport panel (`editor_core/{ViewportInput, ViewportPick, Viewports}`,
`editor_ui/{ViewportPanel, shaders/Shapes.slang}`, `rhi::Format::R32G32Uint`). ADR-0042 rule 22
left open when the read happens ("when the frame's fence signals"), what a click is at a display
scale other than 100%, and what happens to a click that can't be picked. ADR-0045 deferred the id
target to this slice. Building it fixed those points.

This ADR restates the affected rules as built. Rules not listed stand, rule 21 included.

## Options considered

When the CPU reads the picked texel:

- **Query the copy frame's fence each frame.** Needs a new RHI call for a state the device already
  tracks. Rejected.
- **Wait for the GPU after the copy.** Simple, but stalls the editor on every click. Rejected.
- **Count frames (chosen).** Inside frame N, `beginFrame()` has already waited for every frame up
  to N − `kFramesInFlight`. A copy made in frame F is readable from frame F + `kFramesInFlight`,
  with no new RHI call and no wait.

How the shapes pass gets each draw's entity:

- **Larger push constants.** They are at `rhi::kMaxPushConstantSize` (128 bytes) already. Not
  possible.
- **A per-draw buffer.** A new upload buffer and an index for a pass that has two values per draw.
  Rejected for KISS.
- **The unused fourth words of the view rows (chosen).** The local-to-view 3x3 leaves free words
  after its first two rows; the id texel fits there.

Where the read-back goes:

- **A buffer per instance.** Four buffers for four texels. Rejected.
- **One Readback buffer per panel (chosen),** one slot per instance.

## Decision

1. **Rule 12, the click.** An LMB press without Alt on a hovered image is a click candidate. It is
   a click if the pointer never moves more than `kClickSlop` (4) logical points from the press
   until the release; in pixels that is `kClickSlop` times the window's display scale (the
   platform's content scale, which includes the pixel density on every platform), so the slop has
   the same physical size on Windows, X11, Wayland and high-density displays. The whole
   press-to-release path counts: once past the slop, the press stays a drag even if the pointer
   comes back, and the release is checked too. The click's position (window pixels from the image's
   top-left corner) and its modifiers are taken at the press. A longer LMB drag does nothing in P2.
   RMB, MMB, the Alt gestures and a press during another drag never click. *Reason:* a click feels
   the same at any display scale; a drag that wanders back is still a drag.
2. **Rule 12, focus loss.** Losing window focus while LMB is held drops the click candidate. The
   release Input synthesises on focus loss (`releaseAll`) never picks. *Reason:* the user didn't
   release over the image; an Alt+Tab must not change the selection.
3. **ADR-0045 item 6, targets.** Each instance keeps two targets, created and released together:
   colour, and the id target (`R32G32Uint`, `RenderTarget | CopySrc`, the colour target's size,
   cleared to 0). If either fails to create, neither is kept. Depth stays a render-graph transient
   per frame. A hidden instance releases both at the start of the next frame. *Reason:* a click
   reads the id target in a later frame than the one that drew it, so it must live as long as the
   picture it matches.
4. **ADR-0045 item 7, the shapes pass.** `ViewportShapes` writes colour, the id target and depth.
   The id texel (slot index + 1, generation; `pickTexel()`) rides in the unused fourth words of the
   first two view rows of the push constants, which are at the 128-byte limit. *Reason:* no new
   buffer for two values per draw.
5. **Rule 15, the pick pass.** A third pass, `ViewportPick` (copy, `NeverCull`), runs after the
   scene passes in a frame that has a click. It copies the click's texel of the id target into the
   instance's slot of the panel's Readback buffer. It also runs in frames where the scene doesn't
   re-render: it imports the id target itself. The click's window position is scaled by the
   target's size over the image's, so a clamped target maps right. *Reason:* the copy needs no
   redraw; the id target already matches the picture on screen.
6. **Rule 22, the read-back.** One Readback buffer per panel, one 512-byte slot per instance
   (`rhi::kTextureCopyOffsetAlignment`). The pick waits in the instance with its mode and the
   device frame that copied it. The first `addRenderPasses()` whose device frame is at least the
   copy frame + `kFramesInFlight` reads it, relying on `beginFrame()`'s wait. There is no fence
   query and the panel never waits on the GPU. Latency is exactly `kFramesInFlight` frames.
   *Reason:* the device already guarantees the copy is done; nothing new in the RHI.
7. **Rule 22, one pick per instance.** At most one pick waits per instance. A newer click replaces
   it, and only the newest copy is read, after its own frame. `invalidate()` drops waiting picks.
   A pick already copied completes after its viewport is hidden: it reads the buffer, not the
   target. *Reason:* the user's last click is the one that counts.
8. **Rule 22, where the selection changes.** The pick is applied at the start of
   `addRenderPasses()`, in the render phase, before the scene passes. The same frame draws the new
   tint; the Outliner and Details, already drawn, show it a frame later. *Reason:* the read must
   happen inside a device frame; the one-frame lag in the panels is invisible.
9. **Rule 22, the modifiers.** A plain click replaces; Ctrl toggles; Shift adds (as primary). Ctrl
   wins over Shift, so Ctrl+Shift toggles. Empty space clears the selection on a plain click and
   changes nothing with Ctrl or Shift. *Reason:* the common editor convention, and one rule for
   both modifiers held.
10. **Rule 22, hits that change nothing.** A stale hit (the entity was despawned since the frame was
    drawn) and a hit on an entity without a `StableId` change nothing, even on a plain click; they
    don't clear like empty space. A click on a hidden instance, or one with no rendered target, is
    dropped. A refused `ith.selection.set` is logged as a warning in the `Viewport` category.
    *Reason:* a click on something must never empty the selection by accident.
11. **Rule 22, frames while picking.** While a pick waits, the editor keeps drawing frames:
    `ViewportPanel::picking()` makes the application request a frame from its idle policy. A
    minimized window draws no device frames, so it requests none; the pick completes once the
    window is restored. *Reason:* an idle editor must not leave a click unapplied until the next
    event, and a minimized one must not spin.
12. **Integer colour targets (RHI rules).** An integer colour format (`R32Uint`, `R32G32Uint`)
    must clear to all zeros: Direct3D 12 converts the clear floats, Vulkan reads them as bits, so
    no other value is portable. Command-list recording checks it in debug builds
    (`rhi::ColorAttachment::clearColor` documents it). A pipeline must not blend into an integer
    colour target; pipeline creation rejects it. The id target's "no entity" value is therefore 0,
    and the shapes pipeline writes it opaque. *Reason:* the same id target on both backends, and
    blending integers is undefined on both APIs.

## Consequences

- ADR-0042's picking rules and ADR-0045's target and pass rules now match the code and the guides
  (`docs/development/editor-core.md` "Click picking", `editor-ui.md` "Viewport").
- `rhi::Format::R32G32Uint` exists on both backends and the null device, with texel-size and copy
  tests.
- The RHI gains two checks for integer colour targets (item 12): a debug check of the clear value
  at recording, and a pipeline-creation error for blending into one.
- RHI documentation fix, no architectural change: `Device::frameNumber()` inside an open frame
  returns that frame's number, and inside frame N every frame up to N − `kFramesInFlight` has
  finished on the GPU. Item 6 relies on it.
- A click selects two frames after it (`kFramesInFlight` = 2); accepted, as in ADR-0042.
- The push constants have no free words left; the next per-draw value needs a buffer.
- The translate gizmo (rule 25) will hit-test its handles at the press first; only a press that
  misses them stays a click candidate.
- ADR-0042 and ADR-0045 are marked as amended by this ADR in their headers and in the index when
  it is accepted.
- **Revisit when:** `kFramesInFlight` changes or the RHI gains a fence query; the gizmo slice
  lands (rule 12's LMB press); marquee selection needs more than one texel; the P3 renderer
  replaces the shapes pass (the id target and the read-back stay).
