# ADR-0029: Editor UI toolkit

- **Status:** Accepted
- **Amended by:** [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md)
- **Date:** 2026-11-22
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions or RTTI,
  `ITH_ASSERT`), [ADR-0003](0003-clean-room.md), [ADR-0004](0004-modules-and-layering.md)
  (layers, no third-party types in public headers),
  [ADR-0006](0006-math-and-coordinate-conventions.md) (linear colour),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (layout, names, shortcuts),
  [ADR-0010](0010-serialization.md) and [ADR-0020](0020-serialization-rules-completed.md)
  (documents, versions), [ADR-0011](0011-shader-language-and-pipeline.md) and
  [ADR-0023](0023-build-time-shaders-for-samples-and-tests.md) (shaders),
  [ADR-0012](0012-windowing-and-event-loop.md) (SDL private to `platform`),
  [ADR-0013](0013-input.md) and [ADR-0027](0027-input-rules-completed.md) (consumer stack),
  [ADR-0017](0017-runtime-and-editor-separation.md) (module kinds, Play In Editor without
  globals), [ADR-0009](0009-rhi.md) and [ADR-0021](0021-rhi-device-structure-and-backend-modules.md)
  (RHI), [ADR-0028](0028-render-graph-v1.md) (render graph),
  [third-party libraries](../development/third-party.md)

## Context

P2 builds the editor shell: docking panels, a main menu and toolbar, the Outliner, Details,
Content Browser, Output Log and viewport, with every action a command that tests can script
(ADR-0008, [architecture: Editor](../architecture.md#editor)). The engine has no UI and no text
rendering yet. The game UI framework (retained widgets, view models, shaped SDF text) is P4 and
size L; its requirements are not known in detail before then.

**Unreal prior art** (Epic's public documentation only,
[Slate overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/slate-overview-for-unreal-engine),
[Slate architecture](https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-the-slate-ui-architecture-in-unreal-engine)).
The editor is built on Slate, a retained C++ toolkit with its own layout, styles and docking.
UMG came later as the game-facing layer over Slate, and Epic points games to UMG. The editor's
widgets never became game UI; what both share is the low-level core (drawing, text, input
routing). Lesson: share a core, not widgets, and don't freeze that core around an editor before
the game's needs are known. The editor also uses less CPU in the background (an Editor
Preferences option): an always-redrawing UI needs an idle policy.

**Why immediate mode now.** Dear ImGui gives docking, tables, text input, a dynamic font atlas
and styles today, without exceptions, RTTI or warnings under our flags (dependency audit,
2026-11-22). Panels are functions of the editor's state, with no widget tree to keep in sync.
**What it costs:**
- it rebuilds and redraws the UI every frame it runs, so the editor needs an idle policy
  (rule 12);
- it has no text shaping or bidirectional text, so right-to-left and complex scripts (Arabic,
  Hebrew, Devanagari, Thai) render wrong, for example in asset names. Accepted for P2; shaped
  text comes with the P4 UI;
- it keeps one global context and is not thread-safe;
- docking is built through `imgui_internal.h` (DockBuilder), which the library itself calls
  unstable.

The owner settled the toolkit, its boundary, fonts, icons, in-window floating panels, the keymap
as configuration, the native title bar and our own layouts on 2026-11-22. ADR-0008 decides
layout, panel names and shortcuts; the editor theme decides only the look, and ADR-0008 wins any
conflict. This ADR turns those decisions into testable rules.

## Options considered

**A. Our own retained UI now, shared with P4.** One UI stack and full control of text. It puts
the P4 framework (size L) in front of P2 (size L) and designs it around an editor before the game
UI requirements exist. Rejected for now; reconsidered after P4 (Consequences).

**B. Dear ImGui, docking branch, `v1.93.0-docking`** (MIT; chosen). Mature, no transitive
dependencies, widely used for engine editors; approved by the audit with the conditions in
rules 2–11.

**C. RmlUi 6.3** (MIT, retained, HTML/CSS-like). A game UI candidate, which is P4's question.
No docking, and markup panels would cost more than ImGui panels.

**D. Nuklear** (single-header C, MIT or public domain). Immediate mode without docking and with
fewer widgets. No advantage over B.

**E. Qt 6.** Docking, shaping and accessibility, but LGPL-3.0/GPL or a paid commercial licence,
which the permissive-only rule excludes, and its own event loop next to SDL. Rejected on licence.

**F. A widget wrapper over ImGui** (our own `Button`, `TreeView`). A speculative layer (KISS) that
a retained toolkit wouldn't fit anyway. Rejected in favour of a module boundary (rule 1).

## Decision

### Modules and boundaries

1. **Two editor modules**, both layer `editor`, kind `editor`:
   - **`editor_core`**: commands, transactions and undo, selection, the keymap consumer, the
     automation API and the panels' **view models** (plain data the panels read and the commands
     they call). It has **no ImGui include** and doesn't link ImGui.
   - **`editor_ui`**: ImGui, our platform and renderer backends, the theme, fonts, icons, docking
     and layouts, and the panels, which draw view models and issue commands. It depends on
     `editor_core`; never the other way.

   Replacing the toolkit means rewriting `editor_ui` only. Game UI never depends on either module.
