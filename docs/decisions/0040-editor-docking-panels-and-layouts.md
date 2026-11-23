# ADR-0040: Editor docking, panels and layouts (amends 0029)

- **Status:** Accepted
- **Date:** 2026-11-23
- **Related:** amends [ADR-0029](0029-editor-ui-toolkit.md) rules 4, 15 and 18;
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) rule 2 (default layout, saved layouts,
  reset), [ADR-0010](0010-serialization.md) and [ADR-0020](0020-serialization-rules-completed.md)
  (envelope, versions, migrations), [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md)
  rules 12, 13, 17, 18 and 21 (layers, `Saved/Editor`, whole files, `Layouts/<name>.json`),
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rules 1–4, 17–20
  (commands, queries, scripts, every action scripted)

## Context

P2 step 9 makes the editor's panels dockable, with layouts a user can save, load, set as their
default and reset (ADR-0008 rule 2). ADR-0029 rule 15 fixed the outline: our own
`ith.editor_layout` documents instead of `imgui.ini`, stable panel ids, DockBuilder only in
`editor_ui/DockLayout.cpp`, engine, project and user layers, and the last session restored at
start-up. ADR-0035 placed the files. Still open:

- what a panel is, and how the panel list and the layout model are tested without ImGui;
- the exact document schema (`engine/config/Layouts/Default.json`'s field names are provisional);
- how the model reaches ImGui and how user drags come back, and when session state is written;
- the commands' arguments, the reset order and the engine default in detail;
- room for a nested dock space per asset-editor document (P3+): a top dock space of documents,
  and inside each a dock space with its own window class, so its panels can't dock elsewhere.

`editor_ui` today has the dock space host (`drawDockSpace`) and no panels. `Config` has whole-file
lookup (`findFile`, `listFiles`, `writeFile`) but no way to read a value as the layers below one
layer set it.

**Unreal prior art** (public documentation and observed behaviour). The level editor's Window
menu has a Layout section: **Load Layout** (with the shipped *Default Editor Layout*, and in UE5
also a classic layout), **Save Layout** > **Save Layout As…**, and **Remove Layout**
([Interface overview, 4.27](https://docs.unrealengine.com/4.27/Basics/UI/InterfaceOverview),
[Menu bar, 4.26](https://docs.unrealengine.com/4.26/BuildingWorlds/LevelEditor/MenuBar)). The
current layout is kept between sessions per project (in its `Saved` folder, per community answers;
not confirmed by Epic documentation). Tabs are dragged, docked and closed directly; none of that is
an undo step. The default level editor has the viewport in the centre, the Outliner at the top
right with Details below it
([Unreal Editor interface](https://dev.epicgames.com/documentation/unreal-engine/unreal-editor-interface));
observed: the Outliner and Details column runs the full height, and the content area sits below
the viewport only. Lesson: named layouts plus one shipped default, managed from the Window menu,
are enough; layout changes are not undoable.

**Dear ImGui 1.93 facts used here** (from the pinned source): `DockBuilderSplitNode(node, dir,
ratio, …)` keeps the node's content and central flag in the child opposite `dir`; dock nodes
expose `SplitAxis`, `ChildNodes`, `Size`, `TabBar`, `SelectedTabId` and `IsCentralNode()`;
docking, undocking and splitter drags call `MarkIniSettingsDirty`, which sets
`io.WantSaveIniSettings` after `io.IniSavingRate` seconds even when `io.IniFilename` is null (the
library calls one flag change of this path "a bit flaky"). With `IMGUI_DISABLE_DEFAULT_ALLOCATORS`
the built-in allocator wrappers still exist and contain `IM_ASSERT(0)`: a missing allocator hook
asserts at the first allocation and does not fail to link.

## Options considered

Layout model:
- **Store ImGui's settings text** (`SaveIniSettingsToMemory`) in our envelope. Pixel sizes and
  hashed ids, no headless tests, no way to drop an unknown panel or migrate, and tied to ImGui's
  version. Rejected.
- **Our own dock model in `editor_core`, applied with DockBuilder** (chosen). Testable headless;
  `DockLayout.cpp` stays the only file that knows ImGui's docking.
- **Our own docking instead of ImGui's.** Weeks of work ImGui already does. Rejected.

Split names in the document:
- **`horizontal` / `vertical`** (today's file). Read either as the axis of the children or of the
  divider; both readings exist in toolkits. Rejected as ambiguous.
- **`left_right` / `top_bottom`** (chosen): `first` is left or top. Nothing to misread.

When session state is written:
- **Only at exit.** A crash loses the session's arrangement. Rejected.
- **Read back every frame and write on any difference.** Writes during every splitter drag.
- **Read back when ImGui marks its settings dirty, after each layout command and at exit; write
  when the result differs from the file** (chosen). ImGui's flag debounces drags; the exit check
  catches what the flag misses.

Where Open Panel docks a closed panel:
- **Where it was last** (Unreal remembers closed tabs). Needs closed-panel records in the document.
- **At the place the panel declares** (chosen): next to a panel with the same declared place,
  else a new split at that side. Simple and headless; revisit if users ask for the old place.

## Decision

### Panels (`editor_core`, no ImGui)

1. **`PanelRegistry`**, owned by `EditorSession` (`session.panels()`), main thread only. A
   `PanelDesc` has:
   - `id`: stable, `ith.<name>` in lower snake case, never reused (`ith.outliner`);
   - `displayName` in ADR-0008 vocabulary (`Outliner`);
   - `defaultDock`: `Center`, `Left`, `Right`, `Bottom` or `Floating`;
   - `maxInstances`: 1 for a single panel; more for a multi-instance one;
   - `draw`: `std::function<void(std::string_view instanceId)>`, set by the module that brings the
     panel's view; null in headless tests. `editor_core` never calls it.

   Registering an id twice or with `maxInstances` 0 asserts.
2. **Instance ids.** A single panel's instance id is its `id`. A multi-instance panel's instances
   are `<id>.<n>`, n from 1 to `maxInstances` (`ith.viewport.1`), shown as `<displayName> <n>`
   (`Viewport 1`). ImGui window names are `"<shown name>###<instance id>"` (ADR-0029 rule 15).
3. **P2 panels:**

   | Id | Name | Default dock | Instances |
   |---|---|---|---|
   | `ith.viewport` | Viewport | Center | 4 |
   | `ith.outliner` | Outliner | Right | 1 |
   | `ith.details` | Details | Right | 1 |
   | `ith.content_browser` | Content Browser | Bottom | 1 |
   | `ith.output_log` | Output Log | Bottom | 1 |

   Each panel is registered by the step that builds its view; no panel is registered before it
   draws something real. Making a single panel multi-instance later renames `<id>` to `<id>.1`
   through a document migration (rule 10).

### The layout model (`editor_core`, no ImGui)

4. **`EditorLayout`** is a value type: a **main tree**, a list of **floating** groups, and the
   **editor sub-layouts** (rule 7). Nodes are a flat array with child indices (no owning pointers;
   copies and `==` are cheap). A node is either:
   - a **split**: `left_right` or `top_bottom`, `ratio` (the share of `first`, a float), `first`,
     `second`;
   - a **tab stack**: instance ids in tab order, the `active` one, and `central` (at most one tab
     stack of the main tree; it may be empty and stays when emptied, as ImGui's central node does).

   A floating group is `{rect, node}`: a node tree in a rectangle given as fractions of the dock
   space (`x`, `y`, `width`, `height`), inside the main window (ADR-0029 rule 5).
   Model operations, all headless: `parse`, `write`, `validate` (rule 9), `contains`,
   `openPanel` and `closePanel` (rule 13). Ratios are rounded to 4 decimals and rectangle fractions
   to 4 decimals when written and compared, so read-back noise doesn't count as a change.

### The document, `ith.editor_layout` version 1

5. **Schema** (confirms `Default.json`'s `root`, `first`, `second`, `ratio`, `tabs`, `active` and
   `floating`; changes the split values; adds `central`, `rect` and `editors`):

   ```json
   {
     "format": "ith.editor_layout",
     "version": 1,
     "data": {
       "root": <node>,
       "floating": [ { "rect": { "x": 0.3, "y": 0.25, "width": 0.4, "height": 0.5 }, "node": <node> } ],
       "editors": { }
     }
   }
   ```

   - split: `{ "split": "left_right" | "top_bottom", "ratio": 0.78, "first": <node>, "second": <node> }`
   - tab stack: `{ "tabs": ["ith.content_browser", "ith.output_log"], "active": "ith.content_browser" }`,
     plus `"central": true` on the central one. Writers always write `active`; a reader takes the
     first tab when it is missing.

   The layout's **name is its file name** (`Layouts/<name>.json`); the document has no name field.
   Comments are not written. A file is at most 1 MiB, at most 256 nodes and 32 levels deep.
6. **The parser is hand-written over `serialization::Value`** (a recursive tree), in
   `EditorLayout.cpp`, with ADR-0020's envelope and version checks.
7. **Room for per-document dock spaces.** `editors` maps an asset-editor kind id
   (`ith.material_editor`, P3+) to a sub-layout `{root, floating}` of the same grammar, whose
   panel ids belong to that editor kind. Every open document of a kind builds its nested dock space
   from the kind's sub-layout; the last one arranged is what is saved. P2 writes `editors` empty
   and keeps entries it can't validate unchanged on read and write, so P3 needs no version bump.
   Where documents themselves dock in the main tree is decided with P3's asset editors; open
   documents are session state, not layout. The nested dock spaces and their window classes are
   built in `DockLayout.cpp` like the main one.
8. **Versions.** A new format version comes with a migration (ADR-0020). A document with a newer
   version than the editor knows fails to load with a warning and is never read partly.
9. **Validation on load** (amends ADR-0029 rule 15's wording, same intent). Warnings are logged and
   also returned in the command result as `{code, message}`; the document still loads:
   - an unknown panel id, or an instance number above `maxInstances`, is dropped
     (`editor.layout.unknown_panel`, once per id per document);
   - a duplicate instance is kept at its first place (main tree depth-first, then floating) and
     dropped elsewhere (`editor.layout.duplicate_panel`);
   - an emptied tab stack is removed and its sibling takes the parent split's place, except the
     central stack, which stays empty; an emptied floating group is removed;
   - `active` not among the tabs becomes the first tab; no `central` stack makes the first tab
     stack of the main tree central; a second one is cleared;
   - a ratio outside [0.02, 0.98] is clamped; a rectangle is clamped into [0, 1] with at least
     0.1 × 0.1 (`editor.layout.clamped`);
   - unknown keys are ignored with one warning, except `editors` (rule 7).

   A **registered panel the document doesn't mention starts closed.**
   These fail the load (`editor.layout.invalid`, with the JSON path), and the caller falls back
   (rule 15): unreadable or unparsable text, a wrong `format`, a newer `version`, a node that is
   neither a split nor a tab stack, a missing `root`, or the size limits of rule 5.

### Bridge to ImGui (`editor_ui/DockLayout.cpp`)

10. **Applying.** `LayoutSession::generation()` (rule 12) changes when a command replaces the
    layout. In `drawDockSpace`, before `ImGui::DockSpace`, when the generation differs from the one
    applied, `DockLayout` rebuilds the dock tree:
    - `DockBuilderRemoveNode` on the dock space, `DockBuilderAddNode` with the dock space flag,
      `DockBuilderSetNodeSize` to the work area, then the splits top-down. Each split picks its
      direction so the child holding the central stack is the one ImGui keeps the central flag on
      (the child opposite the split direction), and the ratio is converted to match;
    - `DockBuilderDockWindow` for each tab in order; each floating group gets a floating node with
      `DockBuilderSetNodePos` and `SetNodeSize` from its rectangle times the dock space;
    - `DockBuilderFinish`; ImGui docks the windows as they begin and a new tab bar selects its
      last tab, so each stack's active tab is selected through its tab bar on the next frame;
    - focus requests from Open Panel (rule 13) are served with `ImGui::SetWindowFocus`.
    Panel windows are submitted with their `draw` each frame while the model holds them open;
    they never use `ImGuiWindowFlags_NoSavedSettings`, which would hide their changes from
    ImGui's dirty flag. A tab's close button runs `ith.window.close_panel` (every action is a
    command, ADR-0037 rule 1).
11. **Reading back.** `DockLayout` walks the dock space's nodes into an `EditorLayout`: split axis
    and the first child's share of the size along it, tab stacks in `TabBar` order (a lone window
    without a tab bar counts as one tab), the selected tab, the central flag; floating nodes and
    undocked panel windows become floating groups with their rectangle as fractions. Panel windows
    are matched by the instance id after `###`; other windows are ignored. Then
    `LayoutSession::observe(layout)` stores it as the current layout **without** changing the
    generation, so the UI doesn't rebuild what the user just did.
    The read-back happens **(a)** when `io.WantSaveIniSettings` is set (it is then cleared;
    `io.IniSavingRate` is 2 s), **(b)** before every layout command runs, so Save Layout As saves
    what is on screen, **(c)** once a rebuilt tree has settled (the frame after the active tabs
    are selected), so ImGui's minimum sizes settle into the model, and **(d)** at exit.
    Dragging, docking, undocking and resizing are direct manipulation, not commands, as in Unreal;
    their scripted equivalent is Load Layout of a prepared document.
12. **Change detection and session state.** `LayoutSession` keeps the current layout and the one
    last written to `<project>/Saved/Editor/Layout.json`. After each read-back and each layout
    command it writes the file when the two differ (`writeFileAtomic`, `Durability::Atomic`: the
    state can be rebuilt from the default; on the main thread, the file is a few KiB). A failed
    write is logged once and retried at the next change. A changed layout requests a frame
    (ADR-0029 rule 12).
13. **DPI.** Ratios and fractions don't depend on pixels. ImGui keeps non-central nodes at their
    pixel size when the dock space resizes, so on `WindowDisplayScaleChanged` `DockLayout` reads
    back and rebuilds from the model, keeping proportions after the theme rescales (ADR-0029
    rule 8). A main-window resize keeps ImGui's behaviour (the central node absorbs it); the
    read-back then updates the session file.

### Commands and storage

14. **Commands**, registered by `editor_core` in the session's registry, kind `Action` (no
    transaction, not undoable, allowed in Play In Editor; like every Action, refused while an
    interaction is open or a level opens), category `Window`:

    | Id | Args | Result | Errors |
    |---|---|---|---|
    | `ith.window.save_layout_as` | `{name, overwrite?: false}` | `{name}` | `editor.layout.invalid_name`, `editor.layout.exists`, `editor.layout.reserved_name`, file errors |
    | `ith.window.load_layout` | `{name}` | `{name, warnings}` | `editor.layout.not_found`, `editor.layout.invalid` |
    | `ith.window.set_default_layout` | `{name}` | `{name}` | `editor.layout.not_found`, `editor.layout.invalid`, config errors |
    | `ith.window.reset_layout` | `{}` | `{name, warnings}` | none: it always ends with a layout |
    | `ith.window.open_panel` | `{panel}` | `{panel, opened}` | `editor.layout.unknown_panel`, `editor.layout.no_free_instance` |
    | `ith.window.close_panel` | `{panel}` | `{panel, closed}` | `editor.layout.unknown_panel` |
    | `ith.window.delete_layout` | `{name}` | `{name}` | `editor.layout.not_found`, `editor.layout.reserved_name` (an engine or project layout), file errors |

    Queries (ADR-0037 rule 17): `ith.query.layout` → `{layout, open}` (the current document's
    `data` and the open instance ids), `ith.query.layouts` → `{layouts: [{name, layer}], default}`.
    - **Names:** 1–64 characters of letters, digits, space, `_` and `-`, not starting or ending with
      a space; compared case-insensitively (ASCII), so no two files differ only in case. A name
      held by an engine or project layout is reserved; Save Layout As writes the user layer only.
    - **Open Panel** takes an instance id, or a multi-instance panel's id for its lowest free
      instance. An open panel is focused and returns `opened: false`. A closed one docks per its
      `defaultDock`: as a tab in the first stack holding a panel with the same default dock;
      else `Center` joins the central stack, and `Left`, `Right`, `Bottom` split the main tree's
      root at that side with a share of 0.2, 0.22 and 0.3; `Floating` makes a group of 0.4 × 0.5
      in the middle. It becomes the active tab.
    - **Close Panel** of a closed panel returns `closed: false`.
    - **Set as Default** writes `layouts.defaultName` to the user layer with `config.set`, which writes
      nothing when the name equals the project's or engine's default, so choosing that default
      clears the user's choice. It doesn't load the layout.
    - **Delete Layout** removes a user layout file through `Config::removeFile` (rule 16); engine
      and project layouts can't be deleted from the editor. Deleting the layout the user chose as
      default also removes the user's `layouts.defaultName`, so the project's or engine's default
      applies again. The current layout stays on screen.
    - **The Window menu** (a third shell menu): the panels, with a check mark on open ones; then
      Load Layout ▸ (every layout by name, the default first), Save Layout As… (a modal asking for
      the name and confirming an overwrite, which then issues the command with `overwrite`),
      Set as Default ▸, Delete Layout ▸ (user layouts only), Reset Layout.
15. **Files:**

    | What | Where | Written by |
    |---|---|---|
    | Engine layouts | `<exe>/Config/Layouts/<name>.json` (`Default`) | the build |
    | Project layouts | `<project>/Config/Layouts/<name>.json` | the project, by hand |
    | User layouts | `<userConfig>/Ironthorn/Editor/Layouts/<name>.json` | Save Layout As (`config.writeFile`, durable); Delete Layout removes |
    | Default choice | `Editor.json` `layouts.defaultName`, merged across layers | Set as Default (user layer) |
    | Last session | `<project>/Saved/Editor/Layout.json` | rule 12 |

    Layouts are whole files (ADR-0035 rule 18): a name resolves to the highest layer that has it.
    `layouts` is the reflected section `EditorLayoutSettings { std::string defaultName = "Default"; }`
    (`default` is a C++ keyword, so it can't be the field's name); its initializer is the engine
    default (ADR-0035 rule 16), so the engine `Editor.json` doesn't repeat it.
16. **Reset and start-up order.** Reset tries, in order, the first that loads:
    1. the merged `layouts.defaultName` (the user's choice, else the project's, else the engine's);
    2. `layouts.defaultName` as the layers below the user layer set it (the project's, else the
       engine's);
    3. `Default`;
    4. the **built-in default**, a constant in `editor_core` equal to the engine's `Default.json`
       (a test compares them).

    Each failed step logs a warning. **Start-up** loads `Saved/Editor/Layout.json`; when it is
    missing or fails, it resets. The editor always starts with a layout. Step 2 needs one small
    `Config` addition: `data(file, belowLayer)`, the merge of the layers below a layer, which
    `Config::set` already computes privately. Delete Layout needs the other: `removeFile(layer,
    relative)`, which removes a whole file from a writable layer and bumps its generation; a
    missing file is `fs.not_found`.
17. **Not undoable.** Layout changes are not world edits: they make no undo entry, don't mark the
    level dirty, and undo and redo never change the layout, as in Unreal.

### The engine default layout (ADR-0008 rule 2)

18. `engine/config/Layouts/Default.json` holds the full P2 default once all its panels exist:

    ```
    root: left_right 0.78
      first:  top_bottom 0.70
        first:  [ith.viewport.1]  central
        second: [ith.content_browser, ith.output_log]  active ith.content_browser
      second: top_bottom 0.40
        first:  [ith.outliner]
        second: [ith.details]
    floating: []
    ```

    The Outliner and Details column runs the full height on the right, Outliner above Details; the
    Content Browser sits under the viewport with the Output Log as a tab beside it. The toolbar
    and main menu are above the dock space and the status bar below it; they are not panels. Until
    steps 10–12 register their panels, the shipped file holds only registered panels (step 9: the
    Output Log under an empty central stack); each step adds its panel to the file at the place
    shown here. A test checks the shipped file against this tree, limited to registered panels.

### Testing (amends ADR-0029 rule 18)

19. - **`editor_core`, headless:** parse and write round trips (byte-identical text); every
      validation case of rule 9; `openPanel` and `closePanel` placement; instance ids; the reset
      chain with broken and missing files at each step; Save Layout As name rules, overwrite and
      reserved names; Delete Layout of user, engine and project layouts and of the
      user's default; Set as Default writing nothing for a lower layer's default; session file
      written only when the layout changed; the built-in default equal to `Default.json`. Config
      directories are explicit test folders.
    - **`editor_ui`, ImGui null backend and RHI null device:** apply each of a set of layouts
      (the default, nested splits, a floating group with two tabs, an empty central stack, two
      viewports), run three frames, read back, and require equality after rounding. A splitter
      drag and a tab close button click, injected through the platform backend, change the
      read-back ratio and close the panel through `ith.window.close_panel`; the session file is
      then written once. A display-scale change keeps the ratios.
    - **Automation:** `tests/editor_core/scripts/layouts.json` runs every layout command and both
      queries (close and reopen the Output Log, save `Test`, set it as default, load `Default`,
      reset and expect `Test`, delete `Test`, reset and expect `Default`), headless in CTest and
      in the editor's `--automation` run, which satisfies ADR-0037 rule 20 for these ids.
    - **Golden image:** the existing shell golden becomes the default-layout golden (ADR-0029
      rule 18's first shot) and is regenerated by each step that adds a panel.

### Correction to ADR-0029 rule 4

20. ADR-0029 rule 4's last bullet reads: `IMGUI_DISABLE_DEFAULT_ALLOCATORS`, so forgetting the
    allocator hook **asserts at ImGui's first allocation** (its default wrappers become
    `IM_ASSERT(0)`, which is `ITH_ASSERT`). It does not fail to link. With asserts compiled out the
    wrappers return null, so the hook is also covered by every headless `editor_ui` test, which
    creates the context through `EditorUi::create`.

## Consequences

- The panel list, the layout model, the commands and the reset chain are tested without ImGui; the
  only ImGui-facing code is apply and read-back in `DockLayout.cpp`, covered by the round-trip
  test, which also catches ImGui upgrades that break docking internals.
- `Default.json` changes its split values to `left_right` and `top_bottom`, gains `central`, and
  changes shape to match Unreal's full-height right column.
- A crash loses at most the last couple of seconds of arrangement; the exit check catches what
  ImGui's dirty flag misses.
- P3 asset editors add nested dock spaces without a format version.
- Follow-up work: `PanelRegistry`, `EditorLayout`, `LayoutSession` and the layout commands and
  queries in `editor_core`; apply and read-back in `DockLayout.cpp`; the Window menu; the Output
  Log as the first panel; `Config::data(file, belowLayer)` and `Config::removeFile`.
  (`ImGuiConfig.h` already says an allocation without the hook asserts; only ADR-0029's text was
  wrong.)
- **Revisit when:**
  - users want Open Panel to return a panel to where it was closed;
  - separate OS windows for panels arrive (after P2): floating groups then gain a target window;
  - asset editors (P3) define where documents dock in the main tree;
  - an ImGui upgrade changes how splits keep the central node or how selected tabs restore.
- **Owner's answers at acceptance (2026-11-23):** the default layout takes Unreal's shape with a
  full-height right column (rule 18); Delete Layout is added for user layouts only, with
  `Config::removeFile` (rules 14 and 16); each panel is registered by the step that builds its view
  (rule 3) and Open Panel docks a closed panel at its declared place (rule 14), as the engineer
  recommended. The rest follows the engineer's recommendation, accepted by the owner.
