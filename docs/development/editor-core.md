# Editor core

`engine/ith/editor_core` is the editor without its UI
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 1,
[ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md)): the
document and its world, commands, transactions and undo, selection, change counters, the panels'
view models, panels and layouts, the viewports, level files, the keymap and automation. It includes
no ImGui and opens no window, so everything here is tested headless, under sanitizers too.
`editor_ui` draws its state and calls its commands.

## The session

```cpp
#include "ith/editor_core/EditorSession.h"

ith::TypeRegistry types;
ith::generated::registerReflection_ecs(types);
ith::generated::registerReflection_editor_core(types);
// ... and the game's modules

ith::EditorSession session(types, {.projectDirectory = project, .jobs = &jobs, .assets = &assets});
auto created = session.execute("ith.entity.create", args); // args: a serialization::Value object
```

An `EditorSession` owns one `EditorDocument` and the `CommandRegistry`, with the built-in
commands registered. `types` names the component types that commands and level files may use.
The options give the project directory that relative paths start from, the job system for file
work (without one, files are read and written at once on the calling thread) and the asset
database, for opening levels by id and for the [Content Browser](#content-browser), which scans it
(and, with file operations, will change it). Everything is main thread only.

The document owns the editor world, the open level's asset id, the undo history, the selection,
the change counters and the transactions. The world is read through `document().world()`;
changes go through commands or transactions, never straight to the world, so the history always
matches it. Systems that run in the editor world write only Transient data.

## Commands

Every action is a command with a stable id (`ith.<area>.<verb>`), a display name, a tooltip, a
menu category, a kind, an optional `canExecute` and `execute`. Menus, the toolbar, shortcuts,
panels and automation all call `registry.execute(session, id, args)`.

| Kind | What it is | Refused |
|---|---|---|
| `Edit` | changes the world, inside one transaction named by the display name; a failure cancels it | while playing, and while an interaction holds a transaction open |
| `Action` | changes editor state without its own transaction (undo, redo, selection, files) | while an interaction holds a transaction open |
| `Query` | reads only | never |

While a level is being opened, `Edit` and `Action` commands are refused with
`editor.document.busy`. `canExecute` failures come back as `editor.command.disabled` with the
reason. Arguments and results are JSON objects (`serialization::Value`): entities by stable id,
components by fully qualified type name, values as `fromValue` reads them, unknown fields
refused. Commands that take `ids` use the selection when it is left out.

Built-in commands:

| Id | Arguments | What it does |
|---|---|---|
| `ith.edit.undo`, `ith.edit.redo` | | Undo or redo one entry, restoring the selection |
| `ith.entity.create` | `id`, `label`, `parent`, `components` | An entity with a stable id, an `EditorLabel` and a transform; it becomes the selection. Returns `{"id"}` |
| `ith.edit.rename` | `id` (else the primary selection), `label` | Sets the `EditorLabel` |
| `ith.entity.attach`, `ith.entity.detach` | `ids`, `parent` (attach), `before` | Reparent or make roots, keeping the world placement. Without `before` a new child is appended and a root stays where it is; with it the entities, in the order given, go before that sibling (a child of the parent, or a root), or to the end when it is null |
| `ith.entity.move_before` | `id`, `before` (null: the end) | Reorder among siblings |
| `ith.entity.add_component`, `ith.entity.remove_component` | `ids`, `type`, `value` | Add (or replace) or remove a component |
| `ith.entity.set_property` | `ids`, `path`, `value` | Set one value by component path (`ith::LocalTransform.value.translation.x`); read-only fields are refused with `editor.command.read_only`. An optional takes null to empty it |
| `ith.entity.reset_property` | `ids`, `path` | Reset to Default: the value a default-constructed component has there (an element or an optional's value the default lacks: its type's default) |
| `ith.entity.insert_element`, `ith.entity.erase_element` | `ids`, `path` (a `std::vector`), `index`, `value` (insert) | Insert an element (at the end of each entity's list without `index`), or remove the one at `index` |
| `ith.entity.set_enabled` | `ids`, `enabled` | Remove or add the `Disabled` tag (ADR-0031 rule 9) |
| `ith.edit.delete` | `ids` | Delete; other children are detached in place |
| `ith.edit.delete_with_children` | `ids` | Delete with the whole subtree |
| `ith.edit.duplicate` | `ids` | Copies with new ids, each right after its original; they become the selection. Children are copied only when selected too; references between the copied entities (a selected child's parent) point at the copies. Returns `{"ids"}` |
| `ith.selection.set` | `entities`, `mode` (`replace`, `add`, `toggle`, `remove`) | Change the selection |
| `ith.edit.select_all`, `ith.edit.select_none` | | Select every entity in Outliner order, or none |
| `ith.file.new_level` | `unsaved` | An empty, untitled level |
| `ith.file.open_level` | `path` or `id`, `unsaved` | Open a level file, by path or by asset id; finishes in the background |
| `ith.file.save`, `ith.file.save_as` | `path` (Save As) | Save the level; the write finishes in the background. Save As replaces an existing file without asking (the dialog asks), gives the level a new id on a new path and takes an overwritten level's id (see Level files) |
| `ith.query.document` | | `path`, `levelId`, `dirty`, `saving`, `opening`, `playing`, `entities`, `lastError` |
| `ith.query.selection` | | `entities`, `primary` |
| `ith.query.entity` | `id` | `label`, `parent`, `children` and `components` (JSON, entities as stable ids) |
| `ith.query.find` | `label` | `ids`, in Outliner order |
| `ith.query.history` | | `entries` (names), `cursor`, `canUndo`, `canRedo` |
| `ith.query.world_hash` | | `hash` of the level as it would be saved, `entities` |

## Transactions and undo

```cpp
ith::TransactionScope drag = session.document().transactions().begin("Move");
drag.apply(patches); // every frame: applied at once
drag.commit();       // one undo entry; or drag.cancel() on Esc
```

- One transaction per interaction. A scope destroyed without `commit()` cancels: the collected
  inverse is applied and nothing is recorded. Scopes nest: an inner scope joins the outer one,
  and an inner `cancel()` cancels the whole transaction.
- Each apply is one `World::applyPatches`; its inverse goes in front of the earlier ones, and a
  `set` of a path an earlier inverse already sets is dropped, so a long drag keeps one old value
  per property.
- An undo entry is a name, the patches in the stored form, and the selection before and after.
  Stored patches name entities by stable id (`StoredPatches.h`: placeholders indexing a table of
  ids), so entries keep working after undo spawns an entity again with a new handle.
- The history keeps at most 500 entries and 256 MiB (estimated through reflection) by default,
  dropping the oldest first; the newest entry is always kept.
- The document is dirty while the history is not at the saved marker; undoing back to it makes
  the document clean again. A saved position that can't come back keeps it dirty until the next
  save.
- While playing, nothing is undoable and `Edit` commands are refused.
- Selecting alone makes no entry: entries carry the selection, and undo and redo restore it.

## Change counters

`document().changes()` holds generation counters that view models poll once per frame:
`structure` (spawns, despawns, parents, order, component sets, labels), `values`, `selection` and
`document`. A gizmo drag bumps only `values`, so the Outliner doesn't rebuild.

Each view model keeps the `values` generation it last saw and asks
`touchedSince(seen)` which entities' values changed after it, so several panels read the same
changes. The log behind it keeps the last 4,096 bumps; a panel that fell further behind gets
`nullopt` and refreshes everything.

## Editor camera

`EditorCamera` is the camera state of one viewport ([ADR-0042](../decisions/0042-editor-viewport.md)
rules 2 and 5, amended by [ADR-0046](../decisions/0046-editor-viewport-horizontal-field-of-view.md)).
It is plain view state, not an entity, and changing it does not dirty the document or make an undo
entry. Its default is six metres back-right and above the origin, looking at the origin with yaw 45
degrees and pitch −30 degrees, a 90-degree horizontal field of view (`fovX`) and the origin as its
orbit pivot. All angles in the API are radians.

The camera stores only the horizontal angle. The vertical one follows from the image's aspect
(width over height) each time it is needed and is never stored:
`verticalFov(fovX, aspect) = 2 * atan(tan(fovX / 2) / aspect)`. At 90 degrees across, a 1280x460
image is about 39.5 degrees tall, a square one 90 degrees and a 640x960 one about 112.6 degrees.

```cpp
#include "ith/editor_core/EditorCamera.h"

ith::EditorCamera camera;
ith::CameraInput input;
input.rmb = true;
input.forward = true;
ith::updateCamera(camera, input, deltaSeconds);

const ith::Mat4 view = ith::viewMatrix(camera);
const ith::Mat4 projection = ith::projectionMatrix(camera, width / height, 0.1f);
ith::focus(camera, worldBounds, width / height); // an ith::Aabb in world space
```

`CameraInput` is one frame of held input, with mouse motion in pixels and wheel input in notches.
The viewport routes input to it and accumulates fractional wheel input for speed stepping; the camera
controller has no UI or platform dependency.

| Input | Action |
|---|---|
| RMB + mouse | Look; 0.005 radians per pixel |
| RMB + W/A/S/D | Fly along the camera's forward and right axes |
| RMB + Q/E | Fly down/up along world Y |
| RMB + wheel | Change speed by one step per whole notch, clamped to 1–8 |
| Alt + LMB | Orbit the pivot, preserving its distance; the scene follows the drag (right decreases yaw, down decreases pitch) |
| Alt + RMB | Dolly toward or away from the pivot; 100 vertical pixels are one step |
| Wheel without RMB | Dolly by steps |
| MMB | Pan position and pivot together |

The eight speed steps are **0.5, 1, 2, 4, 8, 16, 32 and 64 m/s**, each twice the previous one; the
default step is 4. `updateCamera` clamps `speedStep` to 1–8, wraps yaw into [−π, π), and normalizes
diagonal fly input. Pitch stays strictly between −π/2 and π/2. RMB look and fly finish by setting the
pivot to `position + forward * max(previous pivot distance, 1 m)`, so a later orbit or wheel dolly
uses the point in view. Each dolly step multiplies the pivot distance by 0.9, with upward wheel or
drag motion moving closer. Panning moves opposite the horizontal drag and along the vertical drag,
scaled by 0.001 times the pivot distance per pixel (using one metre when the distance is smaller); it
does not track the cursor exactly.

`viewMatrix` follows the right-handed, +Y-up, −Z-forward convention. `projectionMatrix` delegates to
`perspectiveReversedInfinite` with `verticalFov(fovX, aspect)`, so the image is `fovX` wide at any
aspect; near depth is 1, distant depth approaches 0 and there is no far plane. Use the viewport's
0.1 m near plane.

`focus` keeps yaw and pitch, moves the pivot to the world bounds' centre and backs away until the
bounds fit at the supplied width/height aspect, in both directions. For `radius = length(bounds.halfExtents())` and
`limitingHalfFov = min(fovX, verticalFov(fovX, aspect)) / 2`, the pivot distance is
`max(1 m, 1.1 * radius / sin(limitingHalfFov))`. This enclosing-sphere fit has a 10% margin and
conservatively includes every corner; point bounds are viewed from one metre. Empty bounds are a
programmer error and leave the camera unchanged.

## Viewports

`session.viewports()` is the `ith::Viewports` store (`Viewports.h`,
[ADR-0042](../decisions/0042-editor-viewport.md) rules 3, 4, 20 and 26). It holds one
`ViewportState` per viewport panel instance, `ith.viewport.1` to `ith.viewport.4`, whether the
panel is open or not: the `camera`, the `gameView` flag and the image rectangle the panel reports
(`imagePosition`, `imageSize` in window pixels; `aspect()` is 1 until the panel has drawn) and
whether the image is hovered with nothing on top of it (`imageHovered`), the last `click` on
the image that the panel hasn't taken yet (see [Click picking](#click-picking)), and the gizmo
handle a drag in it holds (`gizmoDrag`, see [Translate gizmo](#translate-gizmo)). Next to
them is one editor-wide state: the transform tool (translate at first), the gizmo space (world) and
snapping (on, 0.1 m). Panels draw the store, the commands below change it and tests drive it
directly. None of it dirties the level or makes an undo entry.

```cpp
ith::ViewportState* viewport = session.viewports().find("ith.viewport.1");
ith::updateCamera(viewport->camera, input, deltaSeconds);
const ith::Mat4 projection =
    ith::projectionMatrix(viewport->camera, viewport->aspect(), ith::kViewportNearPlane);
```

Cameras are remembered per user, per project, per level in `<project>/Saved/Editor/Viewports.json`,
an `ith.editor_viewports` document (version 1) whose `levels` object holds, by level asset id, each
viewport instance's reflected `EditorCamera`. The session keeps the closing level's cameras when New
Level runs and when Open Level has loaded its level, then gives the new level its saved cameras or
the default ones; the editor calls `session.saveViewports()` at exit. An untitled level keeps none,
and a level whose cameras are all the default gets no entry until they change. The file is read the
first time it is needed and written atomically, only when what it would hold changed. A missing
file, as in a fresh project, simply means default cameras. An unreadable file (another format, a
newer version, a camera with a speed step outside 1–8 or a pitch outside (−π/2, π/2)) means default
cameras and one warning in the `EditorViewports` log category; the next save replaces it. A
camera's missing field keeps its default and a field it doesn't have is ignored silently, so a
camera written with the vertical `fovY` of before ADR-0046 keeps its position, yaw and pitch and
gets the default `fovX`. The tool, the gizmo space, snapping and the game-view flags are session
state and are not saved. Follow-up: pruning the entries of levels that no longer exist.

The Viewport commands ([ADR-0042](../decisions/0042-editor-viewport.md) rule 29) are bound in
`Keymap.json`. Those that take a `"viewport"` argument use `ith::kDefaultViewport`
(`ith.viewport.1`) when it is left out.

| Id | Key | Kind | What it does |
|---|---|---|---|
| `ith.viewport.translate_tool`, `rotate_tool`, `scale_tool` | W, E, R | Action | Selects the tool; returns `{"tool": "translate"}` and so on |
| `ith.viewport.cycle_tool` | Space | Action | Translate → rotate → scale → translate |
| `ith.viewport.focus_selection` | F | Action | Frames the selection in `"viewport"`; disabled with an empty selection; not undoable |
| `ith.viewport.snap_to_floor` | End | Edit | Drops the selection (or `"ids"`) onto what is below it; one undo entry |
| `ith.viewport.toggle_gizmo_space` | | Action | World ↔ local; returns `{"space": ...}` |
| `ith.viewport.set_snap` | | Action | `"enabled"` and `"step"` (metres, positive), each optional; returns both |
| `ith.viewport.game_view` | G | Action | Toggles the game-view flag of `"viewport"` |
| `ith.viewport.immersive` | F11 | Action | Registered; disabled with the reason "not in P2" |

World bounds are a `ShapeRenderer`'s `shapeBounds` moved by `World::worldMatrixOf`, the matrix
`WorldTransform` holds once transforms are propagated, computed at once. **Focus** frames the union
of the selected entities' bounds (an entity without a shape adds its position) with `ith::focus` at
the viewport's aspect. **Snap to Floor** moves each selected root that has a shape (a selected
entity with no selected ancestor) straight down or up, by its own shape only: its children's shapes
don't count (ADR-0042 rule 28 as written). Its bottom comes to rest on the highest support at or
below it, within 1 mm, or, when it hangs below every support, on the highest one. Supports are the
ground (y = 0) and the tops of the bounds of enabled shapes outside the moved subtrees whose X and Z
ranges overlap the entity's; a disabled entity is never a support. Children follow their roots;
entities without a shape are skipped, and so is a root whose parent has a zero scale, where no local
translation can move it. A drop under 0.01 mm counts as resting, so what already rests makes no
entry.

When a level is opened, every entity with a `LocalTransform` gets an identity `WorldTransform`, as
`ith.entity.create` gives one: the component is Transient, so levels don't save it, and transform
propagation fills only entities that have it. This is not a change of the level: no undo entry, and
the level stays clean.

Placing a shape needs no command of its own: `ith.entity.create` with an `ith::ShapeRenderer` in
`components` does it. The editor registers the `shapes` module's reflection for that.

**Add Shape** (`AddShape.h`): `kShapeChoices` lists the four shapes with their names, in
enumerator order, and `addShapeArgs(shape, camera)` gives the arguments of `ith.entity.create` that
add one: labelled with its name, a `ShapeRenderer` of the default colour, 2 m in front of the camera
along its view direction, its bottom on y = 0. The Outliner's context menu runs it with the camera
of `ith.viewport.1` ([editor UI](editor-ui.md#viewport)).

### Viewport input

`ith::ViewportConsumer` (`ViewportInput.h`, [ADR-0042](../decisions/0042-editor-viewport.md) rules
6 to 12) is the viewport consumer of an editor window's input stack. The application adds it after
the keymap consumer and before creating the UI, so the stack is, top first: the UI's consumer, the
viewports', the keymap's (ADR-0042 rule 8 amends ADR-0029 rule 7). Each frame, after
`processEvents`, it calls `update` with the frame's time:

```cpp
ith::KeymapConsumer shortcuts(session, keymap);
ith::ViewportConsumer viewportInput(session);
input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
// ... EditorUi::create({..., .viewports = &viewportInput}) ...
viewportInput.setPixelDensity(pixelDensity); // window pixels per window coordinate
viewportInput.setDisplayScale(window.displayScale()); // the click slop's points to pixels
if (/* the batch holds WindowFocusLost for the window */) {
    viewportInput.focusLost(); // before routing: the synthetic release neither picks nor commits
}
input.processEvents(events, platform.pumpTimeNs());
if (viewportInput.update(deltaSeconds)) {
    idle.requestFrame(); // a camera moved: keep drawing, also while a held W sends no events
}
```

It routes by the image rectangles the panels reported to the `Viewports` store in the last frame.
Mouse presses, motion and the wheel are consumed only over a hovered viewport image (`imageHovered`:
no floating panel or popup over it in that frame), or while the consumer
holds a drag that started on one; the drag stays held outside the image until its button's release.
One drag is held at a time, and presses of other buttons during it are swallowed. The button and
Alt at the press decide the gesture:

| Press on an image | Gesture |
|---|---|
| RMB | Fly: mouse look, W/A/S/D/Q/E move at the speed step, the wheel steps the speed |
| Alt + RMB | Dolly toward the pivot |
| Alt + LMB | Orbit around the pivot |
| MMB | Pan |
| LMB, with or without Alt, on a gizmo handle | A gizmo drag ([Translate gizmo](#translate-gizmo)); checked first |
| LMB | A click candidate: a click picks ([Click picking](#click-picking)) |
| The wheel, without a drag | Dolly by steps |

**The letter-key rule.** While fly is active, the consumer takes every key press without Ctrl or
Alt: the physical keys W/A/S/D move along the camera's axes and Q/E down and up in world space, and
other keys are swallowed, so W doesn't switch the tool mid-flight. Chords with Ctrl or Alt go on to
the keymap, so Ctrl + Z still undoes. Without fly it takes no key: **W is the translate tool without
RMB and forward with it.** Releases reach only the consumers that saw the press, so a W pressed
during fly never releases into the keymap, and a W held from before fly doesn't fly.

Mouse motion and the wheel change the camera of the dragged or hovered viewport through
`updateCamera` as their events arrive. Drag motion reaches the camera in window coordinates: the
application calls `setPixelDensity` each frame with the window's pixels per window coordinate, and
the consumer divides event pixels by it, so orbit, pan and dolly feel the same at 100% and 200%
display scale. Fly's look motion is relative mode's raw mouse motion, which no density scales.

Held fly keys move the camera in `update`. The first update of a movement (keys held after an
update without them) moves for at most `kMaxFlyStepSeconds` (1/30 s), so the frame after an idle
wait doesn't jump; later updates use their whole frame time, so the fly speed is the same at any
frame rate, the lowest scalability tier's included. `update` also reports whether any camera
changed since the last call. The wheel's fractions during fly are kept until they make a whole
notch. The UI asks `flying()` for relative mouse mode, `dragStart()` for where to put the cursor
back when fly ends, and `holdsDrag()` and `viewportAt()` to leave the viewport's mouse events alone
([editor UI](editor-ui.md#input-routing)).

The Alt gestures can collide with the window manager: some Linux desktops (Xfce, Cinnamon, MATE,
Plasma 5) move or resize windows on Alt + drag by default, and the editor never sees those drags.
The user changes the window manager's modifier (often to Super); a configurable orbit modifier is
follow-up work.

### Click picking

A **click** is an LMB press without Alt on an image and its release, with the pointer never more
than `kClickSlop` (4) logical points from the press in between. In pixels that is `kClickSlop`
times the window's display scale, which the application sets each frame with
`setDisplayScale(window.displayScale())`: the platform's content scale, which includes the pixel
density on every platform. So the slop is 4 pixels at 100% and 8 at 200% on Windows, X11, Wayland
and macOS alike (ADR-0047 item 1). It is separate from `setPixelDensity`, which only scales pan,
orbit and dolly. A longer drag stays a drag even if the pointer comes back, and does nothing. RMB,
MMB, the Alt gestures and an LMB press during another drag never click.
On the release the consumer sets the viewport's `ViewportState::click` (`ViewportClick`, in
`Viewports.h`): the press's position in window pixels from the image's corner, and the press's
modifiers as a `PickMode`. Ctrl toggles, else Shift adds, else the click replaces; Ctrl+Shift
toggles. A newer click replaces one the panel hasn't taken yet. The editor UI's Viewport panel
takes the click, reads the entity under it back from its id target, and applies it exactly
`kFramesInFlight` frames later, in the render phase: the viewport's tint changes in that frame, the
Outliner and Details a frame later ([editor UI](editor-ui.md#viewport)).

**Focus loss never picks or moves anything.** On focus loss `Input` synthesises the release of
every held button before it delivers `WindowFocusLost`. For a held LMB press that release would be
a click, and for a gizmo drag a commit. So when a batch of events holds `WindowFocusLost` for the
window, the application calls `focusLost()` before `Input` routes the batch. It cancels a held
gizmo drag (removing the copies of an Alt drag), and drops every viewport's waiting click and the
held press's candidate (ADR-0047 item 2). The batch counts as a whole: a real release just before
the focus loss in the same batch is cancelled too.

The translate gizmo hit-tests its handles at the press first ([Translate gizmo](#translate-gizmo)).
Only a press that misses them, or lands on the pivot itself, stays a click candidate.

`ViewportPick.h` holds the GPU-free half (ADR-0042 rules 21 and 22):

- `pickTexel(entity)` is what the shapes pass writes into the `R32G32Uint` id target: the slot
  index + 1 and the generation. The empty texel (index 0) means no entity, and the target is
  cleared to it. The world id isn't stored: the target only shows the editor world.
- `pickedEntity(texel, worldId)` turns a texel back into a handle.
- `applyPick(session, texel, mode)` changes the selection through `ith.selection.set`:

| Texel | `Replace` (click) | `Toggle` (Ctrl) | `Add` (Shift) |
|---|---|---|---|
| Empty space | Clears the selection | Nothing | Nothing |
| A live entity with a stable id | Selects it alone | Toggles it | Adds it as primary |
| A stale entity (despawned since the frame was drawn: `isAlive` fails) | Nothing | Nothing | Nothing |
| An entity without a `StableId` (ADR-0037 rule 5) | Nothing | Nothing | Nothing |

### Translate gizmo

The translate gizmo ([ADR-0042](../decisions/0042-editor-viewport.md) rules 23 to 26) has two
headless halves: the pure math in `Gizmo.h`, and the session side in `ViewportGizmo.h`, which the
viewport consumer runs. The editor UI only draws it ([editor UI](editor-ui.md#viewport)).

**The math** (`Gizmo.h`; pure, thread-safe). Pixels are image pixels: window pixels from the
image's top-left corner.

- `pickRay(camera, imageSize, pixel)` is the ray from the camera through a pixel, and
  `projectToImage(camera, imageSize, point)` its inverse. Both use the viewport's projection: the
  horizontal `fovX` across the image and the derived vertical angle up it
  ([ADR-0046](../decisions/0046-editor-viewport-horizontal-field-of-view.md)).
- `closestOnAxis(ray, origin, axis)` and `closestOnPlane(ray, origin, normal)` give the point an
  axis line or a plane offers a ray. A ray within about half a degree of parallel, or one whose
  point lies behind the camera, gives none.
- `gizmoFrame(worldMatrix, space)` is the gizmo's origin and axes: the world's (`GizmoSpace::World`)
  or the matrix's rotation with scale and shear removed (`Local`; a collapsed matrix falls back to
  the world's axes).
- `layoutGizmo(camera, imageSize, frame, arrowPixels)` places the handles on screen: three arrows
  `arrowPixels` long when seen side-on (`kGizmoArrowLength`, 100 points, times the display
  scale), and three plane squares (XY, YZ, XZ) spanning 0.2 to 0.45 of the arrow along their two
  axes. Each handle has an opacity: an axis whose angle to the view direction has a sine below
  `kGizmoHiddenFacing` (0.1), or a plane whose normal's cosine to it is below that, is hidden and
  can't be grabbed; it fades in up to `kGizmoOpaqueFacing` (0.2). No layout when the origin is
  behind the near plane.
- `hitGizmo(layout, pixel, tolerance)` is the handle a press grabs: nothing within the tolerance of
  the centre (there is no centre handle in P2, so a click on the pivot still picks), else the
  nearest visible arrow within the tolerance (`kGizmoGrabTolerance`, 6 points, times the display
  scale), else the first visible plane square that contains the pixel.
- `grabGizmo(handle, frame, ray)` starts a drag at the point the press's ray grabs;
  `dragGizmo(grab, ray)` is the world-space translation since then, along the axis or within the
  plane. A ray that grabs no point gives none, so the drag keeps its last translation, and the
  translation is clamped to `kGizmoMaxReach` (50) times the gizmo's distance from the camera.
  Nothing explodes near the horizon.
- `snapGizmo(delta, frame, step)` rounds each component along the frame's axes to the step, so
  snapping happens in gizmo space.

**The session side** (`ViewportGizmo.h`). `viewportGizmo(session, state, displayScale)` is the
gizmo a viewport shows, or none: it needs the Translate tool, a live primary selected entity, no
game view, an image with area, and a level that is neither playing nor being opened. It is also
hidden while another interaction's transaction is open (a Details field being typed into), since
a drag would be refused; a gizmo drag's own transaction keeps it shown. It sits at
the primary's world position (`World::worldMatrixOf`, so it follows a drag at once) with the axes
of `Viewports::gizmoSpace()`.

`GizmoDrag` is one drag, one transaction named "Translate" (ADR-0037 rule 6, the Details
pattern):

```cpp
ith::Expected<ith::GizmoDrag> drag = ith::GizmoDrag::begin(session, handle, gizmo.frame, pressRay, alt);
drag->preview(ray); // whenever the pointer moved
drag->commit();     // one undo entry if it moved; cancel() or Esc puts everything back
```

- `begin` opens the transaction. With Alt at the press it first runs the duplicate command's
  function inside it, so the copies become the selection and are what moves; the registry's checks
  for `ith.edit.duplicate` run before. That function runs outside the registry's depth counter, so
  it must not run registry commands itself (recorded at `duplicate()`). `begin` refuses during
  another interaction, while playing or opening, with an empty selection, and when the press grabs
  nothing.
- The moved entities are the selected roots: a selected entity whose ancestor is also selected
  moves with it. An entity without a `LocalTransform` is skipped, and so is one whose parent's
  matrix collapses a direction. That test is scale-invariant: the determinant of the parent's 3x3
  with unit columns must be at least 1e-6, computed in double, so a tiny but uniform scale still
  moves and a zero scale doesn't. Roots skipped for their parent are logged once per drag (Info,
  `Gizmo`).
- `preview` sets each root's `LocalTransform.translation` to its translation at the press plus the
  world-space delta in its parent's space (the inverse of the parent's world 3x3), so it moves the
  same in the world under rotated and scaled parents. The delta is snapped when the store's snapping
  is on, at its current step. A delta equal to the last applied one applies nothing. If any root's
  translation, at the press or moved, isn't finite, the whole frame is skipped and the last
  position stays, so the roots never move apart.
- `commit` records the entry only when a non-zero delta was applied. Otherwise (a press and
  release without a move, nothing that can move, a drag back to the start) it cancels, so an Alt
  press leaves no copies and no "Translate" entry.
- Esc reaches the keymap consumer, which cancels the open transaction
  (`Transactions::cancelOpen`): the world and the selection come back, the copies of an Alt drag
  are removed, and `preview` and `commit` then do nothing.

**In the consumer.** An LMB press on an image, with or without Alt, first hit-tests the handles of
`viewportGizmo()` at `setDisplayScale()`. A hit starts the `Gizmo` gesture and `GizmoDrag::begin`;
`ViewportState::gizmoDrag` names the handle while it is held, so the panel highlights it. A drag
that begins drops every viewport's waiting click, which would otherwise select inside the drag's
transaction and be refused. The pointer's motion is stored, and `update()` previews its latest
position once per frame. The release previews its own position and commits. After Esc, the next
`update()` clears `gizmoDrag`, and the drag stays held, moving nothing, until the release; a
transaction opened or an undo made before that release is left alone. A drag that can't begin is
logged as a warning in the `Gizmo` category (except a press that grabs no point of the handle, an
expected fallback), and the press starts the gesture it would have started without the gizmo. A
drag never clicks. `focusLost()` cancels a held drag ([Click picking](#click-picking)).

**Not yet.** Each preview frame applies one patch per root, and `Transactions::merge` compares it
with every earlier inverse of the drag, so a drag of many roots costs quadratic work per frame.
Indexing the collected inverse by entity is follow-up work.

## View models

The panels' state lives here, as plain data and commands; `editor_ui` only draws it
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 1).

### Tree view model

`TreeViewModel` holds the rows of a virtualized tree: the Outliner today, the Content Browser's
folders later. The owner gives items in tree order (each item, then its subtree, with its depth)
and stable keys. `rows()` flattens what is shown:

- the children of collapsed items are left out;
- a filter shows only the items whose label contains it, plus their ancestors, expanded;
- the rows are rebuilt only when the items, the filter or the expansion changed, so the panel
  calls `rows()` every frame and draws the visible part with a list clipper.

Expansion and selection are kept by key across rebuilds. `range(from, to)` gives the keys of a
Shift-click range.

### Outliner

`OutlinerModel` shows the editor world in Outliner order: the roots in the World's order, each
followed by its children in their order (ADR-0031 rule 5). A row's label is the entity's
`EditorLabel`, or its stable id. The rows are rebuilt on a `structure` or `document` change, and
the tree's selected keys follow the `Selection` on a `selection` change. Selecting an entity also
expands its ancestors.

Every change is a command:

| Action | Command |
|---|---|
| Click, Ctrl + click, Shift + click | `ith.selection.set` (replace, toggle, or the range in row order; the clicked entity becomes primary) |
| F2, or rename from a menu | `ith.edit.rename`, through the command's prompt (below) |
| Delete, Delete with children | `ith.edit.delete`, `ith.edit.delete_with_children` |
| Enabled check box | `ith.entity.set_enabled`, for the whole selection when the row is selected |
| Drag and drop | `ith.entity.attach` or `ith.entity.detach` with `before`: one undo entry per drop |

Dragging a selected row drags the whole selection, without the entities below another dragged one
(they move with it), in Outliner order. Where a drop lands depends on where the mouse is over the
row under it:

- top quarter: before that row, as its sibling;
- bottom quarter: after it;
- middle: inside it, as its last child;
- empty space below the rows: at the end of the roots.

A drop onto a dragged entity or below one is refused (`editor.outliner.invalid_drop`); a drop
where the entity already is runs nothing.

### Details

`DetailsModel` shows the selected entities' components as rows generated from reflection:

- **Rows.** Each component is a group, LocalTransform first, then by name. Its fields follow,
  grouped by their `Category` attribute. A struct, array or optional that isn't shown inline
  continues with the rows of its parts.
  - Labels come from `DisplayName`, else the name spelled as words (`despawnWithParent` becomes
    "Despawn With Parent"). Tooltips come from `Tooltip`, else the field's name.
  - Inline values: numbers, strings, enums, entity references, Uuids, the math vectors, `Quat`,
    asset references, fields with `Meta(Editor = ...)`, and optionals of those.
  - Expansion is kept by property path. Components, categories and structs start expanded;
    arrays start collapsed.
- **Not listed:**
  - Transient components;
  - `StableId`, `Children` and `DisabledInHierarchy`, which the World keeps;
  - `EditorLabel` and `Disabled`, which the header shows as the name field and the Enabled check
    box.

  The name field is editable with one entity selected, as in Unreal.
- **Multi-select.** Only the components every selected entity has are listed. A row whose values
  differ is `mixed` ("Multiple Values"), and an edit writes one value to all of them. A list whose
  sizes differ shows no element rows.
- **Working copy.** The model draws from its own copy of the components, never from the world.
  It refreshes the copy when the selection, the structure or the document changed, or when
  `touchedSince` names one of its entities, but never while its own interaction is open.
- **Edits.** These are commands: `setValue` (`ith.entity.set_property`), `resetToDefault`,
  `setPresent` for optionals, `addElement`, `removeElement` and `clearElements` for lists,
  `addComponent` (only to the entities that lack the component), `removeComponent`, `rename` and
  `setEnabled`. Numbers are clamped to the field's `Min` and `Max` before they are written
  (ADR-0008 rule 5). `addableComponents()` lists the listed component types that not every
  selected entity has, without `ChildOf`, which the Outliner sets.
- **Interactions.** A slider drag is `beginEdit(row)`, `previewValue(value)` every frame (applied
  at once, the live preview), then `commitEdit()`. That makes one Set Property undo entry. Esc
  (the keymap's `cancelOpen`) or `cancelEdit()` puts the values back. After Esc, previews fail
  with `editor.details.edit_cancelled` until the widget ends the edit. Read-only fields, Play In
  Editor and another open interaction are refused as the commands refuse them.
- **Entity references.** `entityChoices(filter)` lists the labelled entities for a picker;
  `entityLabel` shows a reference.
- **Rotation.** `eulerDegrees` and `quatFromEulerDegrees` convert between a quaternion and X, Y
  and Z in degrees: pitch, yaw and roll about X, Y and Z, applied as yaw, then pitch, then roll.

### Content Browser

`session.content()` is the `ContentBrowserModel`
([ADR-0049](../decisions/0049-editor-content-browser-v1.md), the
[spec](../specs/content-browser.md)), the view model the Content Browser panel and the content
drawer share. This is the browse slice (ADR-0049 rule 27); file operations and import come next.

```cpp
session.execute("ith.content.rescan");                                 // scan Content/, build the items
session.execute("ith.content.navigate", json(R"({"folder": "Meshes"})"));
for (const ith::ContentItem* item : session.content().shown()) { ... }  // display order
```

- **Items.** Every folder of `AssetDatabase::folders()`, every asset of the database (labels:
  native assets without `.ithasset` or `.ithmap`, imported files with their full name), and a
  problem entry for each file the last scan found wrong that is not an asset (a file without a
  sidecar, an orphan sidecar, an unresolved duplicate id, an unreadable or leftover temporary
  file). Problems about folders and assets are badges on them (case conflicts on both sides, format
  mismatches); a duplicate the scan resolved is one info line in the log. Items are keyed by asset
  id for assets and by content path otherwise (`ContentKey`), so a selected asset stays selected
  when Rescan finds it renamed.
- **Type categories.** `contentTypeOf(type)` is the table of spec 3.2: `ith.level` is a Level,
  `gltf` a Model, the input action and context formats Data; a type the table doesn't know is
  Other, shown by its raw type. The module that brings a format or an importer adds its row.
- **Shown items.** Without a search or filter, the current folder's own items; with one, the
  matching assets and problem entries of the current folder and every folder below it. Search
  words all have to be in the label, ASCII without case; the type filter combines categories with
  OR and Problems, and the search with AND. Folders come first; the rest sorts by name in natural
  order (`naturalCompare`: `Rock2` before `Rock10`, `Rock7` before `Rock07`) or by type, either
  way ascending or descending. Each item's name rank is computed once per refresh, so sorting what
  is shown compares numbers.
- **Selection.** An ordered list (the last is primary), an anchor and a focus item, changed by
  `ith.content.select` with a mode: `replace` (a click; no items clears), `add`, `toggle`,
  `remove`, `range` (Shift: from the anchor, which stays) and `add_range` (Ctrl + Shift). Items
  that are no longer shown leave it. It never touches the level, its history or its entity
  selection.
- **Nothing per frame.** Every change rebuilds what it affects at once and bumps `generation()`;
  `viewBuilds()` counts the rebuilds of the shown items for tests. Rescan is the only way outside
  changes come in (no watcher, ADR-0035 rule 9); a save into `Content/` of a file the database
  doesn't know under the level's id (a level's first save, a Save As copy, an overwritten file the
  database hasn't scanned) rescans by itself, so the level shows at once. A Save, or a Save As over a
  level the database knows by its path, needs no Rescan: the id it holds is unchanged.
- **Session state** in `<project>/Saved/Editor/ContentBrowser.json` (`ith.content_browser_state`,
  version 1, written atomically when it changes): the current folder, the view mode and sort, the
  expanded folders, the tree's width and the drawer's height. A missing file means the defaults; an
  unreadable one means the defaults and one warning; a saved folder that is gone falls back to its
  nearest ancestor. Search, filter, selection and the drawer's open state start empty.

The commands are Actions, so they record no undo entry and never make the level dirty. Without an
asset database every one fails with `editor.document.no_asset_database`; the queries then show an
empty browser.

| Id | Arguments | What it does |
|---|---|---|
| `ith.content.navigate` | `folder` | Makes the folder current, expands its ancestors, clears the search and the selection; `editor.content.not_found` for anything else |
| `ith.content.navigate_up` | | The parent folder (Alt + Up); nothing at the root |
| `ith.content.select` | `items` (`{"id"}` or `{"path"}` each), `mode` | Changes the selection; an item not shown is `editor.content.not_found`. Returns `selection`, `primary` |
| `ith.content.select_all` | | Every shown item (Ctrl + A in the browser) |
| `ith.content.dismiss` | | Clears the selection; with nothing selected, closes the drawer (Escape in the browser) |
| `ith.content.set_view` | `mode` (`grid`, `list`), `sortBy` (`name`, `type`), `descending` | The view settings |
| `ith.content.set_filter` | `search` (at most `kMaxContentSearchSize` bytes, cut at a UTF-8 character), `categories` (`level` … `other`, which match assets only, and `problems`) | Returns `shown` |
| `ith.content.rescan` | | Refused while a save is written (`editor.document.save_in_progress`). Scans the disk and rebuilds; the open level follows (not a dirty change, `EditorSession::syncLevelWithAssets`): it takes the id of the record at its path and keeps its path; only when its file is gone does the id find it at a new path (moved outside the editor), else a warning says the file is gone. Returns `assets`, `problems` |
| `ith.content.open` | `item` (else the primary item), `unsaved` | A folder becomes current; a level opens through `ith.file.open_level` by id, with its unsaved-changes rule, refused while playing (`editor.command.playing`), and closes the drawer; other items do nothing |
| `ith.content.toggle_drawer` | `open` | Opens or closes the drawer; while the panel is in the layout, focuses it instead (`target`: `panel`), unless `open` is false |
| `ith.query.content` | | The current folder, search, categories, view, the shown items (kind, path, id, label, type, the category of an asset, problems), the problem count, selection, primary, anchor, focus, `prompt` (null in this slice) and `drawerOpen` |
| `ith.query.content_tree` | | `current` and the tree's rows (path, name, depth, children, expanded) |
| `ith.query.content_problems` | | Every problem of the last scan |

## Prompts

A command that needs input from the user, such as Rename's new name, can have a **prompt**, set by
the module that brings the UI:

```cpp
session.commands().setPrompt("ith.edit.rename",
                             [&outliner](ith::EditorSession&) { return outliner.beginRename(); });
```

Shortcuts and menus call `registry.invoke(session, id)`, a user's gesture. It runs the command's
checks, then its prompt, which starts gathering the input (inline rename) and later runs the
command with its arguments. A prompt that returns false, or no prompt at all, runs the command
without arguments. Automation calls `execute` with arguments and never prompts.

## Panels and layouts

The panel list, the layout model and the layout commands live here, headless
([ADR-0040](../decisions/0040-editor-docking-panels-and-layouts.md)). Only applying a layout to
ImGui's dock space and reading it back are in `editor_ui` (`DockLayout.cpp`).

### Panels

`session.panels()` is the `PanelRegistry`. Each module that brings a panel's view registers it:
an id `ith.<name>`, a display name, a default dock place (`Center`, `Left`, `Right`, `Bottom` or
`Floating`), a number of instances and a `draw` function, which `editor_core` never calls.

| Id | Name | Default dock | Instances | Registered by |
|---|---|---|---|---|
| `ith.outliner` | Outliner | Right | 1 | `editor_ui` (`registerOutliner`) |
| `ith.details` | Details | Right | 1 | `editor_ui` (`registerDetails`) |
| `ith.output_log` | Output Log | Bottom | 1 | `editor_ui` (`registerOutputLog`) |
| `ith.viewport` | Viewport | Center | 4 | the viewport step |
| `ith.content_browser` | Content Browser | Bottom | 1 | `editor_ui` (`registerContentBrowser`) |

A single panel's instance id is its id. A multi-instance panel's instances are `<id>.<n>`
(`ith.viewport.2`), shown as "Viewport 2". ImGui window names end in `###<instance id>`, so a
renamed or translated title keeps its place.

### Layouts

An `EditorLayout` holds three parts:

- the main dock tree, as split nodes (`left_right` or `top_bottom`, a ratio, two children) and tab
  stacks (instance ids, the active one, and one `central` stack);
- floating groups, each a tree in a rectangle given as fractions of the dock space;
- the sub-layouts of P3's asset editors, kept unchanged until then.

It is stored as an `ith.editor_layout` document:

```json
{"format": "ith.editor_layout", "version": 1, "data": {
  "root": {"split": "left_right", "ratio": 0.78,
           "first": {"tabs": [], "central": true},
           "second": {"tabs": ["ith.outliner"], "active": "ith.outliner"}},
  "floating": [], "editors": {}}}
```

Loading validates the document and loads it with warnings:

- an unknown panel, or a repeated one, is dropped;
- an emptied stack gives its place to its sibling, except the central stack, which stays;
- ratios and rectangles are clamped;
- a registered panel the document doesn't name starts closed.

A document that can't be read fails, and the next default applies.

The engine's `Layouts/Default.json` is ADR-0008's layout, limited to the panels registered so far:

- the Outliner above Details in a full-height right column;
- the Output Log under the central area, where the Content Browser joins it later and the
  viewport fills the centre.

`builtInDefaultLayout()` is the whole layout as a constant; a test compares the two.

### The layout session and commands

`session.layouts()` keeps the current layout and its generation. The generation changes when a
command replaces the layout, so the UI rebuilds its dock space. `DockLayout` hands back what the
user arranges with `observe()`, which doesn't change the generation. The session writes
`<project>/Saved/Editor/Layout.json` whenever the layout differs from it, and `restore()` loads that
file at start-up.

The Window commands are Actions: no undo entry, allowed while playing.

- `ith.window.open_panel` and `ith.window.close_panel`;
- `ith.window.load_layout`, `ith.window.save_layout_as` (user layer only),
  `ith.window.set_default_layout`, `ith.window.delete_layout` (user layouts only) and
  `ith.window.reset_layout`;
- the queries `ith.query.layout` and `ith.query.layouts`.

Layouts are whole files in the configuration layers (`Layouts/<name>.json`), and a name resolves
to the highest layer that has it. Reset Layout tries these in order, taking the first that loads:

1. the user's default;
2. the default of the layers below the user's;
3. `Default`;
4. the built-in layout.

Open Panel docks a closed panel next to a panel of the same default place, else in a new split on
that side.

## Level files

- **Save** serializes the level on the main thread (`saveLevel` reads the world), moves the saved
  marker, and writes the file durably on the background lane; the result comes back through the
  job system's main-thread queue. A failed write puts the marker back, so the level stays dirty,
  and `lastFileError()` (and `ith.query.document`'s `lastError`) says why. Editing goes on while
  the file is written; another save or an open waits (`editor.document.save_in_progress`).
- **The level id after Save As** ([ADR-0050](../decisions/0050-editor-content-browser-browse-slice-as-built.md)
  item 9). `ith.file.save_as` replaces an existing file without asking; the overwrite prompt
  belongs to the Save As dialog when it lands. Save As to the level's own path (after resolving it)
  is a Save and keeps the id. Otherwise the file at the target is read on the background lane just
  before the write (at most `serialization::kMaxTextSize` bytes): over another level file the saved
  level takes that file's id, so references to the overwritten level keep working; nothing there,
  or a file that is not a level document, gives the copy a new random id, so the original file
  keeps its own. A level that has no file yet keeps its id on its first file. Any other read error
  (busy, access denied, too large, a directory, a FIFO or a device) fails the save without writing.
  Another spelling of the own path (letter case, a link) counts as a Save As and reads back the
  level's own id. The document takes the resulting id and the path only when the write succeeds; a
  failed write changes neither. When the save's own rescan gives the file another id (a copy of a
  known level made outside the editor), the document takes it. The viewport cameras stay as they
  are and are kept under the resulting id when the level closes.
- **Open** reads the file on the background lane and loads it on the main thread, all or
  nothing; until then the document is busy. A failed read or load keeps the open level.
- **New** and **Open** on a dirty level fail with `editor.document.unsaved_changes` unless the
  arguments say `"unsaved": "save"` (saved first, waiting for the write) or `"discard"`. The UI
  asks Save / Don't Save / Cancel and runs the command again with the answer. Both clear the
  history; Save keeps it.
- `waitIdle()` returns once no file operation runs; the session's destructor calls it.

## Keymap

`Keymap::load(config, registry, warnings)` reads the `commands` section of `Keymap.json` from
each configuration layer: a command id maps to chords such as `"Ctrl+Keyboard.Z"`, the highest
layer that lists a command decides its chords, and `[]` or `null` unbinds it. Invalid chords,
unknown command ids and conflicts are warnings; in a conflict the command bound in the higher
layer wins, and within one layer the lower id. Conflicts are checked per context. There are three
contexts, each command in one, chosen by its id
([ADR-0049](../decisions/0049-editor-content-browser-v1.md) rule 3, amending ADR-0037 rule 4):

- **Play In Editor**: `ith.play.stop`, `ith.play.free_mouse`, `ith.play.possess_or_eject`, handled by
  the Play In Editor consumer while a game runs;
- **Content**: every `ith.content.*` command except `ith.content.toggle_drawer`, looked up first
  while the Content Browser had the keyboard focus in the UI's last frame
  (`ContentBrowserModel::keyboardFocus`, which `editor_ui` reports), and never otherwise;
- **Editor**: everything else.

So Escape stops play, clears the content selection with the focus in the browser, and clears the
entity selection elsewhere; F2, Delete, Ctrl + D and Ctrl + A work the same way. Ctrl + Space
(`ith.content.toggle_drawer`) works with the focus anywhere except in a text field. With the focus
in the browser, the asset view's widget keys (`kContentWidgetChords`: the arrows, Home and End,
alone and with Shift, and Ctrl + F) are consumed without running anything, so End never also runs
Snap to Floor; so are F2, Delete and Ctrl + D (`kContentReservedChords`) until the file operations
register their commands, so unbinding a content command can't let Delete reach the level. The
focus flag also follows a mouse press (none while a popup is open) and `ith.content.toggle_drawer`
between frames, so keys in the same batch of events reach the right context.

`KeymapConsumer` is the keymap consumer of an editor window's input stack (ADR-0029 rule 7). It
invokes the bound command of a key press (`CommandRegistry::invoke`, so a prompt can ask for
input); letter keys follow the user's layout, so Ctrl + Z is the key that prints Z. While an
interaction holds a transaction open, Escape cancels it (`Transactions::cancelOpen`) and other keys
go on to the consumers below.

## Preferences

`ith::EditorPreferences` (`EditorPreferences.h`) is the `preferences` section of `Editor.json` in
the layered configuration, read with `config.get(ith::kEditorPreferences)`. Today it holds the
idle rates of the [editor UI](editor-ui.md#idle-policy): `idleFramesPerSecond` (10) and
`idleFramesPerSecondUnfocused` (2).

## Automation

```cpp
ith::Automation automation(session);
automation.run("ith.entity.create", args);
ith::Expected<ith::ScriptReport> report = automation.runScriptFile("build_and_save.json");
```

A script is an `ith.automation_script` document whose `data.steps` are commands with `args`, an
optional `expect` (a subset of the result: object keys, whole arrays, numbers by value) or
`expectError` (an error code), and `{"wait": "idle"}` steps. There are no variables: commands that
create things take their ids. The runner stops at the first failing step and names it. The same
runner serves the headless tests and the editor's `--automation` run.

## Tests

`tests/editor_core` holds:

- the camera tests: the speed table and all fly axes, speed-step clamping, pitch limits and yaw
  wrapping, pivot tracking, orbit direction and precedence, dolly and pan, transformed world-bounds
  focus and its computed distance at several aspect ratios, and the view and reversed-Z projection
  conventions;
- the transaction and history tests, and the command tests (every command with undo and redo,
  refusals, argument errors);
- the file tests (save and reopen with and without a job system, failed writes and opens, unsaved
  changes, busy refusals, opening by asset id) and the query tests;
- the viewport tests: the store's defaults and the default camera, the viewports file's round trip
  per level, writing only when changed, a missing file (silent) and unreadable ones (one warning),
  the session keeping a level's cameras when it closes and at exit, the tool, gizmo-space, snap and
  game-view commands, Focus (the selection's world bounds at the viewport's aspect, disabled with an
  empty selection, not undoable) and Snap to Floor (the ground, stacked shapes, a plane, hanging
  below every support, entities without bounds, disabled shapes, children following their roots
  under scaled, rotated, stretched and collapsed parents, one undo entry and none for a second
  End), and the WorldTransforms an opened level gets without an undo entry;
- the keymap tests: chords, layers and conflicts, the Content context beside the Editor's, the
  engine's `Keymap.json` (every id registered or listed as coming with a later step; the viewport
  keys resolve), and the consumer;
- the Content Browser tests (`ContentBrowserTests.cpp`, spec requirements 1 to 15, 50 to 52, 54,
  55, 57 and 58) in a temporary project with an asset database: folders, assets, hidden files and
  the tree; every scan problem as an entry or a badge (case conflicts where the file system keeps
  case); the type table and natural order; sorting by name and type in both directions; search and
  filter over the subtree, ASCII without case; every selection mode with its anchor; selecting
  leaving the level alone; navigation; no asset database; Rescan after outside changes, the open
  level following its file; opening levels with the unsaved-changes answer and while playing; the
  drawer and the panel's focus; the Content keymap context; the session state, a deleted folder and
  an unreadable file; nothing rebuilt without a change; the widget keys running nothing with the
  browser focused (End is not Snap to Floor); Ctrl + Space giving the focus at once and the
  press hit test; a new level showing after its first save; Rescan refused during a save; the
  search limit at a UTF-8 boundary; categories for assets only; Keypad Enter; and the state reader.
  Requirement 13's F2 half comes with renaming in slice 2;
- the Content Browser benchmark (`ContentBrowserBenchmark.cpp`, CTest `content_browser_benchmark`,
  label `benchmark`): 10 000 assets in 500 folders; the view model of a 1 000-item folder and a
  search over every item, each against its 5 ms target (spec requirement 59);
- the viewport input tests, through a real `Input` with the keymap and viewport consumers: RMB + W
  flies at the step's speed without running `translate_tool`, all fly keys by physical key, W alone
  runs it, Ctrl + Z still undoes during fly and other keys are swallowed, releases go to the
  consumers that saw the press, the mouse outside every image isn't consumed, a drag that leaves the
  image stays held until its release and swallows other presses, the wheel with and without RMB
  (fractions kept, the step clamped), pan, orbit, dolly and look, the click candidate, and losing
  focus ending fly; the first fly frame clamped to `kMaxFlyStepSeconds` and later ones not; an
  orbit at pixel density 2 matching density 1; clicks: the press's position and modifiers (Ctrl,
  Shift, both), one per viewport, a newer one replacing the last, the slop (a drag past it, even
  back at the press, and a far release don't click; 4 pixels at display scale 1, 8 at 2, the
  pixel density not counting), focus loss with LMB held dropped by `focusLost()` before routing
  (also a real release in the same batch), and RMB,
  MMB, the Alt gestures, presses during another drag and presses outside every image never clicking;
- the gizmo math tests (`GizmoTests.cpp`): the pick ray and the projection against the viewport's
  matrices and the horizontal field of view at wide and tall aspects; nearly parallel and behind
  rays on axes and planes; the world and local frames (scale removed, a zero scale falling back);
  the arrows' constant length on screen at any distance; end-on axes and edge-on planes hidden,
  ungrabbable and fading in; the hit test's order, tolerance and empty centre; axis and plane
  drags; the reach clamp toward the horizon; snapping in world and local frames;
- the gizmo tests (`ViewportGizmoTests.cpp`), through a real `Input` with the keymap and viewport
  consumers: where the gizmo shows (the primary's world position, display scale 2) and where not
  (rotate and scale tools, game view, playing, no image, no selection); local space; a drag
  previewed once per frame and one "Translate" entry on the release, with undo and redo; a press
  and release without a move recording nothing; Esc restoring the world and recording nothing;
  Alt duplicating and dragging the copies in one entry, and Esc removing them; presses off the
  handles still clicking or orbiting, and the pivot clicking; a handle's place clicking with
  another tool, in game view and from a consumer without the gizmo; snapping on and off; plane
  drags; a child under a rotated, scaled parent and a selected child of a selected parent; a drag
  released outside the image; the gizmo hidden during another interaction; an Alt press without a
  move leaving no copy or entry (also repeated), and an Alt drag of a primary without a
  `LocalTransform` recording nothing; redo of an Alt drag; several roots; focus loss cancelling a
  plain and an Alt drag; waiting clicks dropped when a drag begins; after Esc, a new transaction, a
  duplicate or an undo before the release left alone; a NaN root and an overflowing root skipping
  the whole frame; a tiny-scale parent moving and a zero-scale one skipped; and `GizmoDrag`'s
  refusals, its unchanged deltas and its cancel-on-destruction;
- the picking tests (`ViewportPickTests.cpp`): an entity's texel and back (slot 0, the largest
  generation, the empty texel), replace, toggle and add on hits, empty space with each mode, and
  stale hits (a despawned entity, its reused slot, an unused slot) and entities without a stable id
  changing nothing;
- the automation tests: the matcher, script errors, every script in `tests/editor_core/scripts`
  run in a fresh project with an asset database (`content_browser.json` browses the levels it
  saves), and the **coverage test**, which fails when a registered command is run
  by no script. A new command therefore comes with a script step;
- the view-model tests: the tree's rows, filter, expansion and ranges; the Outliner's order,
  selection, rename and the F2 prompt, Delete, Enabled and drag and drop; Details' rows,
  multi-select, clamping, reset, lists, optionals, components, entity references, a drag with live
  preview as one undo entry, Esc and the refresh rule; and a 3,000-step random test of both models
  against the world;
- a randomized model test: thousands of random commands mixed with undo and redo, checked against
  the level text after each entry, plus the P2 step 7 sequence (create, rename, reparent, edit,
  delete, undo all, redo all) compared by level text.
