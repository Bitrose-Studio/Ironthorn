# Editor UI

`engine/ith/editor_ui` is the editor's user interface on Dear ImGui
([ADR-0029](../decisions/0029-editor-ui-toolkit.md)): the one ImGui context with our own platform
and renderer backends, the theme, fonts and icons, the idle policy, the dock space and the shell
(main menu bar and status bar). It draws the state of [`editor_core`](editor-core.md) and runs its
commands; `editor_core` never sees ImGui. The editor application, `ironthorn_editor`
(`editor/`), puts it in a window.

## The boundary

- Only `editor_ui` links ImGui (`ith::imgui`), and only its source files include it. Its headers
  name no ImGui type, so nothing outside the module can depend on the toolkit. The
  `imgui_confined` test checks this, and that `imgui_internal.h` appears in `DockLayout.cpp` only,
  the one file allowed to use ImGui's docking internals.
- ImGui's configuration is `editor_ui/ImGuiConfig.h`, set as `IMGUI_USER_CONFIG` on `ith::imgui`
  for every translation unit: `IM_ASSERT` is `ITH_ASSERT`, no obsolete functions, 32-bit
  `ImWchar`, no Win32 or shell functions, and no default allocator. `EditorUi` installs the
  allocation functions (the C heap until the engine has a general-purpose allocator) before it
  creates the context; without them an allocation asserts.
- One `EditorUi` exists at a time: ImGui has one global context. Every ImGui call runs on the main
  thread. Render graph execute callables read the renderer's own copy of the frame, never ImGui.
- ImGui writes no file (`imgui.ini` and its log are off). Layouts are the editor's own documents
  (ADR-0029 rule 15, ADR-0040), applied and read back by `DockLayout.cpp`.
- Multi-viewports are off: floating panels stay inside the main window.

## A frame

```cpp
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/EditorUi.h"

ith::Input input;
ith::KeymapConsumer shortcuts(session, keymap);
ith::ViewportConsumer viewportInput(session.viewports());
input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
ith::WindowUiHost host(platform, window);
auto ui = ith::EditorUi::create({.device = &device, .targetFormat = format,
                                 .vertexShader = vertex, .pixelShader = pixel,
                                 .fontDirectory = fonts, .input = &input,
                                 .window = window.id(), .host = &host,
                                 .viewports = &viewportInput,
                                 .displayScale = window.displayScale()});
ith::EditorShellState shell;

// every frame, after the events and main-thread jobs (ADR-0035 rule 22):
input.processEvents(events, platform.pumpTimeNs());
if (viewportInput.update(dt)) idle.requestFrame();
(*ui)->newFrame({.size = window.size(), .pixelSize = window.pixelSize(), .deltaSeconds = dt,
                 .focused = input.focusedWindow() == window.id()});
ith::drawEditorShell(session, keymap, shell);
(*ui)->endFrame();
// after device.beginFrame():
graph.reset();
const ith::rg::Texture target = graph.importBackbuffer(frame);
(*ui)->addRenderPasses(graph, target, frame.slot, ith::rhi::LoadOp::Clear, clearColor);
graph.compile();
graph.execute(pool);
```

`EditorUi::create` fails with `editor_ui.font_failed` when a font can't be read and with
`editor_ui.renderer_failed` when the pipeline or sampler can't be created.

## Platform backend

`ImGuiPlatform.cpp` feeds ImGui from engine events, not from SDL (which stays private to
`platform`):

- Each frame it hands over the window's size in window coordinates, the framebuffer scale
  (pixels per coordinate) and the time step. Event positions arrive in window pixels and are
  divided by that scale.
- Keys, mouse buttons, the wheel and text become ImGui input events. Letters and digits follow the
  user's layout (`Event::layoutKey`), as editor shortcuts do, so Ctrl + Z in a text field is the
  key that prints Z; other keys are physical.
- The clipboard, the IME's place, the cursor shape and relative mouse mode go through a `UiHost`.
  `WindowUiHost` implements it over `ith::Platform` (`clipboardText`, `setClipboardText`,
  `setCursor`, `setCursorVisible`) and `ith::Window` (`setTextInputArea`, `setRelativeMouseMode`);
  failures are dropped, as on the offscreen driver, which has no system cursors. Tests implement
  `UiHost` to record the calls.
