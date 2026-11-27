# ADR-0042: Editor world and viewport (amends 0029, 0037)

- **Status:** Accepted
- **Amended by:** [ADR-0043](0043-editor-ui-user-texture-state.md), [ADR-0045](0045-editor-viewport-amendments-after-the-viewport-panel.md), [ADR-0046](0046-editor-viewport-horizontal-field-of-view.md), [ADR-0047](0047-editor-viewport-picking-as-built.md), [ADR-0048](0048-editor-viewport-translate-gizmo-as-built.md)
- **Date:** 2026-11-24
- **Related:** amends [ADR-0029](0029-editor-ui-toolkit.md) rule 7 (the viewport consumer's place
  in the input stack) and states the viewport redraw rule its rule 12 asks for; amends
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rule 21 (`editor_core`
  depends on `shapes`); [ADR-0006](0006-math-and-coordinate-conventions.md) (conventions,
  camera-relative rendering), [ADR-0008](0008-editor-familiar-to-unreal-developers.md) rule 4 (the
  viewport controls), [ADR-0013](0013-input.md) and
  [ADR-0027](0027-input-rules-completed.md) (input routing, focus and capture),
  [ADR-0017](0017-runtime-and-editor-separation.md) rules 10 and 11 (the editor world is never
  simulated), [ADR-0028](0028-render-graph-v1.md) (the render graph),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) (transform
  propagation), [ADR-0032](0032-entity-references-stable-ids-and-world-copy.md) (stable ids),
  [ADR-0034](0034-level-documents.md) (level documents),
  [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) (`Saved/Editor`),
  [ADR-0040](0040-editor-docking-panels-and-layouts.md) (panels and layouts)

## Context

P2 step 11 builds the Viewport panel: an editor camera with the ADR-0008 rule 4 controls, a grid
and primitive shapes to place, click picking and a translate gizmo. What is already fixed:

- ADR-0008 rule 4's control table (fly, orbit, dolly, pan, F, W/E/R, Space, End, G, F11), and
  `engine/config/Keymap.json`, which binds `ith.viewport.focus_selection`, `translate_tool`,
  `rotate_tool`, `scale_tool`, `cycle_tool`, `snap_to_floor`, `game_view` and `immersive` to
  commands that don't exist yet. Its header comment records that fly, orbit, dolly, pan and the
  fly speed are held input, not commands, and come with the viewport.
- ADR-0017 rules 10 and 11: the editor world is never simulated and runs engine display systems
  only.
- ADR-0029: panels draw with ImGui, confined to `editor_ui` (rule 2); rule 7 sets the input
  consumer stack; rule 9's renderer already reads "any viewport texture a panel shows" in its
  `EditorUi` pass; rule 12's idle policy promises that "viewports re-render their scene only when
  their camera or world changed; that rule belongs to the viewport ADR" — this one.
- ADR-0037: every action is a command (rule 1); an interaction is one transaction with live
  preview (rule 6, the Details drag pattern); the selection holds entity stable ids, primary last
  (rule 11); `EditorChanges` reports what changed as polled counters (rule 13).
- ADR-0040 rule 3: `ith.viewport` is a panel with up to four instances, docked centre.

Code facts this ADR builds on:

- `ith::Input` routes events through per-window consumer stacks; a consumed press hides that key
  from lower consumers until its release; per-frame state gives held keys and summed mouse motion
  and wheel (ADR-0013 rules 9 and 10). Relative mouse mode is a window operation
  (`Window::setRelativeMouseMode`), which the application applies from `Input::mouseCaptured` —
  capture that sends the mouse to the Game layer, Play In Editor's mechanism (ADR-0013 rule 11).
- The ImGui renderer (`editor_ui/ImGuiRenderer.cpp`) draws only ImGui-managed textures today:
  `ImTextureID` is an index into its texture table plus one, and draws with an unknown id are
  skipped. ImGui 1.93's `ImGui::Image` takes an `ImTextureRef`, which an `ImTextureID` converts
  to.
- The RHI copies boxes of texels (`rhi::TexelRegion`, `rhi::BufferTextureCopy`, with 256-byte row
  and 512-byte offset alignments) and has persistently mapped readback buffers. `rhi::Format` has
  `R32Uint` but no two-channel integer format.
- An `ith::Entity` is 64 bits: a 32-bit slot index, a 24-bit generation and an 8-bit world id;
  the null handle is all zeros and world ids start at 1.
