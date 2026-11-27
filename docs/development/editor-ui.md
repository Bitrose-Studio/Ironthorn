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
- The clipboard, the IME's place, the cursor shape, relative mouse mode and the open-file dialog go
  through a `UiHost`. `WindowUiHost` implements it over `ith::Platform` (`clipboardText`,
  `setClipboardText`, `setCursor`, `setCursorVisible`, `showOpenFileDialog`) and `ith::Window`
  (`setTextInputArea`, `setRelativeMouseMode`); failures are dropped, as on the offscreen driver,
  which has no system cursors, except the dialog's, which goes back to the caller. Tests implement
  `UiHost` to record the calls.
- Events that `Input` doesn't route, the operating system's drops (`DropBegin`, `DropPosition`,
  `DropFile`, `DropComplete`) and the open-file dialog's results (`FileDialogFile`,
  `FileDialogClosed`), reach the UI through `EditorUi::handleEvent`, which the application calls
  for every pumped event of its window. It turns the positions into UI coordinates and hands them to
  the Content Browser panel (`EditorUiDesc::contentBrowser`); without one, they are ignored
  ([ADR-0049](../decisions/0049-editor-content-browser-v1.md) rule 26).
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

- **key presses only while a text field is active or an ImGui popup is open** (a menu, a context
  menu, a dialog), so Escape and Enter in a menu or a dialog never also run a shortcut, and W in a
  text field types W instead of reaching the keymap. While a text field is active, the UI's consumer
  is also the window's focused consumer with `acceptsText`, so `Input::textInputWanted()` turns on
  and the application starts the platform's text input; it gives the focus back when the field
  closes. ImGui reports an active field at the end of the frame that activates it, so the focus
  follows at once;