- Relative mouse mode is on while the mouse is captured (Play In Editor) or a viewport flies
  ([ADR-0042](../decisions/0042-editor-viewport.md) rule 11); the backend asks the host only when
  the mode changes, and leaves the cursor alone while it is on. When fly ends it first warps the
  cursor back to where the fly's press was (`UiHost::warpMouse`, `Window::warpMouse`; Wayland
  compositors may ignore the warp), then turns relative mode off. The editor application therefore
  doesn't set relative mode itself. `WindowUiHost` logs the first failed switch as a warning with
  the platform's message.

### Input routing

The UI is an input consumer in the `Editor` layer of its window's stack. `EditorUi::create` adds
it, so it sits above the consumers added before it: the application adds the keymap consumer, then
the viewport consumer ([editor core](editor-core.md#viewport-input)), so the stack is, top first,
the UI, the viewports, the keymap ([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 7 as
amended by [ADR-0042](../decisions/0042-editor-viewport.md) rule 8). It hands every event that
reaches it to ImGui, and consumes:

- **key presses only while a text field is active**, so W in a text field types W instead of
  reaching the keymap. The UI's consumer is then also the window's focused consumer with
  `acceptsText`, so `Input::textInputWanted()` turns on and the application starts the platform's
  text input; it gives the focus back when the field closes. ImGui reports an active field at the
  end of the frame that activates it, so the focus follows at once;
- **mouse presses, motion and the wheel while ImGui wants the mouse** (the pointer is over an ImGui
  window, or a drag started in one). Over the empty dock space the events go on to the consumers
  below. So do the mouse events over a hovered viewport image, by the rectangles and hover the
  panels reported last frame ([Viewport](#viewport)), while no ImGui popup is open: ImGui sees them
  but leaves them to the viewport consumer (`EditorUiDesc::viewports`, ADR-0042 rule 9).

While the viewport consumer holds a drag, its presses, motion and wheel are the viewport's alone:
ImGui neither sees nor takes them and is told the mouse is gone, so a camera drag that leaves the
image keeps going and never clicks, hovers or scrolls another panel. Releases still reach ImGui
(without a position), since it saw the drag's own press before the drag began. When the drag ends,
ImGui gets the mouse's last position back (for a fly, the press position the cursor is warped to),
so hover follows the cursor before it next moves.

The want-capture flags come from the previous ImGui frame, because routing runs before
`NewFrame`. While the mouse is captured (Play In Editor), ImGui is told the mouse is gone. Editor
shortcuts are always bound through the keymap consumer, never through ImGui's own shortcut
functions.

## Renderer backend

`ImGuiRenderer.cpp` draws ImGui's draw data with the RHI, in render graph passes:

- **On the main thread**, `addRenderPasses()` first handles ImGui's texture requests: a new
  texture is created as `R8G8B8A8Unorm` (`Sampled | CopyDst`) and uploaded whole; an update copies
  only the changed rectangles (a `rhi::TexelRegion` per rectangle); a texture ImGui no longer needs
  is destroyed once unused for `kFramesInFlight` frames, which the device defers as usual. It then
  copies vertices, indices and texel rows into the upload buffer of the frame in flight
  (`MemoryType::Upload`, `ShaderRead | Index | CopySrc`, grown by half again when too small), and
  turns the draw commands into records with scissors projected to the target and clamped to it.
- **`EditorUiUpload`** (Copy) copies the texel rows into the textures, when any changed.
- **`EditorUi`** (Raster) draws into the target: one pipeline with alpha blending and no depth,
  vertices pulled by bindless index (the RHI has no vertex input), the draw's first vertex in the
  push constants, one `drawIndexed` per command with its scissor, and a linear clamp sampler.
  ImGui-owned texture ids are indices into the renderer's texture table; user-texture ids also
  carry a generation. Both resolve to bindless sampled indices in the pass, per draw command.

Colours are linear inside the shader: ImGui's colours are sRGB-encoded bytes, which the vertex
shader decodes, so blending happens in linear space and the sRGB target encodes on write
([ADR-0006](../decisions/0006-math-and-coordinate-conventions.md) rule 8, ADR-0029 rule 11).
Translucent fills and anti-aliased text therefore blend slightly differently from stock ImGui.

The shader is `editor_ui/shaders/EditorUi.slang`. The module carries it as text
(`editorUiShaderSource()`), and the editor application compiles both stages at start-up with the
[shader compiler](shaders.md), for the device's bytecode format (SPIR-V for the null device, which
takes any bytes). So `editor_ui` builds without the shader compiler, in sanitizer builds too.
There is no shader cache yet; compiling takes a fraction of a second.

`EditorUi::renderStats()` reports the last frame's draws, vertices, indices, texture uploads,
ImGui-owned textures held, the upload buffer's size and the bounds of the scissors.

### User textures

A panel registers a borrowed texture with `EditorUi::registerUserTexture(handle, desc,
initialState)`: a single-layer 2D texture with `Sampled` usage, a non-empty `desc.debugName` and a
linear-filterable format (`rhi::isLinearFilterable`: no integer, 32-bit float or depth formats,
since the UI's sampler filters linearly and Vulkan doesn't guarantee that for 32-bit floats). The
texture must belong to the UI's device; that is the caller's responsibility and is not checked.
The renderer owns its persistent `ImportedState` from then on, starting at a copy of
`initialState`, but never owns or destroys the RHI texture. Pass the texture's current state when
the caller knows it (as its last graph wrote it back), so the first barrier orders after that use;
the default `{}` (Undefined) lets the first use discard the contents.

The returned `UiTextureId` converts to `ImTextureID` in the panel's `.cpp` for `ImGui::Image`
(which accepts it as an `ImTextureRef` in the pinned ImGui version). Unknown, unregistered and stale
ids skip their draws, and generations prevent an old id from naming a later occupant of the same
table slot. Registration changes assert while the graph-import phase between `endFrame()` and
`addRenderPasses()` is open.

After `endFrame()`, `importUserTexture(graph, id)` returns the texture's one import for that graph.
The viewport's scene passes write that handle before `addRenderPasses()` adds `EditorUi`, which
samples the same import; the graph therefore places the render-target-to-shader-read barrier. Use
one graph per frame, don't reset it between these calls and `addRenderPasses()`, and never import a
registered RHI texture directly with `graph.importTexture()`. The target given to
`addRenderPasses()` must belong to the graph of that frame's `importUserTexture()` calls; a mismatch
asserts and adds nothing, so the frame's texture changes are kept for later. A registration enters
the UI graph only through an explicit request or a reference from that frame's draws; an otherwise
unused texture is not imported.

The UI's passes read the renderer's copies of the frame's records, which stay valid until the next
`endFrame()`: execute or reset the graph before then, because the next `endFrame()` starts new
imports from the states that graph writes back and lets freed table slots be reused. Nothing checks
this in debug builds: a check could not tell a graph still waiting to execute from one abandoned on
purpose (after a failed compile, say). Unregistering between `addRenderPasses()` and execution is
safe: the pending graph still samples the original texture, and its slot is not reused before the
next `endFrame()`. Before destroying the `EditorUi`, execute or reset every graph that holds its
passes or user-texture imports, since those graphs point at state the `EditorUi` owns.

A frame that won't be rendered after `endFrame()` (the device had no frame, the target couldn't be
imported, rendering failed, or the editor exits) leaves the import phase open, which refuses
registrations; `abandonFrame()` closes it. The next `newFrame()` closes it too.

`newFrame`, `endFrame`, `abandonFrame`, `registerUserTexture`, `unregisterUserTexture`, `importUserTexture` and
`addRenderPasses` assert that they run on the thread that created the `EditorUi`.

Normally the owner unregisters after every participating graph has executed and before
`Device::destroy()`. Destroying first is tolerated once every graph that imported the texture has
executed: subsequent draws are skipped and unregistration is still required. RHI deferred
destruction keeps the native texture and its descriptors alive until frames in flight no longer use
them. After unregistration the texture's graph state stays with the renderer and is unknown to the
caller, so registering or importing the same RHI texture again must start from the default `{}`
(Undefined), which discards its contents.

## Theme, fonts and icons

- **Theme** (`Theme.h`): `ThemeTokens` are colours as sRGB hex values (`0xRRGGBB` and an alpha),
  spacing, radii, borders and type sizes. `applyTheme(tokens, displayScale)` maps them onto
  ImGui's style, sizes times the display scale (ImGui rounds them down to whole pixels), and sets
  the DPI font scale. `EditorUi::setDisplayScale` applies it again on
  `WindowDisplayScaleChanged`; fonts follow without rebuilding the atlas, since ImGui rasterizes
  glyphs on demand. `defaultDarkTheme()` is a neutral dark theme until the editor theme sets its
  values.
- **Fonts**: Inter Regular and SemiBold for text (`UiFont::Text`, `UiFont::Bold`) and JetBrains
  Mono for code and logs (`UiFont::Mono`), read with the engine's file API from the `fonts/` folder
  next to the executable and handed to ImGui from memory. `EditorUi::pushFont`/`popFont` select
  one. ImGui's bundled stb_truetype rasterizes only these pinned fonts; fonts a user picks would
  need FreeType first.
- **Icons** (`Icons.h`): Lucide's icon font, merged into the text fonts, so an icon is text
  (`iconText(icons::kSave)`). Inter has glyphs of its own in the Unicode Private Use Area, where
  Lucide's icons are, so the text fonts leave that area out (`GlyphExcludeRanges`) and the icons
  show. Panels name icons only through `Icons.h`, never by codepoint, because
  Lucide moves codepoints between releases; a test checks every entry against the pinned
  `codepoints.json`. `commandIcon(id)` gives the icon a menu shows for a command.

The build copies the font files and their licences (`Inter-LICENSE.txt`, `JetBrainsMono-OFL.txt`,
`Lucide-LICENSE.txt`) to `fonts/` next to the executables ([third-party code](third-party.md)).

## Idle policy

The editor redraws on demand (ADR-0029 rule 12). `IdlePolicy` decides how long the loop may wait:

```cpp
ith::IdlePolicy idle({.focused = preferences.idleFramesPerSecond,
                      .unfocused = preferences.idleFramesPerSecondUnfocused});
const auto timeout = idle.waitTimeout(focused);
auto events = timeout.count() == 0 ? platform.pumpEvents() : platform.waitEvents(timeout);
if (!events.empty() || jobs.runMainThreadJobs() > 0 || changed) {
    idle.requestFrame(); // this frame and kSettleFrames (2) more
}
// ... the frame ...
idle.frameFinished();
```

Activity (events, a change of the document's counters, a finished job, later Play In Editor and
animations) draws at once, plus two frames for ImGui to settle. Otherwise the editor draws at the
idle rate, 10 frames per second while one of its windows has focus and 2 while none has: the
`preferences` section of `Editor.json` (`ith::EditorPreferences`, `ith::kEditorPreferences` in
`editor_core`). A job that posts to the main thread ends the wait through `Platform::wake()`.

## The shell

`drawEditorShell(session, keymap, state)` draws:

- the **main menu bar**: File and Edit (`kShellMenus`), listing the registry's commands of those
  categories, the built-in ones in Unreal's order (New Level, Open Level, Save, Save As; Undo,
  Redo, Duplicate, Delete, ...), each with its icon and the first chord the keymap binds to it
  ("Ctrl+Z"). A command that can't run now is disabled, its reason as the tooltip. Choosing one runs
  it through the registry as a user's gesture (`CommandRegistry::invoke`, so Rename starts renaming
  in the Outliner); commands that need arguments, such as Open Level's path, report the missing
  argument in the status bar until their dialogs exist;
- the **Window** menu: the panels (checked when open; choosing one opens or focuses it) and Load
  Layout, Save Layout As (a dialog for the name that asks before overwriting), Set as Default,
  Delete Layout and Reset Layout ([ADR-0040](../decisions/0040-editor-docking-panels-and-layouts.md));
- the **dock space** over the rest of the window, where `ith::DockLayout` (`DockLayout.cpp`) builds
  the layout session's current layout with DockBuilder, draws every open panel and reads what the
  user arranges back into the layout session;
- the **status bar**: the level's file name or "Untitled", with " *" for unsaved changes, and the
  last menu command or its error.

## Panels

Each panel draws a view model of `editor_core` ([editor core](editor-core.md#view-models)) and
changes the world only through its commands. A panel is registered by the code that owns its view
(ADR-0040 rule 3):

```cpp
ith::OutlinerPanel outliner(session);
ith::registerOutliner(session, outliner);        // "ith.outliner", and the F2 rename prompt
ith::DetailsPanel details(session);
ith::registerDetails(session.panels(), details); // "ith.details"
ith::OutputLog log(history, ui.theme());
ith::registerOutputLog(session.panels(), log);   // "ith.output_log"
ith::registerViewport(session.panels(), **viewport); // "ith.viewport", instances 1 to 4
```

A change a panel can't make (a refused command) goes to the log, in the category `Outliner`,
`Details` or `Viewport`.

### Outliner

`OutlinerPanel` draws `OutlinerModel`:

- a search box;
- the entities as a tree with a list clipper, so only the visible rows are drawn;
- an Enabled check box per row.

Like Details, the rows have no header row.

Disabled entities, those below a disabled one, and the ancestors a search shows only for their
matches are dimmed.

| Gesture | What it does |
|---|---|
| Click, Ctrl + click, Shift + click | Select, toggle, select the range. A selected row selects on release, so dragging it keeps the selection |
| Double click, or F2 | Rename in place: Enter or leaving the field keeps the name, Esc drops it |
| Drag a row | Drop before or after another row (its top or bottom quarter), inside it (the middle), or on the empty space below the rows (the end of the roots); a line or frame shows where |
| Click the empty space | Select None |
| Right click | The context menu: Add Shape (see [Viewport](#viewport)) |

F2 reaches the Outliner through the rename command's prompt: the keymap and the Edit menu
*invoke* `ith.edit.rename`, and `registerOutliner` makes the prompt start renaming the primary
selection.

### Details

`DetailsPanel` draws `DetailsModel`.

- **Header.** The name field (disabled when more than one entity is selected, as in Unreal), the
  Enabled check box, and **Add**, a searchable list of the component types the selection can
  take, grouped by category.
- **Rows.** Each component is a framed group with a Remove button. Below it come its properties:
  a name, with the field's tooltip, and a value editor. A row that isn't at its default value has
  a Reset to Default button.

Editors are found by the value's type id, then by its kind:

| Type | Editor |
|---|---|
| `bool` | check box |
| integers, `float`, `double` | drag field, limited to the field's `Min` and `Max` |
| `std::string` | text field, set on Enter or when it loses focus |
| enums | combo box of the enumerators, spelled as words |
| `Vec2`/`Vec3`/`Vec4`, `IVec`, `UVec`, `DVec3` | one drag field per component, limited to `Min` and `Max` |
| `Quat` | X, Y and Z in degrees; the degrees are kept while dragging, so the drag doesn't drift |
| `Entity` | combo box with a search field over the entities by name, and None |
| `AssetRef`, `SoftAssetRef`, `Uuid` | the id, read-only (an asset picker comes with the Content Browser) |
| `std::optional` | a check box for present, and the value's editor |
| `std::vector`, `std::array`, structs | a row that expands into its elements or fields; a vector has Add Element and Remove All, each element Remove |

**How editors change values.** Drag fields are interactions (ADR-0037 rule 6):

1. Pressing the field begins an edit.
2. Every frame of the drag previews the value in the world.
3. Releasing the field commits it as one Set Property undo entry.

Esc during a drag reaches the keymap, because the UI takes keys only while a text field is active.
It cancels the edit and the value goes back; the rest of the drag does nothing. Check boxes, combo
boxes, pickers and text fields set their value with one command each.

**Typing a number.** A click on a drag field without moving the mouse, or a Ctrl + click, turns
the field into a text field with its whole value selected. A double click's second click lands in
the text field that is already open, where it selects the word under the mouse. Enter or a click
elsewhere sets the typed value with one Set Property command, for every selected entity; Esc drops
the text. A rotation is typed in degrees. The value isn't previewed while it is typed.

The press opens a drag edit and its release ends it, empty; ImGui then opens the text field with
no new activation, so DetailsPanel sets a change that arrives while no edit is open. A press whose
edit is refused (while playing, for one) is remembered until its release, so the drag that follows
changes nothing and logs one warning instead of one per frame.

**Multi-select.** Values that differ between the selected entities show "Multiple Values"; an edit
writes the same value to all of them.

### Viewport

`ViewportPanel` (`ViewportPanel.h`) draws the editor world from editor core's `Viewports` store
([editor core](editor-core.md#viewports)), as [ADR-0042](../decisions/0042-editor-viewport.md)
rules 8 and 13 to 20 decide. It is the panel `ith.viewport`, instances `ith.viewport.1` to
`ith.viewport.4`, docked in the centre; the engine's `Default.json` opens instance 1 in the central
stack.

```cpp
// at start-up, after EditorUi::create; the shaders are compiled by the application
auto viewport = ith::ViewportPanel::create(session, ui, device,
    {.shapesVertex = shapesVertex, .shapesPixel = shapesPixel,
     .gridVertex = gridVertex, .gridPixel = gridPixel});
ith::registerViewport(session.panels(), **viewport);

// every frame, after ui.newFrame(), before the panels draw:
(*viewport)->newFrame();               // releases the targets of hidden viewports
// after ui.endFrame() and device.beginFrame():
graph.reset();
const ith::rg::Texture target = graph.importBackbuffer(frame);
(*viewport)->addRenderPasses(graph);   // the scene passes, before the UI's
ui.addRenderPasses(graph, target, frame.slot, ith::rhi::LoadOp::Clear, clearColor);
```

Destroy the panel before the `EditorUi`: its targets are user textures of the UI. Its destructor,
`newFrame()` and `draw()` register and unregister targets, so they run outside the UI's
graph-import phase; when the last frame wasn't rendered (a minimized window, a failed frame),
`EditorUi::abandonFrame()` closes that phase first. After a graph with the panel's passes fails to
execute, `invalidate()` makes every instance render again. The shell must not draw the panel after
it is destroyed.

**Targets.** Each open instance owns a colour target, `R8G8B8A8Srgb` with `RenderTarget | Sampled`,
the size of its image in pixels (the image's size in window coordinates times the framebuffer
scale; each side at most the device's `maxTextureDimension2D`, with one warning when the image
is larger). It is registered as a [user texture](#user-textures) and shown with an image under an
invisible button. When the size changes, the panel unregisters the old target, destroys it (the
device defers that until the GPU is done) and creates a new one, during its draw, never in the
import phase. A target that can't be created is logged once and tried again only at another size.
An image without area has no target and adds no passes. The depth target, `D32Float` cleared to
0, is a render graph transient of the same size. An instance not drawn in a frame (a
hidden tab, a closed panel) releases its target at the start of the next frame, in
`ViewportPanel::newFrame()`, outside the import phase; shown again, it gets a new target and
renders once.

**Passes.** `addRenderPasses()` adds two raster passes per instance that must render, on the
colour target's one import from `EditorUi::importUserTexture()`, so the `EditorUi` pass that shows
the image reads after them:

- `ViewportShapes` clears colour (the theme's `viewportBackground`) and depth, then draws every
  enabled entity with a `ShapeRenderer` and a `WorldTransform`, one draw each. The four unit meshes
  are in one upload buffer, written once at creation, the indices already offset to each shape's
  first vertex. Each draw's push constants (128 bytes) carry the clip matrix, the local-to-view
  3x3 for the normals and the colour. Shading is unlit with a fixed light from the camera, a little
  above and to the left, so the faces of a box differ. Selected entities are mixed halfway toward
  the theme's `viewportSelection`. Depth is reversed (greater-equal). Nothing is culled, so a
  mirrored shape (a negative scale) draws, its normals flipped by the sign of its determinant.
- `ViewportGrid` draws the grid as one fullscreen triangle. Each pixel casts its view ray at
  y = 0 and writes the hit's depth, so the grid is depth-tested against the shapes. The rays span
  the same angles as the shapes' projection: the camera's horizontal `fovX` across the image and
  `verticalFov(fovX, aspect)` up it, with the aspect of the image, not of the panel with its
  toolbar ([ADR-0046](../decisions/0046-editor-viewport-horizontal-field-of-view.md)). It has minor
  lines every metre and major lines every 10 m; the X axis (z = 0) and the Z axis (x = 0) have
  their own colours. Every line is about one pixel wide on screen, antialiased with the screen
  gradient of the world coordinates. Each level (minor, major) fades out as its cells shrink on
  screen, fully drawn while its lines are at least 10 pixels apart in both directions and gone once
  they are 3 pixels apart in either, so lines seen far away or at a grazing angle never alias into
  false curves or moire. The axes are single lines and only fade with distance. Everything fades
  out smoothly (a smoothstep, with no kink at its end, so no rim shows) at 50 m plus 10 times the
  camera's height. The colours are the theme's `gridMinor`,
  `gridMajor`, `gridAxisX` and `gridAxisZ`.

Rendering is camera-relative ([ADR-0006](../decisions/0006-math-and-coordinate-conventions.md)
rule 7): the camera's position is subtracted from every world matrix on the CPU. Game view
(`ith.viewport.game_view`) draws neither the grid nor the tint. Once per graph, before the first
draw list, the panel runs `propagateTransforms` on the editor world. That writes only the Transient
`WorldTransform`, so the history and the dirty state stay as they were.

**The redraw rule** (ADR-0042 rule 16). An instance renders only when something it shows changed
since its last render:

- the camera's position, yaw, pitch or horizontal field of view (not its speed step or pivot; a
  resize changes the derived vertical angle and is covered by the target's size);
- the entities, their values or the open level (`EditorChanges`);
- the selection, except in game view, which doesn't show it;
- its target's size;
- the game-view flag.

Otherwise it adds no passes and the image shows the last render. The UI around it still follows
the [idle policy](#idle-policy).

**Input contract.** Every frame an instance draws, it reports its image to its `ViewportState`:

- `imagePosition` and `imageSize` in window pixels (ImGui coordinates times the framebuffer scale,
  the units of mouse events);
- `imageHovered`, which is ImGui's `IsItemHovered` on the invisible button: false while a floating
  panel or a popup lies over the image.

`addRenderPasses()` gives an instance that wasn't drawn that frame (a hidden tab, a closed panel) a
zero size and no hover. The viewport consumer and the platform backend route mouse input to an
image only while it is hovered, or while a drag that started on it is held. The invisible button
keeps an LMB drag on the image from moving a floating window. When a held drag ends, the platform
backend gives ImGui the mouse's position back, so the image is hovered again without a move.

**Toolbar.** Above the image, every control runs a command and changes nothing itself:

| Control | Command |
|---|---|
| Translate, Rotate, Scale | `ith.viewport.translate_tool`, `rotate_tool`, `scale_tool` |
| Snap, and the step (0.01 to 1 m) | `ith.viewport.set_snap` with `enabled` or `step` |
| World / Local | `ith.viewport.toggle_gizmo_space` |
| Game View | `ith.viewport.game_view` with the instance as `viewport` |
| Speed *n* | shows the camera's fly speed step (the wheel with RMB held changes it) |

A refused command is logged in the category `Viewport`.

**Add Shape.** The Outliner's context menu (a right click in its rows) has **Add Shape**, with
Cube, Sphere, Plane and Cylinder. Each runs `ith.entity.create` with the arguments of
`addShapeArgs(shape, camera)` ([editor core](editor-core.md), `AddShape.h`, with the shapes and
their names in `kShapeChoices`):

- the shape's name as the label;
- a `ShapeRenderer` of the default colour;
- a `LocalTransform` 2 m in front of the camera of `ith.viewport.1`, along its view direction,
  lifted or lowered so the shape's bottom rests on y = 0.

The new entity becomes the selection, and the next frame draws it.

**Shaders.** `Shapes.slang` and `Grid.slang` in `editor_ui/shaders` are compiled into the module as
text (`ViewportShaders.h`: `viewportShapesShaderSource()`, `viewportGridShaderSource()`, the entry
points and the push-constant sizes) and compiled by the application at start-up. They have no
variants.

## Tests of the panels

`tests/editor_ui/PanelTests.cpp` draws the Outliner and Details in fixed windows on the null
device. It drives them with mouse and key events through the platform backend and the engine
keymap's consumer:

- a click selects, and a click on the empty space clears the selection;
- F2 renames in place, Enter keeps the name, and Esc drops it without reaching Select None;
- dragging a row onto another reparents it in one undo entry;
- a translation drag previews live and becomes one undo entry, and Esc cancels a drag;
- a click or Ctrl + click on a number field types into it: Enter or a click away sets the value in
  one undo entry, Enter without typing records nothing, and Esc drops the text without reaching
  Select None; typed degrees set a rotation, and a typed value sets every selected entity;
- a drag whose edit is refused while playing changes nothing and logs one warning;
- one drag edits two selected entities whose values differed.

## The editor application

```
ironthorn_editor --project <dir> [--user-dir <dir>] [--automation <script>] [--frames N]
                 [--backend d3d12|vulkan|null] [--adapter hardware|software]
```

It creates the platform window, the job system (whose main-thread wake ends the idle wait), the
RHI device and the window's swapchain, the configuration layers (engine `<exe>/Config` and its
platform folder, project `<project>/Config` and its platform folder, and the user layer:
`--user-dir`, else `Ironthorn/Editor` in the user's configuration folder), an `EditorSession`
of the project, the keymap, the UI and the Viewport, Outliner, Details and Output Log panels (the
UI and viewport shaders compiled from Slang for the device at start-up), then runs the
loop of ADR-0035 rule 22: events (waiting while idle), main-thread jobs, input routing, the UI
frame, rendering. Where the window has no native handle (SDL's `offscreen` driver) it draws into an offscreen texture; where no GPU device
can be created it renders on the null device. Closing the window quits after a running save has
finished. With unsaved changes the first close is refused with a message in the status bar, and a
second close discards them, until the Save / Don't Save / Cancel dialog exists.

`--automation <script>` runs an `ith.automation_script` ([editor core](editor-core.md#automation))
once the shell has drawn its first frames, draws a few frames more, and exits 0 when the script
passed and 1 when it failed.

## Tests

- `editor_ui` (headless, also under ASan, UBSan and TSan): the platform backend with injected
  events (routing over a window and over empty space, keys and text into a text field, layout
  letters, focus loss, mouse capture; the mouse left to the viewport consumer over its image but not
  under a popup, kept from ImGui during its drag so no button clicks and nothing scrolls, and fly
  ending on focus loss), the clipboard, the IME area, the cursor, relative mouse mode (captured,
  flying) and the warp back before it ends, in order, through a recording host; the renderer on the
  null device (atlas creation and partial updates, a growing atlas destroying its old texture,
  upload buffer growth, scissor clamping, recorded commands, user textures and their shared
  scene/UI graph imports, stale-id handling, released GPU objects); the theme and fonts; the
  shell's menus, chords and status; the icons against `codepoints.json`; the shader source; the
  idle policy; the Viewport panel (`ViewportPanelTests.cpp`: registration and the default layout's
  central stack, the colour target and its user texture following the image's size at framebuffer
  scales 1 and 2, the redraw rule over ten idle frames and after each kind of change, no passes
  without area, the rectangle in window pixels, a zero rectangle while hidden and the target
  released the frame after, no hover under a floating window and hover back after a drag, disabled
  and mirrored shapes, two viewports in one graph, a clamped oversize target with one warning,
  teardown after an unrendered frame, the toolbar, and Add Shape from the Outliner's context menu,
  drawn by the next frame).
- `editor_automation_null` and `editor_automation_gpu` (label `sample`, the second also `gpu`):
  `ironthorn_editor` on SDL's offscreen driver runs `tests/editor/open_edit_save_reopen.json` in a
  fresh project - create, rename, attach, undo, redo, save, new level, open, query - on the null
  device and on the software GPU (WARP; lavapipe on Linux, else the null device), and exits 0.
  Never on a desktop. Not in Release, where the editor isn't built.
- `editor_ui_gpu` (label `gpu`, built with the shader compiler): a golden screenshot of the
  shell in the Default layout (menu bar; the Outliner above Details on the right and the Output Log
  under the empty central area, all of an untitled level; status bar), 1280 x 720 at display scale 1,
  with a fixed time step and no caret blink, on WARP (`golden/shell_d3d12.ppm`) and on lavapipe
  (`golden/shell_vulkan.ppm`), the UI shader compiled at run time. A pixel differs when a channel
  differs by more than 8 of 255, and the shot fails when more than 0.5% of the pixels differ; the
  rendered image is then written next to the test executable (`shell_<backend>.actual.ppm`). The
  golden images in `tests/editor_ui_gpu/golden` are uncompressed binary PPM stored with Git LFS
  (`*.ppm` in `.gitattributes`); `ITH_UPDATE_GOLDEN=1` rewrites them, on Windows for WARP and in a
  Linux container with Ubuntu 24.04's `mesa-vulkan-drivers` for lavapipe. A backend without its
  software device reports itself skipped. Separate user-texture tests on WARP and lavapipe clear a
  registered texture to a known colour in a scene pass, draw it with `ImGui::Image`, and check every
  readback pixel; they need no golden file. A second golden image shows the Viewport panel, 640 x
  400, with a red cube and a blue sphere on the grid from a pinned camera
  (`golden/viewport_d3d12.ppm`, `golden/viewport_vulkan.ppm`), every shader compiled at run time.
  A test on each backend also resizes the viewport and hides it across frames, so its targets are
  destroyed and recreated under the debug layers, and must end on that golden image. A third golden
  image shows the same scene from a camera 2 m up looking 10 degrees down in a wide 1280 x 448
  window (`golden/viewport_grazing_<backend>.ppm`), where most of the grid is seen at a grazing
  angle and must fade out instead of aliasing. The helpers of
  the golden tests are in `GoldenImage.h`.
- `imgui_confined` and `imgui_confined_refuses_imgui_outside_editor_ui` (label `lint`).

## Not here yet

- The Content Browser and the main toolbar (later P2 steps); asset pickers in Details.
- In the viewport: picking, the translate gizmo and the selection outline (ADR-0042 rules 21 to
  26); the active viewport for Add Shape is always `ith.viewport.1`.
- A shader cache for the UI and viewport shaders (with the shader cache of ADR-0011 rule 8).
- File dialogs, and a Quit command with the Save / Don't Save / Cancel dialog.
- Right-to-left and complex scripts, which ImGui can't shape (P4 brings shaped text).