- `propagateTransforms` (`ecs`) maintains the Transient `WorldTransform`, writing only changed
  subtrees; the editor doesn't run it yet. Entities spawn with a `WorldTransform`
  (`editor_core/EditCommands.cpp`).
- `math` has `Aabb`, `transformBounds`, ray tests and `perspectiveReversedInfinite` (reversed
  depth, ADR-0006 rule 6).

**Unreal prior art** (public documentation and observed behaviour; no engine source). The
documented viewport controls
([Viewport Controls](https://dev.epicgames.com/documentation/unreal-engine/viewport-controls-in-unreal-engine?lang=en-US)):
RMB with W/A/S/D and Q/E flies, the wheel while RMB is held changes the camera speed, Alt+LMB
orbits, Alt+RMB dollies, MMB pans, F focuses the selection, W/E/R select the transform tools and
Space cycles them, G toggles a game-only view, F11 is immersive; the transform gizmo has three
axis handles, two-axis plane handles and a centre handle. Camera speed is a per-user viewport
setting with a separate mouse sensitivity
([ULevelEditorViewportSettings](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/ULevelEditorViewportSettings));
observed: it was eight discrete steps for years and later became a scalar, the wheel steps it in
both, and the editor restores each level's viewport camera between sessions, per user (the state
lives in per-user project data, not in the level). Observed too: the grid fades with distance,
the snap-to-floor key drops the selection onto the surface below it, and the selection is shown
with an outline. Lesson: the controls and the per-user camera are the muscle memory to keep; the
speed model and the selection look are free choices.

## Options considered

### Where the camera lives

- **An entity in the editor world.** Rejected: the camera would dirty the level, enter the undo
  history and be copied into Play In Editor worlds; it is view state, not level content.
- **Panel state in `editor_ui`.** Rejected: commands and headless tests couldn't reach it, and
  `editor_core` would have to ask ImGui for it.
- **Per-viewport state in `editor_core` (chosen).** An `EditorCamera` per viewport instance in a
  `Viewports` store: testable headless, reachable by commands, invisible to the document.

Where it is saved:

- **In the level's `editor` section.** Rejected: every camera move would dirty the document or be
  lost on save, and the camera would be shared by every user of the level. Unreal keeps it per
  user.
- **The user configuration layer, keyed by level id.** Works (level ids are unique), but the user
  layer is for settings that travel across projects; a camera is meaningless outside its project.
- **The project's `Saved/Editor`, next to the session layout (chosen).** Per user in practice
  (each user's checkout has its own `Saved`), per project, rebuildable, never shared; the session
  layout already lives there (ADR-0035 rule 13, ADR-0040 rule 12).

### Fly speed

- **Eight discrete steps with a fixed table (chosen).** The classic model: predictable, the wheel
  steps it, the toolbar shows it, tests pin it.
- **A continuous scalar** (later Unreal versions): smoother, but needs a value UI of its own and
  the wheel still steps. Rejected for P2; the step model doesn't rule it out later.
- **Distance-scaled speed** (an option in Unreal): automatic, but surprising on a near-empty grid.
  Rejected.

### Input routing

- **A viewport consumer directly below the ImGui consumer, above the keymap (chosen).** While RMB
  is held it takes the letter keys before the keymap; otherwise it takes no keys, so W runs the
  translate tool. Amends ADR-0029 rule 7, which placed the viewport consumers below the keymap —
  there the keymap would consume W before the camera ever saw it.
- **Below the keymap, with the keymap yielding while RMB is held.** Couples `editor_core`'s
  keymap to viewport state; rejected.
- **`Input::captureMouse` with the camera in the Game layer.** Capture routes the mouse to the
  Game layer only (ADR-0013 rule 11), and the keymap above would still take the letters; capture
  stays Play In Editor's mechanism. Rejected.

### The preview pass

- **The viewport renders into its own colour and depth targets, shown as an ImGui image
  (chosen).** Works inside the dock tree, clips and scales with the panel, and several viewports
  can coexist. Needs user textures in the ImGui renderer (new work: it draws only ImGui's own
  textures today).
- **Render the scene into the backbuffer under the UI.** Docking, panel clipping and floating
  panels all break; rejected.
- **Redraw every frame.** Rejected: ADR-0029 rule 12's idle policy exists to not do that; the
  scene re-renders only when the camera, the world, the selection or the size changed.

### What draws a shape

- **A `ShapeRenderer` component in a new `shapes` module (core layer, runtime kind), with the
  shapes' bounds and unit meshes (chosen).** Saved into levels like any component, copied into
  Play In Editor worlds, loadable by every target (ADR-0017); `editor_core` (focus, snap) and
  `editor_ui` (the preview) both depend on it.
- **In `level`.** A display component and mesh generation don't belong in the document module;
  rejected.
- **In `ecs`.** The container machinery stays free of display components; rejected.
- **In `editor_ui`.** An editor module's component can't be in a level a player loads
  (ADR-0017); rejected.

### Picking

- **An entity id target read back per click (chosen).** The shapes pass writes each fragment's
  entity handle; a click copies one texel (`TexelRegion`) into a readback buffer and the selection
  follows a frame or two later. It picks whatever the pass rasterizes — shapes today, meshes in
  P3 — with no second scene representation, and a readback test on both backends proves it.
- **A CPU ray against shape bounds.** No readback latency and headless, but it duplicates the
  GPU's view of the world (transforms, culling) and breaks the day meshes arrive. Rejected as the
  mechanism; the bounds stay for Focus and Snap to Floor.

### The gizmo

- **Translate only, math in `editor_core` as pure functions, drawn on ImGui's foreground draw
  list (chosen).** Screen-space drawing is always visible and needs no depth buffer or pass; the
  math is headless-testable; the drag reuses the Details transaction pattern.
- **A render pass with depth testing.** The gizmo would hide behind shapes; more GPU work for no
  gain. Rejected.
- **ImGuizmo (MIT).** A third-party dependency for what is a few hundred lines of our own math;
  rejected.

### Focus and Snap to Floor

- **F frames the selection's bounds; End drops it onto the highest support below — the ground
  plane and other shapes' tops (chosen).** Bounds are the geometry in P2; a ray against real
  meshes comes with meshes.
- **End drops only onto the ground plane.** Stacked shapes would fall through each other;
  rejected.

## Decision

### The editor camera

1. **The viewport draws the editor world** (ADR-0017 rule 10) with the preview pass of this ADR
   and nothing else: no game systems, no simulation. How the panel follows Play In Editor (which
   world it shows, what the camera does) is decided with the Play In Editor ADR.
2. **`EditorCamera`** (`editor_core`, ImGui-free): `position`, `yaw`, `pitch` (clamped to
   (−π/2, π/2), never flipping), `fovY` (default 90°), `speedStep` (1–8) and the orbit `pivot`.
   It is not an entity and never enters the level, the undo history or the dirty state.
3. **A `Viewports` store in `editor_core`** holds one `ViewportState` per viewport panel instance
   (`ith.viewport.1` … `ith.viewport.4`, ADR-0040 rules 2 and 3): the camera and the game-view
   flag. The current tool, the gizmo space and the snap state are one editor-wide state next to
   them, as in Unreal, where every viewport shows the same tool. Panels render the store; commands
   change it; headless tests drive it.
4. **Cameras are remembered per user, per project, per level** in
   `<project>/Saved/Editor/Viewports.json` (an `ith.editor_viewports` document, version 1): keyed
   by the level's asset id, then the viewport instance id; position, yaw, pitch, fovY, speedStep
   and pivot. Written when a level closes and at exit, atomically, only when changed (as the
   session layout is, ADR-0040 rule 12). A missing or unreadable file means default cameras and
   one warning. The tool, gizmo space and snap state are session state in P2.
5. **The default camera** of a level without a saved one sits a few metres back, right and above,
   looking at the origin; the exact values are pinned by tests. The projection is
   `perspectiveReversedInfinite` with a 0.1 m near plane (ADR-0006 rule 6).

### Fly speed

6. **Eight steps, a fixed geometric table**: 0.5, 1, 2, 4, 8, 16, 32 and 64 m/s; default step 4
   (4 m/s). The wheel while RMB is held moves one step per notch, clamped to 1–8, up faster. The
   viewport toolbar shows the current step.
7. **While RMB is held**, W/A/S/D move along the camera's axes and Q/E down and up in world space
   at the step's speed, constant (no ramp in P2). The fly keys are physical keys (ADR-0013
   rule 3): they stay in place on any layout, while the keymap's letter shortcuts follow the
   layout.

### Input routing

8. **One viewport consumer per editor window** (`editor_core`, an `InputConsumer` like the
   keymap's), added between the ImGui consumer and the keymap consumer (amends ADR-0029 rule 7).
   Panels report their image rectangle in window pixels to the `Viewports` store every frame; the
   consumer routes by the last frame's rectangles, the same one-frame lag ImGui's want-capture
   flags have.
9. **Mouse.** The consumer consumes presses, motion, wheel and releases only while the pointer is
   over a viewport image with no ImGui popup on top (the ImGui consumer already passes those
   through, ADR-0029 rule 7), or while it holds a drag that started on a viewport image, until the
   release. While it holds a drag it tells the ImGui consumer to leave mouse events alone (a new
   flag in the platform backend), so a drag that leaves the panel doesn't break. One drag is held
   at a time; presses of other buttons during it are swallowed.
10. **The letter-key rule.** While fly is active (RMB held from a press on a viewport image) the
    consumer consumes every key press without Ctrl or Alt: W/A/S/D/Q/E feed the camera and other
    unmodified keys are swallowed, so W doesn't switch the tool mid-flight. Chords with Ctrl or
    Alt pass to the keymap (Ctrl+Z still undoes). Without fly the consumer consumes no keys:
    **W is the translate tool without RMB and forward with it.** Releases always route to the
    consumers that saw the press (ADR-0013 rule 10).
11. **Fly uses relative mouse mode** (the cursor hidden and locked, unaccelerated motion,
    ADR-0013 rule 5): the panel asks for it through a new `UiHost::setRelativeMouseMode`,
    forwarded to `Window::setRelativeMouseMode` (which exists), while fly is active. It is not
    `Input::captureMouse`: capture sends the mouse to the Game layer and stays Play In Editor's
    mechanism (ADR-0013 rule 11).
12. **The other gestures** on a viewport image, without relative mode: MMB drag pans, Alt+LMB
    orbits around the pivot, Alt+RMB dollies toward it, and the wheel without RMB dollies by
    steps. An LMB press starts a gizmo drag when it hits a handle (rule 25); otherwise it is a
    click candidate: a release within 4 pixels picks (rule 22), and a longer drag on empty space
    does nothing in P2.

### The preview pass

13. **Each viewport panel owns three targets**, sized to its image rectangle in pixels (the
    framebuffer scale applied) and recreated on resize with deferred destruction: colour
    (`R8G8B8A8Srgb`, `RenderTarget | Sampled`; colour images are sRGB, ADR-0029 rule 11), depth
    (`D32Float`, `DepthStencil`, cleared to 0, greater-equal, ADR-0006 rule 6) and the id target
    (rule 21).
14. **User textures in the ImGui renderer (new work).** The renderer's texture table takes
    textures it doesn't own: the panel registers its colour target (handle, description and its
    `ImportedState`) and gets a token it passes to `ImGui::Image`. Every frame the panel asks the
    renderer for the token's graph import — the renderer imports the texture once per frame — and
    uses that import in its scene passes, so the `EditorUi` pass binds the same import and the
    graph orders the write before the read and places the barriers (ADR-0029 rule 9 anticipated
    exactly this). The panel unregisters before it destroys a target. Today the renderer skips
    draws whose texture it doesn't know; this adds the second kind of entry, with null-device
    tests.
15. **Two passes before `EditorUi` in the same graph**, added after the graph's reset and before
    `addRenderPasses` (the application drives the order, as it does for the UI).
    `ViewportShapes` (raster) draws every `ShapeRenderer` entity with its `WorldTransform`, unlit
    with a fixed light from the camera so shapes read as 3D, selected entities tinted (rule 20);
    it writes colour, depth and the id target. `ViewportGrid` (raster) draws the grid as a
    fullscreen pass, depth-tested, writing colour. Rendering is camera-relative (ADR-0006 rule 7).
    Before the draw list is built, `propagateTransforms` runs for the editor world: it writes only
    the Transient `WorldTransform`, so the history is untouched (ADR-0037 rule 8).
16. **The scene re-renders on change only** (ADR-0029 rule 12's viewport rule): when the camera,
    the world (`EditorChanges::any`), the selection, the size or the game-view flag changed since
    the panel's last render. Otherwise the panel shows the existing texture and adds no scene
    passes; the UI itself still follows the idle policy.
17. **The shaders** (`Grid.slang`, `Shapes.slang`) are compiled by the editor application through
    `shader_compiler` at start-up and handed to `editor_ui`, as the UI shader is (ADR-0029
    rule 9); `editor_ui` doesn't link the compiler.

### What is drawn

18. **The grid**: infinite, on y = 0, minor lines every 1 m, major lines every 10 m, the world X
    and Z axis lines coloured, fading with distance. Colours are theme tokens.
19. **`ShapeRenderer { shape, color }`** — `shape`: Cube, Sphere, Plane or Cylinder; `color`:
    linear RGB (ADR-0006 rule 8) — is a reflected `Component` in the new **`shapes`** module
    (core layer, runtime kind), which also holds each shape's local bounds and unit-mesh vertices
    and indices. It is saved into levels like any component (ADR-0034), copied into Play In
    Editor worlds (ADR-0017 rule 10) and drawn by the preview pass. The preview is unlit, has no
    materials and no lights, and is replaced by mesh components in P3; `ShapeRenderer` leaves with
    it. Placing a shape needs no new command: `ith.entity.create` with a `ShapeRenderer` in
    `components` does it (a Place menu is panel follow-up).
20. **Selection is shown as a tint** on the selected shapes (a theme colour); an outline is
    follow-up. The game-view flag (`ith.viewport.game_view`, G) hides the grid, the gizmo and the
    tint, drawing what a game would see.

### Picking

21. **The id target** is `R32G32Uint` — a new RHI format (both backends and the null device, with
    texel size and copy tests) — the colour target's size, `RenderTarget | CopySrc`, cleared to 0.
    The shapes pass writes each fragment's entity: R = slot index + 1, G = generation; R = 0 means
    no entity. (The world id is the editor world's; the generation is what makes a stale hit
    checkable.)
22. **A click picks.** The click's texel is copied into a readback buffer in that frame's graph
    (`TexelRegion`, `BufferTextureCopy` with the RHI's copy alignments); when the frame's fence
    signals, the handle is read, validated against the world (`isAlive`; stale hits are dropped),
    mapped to its stable id and selected through `ith.selection.set` with the click's modifiers:
    plain click replaces (empty space clears), Ctrl toggles, Shift adds. An entity without a
    stable id is ignored (ADR-0037 rule 5). The selection changes a frame or two after the click;
    the click's position and modifiers wait with the pending pick.

### The translate gizmo

23. **The math is pure and lives in `editor_core`** (`Gizmo.h`): the pick ray from the camera
    through a pixel, the closest point on an axis line or a plane to a ray (clamped when nearly
    parallel, so nothing explodes), the drag delta and snapping. Tests pin the parallel cases and
    the snapping.
24. **The drawing is screen-space**, on ImGui's foreground draw list: three axis handles (X red,
    Y green, Z blue; theme tokens) and three plane handles (XY, YZ, XZ), the hovered one
    highlighted. It is always visible (no depth test) and hit-tested in pixels.
25. **A drag is one transaction** (ADR-0037 rule 6), the Details pattern: begin on the press
    ("Translate"), preview the delta through the scope every frame (it sets
    `LocalTransform.translation` of every selected root entity; children follow), commit on the
    release as one undo entry, and Escape cancels (the keymap consumer already routes Escape to
    `cancelOpen` while an interaction is open). Alt at the press duplicates the selection in a
    nested scope first and drags the copies (ADR-0008 rule 4's Alt + drag).
26. **Axes are world-aligned or aligned to the primary's rotation** (the gizmo space: a toolbar
    toggle and `ith.viewport.toggle_gizmo_space`; default world). **Snapping** (a toolbar toggle
    and step; default on at 0.1 m — Unreal's 10 cm in our metres, observed) rounds the drag's
    translation to the step per axis. With the rotate or scale tool active no gizmo is drawn in
    P2; those edits go through Details.

### Focus and Snap to Floor

27. **Focus** (`ith.viewport.focus_selection`, F) frames the selection: the union of the selected
    entities' world bounds (shape bounds transformed by `WorldTransform`; an entity without a
    shape contributes its position). The camera keeps yaw and pitch, moves its pivot to the
    bounds' centre and backs off until they fit, with a margin, from `fovY` and the aspect. With
    no selection the command is disabled, with the reason. It changes no document state and is not
    undoable.
28. **Snap to Floor** (`ith.viewport.snap_to_floor`, End; an Edit command, one transaction) drops
    each selected root entity with bounds onto its support. Supports are the ground plane
    (y = 0) and the tops of the bounds of entities outside the moved subtrees whose XZ ranges
    overlap the entity's: the entity's bottom rests on the highest support at or below its current
    bottom (a small tolerance), or, when it hangs below every support, on the highest of them.
    Entities without bounds are skipped; children follow their roots.

### Commands and testing

29. **The commands**, registered by `editor_core` in the session's registry (ADR-0037 rule 1).
    `focus_selection` takes an optional viewport instance argument (default `ith.viewport.1`); the
    others are viewport-independent:

    | Id | Kind | What it does |
    |---|---|---|
    | `ith.viewport.translate_tool`, `ith.viewport.rotate_tool`, `ith.viewport.scale_tool` | Action | Select the tool |
    | `ith.viewport.cycle_tool` | Action | Translate → rotate → scale |
    | `ith.viewport.focus_selection` | Action | Rule 27; disabled with an empty selection |
    | `ith.viewport.snap_to_floor` | Edit | Rule 28; one undo entry |
    | `ith.viewport.toggle_gizmo_space` | Action | World ↔ local (rule 26) |
    | `ith.viewport.set_snap` | Action | Snapping on/off and its step (rule 26) |
    | `ith.viewport.game_view` | Action | Rule 20's flag |
    | `ith.viewport.immersive` | Action | Registered; reports "not in P2" (follow-up: the shell hides its chrome) |

    The keymap's existing bindings now resolve and the unknown-command warnings for them stop.
    Every id appears in an automation script (ADR-0037 rule 20). The toolbar's buttons issue these
    commands; nothing in the panel changes state directly.
30. **Tests.**
    - `editor_core`, headless: the camera controller with injected input (a step's speed along
      the forward vector, the wheel clamping the step, pitch clamping, orbit keeping the pivot
      distance, pan moving position and pivot together, focus fitting the bounds, the reversed-Z
      projection pins of ADR-0006); the gizmo math (parallel rays, snapping, world and local);
      Snap to Floor (supports, stacked shapes, the undo entry); the letter-key rule through a real
      `Input` with the keymap and viewport consumers (RMB + W moves the camera, W alone runs
      `ith.viewport.translate_tool`); the persistence round trip.
    - `editor_ui`, null RHI (ADR-0029 rule 18): the panel registers as `ith.viewport.1` and docks
      centre; the targets follow the panel's size; user textures draw; the redraw rule (no scene
      passes across ten idle frames, one after a camera move); the default layout's central stack
      holds the viewport (`Default.json` and its test updated).
    - `editor_ui_gpu`, WARP and lavapipe: a golden image (one cube, one sphere and the grid from a
      fixed camera; a new LFS PPM) and **picking by readback**: a click on a known pixel selects
      that entity.
    - Automation: a script runs every new command.

## Consequences

- The keymap's viewport bindings work, the editor has something visible to place, and the tests
  of rule 30 pin the camera, the routing, the redraw policy, picking and the gizmo.
- **New work this ADR asks for, explicitly:**
  - `rhi`: the `R32G32Uint` format on both backends and the null device, with copy tests.
  - `editor_ui`: user textures in the ImGui renderer (rule 14); the drag flag in the platform
    backend (rule 9); `UiHost::setRelativeMouseMode` (rule 11 — `Window::setRelativeMouseMode`
    itself exists); the Viewport panel and its toolbar; `Grid.slang` and `Shapes.slang`.
  - `editor_core`: `EditorCamera` and its controller, the `Viewports` store and its persistence,
    the viewport consumer, the commands, the gizmo math; a dependency on `shapes` (amends
    ADR-0037 rule 21).
  - `shapes`: the new module (rule 19).
  - The editor application: transform propagation for the editor world before the preview
    (rule 15), the scene-pass ordering (rule 15), and `Default.json`'s central stack gaining
    `ith.viewport.1` with its layout test.
- The preview is a second, deliberately small renderer: unlit, no materials, no lights, replaced
  in P3. Picking, the camera and the gizmo math survive that replacement; the shapes pass and
  `ShapeRenderer` don't.
- A click's selection arrives a frame or two late; accepted (the alternative, a synchronous CPU
  pick, was rejected in Options).
- Follow-up work: the picking and gizmo slices after the panel; rotate and scale gizmos; the
  selection outline; marquee selection; the gizmo's centre handle; orthographic viewports;
  immersive mode; persisting the tool, gizmo space and snap state; the Play In Editor wiring of
  the panel.
- **Revisit when:**
  - the P3 renderer replaces the preview (the shapes pass and `ShapeRenderer` go; picking, the
    camera and the gizmo stay);
  - meshes arrive and shapes' bounds are needed for more than shapes (the id target already picks
    any rasterized mesh);
  - users ask for orthographic viewports, more than four viewports, or a continuous fly speed.