- **mouse presses, motion and the wheel while ImGui wants the mouse** (the pointer is over an ImGui
  window, or a drag started in one). Over the empty dock space the events go on to the consumers
  below. So do the mouse events over a hovered viewport image, by the rectangles and hover the
  panels reported last frame ([Viewport](#viewport)), while no ImGui popup is open: ImGui sees them
  but leaves them to the viewport consumer (`EditorUiDesc::viewports`, ADR-0042 rule 9).

A mouse press is also handed to the Content Browser's model (`EditorUiDesc::content`), which
hit-tests it against the browser's rectangles of the last frame and sets its keyboard-focus flag at
once, so a click into the browser followed by Delete in the same batch of events reaches the
Content context, not the level (ADR-0049 rule 4).

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

Activity (events, a change of the document's counters, a finished job, a viewport pick waiting for
its read-back (`ViewportPanel::picking()`), later Play In Editor and
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
- the **status bar**: the **Content Drawer** button, which runs `ith.content.toggle_drawer` on the
  press (a release would first close the open drawer as a click outside it), with the keymap's chord
  in its tooltip; the level's file name or "Untitled", with " *" for unsaved changes; and the last
  menu command or its error.

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
ith::ContentBrowserPanel content(session, &host); // made before the EditorUi: .contentBrowser = &content
ith::registerContentBrowser(session, content);  // "ith.content_browser", and the Enter prompt
// every frame, after drawEditorShell: content.drawDrawer();
```

A change a panel can't make (a refused command) goes to the log, in the category `Outliner`,
`Details`, `Viewport` or `ContentBrowser`.

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

Esc during a drag reaches the keymap, because the UI takes keys only while a text field is active or a popup is open.
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
rules 8 and 13 to 26 decide. It is the panel `ith.viewport`, instances `ith.viewport.1` to
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
(*viewport)->addRenderPasses(graph, frame); // the scene passes, before the UI's
ui.addRenderPasses(graph, target, frame.slot, ith::rhi::LoadOp::Clear, clearColor);
```

Destroy the panel before the `EditorUi`: its targets are user textures of the UI. Its destructor,
`newFrame()` and `draw()` register and unregister targets, so they run outside the UI's
graph-import phase; when the last frame wasn't rendered (a minimized window, a failed frame),
`EditorUi::abandonFrame()` closes that phase first. After a graph with the panel's passes fails to
execute, `invalidate()` makes every instance render again and drops the picks waiting for that
graph's copies. The shell must not draw the panel after
it is destroyed.

**Targets.** Each open instance owns a colour target, `R8G8B8A8Srgb` with `RenderTarget | Sampled`,
the size of its image in pixels (the image's size in window coordinates times the framebuffer
scale; each side at most the device's `maxTextureDimension2D`, with one warning when the image
is larger). It is registered as a [user texture](#user-textures) and shown with an image under an
invisible button. When the size changes, the panel unregisters the old target, destroys it (the
device defers that until the GPU is done) and creates a new one, during its draw, never in the
import phase. A target that can't be created is logged once and tried again only at another size.
An image without area has no target and adds no passes. Next to the colour target the instance
keeps its id target, `R32G32Uint` with `RenderTarget | CopySrc`, the same size, created and
released with it (ADR-0042 rule 21). It is kept, not a transient, because a click reads it in a
later frame than the one that drew it, and it must match the picture on screen. The depth target,
`D32Float` cleared to 0, is a render graph transient of the same size. An instance not drawn in a
frame (a hidden tab, a closed panel) releases its targets at the start of the next frame, in
`ViewportPanel::newFrame()`, outside the import phase; shown again, it gets new targets and
renders once.

**Passes.** `addRenderPasses()` adds two raster passes per instance that must render, on the
colour target's one import from `EditorUi::importUserTexture()`, so the `EditorUi` pass that shows
the image reads after them:

- `ViewportShapes` clears colour (the theme's `viewportBackground`), the id target (to 0) and
  depth, then draws every enabled entity with a `ShapeRenderer` and a `WorldTransform`, one draw
  each. The four unit meshes are in one upload buffer, written once at creation, the indices already
  offset to each shape's first vertex. Each draw's push constants (128 bytes) carry the clip matrix,
  the local-to-view 3x3 for the normals, the colour, and the entity's id texel in two words the
  3x3's rows leave free. The pixel shader writes the colour and, to the id target, the entity's slot
  index + 1 and generation (`pickTexel()`, editor core's `ViewportPick.h`); 0 means no entity.
  Shading is unlit with a fixed light from the camera, a little
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

**Click picking** (ADR-0042 rules 21 and 22). The viewport consumer turns a click on the image
into the instance's `ViewportState::click` ([editor core](editor-core.md#viewports)). Then:

1. The next `addRenderPasses()` takes the click. The position, in window pixels from the image's
   corner, is scaled by the target's size over the image's, so it maps right at any framebuffer
   scale and with a clamped target. A `ViewportPick` copy pass, after the scene passes, copies that
   one texel of the id target into the instance's 8 bytes of a small Readback buffer (one
   512-byte slot per instance, rows 256-byte aligned: `rhi::kTextureCopyOffsetAlignment`,
   `kTextureCopyRowAlignment`).
2. The pick waits in the instance with the click's mode and the number of the device frame that
   copied it. At most one waits per instance: a newer click replaces it, and only the newest copy
   is read, after its own frame.
3. The first `addRenderPasses()` whose device frame is at least `kFramesInFlight` frames after the
   copy's reads the texel. `addRenderPasses(graph, frame)` takes the open `rhi::Frame` that
   `beginFrame()` returned, so the read and the stamp use that frame's number and a call outside a
   frame can't read early. That frame's `beginFrame()` has already waited for the copy's frame, so
   the panel never waits on the GPU. It applies the texel with `applyPick()` in the render phase,
   before the scene passes: the same frame draws the new tint, and the Outliner and Details, drawn
   earlier in the frame, show the selection a frame later. The selection changes exactly
   `kFramesInFlight` frames (two) after the click. While a pick waits, `picking()` is true and the
   editor requests a frame from its idle policy after every frame, so an idle editor still
   completes the pick; UI frames without a device frame (a minimized window, a skipped render)
   don't count, and a minimized window doesn't spin the loop.

A click on an instance without a rendered target is dropped. A pick already copied completes
after its viewport is hidden: it reads the buffer, not the target. Clicks are only an LMB press
and release on the image without Alt within 4 logical points (editor core); RMB, MMB and the
Alt gestures never pick, and losing focus with LMB held never picks. A pick read while a transaction is open
(a gizmo drag began after the click) is dropped: the drag wins, and the selection would be
refused. A refused selection is logged in the category `Viewport`.

**Translate gizmo** (ADR-0042 rules 24 to 26). When the instance shows a gizmo
(`viewportGizmo()`, [editor core](editor-core.md#translate-gizmo): the Translate tool, a
selection, not game view), `draw()` draws it over the image at the primary's pivot. It uses the
window's draw list, after the image, clipped to it. So the gizmo is always on top of the scene and
needs no scene render, while a floating window or a popup over the image (the snap-step list, for
one) still covers it, as it covers the image's clicks. The geometry is editor core's layout in image
pixels at the UI's display scale:

- three arrows of 100 points, in the theme's `gizmoAxisX`, `gizmoAxisY` and `gizmoAxisZ`;
- three plane squares, translucent, each in the colour of the axis normal to it;
- the handle a drag holds (`ViewportState::gizmoDrag`), or else the one under the pointer while
  the image is hovered with a valid mouse position, in `gizmoHovered`, hit-tested exactly as the
  viewport consumer hit-tests a press. While the consumer holds another drag (orbit, pan, fly) or a
  dead gizmo drag after Esc, the platform backend hides the mouse from ImGui, so nothing is
  highlighted; the theme colours come from `srgbColor()`, shared with `applyTheme`.

Handles fade as they turn end-on or edge-on and vanish when they can't be grabbed. The viewport
consumer runs the drag itself; a preview changes the document, so the scene re-renders by the
redraw rule. `drawnGizmo(instance)` tells tests whether a gizmo was drawn and what it highlighted.

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

### Content Browser

`ith::ContentBrowserPanel` draws the `ContentBrowserModel` of
[editor core](editor-core.md#content-browser) twice over: as the `ith.content_browser` panel
(Content Browser, docked at the bottom, one instance) and as the **content drawer**
([ADR-0049](../decisions/0049-editor-content-browser-v1.md) rules 4, 7 and 26). Both show the same
state; the drawer is never in a layout.

- **The browser.** A toolbar in the order of spec 3.1 (Import, Add with New Folder, the search box,
  the type filter menu with Problems, grid or list, the sort menu, Rescan), the folder tree and the
  asset view side by side with a splitter, the path bar (`Content › Meshes › Rocks`, a click on a
  segment goes there) and a status line (`<n> items`, `<m> selected`, `<p> problems`, which shows
  the Problems filter, and the last notice: a file operation's result, why one was refused, or
  "Importing <n> files…"). An empty folder says "This folder is empty. Drop files here or use
  Import."
  The tree and the asset view draw only their visible rows through list clippers; grid tiles show
  the type icon (chosen by category; folders and problem entries have their own), the label on two
  lines at most ending in "…", and a warning icon on items with problems; the tooltip names the
  type, the path, the folder while searching and each problem. The list sorts by a click on its
  Name or Type header (the Folder column doesn't sort), through `ith.content.set_view`; the table
  is made per sort, so its headers always show the model's sort. Names are drawn whole, also with
  a `##` in them.
- **Input.** Clicks issue `ith.content.select` (Ctrl toggles, Shift selects the range from the
  anchor, Ctrl + Shift adds it, a click on empty space clears, a right click selects an unselected
  item alone; a plain click on one item of a larger selection selects it alone on the release
  without a drag, so dragging it moves the whole selection); a double-click opens (a folder becomes
  current, a level opens). The arrow keys, Home and End move the focus item and select (Shift: the
  range; Left and Right in the grid only, Up and Down by a row), and Ctrl + F goes to the search
  box: widget keys of the asset view, not keymap bindings. Changes asked for while the rows are
  drawn run once the browser is drawn, so no command rebuilds what is being iterated. A refused
  command is logged and shown in the status line.
- **File operations** ([spec](../specs/content-browser.md) 3.7 to 3.13). Everything goes through
  the `ith.content.*` commands; the panel draws the model's pending prompt, so F2, Delete and
  Ctrl + D from the keymap, and scripts, show the same field and dialogs:
  - the **inline rename field** over the item's label in the grid or the list, or on its tree row
    when the asset view doesn't show it: the editable part with the extension after it, checked
    with `checkContentName` as the user types (the reason under the field). Enter runs
    `ith.content.rename` with the name; an invalid name, or one the command refuses (a taken name),
    keeps the field open with the reason. Leaving the field commits a valid name and dismisses an
    invalid one; Escape runs `ith.content.dismiss`. New Folder (Add, or a context menu) starts it on
    the new folder;
  - the **delete confirmation** ("Delete <n> items?"): the first 20 items with the assets inside
    folders and the files the browser doesn't show, then "and <k> more"; each used asset with its
    users, naming the open level and when only its unsaved changes use it; the files that couldn't
    be read; "This can't be undone." Delete runs `ith.content.delete` with the previewed items and
    `confirmed`; Cancel has the keyboard focus, so Enter cancels, and Escape cancels too;
  - the **import conflicts**, one file at a time ("File <k> of <n>"): Replace (greyed unless the
    conflict allows it), Keep Both or Skip, with "Apply to all remaining"; the last answer runs
    `ith.content.import` again with the answers in `choices`, keyed by each conflict's `file` as the
    command reported it; Cancel and Escape dismiss and import nothing;
  - **context menus**: on items Rename, Duplicate, Delete, Reimport (greyed unless the selection
    holds an imported asset) and a problem entry's fix (Import for "Not imported", Assign New Id for
    a duplicate id); on a tree folder New Folder, Rename and Delete; on the asset view's empty space
    New Folder and Import…;
  - **drag and drop** between the browser's own widgets: items and tree folders onto folder items,
    tree folders and path bar segments run `ith.content.move`. A target that can't take the drag
    (a folder into itself or below it; a drag holding a problem entry or the Content folder) isn't
    highlighted, the cursor shows "not allowed" and the drag's tooltip says why.
- **Import.** The Import button (and Import… in the empty-space menu) shows the platform's open-file
  dialog through `UiHost::showOpenFileDialog`: several files, the filter "Importable files" with the
  importers' extensions (`ImporterRegistry::extensions()`) and `ithasset`, `ithmap`, each also in
  upper case for zenity, then "All files"; it starts in the folder the last chosen files came from.
  The files go to the folder that was current when Import was pressed. When the dialog can't be
  shown (no host, one already open, `platform.dialog_busy`, or a `FileDialogClosed` with an error)
  the error is logged and the status line says to drop files onto the browser instead. **Drops from
  the operating system** arrive through `EditorUi::handleEvent`: while a drag hovers the window, the
  folder item, tree folder or path bar segment under it is highlighted, else the whole panel or
  drawer; the drop imports into that folder, else the current folder, by the rectangles of the last
  frame; a drop anywhere else is ignored. Every import is queued: the next frame draws "Importing
  <n> files…", and the frame after runs `ith.content.import` before drawing (ADR-0049 rule 11); the
  editor asks for frames while one is queued.
  - **Refused, never lost.** Files that arrive while a question of the browser is open (the delete
    or conflict dialog, the unsaved-changes question, a pending rename), or while another import is
    queued, are not imported: the status line says why and the log has a warning. Import is
    refused the same way while a question is open, and a drag over the browser isn't highlighted
    then.
  - **Drops without a position.** When Windows can't start its drag and drop service (OLE), SDL
    reports dropped files at 0, 0 with no `DropPosition`. Such a drop goes into the current folder
    when the browser was drawn in the last frame; otherwise the status line asks to open the
    browser and drop again, and the log says why.
  - **The drag cursor.** While files from the operating system hover the window, its cursor always
    shows "copy", also outside the browser where a drop does nothing: SDL answers every drag with
    copy.
  - **As built:** ADR-0049 rule 26 has `EditorUi` hit-test drops; `EditorUi::handleEvent` only
    turns the positions into UI coordinates, and the panel, which knows where its folders were
    drawn, hit-tests them.
- **Unsaved changes.** Opening a level (double-click, or Enter as the prompt of `ith.content.open`)
  while the open one is dirty asks Save / Don't Save / Cancel first, then runs the command with
  `unsaved`; Escape cancels.
- **The drawer.** `drawDrawer()`, called every frame after the shell, draws it while it is open:
  over the bottom of the main window, the dock space's width, above the docked panels and the status
  bar, not dockable and without saved settings. It starts at 40 % of the window, its top edge
  resizes it (150 pixels at display scale 1 to 80 % of the window, kept as session state when the
  drag ends), and it takes the keyboard focus when it opens. A click outside it closes it unless one
  of its popups or dialogs is open; Escape with nothing selected closes it (`ith.content.dismiss`),
  and so does opening a level from it. `drawDrawer()` also draws the delete and conflict dialogs and
  the drop highlight, and runs a queued import, also while the drawer is closed.
- **Focus.** `drawDrawer()` also reports whether the panel or the drawer (child windows included)
  had the keyboard focus this frame (`ContentBrowserModel::setKeyboardFocus`), which the keymap
  reads to choose the Content context, and where each was drawn (`setScreenRects`). Between frames
  a mouse press, which the platform backend hands to the model (`EditorUiDesc::content`), and
  `ith.content.toggle_drawer` update the flag at once. A text field and an open popup keep every
  key, so the shortcuts never fire while the search box is edited or a menu is open. When the panel
  is in the layout the drawer closes: one browser on screen at a time.
- **Ctrl + Space and input methods.** Some input methods take Ctrl + Space for themselves (fcitx5 and
  IBus on Linux, the Chinese IMEs on Windows), so the editor never sees it. Rebind
  `ith.content.toggle_drawer` in the user's `Keymap.json` then.
- **Lifetime.** The panel removes its Enter prompt when it is destroyed; the panel registry has no
  removal, so the session's panels must not be drawn after the panel is gone (the editor draws them
  only inside its frame loop).

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

`tests/editor_ui/ContentBrowserPanelTests.cpp` draws the Content Browser in a fixed window over a
temporary project with an asset database, with the engine keymap's consumer under the UI: its
registration and the bare panel without a project; clicks with Ctrl, Shift and both, empty space,
and the keyboard focus following clicks; the arrow keys, Home and End in a four-column grid and in
the list, with Ctrl + A and Escape through the Content context; typing in the search box keeping
Ctrl + A from the shortcuts, and Ctrl + F; double-clicks into a folder and onto a level, and the
unsaved-changes question from a double-click and from Enter; the drawer's size and focus on open,
Escape, clicks inside and outside, staying open under its question, closing when a level opens
from it, its height clamped and the layout unchanged; and 2 000 items drawn by their visible tiles
and rows only, with idle frames rebuilding nothing; a click on a tile and Delete in one batch of
events sparing the level; Escape closing the filter menu or the unsaved-changes question without
running a shortcut; the list sorting by its Name and Type headers with a `##` in a name; and the
drawer closing when the panel is opened. For the file operations: the empty-folder text; the inline
rename from F2 (Enter renaming with the id kept, an invalid and a taken name keeping the field open,
Escape cancelling), from Add > New Folder (leaving the field commits, Escape keeps the default) and
on a tree folder's row; the delete dialog's text (users, the open level and its unsaved changes,
unreadable files, "and 2 more" past 20 items), Enter on the focused Cancel, Escape, Delete, and the
drawer staying open behind it; Import through the recording host (its filters, `allowMany`, the
start folder of the next dialog), the import queued a frame with its notice, the conflicts asked one
file at a time with Replace greyed for native assets, Skip, then Apply to all with Keep Both, and
Escape importing nothing; a dialog refused at once or failing later saying to drop files, a
cancelled dialog, results without a dialog ignored, and a panel without a host; operating-system
drops onto a folder tile (with its highlight), empty space, a tree folder, a path bar segment and
the drawer, and drops outside, from another window or without files ignored; dragging a selection
onto a folder and a tree folder refused below itself (the "not allowed" cursor and the reason in the
tooltip); the context menus (Reimport greyed for native assets, Duplicate, New Folder, Import…);
and the status bar's Content Drawer button; files refused while the delete dialog is open (a drop,
Import, a dialog result) with the dialog staying open, a dialog result and a drop in one batch
(the second refused with its notice), drops without a position (Windows without OLE) with and
without a browser on screen, a drop at display scale 2, and a dialog file with an unusable name
(POSIX) not becoming the next start folder. What a frame drew is read from ImGui's log buffer
(`drawnText` in `EditorUiTestSupport.h`, which depends on ImGui internals of the pinned version).

## The editor application

```
ironthorn_editor --project <dir> [--user-dir <dir>] [--automation <script>] [--frames N]
                 [--backend d3d12|vulkan|null] [--adapter hardware|software]
```

It creates the platform window, the job system (whose main-thread wake ends the idle wait), the
RHI device and the window's swapchain, the configuration layers (engine `<exe>/Config` and its
platform folder, project `<project>/Config` and its platform folder, and the user layer:
`--user-dir`, else `Ironthorn/Editor` in the user's configuration folder), an `EditorSession`
of the project with its asset database (the glTF importer registered; `Content/` scanned with
`ith.content.rescan` once the panels exist), the keymap, the UI and the Viewport, Outliner,
Details, Content Browser and Output Log panels and the content drawer (the
UI and viewport shaders compiled from Slang for the device at start-up), then runs the
loop of ADR-0035 rule 22: events (waiting while idle), main-thread jobs, input routing, the UI
frame, rendering; a queued import of the Content Browser also asks for frames. The drops and the
open-file dialog's results of its window go to `EditorUi::handleEvent`. Where the window has no
native handle (SDL's `offscreen` driver) it draws into an offscreen texture; where no GPU device
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
  teardown after an unrendered frame, the toolbar, Add Shape from the Outliner's context menu,
  drawn by the next frame, and the id target and picking: its format, usage and size following the
  colour target, a click copied in the next frame and applied exactly `kFramesInFlight` frames
  later with the tint redrawn in that frame, Ctrl and Shift on empty space keeping the selection, a
  newer click replacing one being read, a copied pick completing after its viewport is hidden, and
  clicks on hidden viewports, after `invalidate()`, with RMB or as a drag dropped, and UI frames
  without a device frame not counting toward a pick; and the translate gizmo: drawn with the
  translate tool and a selection and not with the rotate or scale tool, in game view or without a
  selection, the hovered arrow highlighted without a scene render, no highlight under a floating
  window, and a drag through the UI highlighted while held, re-rendering the scene, as one
  "Translate" undo entry; no highlight during an orbit over a handle or after Esc while the dead
  drag is held, and a pick copied before a drag began dropped without changing the selection).
- `editor_automation_null` and `editor_automation_gpu` (label `sample`, the second also `gpu`):
  `ironthorn_editor` on SDL's offscreen driver runs `tests/editor/open_edit_save_reopen.json` in a
  fresh project - create, rename, attach, undo, redo, save, new level, open, query - on the null
  device and on the software GPU (WARP; lavapipe on Linux, else the null device), and exits 0; the
  `_layouts`, `_viewport` and `_content` runs then play `layouts.json`, `viewport.json` and
  `content_browser.json` of `tests/editor_core/scripts` in the same project.
  Never on a desktop. Not in Release, where the editor isn't built.
- `editor_ui_gpu` (label `gpu`, built with the shader compiler): a golden screenshot of the
  shell in the Default layout (menu bar; the Outliner above Details on the right and the Output Log
  under the empty central area, all of an untitled level; the status bar with its Content Drawer
  button), 1280 x 720 at display scale 1, with a fixed time step and no caret blink, on WARP
  (`golden/shell_d3d12.ppm`) and on lavapipe (`golden/shell_vulkan.ppm`), the UI shader compiled
  at run time. A pixel differs when a channel differs by more than 8 of 255, and the shot fails
  when more than 0.5% of the pixels differ; the rendered image is then written next to the test
  executable (`shell_<backend>.actual.ppm`). The
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
  angle and must fade out instead of aliasing. A picking test on each backend renders the default
  scene at framebuffer scale 2 and clicks the cube, Shift + clicks the sphere, Ctrl + clicks the
  cube and clicks empty space: the selection must be the cube, both, the sphere, then nothing. The
  id target is never shown, so it changes no golden image. A fourth golden image shows the default
  scene with the cube selected and the translate gizmo over it
  (`golden/viewport_gizmo_<backend>.ppm`). The helpers of
  the golden tests are in `GoldenImage.h`.
- `imgui_confined` and `imgui_confined_refuses_imgui_outside_editor_ui` (label `lint`).

## Not here yet

- In the Content Browser, with the keyboard focus in the folder tree, F2 and Delete act on the
  content selection instead of the current folder (spec 3.8 and 3.11); the tree folder's context
  menu renames and deletes it.
- The main toolbar (a later P2 step); asset pickers in Details.
- In the viewport: the rotate and scale gizmos and the gizmo's centre handle, the selection
  outline, and marquee selection; the active viewport for Add Shape is always `ith.viewport.1`.
- A shader cache for the UI and viewport shaders (with the shader cache of ADR-0011 rule 8).
- Save and folder dialogs, and a Quit command with the Save / Don't Save / Cancel dialog.
- Right-to-left and complex scripts, which ImGui can't shape (P4 brings shaped text).