2. **What may include ImGui, enforced by a lint test** (a CMake check run by CTest, like
   `module_layering`):
   - `imgui.h` only in `engine/ith/editor_ui/` sources and private headers; no ImGui type in a
     public header of any module;
   - `imgui_internal.h`, every DockBuilder call and every read of ImGui's dock nodes in **one
     file**, `editor_ui/DockLayout.cpp`;
   - only `editor_ui` links `ith::imgui` (checked against the module's declared libraries).
3. **One thread, one context.** Every ImGui call runs on the main thread, which pumps events
   (ADR-0012 rule 9). `editor_ui` creates and owns the context; it is never reachable from
   `editor_core`, runtime or game code, and Play In Editor worlds never touch it (ADR-0017
   rule 11). Render graph execute callables never call ImGui or read its draw data (rule 9).

### The ImGui configuration header

4. **`editor_ui/ImGuiConfig.h`** is the `IMGUI_USER_CONFIG`, a PUBLIC definition of `ith::imgui`,
   so every translation unit that includes `imgui.h` sees it. It sets:
   - `IM_ASSERT(x)` → `ITH_ASSERT(x)`;
   - `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`;
   - `IMGUI_USE_WCHAR32` (code points beyond the BMP in asset names);
   - `IMGUI_DISABLE_WIN32_FUNCTIONS` and `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS` (platform code
     stays in `platform`);
   - `IMGUI_DISABLE_DEFAULT_ALLOCATORS`, so forgetting the allocator hook fails to link.
5. **Allocator and settings at start-up.** `ImGui::SetAllocatorFunctions` is called before
   `CreateContext` with the engine's allocation functions. The engine has no general-purpose
   allocator yet, so today these forward to the C heap, as SDL, VMA and D3D12MA do (ADR-0012
   rule 13); the hook is the one place to change. `io.IniFilename` and `io.LogFilename` are
   `nullptr`: ImGui writes no file. `ImGuiConfigFlags_ViewportsEnable` stays off, so floating
   panels live inside the main window.

### Platform backend

6. **Our own backend, `editor_ui/ImGuiPlatform.cpp`**, fed from `platform_events` and `input`;
   `imgui_impl_sdl3` is not used (SDL is private to `platform`). The official backends serve as
   behavioural reference (MIT).
   - Each frame it sets the display size from `Window::size()`, the framebuffer scale from
     `Window::pixelSize()`, and the time step from `Input::frameTimeNs()`.
   - Keys, mouse, focus and `TextInput` events map onto ImGui's input events.
   - **Clipboard** through `Platform::clipboardText`/`setClipboardText`; **IME** through
     `Window::setTextInputArea`; **cursor shapes** through a cursor call that ADR-0012 rule 8
     promises but `platform` doesn't have yet (added with this backend). "Open in shell" stays
     unset in P2.
7. **Input routing with the consumer stack** (ADR-0013 rule 10, ADR-0027). In each editor window's
   stack, top first:
   1. Play In Editor keys (Esc, F8, Shift + F1), `Editor` layer;
   2. the **ImGui consumer**, `Editor` layer;
   3. the **keymap consumer** of `editor_core` (editor shortcuts as commands), `Editor` layer;
   4. viewport camera and gizmo consumers, then Play In Editor's game consumers (`Ui`, `Game`).

   The ImGui consumer sees every event that reaches it and consumes:
   - **keys** only while ImGui wants text input (a text field is active), so W in a text field
     types W and doesn't switch the gizmo. Then it is also the window's focused consumer with
     `acceptsText`, which turns the platform's text input on;
   - **mouse** presses, motion and wheel while ImGui wants the mouse, **except** over the content
     rectangle of a viewport panel with no ImGui popup on top: there the event goes on to the
     viewport and, in Play In Editor, to capture on click (ADR-0027 rule 3).

   The want-capture flags come from the previous ImGui frame, because routing runs before
   `NewFrame`; that one-frame lag is ImGui's normal contract. While the mouse is captured, ImGui
   gets no mouse events (ADR-0013 rule 11) and is told the mouse is gone. Editor commands are
   bound through the keymap consumer, never through ImGui's own shortcut functions.

   **Consistent with ADR-0013 rule 10.** That rule's top layer holds the editor's own input: the
   console, overlays and editor keys. The editor's UI is editor input, so all three consumers
   above live in the `Editor` layer, ordered within it by the order they are added (`input`
   puts the last added on top). The rule's guarantee holds: everything in the `Editor` layer sits
   above `Ui` and `Game`, so a game in Play In Editor never takes an editor key or an editor
   click. The `Ui` layer stays for game UI.

   **Play In Editor in a new window** (ADR-0017 rule 12) opens a plain game window: its own
   swapchain and consumer stack with the Play In Editor keys and the game's consumers, and no
   ImGui. It is not an ImGui viewport, so it doesn't conflict with multi-viewports being off
   (rule 5).
8. **Display scale.** On `WindowDisplayScaleChanged` the theme rebuilds style sizes from its
   tokens times `Window::displayScale()`; fonts follow without an atlas rebuild (ImGui's dynamic
   atlas).

### Renderer backend

9. **Built on the RHI, as render graph passes** (`editor_ui/ImGuiRenderer.cpp`); the official
   D3D12 and Vulkan backends are not used. It implements `RendererHasTextures` and
   `RendererHasVtxOffset`.
   - **Before the graph executes**, on the main thread, it copies vertices, indices and texture
     updates into a per-frame-in-flight **upload buffer** (`MemoryType::Upload`,
     `ShaderRead | Index | CopySrc`, grown when too small) and the draw commands into the graph's
     arena.
   - **Pass `EditorUiUpload`** (`Copy`): texture creates and updates, `CopyDst` on the imported
     ImGui textures.
   - **Pass `EditorUi`** (`Raster`): renders to the imported backbuffer (load), reads the upload
     buffer (`ShaderRead`, `Index`), the ImGui textures and any viewport texture a panel shows
     (`Sampled`). The scene passes of editor viewports write those textures earlier in the same
     graph, so barriers come from the graph.
   - One pipeline: vertices pulled by bindless index (the RHI has no vertex input),
     `BlendMode::Alpha`, no depth, no culling, a linear clamp sampler. Push constants (32 of 128
     bytes): scale and translate, buffer, texture and sampler indices, and the vertex offset,
     added in the shader so no API's vertex-id rule matters. Clip rectangles become `setScissor`.
   - `ImTextureID` is an index into the backend's per-frame texture table, resolved to a sampled
     index through `PassContext` in the execute callable. Draw callbacks other than
     `ImDrawCallback_ResetRenderState` are not supported (asserted).
   - The shader (`editor_ui/shaders/EditorUi.slang`) is compiled by the editor application
     through `shader_compiler` at start-up, with its cache (ADR-0011 rules 8 and 10), and handed
     to `editor_ui`; `editor_ui` itself doesn't link the compiler (rule 19).
10. **Texture requests** (`ImDrawData::Textures`):
    - `WantCreate`: `createTexture`, `R8G8B8A8Unorm` (the backend asks ImGui for RGBA32),
      `Sampled | CopyDst`, then a full upload;
    - `WantUpdates`: copies of the changed rectangles only;
    - `WantDestroy`: `destroy()` after ImGui's unused-frames count, deferred by the RHI as usual.

    **RHI gap.** `CommandList::copyBufferToTexture` copies one whole mip level; partial updates
    need a texel rectangle, so `rhi::BufferTextureCopy` gains an offset and extent (default: the
    whole level) on every backend, with tests. Everything else exists today: upload buffers with
    index and shader-read usage, bindless textures and samplers, `BlendMode::Alpha`,
    `setScissor`, `drawIndexed`, 128-byte push constants.
11. **Colour.** ImGui's vertex and style colours are sRGB-encoded 8-bit values; ADR-0006 rule 8
    keeps colour math linear. The shader **decodes vertex colours from sRGB to linear**, blending
    happens in linear space, and the sRGB swapchain encodes on write. Colour images (later
    thumbnails) use `Srgb` formats; the white font atlas is unaffected. Theme tokens are sRGB hex
    values, as designers give them. Anti-aliased text and translucent fills then blend slightly
    differently from stock ImGui (light text on dark looks a little heavier); the theme review
    checks it on real monitors, and if needed a gamma on coverage alpha, a theme setting, corrects
    it without changing the colour rule.

### Idle policy

12. **The editor redraws on demand.** A frame runs when events arrive, a view model changed, a job
    reports progress, Play In Editor or Simulate runs, or a panel animates; ImGui then gets two
    more frames to settle. Otherwise the editor waits for events with a timeout and runs at an
    **idle rate of 10 frames per second**, 2 when no editor window has focus, both Editor
    Preferences settings. This needs `Platform::waitEvents(timeout)` (SDL's wait with timeout),
    added to `platform`. Viewports re-render their scene only when their camera or world changed; that
    rule belongs to the viewport ADR. Target, report-only: the UI frame of the default layout
    (`NewFrame` to submitted draw data) under 1 ms on the Low tier's CPU.

### Theme, fonts and icons

13. **Theme.** `editor_ui/Theme.cpp` maps the editor theme's tokens (colours, spacing, radii, type
    sizes) onto `ImGuiStyle` and its colour table, plus a few custom-drawn details through
    `ImDrawList`. The editor theme spec comes later and sets the values; until then a neutral
    dark theme is used. On layout, panel names and keys the theme defers to ADR-0008.
14. **Fonts and icons**, editor only, with their audit's pins:
    - Inter 4.1 (Regular, SemiBold) and JetBrains Mono 2.304 (Regular), SIL **OFL-1.1**. OFL is
      allowed for fonts only; code stays under permissive licences. `third-party.md` records it.
    - The **Lucide 1.77.0** icon font (ISC, plus MIT for icons derived from Feather), merged into
      the UI font. Icons are referenced **by name** through `editor_ui/Icons.h`; a test checks every
      name against the pinned `codepoints.json`. Codepoints are never written in panel code.
    - The archives are **fetched pinned by SHA-256**, never committed (Lucide's licence file from
      the same tag, pinned too). The editor build copies the TTFs and their licence files into
      a `fonts/` folder next to the editor executable; editor distributions list them in their
      third-party notices.
    - Fonts are read through the engine's file API and given to ImGui from memory. The bundled
      stb_truetype rasterizes **only these pinned fonts**; user-supplied fonts need FreeType
      first (audit, deferred).

### Docking and layouts

15. **Layouts are our own documents**, never `imgui.ini`.
    - **Format:** JSON in the ADR-0010 envelope, `format` `ith.editor_layout`, `version` 1. `data`
      holds a dock tree of split nodes (`horizontal` or `vertical`, a ratio, two children) and tab
      nodes (panel ids, the active tab), plus floating panels (panel ids and a rectangle as
      fractions of the main window). Panels are named by **stable ids** (`ith.outliner`,
      `ith.details`, `ith.content_browser`, `ith.viewport.1`, `ith.output_log`); ImGui window ids
      use them (`"Outliner###ith.outliner"`), so renamed or translated titles keep their place.
      The toolbar and main menu are not panels.
    - **Where:** in the layered configuration (ADR-0008 rule 7): the engine layer ships the
      **Default** layout; a project may add layouts and a project default; a user's layouts and
      choice of default live in the user layer. Directories follow the configuration-layout ADR.
    - **Applying:** `DockLayout.cpp` rebuilds ImGui's dock tree from the document with DockBuilder
      and reads it back to save. Nothing else touches docking internals.
    - **Default layout** (ADR-0008 rule 2): toolbar on top; viewport in the centre; Outliner top
      right with Details below; Content Browser at the bottom with Output Log as a tab beside it.
      A test checks the shipped document's shape.
    - **Commands**, registered with `editor_core`'s command registry so automation calls them:
      **Save Layout As** (to the user layer), **Load Layout**, **Set as Default** (records the
      name in user configuration), **Reset Layout** (the user's default, else the project's, else
      the engine's), and **Open Panel** from the Window menu.
    - **Start-up:** the editor opens the last session's layout, which it saves to the user layer
      on exit, as Unreal's editor does; without one, the default chosen as for Reset Layout.
    - **Versioning:** a format change bumps `version` with a migration (ADR-0020). A panel id the
      editor doesn't know is dropped with one warning, and an emptied tab node gives its space to
      its sibling. A panel the layout doesn't mention starts closed; Open Panel docks it at the
      place its panel declares (centre, right, bottom or floating). A document that fails to load
      is skipped with a warning and the next default applies; the editor always starts.

### Keymap and window

16. **Keymap** as layered configuration, by stable control names (ADR-0013 rule 13), with the
    ADR-0008 table as the engine default preset. The rebinding screen comes later. **Title bar:**
    the native one; the main menu bar sits below it.

### Testing

17. **No Dear ImGui Test Engine** in the repository, its build or CI: its licence isn't
    permissive (a paid licence for larger companies after 45 days). A developer may use it locally
    outside the repository.
18. **Tests, cheapest first:**
    - **`editor_core`**: view models and commands without ImGui, including the layout commands
      and document round trips (save, set default, reset, unknown and missing panel ids).
    - **`editor_ui` headless**: panels drawn for a few frames through ImGui's null backend
      (`imgui_impl_null`), with no window and no GPU; the platform backend with injected events
      (routing rule 7, text, IME area, clipboard, focus loss); the renderer backend on the RHI
      null device (texture create, partial update and destroy; upload buffer growth; scissor
      clamping; recorded commands).
    - **Automation**: the editor application started by CTest with `--automation` on the SDL
      offscreen driver runs a script and exits 0, never on a desktop.
    - **Golden screenshots**, a few only: the Default layout on an empty level, and a menu and a
      modal open; 1280 × 720, display scale 1, fixed clock, caret blink off. Per backend, on WARP
      and lavapipe. A pixel differs when any channel differs by more than 8 of 255; a shot fails
      when more than 0.5 % of pixels differ. Golden images are uncompressed binary PPM files,
      stored with Git LFS and read by a small test helper, until the engine has a PNG decoder
      (the image importer); their folder and LFS pattern come with the first golden test.
19. **Sanitizers.** Today the sanitizer builds turn the editor off, because the shader compiler
    is off. The editor **application** stays off there, but `editor_core`, `editor_ui` and their
    headless tests (rule 18 without the GPU and automation parts) are built and run under
    ASan/UBSan and TSan. This works because `editor_ui` takes its shader as data (rule 9) and the
    null device accepts any bytecode. The root `CMakeLists.txt` gating changes accordingly.

## Consequences

- P2 starts without the P4 UI framework, and P4 is designed for games, not around an editor.
- Commands, undo, selection, automation and view models are testable without ImGui, and the lint
  test keeps it so. A later toolkit rewrites panel drawing only.
- New, small work outside `editor_ui`: a texel region in `rhi::BufferTextureCopy`;
  `Platform::waitEvents(timeout)` and cursor shapes in `platform`; the sanitizer gating in CMake;
  the OFL-for-fonts note in `third-party.md`.
- Layout storage and the keymap depend on the configuration-layout ADR; the layout rules here
  replace a separate docking and layouts ADR.
- ImGui upgrades happen at step boundaries; docking internals are one file to check.
- **Cost:** `editor_ui`'s backends, theme, fonts and docking about 2.5–3.5k lines with tests,
  size M.
- **Owner's answers at acceptance (2026-11-22):** the editor restores the last session's layout
  at start-up and Reset Layout goes to the chosen default (rule 15); UI colours blend in linear
  space (rule 11); idle rates of 10 and 2 frames per second as Editor Preferences settings
  (rule 12); golden images as uncompressed PPM in Git LFS until the engine has a PNG decoder
  (rule 18).
- **Revisit after P4**, when the game UI framework exists, if any of these holds:
  - it covers docking, tables, text editing and virtualized trees at the editor's density;
  - the editor needs shaped or right-to-left text (localized editor, asset names in those
    scripts);
  - the idle or UI-frame targets (rule 12) are missed and can't be fixed in panel code;
  - two ImGui upgrades in a row break `DockLayout.cpp`, or ImGui's licence or maintenance changes;
  - separate OS windows for panels (after P2) can't be built cleanly on our backends.
