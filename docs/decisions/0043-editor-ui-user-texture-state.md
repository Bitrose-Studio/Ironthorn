# ADR-0043: Editor UI user textures: the renderer owns the import state (amends 0042)

- **Status:** Accepted
- **Amended by:** [ADR-0045](0045-editor-viewport-amendments-after-the-viewport-panel.md)
- **Date:** 2026-11-26
- **Related:** amends [ADR-0042](0042-editor-viewport.md) rule 14 (user textures in the ImGui
  renderer); [ADR-0028](0028-render-graph-v1.md) (imported resources and their `ImportedState`),
  [ADR-0029](0029-editor-ui-toolkit.md) rules 9 and 11 (the UI renderer, colour images)

## Context

ADR-0042 rule 14 has a panel register its colour target with "its `ImportedState`": the panel
keeps the state and the renderer refers to it. Review of the implementation found that unsafe.
A pending graph writes the state back when it executes (ADR-0028), so the reference can outlive
the panel that owns it; and re-registering a texture reset the state the renderer held, so the
first barrier no longer waited for the texture's last use. Rule 14 also left open what happens
when a texture is unregistered or destroyed around a pending graph, and which formats the UI can
sample.

The implementation (`editor_ui/EditorUi.h`, `ImGuiRenderer.{h,cpp}`) settles these. This ADR
records the rules; it is a clarification of rule 14, not a new design.

## Options considered

Who owns the state:

- **The panel owns it and registers a pointer** (rule 14 as written). A graph built before the
  panel closes still writes through the pointer when it executes. Rejected.
- **The renderer owns it, reset to Undefined at every registration.** Safe storage, but a
  texture used before (a resized target re-registered, a texture also used outside the UI) would
  lose its last use's ordering. Rejected.
- **The renderer owns it, seeded by value at registration (chosen).** Entries live in stable
  storage, so a pending graph's pointer stays valid; the caller passes the current state when it
  has one.

What the caller learns at unregistration:

- **`unregisterUserTexture` returns the last state.** A graph built before the call may still
  execute and write a newer state into the entry, so the returned value could be stale.
  Rejected.
- **The caller treats the state as unknown (chosen).** Re-registering or importing the texture
  directly starts from `{}`; the cost is a discarded content, which a viewport target redraws
  anyway.

## Decision

1. **The renderer owns each registered texture's `rg::ImportedState`**, in stable storage that a
   pending graph may write to until it executes. The panel hands over no pointer or reference.
2. **`registerUserTexture(texture, desc, initialState = {})`** takes the initial state by value.
   The default (Undefined, nothing to order after) lets the first use discard the contents. A
   panel passes the texture's current state, as its last graph wrote it back, when the texture
   was used before.
3. **After unregistration the caller doesn't know the texture's state:** re-registering it, or
   importing it directly with `graph.importTexture()`, uses `{}`.
4. **One import per graph**, shared by the scene and UI passes: the panel asks for the import
   with `importUserTexture()` after `endFrame()` and before `addRenderPasses()`; the renderer
   imports the texture once and the `EditorUi` pass binds the same handle, so the graph orders
   the scene writes before the UI's read. All user imports and the UI passes belong to one graph
   (asserted). A registered texture is never also imported directly in the same graph.
5. **Unregistering between `addRenderPasses()` and the graph's execution is safe.** The pending
   graph keeps the texture it resolved and still samples it; the freed table slot is not reused
   until the next frame's imports begin (`endFrame()`). Draws that still carry the old id are
   skipped. Registering and unregistering while the import phase is open (between `endFrame()`
   and `addRenderPasses()`) is refused (asserted).
6. **Destroying the RHI texture before unregistering is tolerated** once every graph that
   imported it has executed: later imports return a null handle and its draws are skipped.
   Unregistration is still required. The normal order stays: execute, unregister, then
   `Device::destroy()`.
7. **Main thread only** for register, unregister and import (asserted).
8. **Formats.** A user texture is a live, named, single-layer 2D colour texture with `Sampled`
   usage, in a format every backend can sample with a linear filter (`rhi::isLinearFilterable`:
   the normalized, sRGB, packed and 16-bit float formats). Depth, integer and 32-bit float
   formats are rejected (asserted; registration returns 0 without assertions). Integer formats
   can't be read through the UI's float sampler; 32-bit float formats are rejected because the
   UI sampler filters linearly and Vulkan doesn't guarantee linear filtering for them.
9. **Contract, not checked:** execute or reset every graph that used the UI (its passes or a user
   import) before the next `endFrame()` and before destroying `EditorUi`. Such a graph points at
   state the `EditorUi` owns; a debug check can't tell a pending graph from an abandoned one.

## Consequences

- A panel can close, resize or re-register its targets in any frame without dangling state; the
  rules are documented on `EditorUi`'s declarations and covered by the null-device and GPU user
  texture tests.
- After an unregistration the next use discards the texture's contents; viewport targets
  re-render on change anyway (ADR-0042 rule 16).
- An HDR scene target to show in the UI must be 16-bit float or normalized (or resolved to one
  first).
- ADR-0042 is marked as amended by this ADR in its header and in the index.
- **Revisit when:** the UI must show 32-bit float or integer textures (a nearest sampler or a
  visualisation pass); textures are registered from other threads; or the render graph gains a
  way to detect pending graphs, which would turn rule 9 into a check.
