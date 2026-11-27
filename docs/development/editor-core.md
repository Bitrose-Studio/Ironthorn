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
database for opening levels by id. Everything is main thread only.

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
| `ith.file.save`, `ith.file.save_as` | `path` (Save As) | Save the level; the write finishes in the background |
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
whether the image is hovered with nothing on top of it (`imageHovered`). Next to
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
ith::ViewportConsumer viewportInput(session.viewports());
input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
// ... EditorUi::create({..., .viewports = &viewportInput}) ...
viewportInput.setPixelDensity(pixelDensity); // window pixels per window coordinate
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
| LMB | A click candidate: it does nothing yet; picking and the gizmo start here later |
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
| `ith.content_browser` | Content Browser | Bottom | 1 | the Content Browser step |

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
layer wins, and within one layer the lower id. The Play In Editor keys (`ith.play.stop`,
`ith.play.free_mouse`, `ith.play.possess_or_eject`) have their own context, so Escape both stops
play and, in the editor, clears the selection.

`KeymapConsumer` is the keymap consumer of an editor window's input stack (ADR-0029 rule 7). It
invokes the bound command of a key press (`CommandRegistry::invoke`, so a prompt can ask for
input); letter keys follow the user's layout, so Ctrl + Z is the key that prints Z. While an interaction holds a transaction open, Escape cancels it
(`Transactions::cancelOpen`) and other keys go on to the consumers below.

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
- the keymap tests: chords, layers and conflicts, the engine's `Keymap.json` (every id registered
  or listed as coming with a later step; the viewport keys resolve), and the consumer;
- the viewport input tests, through a real `Input` with the keymap and viewport consumers: RMB + W
  flies at the step's speed without running `translate_tool`, all fly keys by physical key, W alone
  runs it, Ctrl + Z still undoes during fly and other keys are swallowed, releases go to the
  consumers that saw the press, the mouse outside every image isn't consumed, a drag that leaves the
  image stays held until its release and swallows other presses, the wheel with and without RMB
  (fractions kept, the step clamped), pan, orbit, dolly and look, the click candidate, and losing
  focus ending fly; the first fly frame clamped to `kMaxFlyStepSeconds` and later ones not; an
  orbit at pixel density 2 matching density 1;
- the automation tests: the matcher, script errors, every script in `tests/editor_core/scripts`
  run in a fresh project, and the **coverage test**, which fails when a registered command is run
  by no script. A new command therefore comes with a script step;
- the view-model tests: the tree's rows, filter, expansion and ranges; the Outliner's order,
  selection, rename and the F2 prompt, Delete, Enabled and drag and drop; Details' rows,
  multi-select, clamping, reset, lists, optionals, components, entity references, a drag with live
  preview as one undo entry, Esc and the refresh rule; and a 3,000-step random test of both models
  against the world;
- a randomized model test: thousands of random commands mixed with undo and redo, checked against
  the level text after each entry, plus the P2 step 7 sequence (create, rename, reparent, edit,
  delete, undo all, redo all) compared by level text.
