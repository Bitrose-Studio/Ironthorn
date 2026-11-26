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
input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
ith::WindowUiHost host(platform, window);
auto ui = ith::EditorUi::create({.device = &device, .targetFormat = format,
                                 .vertexShader = vertex, .pixelShader = pixel,
                                 .fontDirectory = fonts, .input = &input,
                                 .window = window.id(), .host = &host,
                                 .displayScale = window.displayScale()});
ith::EditorShellState shell;

// every frame, after the events and main-thread jobs (ADR-0035 rule 22):
input.processEvents(events, platform.pumpTimeNs());
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
- The clipboard, the IME's place and the cursor shape go through a `UiHost`. `WindowUiHost`
  implements it over `ith::Platform` (`clipboardText`, `setClipboardText`, `setCursor`,
  `setCursorVisible`) and `ith::Window::setTextInputArea`; failures are dropped, as on the
  offscreen driver, which has no system cursors. Tests implement `UiHost` to record the calls.

### Input routing

The UI is an input consumer in the `Editor` layer of its window's stack. `EditorUi::create` adds
it, so it sits above the consumers added before it: the application adds the keymap consumer first
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 7). It hands every event that reaches it
to ImGui, and consumes:

- **key presses only while a text field is active**, so W in a text field types W instead of
  reaching the keymap. The UI's consumer is then also the window's focused consumer with
  `acceptsText`, so `Input::textInputWanted()` turns on and the application starts the platform's
  text input; it gives the focus back when the field closes. ImGui reports an active field at the
  end of the frame that activates it, so the focus follows at once;
- **mouse presses, motion and the wheel while ImGui wants the mouse** (the pointer is over an ImGui
  window, or a drag started in one). Over the empty dock space the events go on to the consumers
  below. The viewport panel's content rectangle will pass the mouse on too (viewport step).

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

`newFrame`, `endFrame`, `registerUserTexture`, `unregisterUserTexture`, `importUserTexture` and
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
```

A change a panel can't make (a refused command) goes to the log, in the category `Outliner` or
`Details`.

### Outliner

`OutlinerPanel` draws `OutlinerModel`:

- a search box;
- the entities as a tree with a list clipper, so only the visible rows are drawn;
- an Enabled check box per row.

Disabled entities, those below a disabled one, and the ancestors a search shows only for their
matches are dimmed.

| Gesture | What it does |
|---|---|
| Click, Ctrl + click, Shift + click | Select, toggle, select the range. A selected row selects on release, so dragging it keeps the selection |
| Double click, or F2 | Rename in place: Enter or leaving the field keeps the name, Esc drops it |
| Drag a row | Drop before or after another row (its top or bottom quarter), inside it (the middle), or on the empty space below the rows (the end of the roots); a line or frame shows where |
| Click the empty space | Select None |

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

**Multi-select.** Values that differ between the selected entities show "Multiple Values"; an edit
writes the same value to all of them.

## Tests of the panels

`tests/editor_ui/PanelTests.cpp` draws the Outliner and Details in fixed windows on the null
device. It drives them with mouse and key events through the platform backend and the engine
keymap's consumer:

- a click selects, and a click on the empty space clears the selection;
- F2 renames in place, Enter keeps the name, and Esc drops it without reaching Select None;
- dragging a row onto another reparents it in one undo entry;
- a translation drag previews live and becomes one undo entry, and Esc cancels a drag;
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
of the project, the keymap, the UI and the Outliner, Details and Output Log panels, then runs the
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
  letters, focus loss, mouse capture), the clipboard, the IME area and the cursor through a
  recording host; the renderer on the null device (atlas creation and partial updates, a growing
  atlas destroying its old texture, upload buffer growth, scissor clamping, recorded commands, user
  textures and their shared scene/UI graph imports, stale-id handling, released GPU objects); the
  theme and fonts; the shell's menus, chords and status; the icons
  against `codepoints.json`; the shader source; the idle policy.
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
  readback pixel; they need no golden file.
- `imgui_confined` and `imgui_confined_refuses_imgui_outside_editor_ui` (label `lint`).

## Not here yet

- The Content Browser, toolbar and viewport (later P2 steps); asset pickers in Details.
- A shader cache for the UI shader (with the shader cache of ADR-0011 rule 8).
- File dialogs, and a Quit command with the Save / Don't Save / Cancel dialog.
- Right-to-left and complex scripts, which ImGui can't shape (P4 brings shaped text).
