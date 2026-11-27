# ADR-0049: Editor Content Browser v1 (amends 0015, 0037)

- **Status:** Accepted
- **Amended by:** [ADR-0050](0050-editor-content-browser-browse-slice-as-built.md),
  [ADR-0051](0051-platform-open-file-dialog-as-built.md)
- **Date:** 2026-11-27
- **Related:** amends [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md)
  rule 4 (a focus context for shortcuts) and
  [ADR-0015](0015-asset-identity-and-reimport.md) rule 7 (delete without replacing references in
  v1); builds on the [Content Browser spec](../specs/content-browser.md),
  [ADR-0004](0004-modules-and-layering.md) (layers), [ADR-0008](0008-editor-familiar-to-unreal-developers.md)
  rules 2, 4 and 6, [ADR-0012](0012-windowing-and-event-loop.md) rule 8 (file dialogs, editor and
  tools only), [ADR-0024](0024-asset-document-envelope-and-extension.md) (`.ithasset`, `.ithmap`),
  [ADR-0034](0034-level-documents.md) (levels), [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md)
  rules 4, 9 and 13 (file operations, no watcher in P2, `Saved/Editor`),
  [ADR-0040](0040-editor-docking-panels-and-layouts.md) rule 3 (the `ith.content_browser` panel)

## Context

P2 step 12 builds the Content Browser. The behaviour is fixed by the
[spec](../specs/content-browser.md) (written to this ADR's recommendations); this ADR settles its
16 open questions, the module work and the order of the work.

Owner decisions (2026-11-27), not reopened here: v1 is the panel plus the Ctrl + Space drawer; tree,
grid and list, path bar, search and type filter; new folder, rename, move, duplicate, delete,
import (button and operating-system drop), reimport and Rescan; double-click opens a level. File
operations are not undoable. Deleting an asset that others use warns, then deletes; the references
load as placeholders. No thumbnails, no file watcher and no placing meshes until after P2. Every
action is an `ith.content.*` command; the view model is in `editor_core`, without GPU or ImGui.

Facts this ADR builds on (checked in the code and the pinned sources on 2026-11-27):

- `AssetDatabase` (`asset_database`, tools kind) scans, imports, reimports, moves, renames and
  removes assets with their sidecars, and `assignNewId` fixes duplicates. It keeps no folder list,
  no dependents, and has no folder, duplicate or copy-in operation. `move` already accepts a
  case-only rename of the asset itself (its target check skips the asset's own file in another
  case). `EditorSessionOptions::assets` is a `const AssetDatabase*`, used to open levels by id.
- `file_system` has `readFile`, `writeFileAtomic`, `listDirectory`, `createDirectories`, `move`
  (never replaces) and `remove` (a file or an empty folder). No copy, no recursive delete.
- The keymap already has two contexts, chosen per command id: Play In Editor
  (`kPlayInEditorCommands`) and Editor. Shortcuts run their command without arguments
  (`CommandRegistry::invoke`; a prompt can gather input first). The ImGui consumer consumes key
  presses only while a text field takes text input, and passes every key to ImGui either way.
- `platform_events` has no drop or dialog events, and `platform` has no file dialog yet, although
  ADR-0012 rule 8 decided one.
- **SDL 3.4.20** (pinned, `SDL_DIALOG` on):
  - Drops: `SDL_EVENT_DROP_BEGIN`, `_POSITION`, `_FILE`, `_TEXT` and `_COMPLETE`, enabled by
    default, with the window and a window-relative position; a file event carries the last hover
    position. Senders exist in the Windows (OLE drop target), X11 (XDND) and Wayland (data device)
    backends.
  - `SDL_ShowOpenFileDialog(callback, userdata, window, filters, n, defaultLocation, allowMany)` is
    asynchronous. On Windows it shows the system file dialog on a thread of its own; on Linux it
    uses the desktop portal over D-Bus, else zenity, and reports an error when neither is there. The
    callback may run on another thread; the filters must stay valid until it runs. File names are
    UTF-8.
  - Not verified: the dialog's behaviour on our Linux lab machines and on the Steam Deck's desktop
    mode; drops from a non-elevated file manager into an elevated editor, which Windows blocks
    (UIPI).

**Prior art.** Unreal's Content Browser (public documentation and observed behaviour): folders
beside an asset view, a path bar, filters and search; the content drawer on Ctrl + Space, which
closes when it loses focus and can be docked as a tab
([Content Browser](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-in-unreal-engine));
import through the system's file dialog or a drop from the file manager (observed); deleting a
used asset lists what references it and offers to replace the references or to delete anyway
([consolidating assets](https://dev.epicgames.com/documentation/unreal-engine/consolidating-assets-in-unreal-engine),
[asset creation, 4.27](https://docs.unrealengine.com/4.27/Basics/AssetsAndPackages/BrowserAssetCreation)).
An earlier engine prototype by the same maintainer used the system's own file dialogs (the desktop
portal on Linux) behind a small interface that tests replaced, and copied files dropped from the
operating system into the project; both worked, and windowless runs took paths through commands.

## Options considered

### Shortcuts that depend on the focused panel (open question 1)

- **No context; the browser uses other chords.** F2 and Delete would not rename or delete in the
  browser, against every file manager and ADR-0008 rule 1. Rejected.
- **The browser panel reads its keys itself with ImGui.** The keys would not be remappable and
  would fire next to the Editor binding of the same chord. Rejected.
- **A `content` section in `Keymap.json` whose chords win while the browser has focus** (the
  spec's recommendation). Works, but adds a second schema to a file that is today one map from
  command id to chords, and a command could be bound in both sections.
- **A third keymap context, chosen per command id like the Play In Editor one (chosen).** The
  `ith.content.*` commands are the Content context; the file stays one map. Same result as the
  spec's recommendation with no format change.

### The drawer while the panel is open (open question 2)

- **Focus the open panel; open the drawer only when the panel is closed (chosen).** One browser on
  screen at a time, with one shared state.
- **Always open the drawer over the panel.** Two views of the same state on screen; rejected.
- **A drawer with its own state.** A second browser to keep in step; rejected (KISS).

### The file dialog (open question 7)

- **An in-editor picker on `file_system`** (the spec's recommendation): one implementation,
  testable headless. But it is a file manager to build and maintain (drives, network paths, home
  and recent folders, hidden files, sorting), it looks like no other application, and it
  contradicts ADR-0012 rule 8, which already chose the platform's dialogs.
- **SDL's dialog through `platform` (chosen).** Already pinned and enabled; the system's dialog on
  Windows, the portal (or zenity) on Linux; asynchronous, so the editor keeps drawing. What can't
  be tested headless is a thin adapter: the import itself is `ith.content.import {files}`, which
  automation calls with paths. Where no dialog is available, drops still import.
- **A dialog library** (nativefiledialog-extended, zlib). Same result as SDL with one more
  dependency; rejected.

### Finding the users of an asset (open question 5)

- **A dependents index kept by the database** (ADR-0015 rule 5). Fast, but it must be kept right
  through every save, import and outside change, without a watcher. Later.
- **On demand, reading every native asset into typed values and walking `AssetRef` fields through
  reflection** (the spec's recommendation). `asset_database` has no type registry, and components
  whose types aren't registered (kept unresolved by the level loader) would hide their references,
  so a delete could warn about nothing. Rejected for that reason.
- **On demand, walking the JSON of every native asset and sidecar for the shape a reference is
  written in (chosen).** `AssetRef` and `SoftAssetRef` always write `{"asset": <uuid>,
  "subAsset": <uuid>}`, so an object whose `asset` is that id is a reference, typed or not. No
  types needed; one walker serves files and the open level.

### Imports on the main thread (open question 6)

- **Synchronous on the main thread (chosen).** `AssetDatabase` is single-threaded; P2 imports are
  a few files. No frame draws during the import, so the spec's "modal progress line" becomes a
  notice drawn the frame before the import starts.
- **One file per frame, on the main thread.** A live progress line and Cancel between files, but
  the command becomes asynchronous and automation must wait for it. Later, if needed.
- **Importers on the background lane.** The database would need locking or a hand-off protocol.
  Later, with large imports.

### The other open questions

Questions 3, 4 and 8 to 16 take the spec's recommendation; the alternatives are in the spec's
section 6 and nothing found here argues against them. Question 8's work is smaller than the spec
expected: SDL already implements the drop targets on all three window systems.

## Decision

### Scope and behaviour

1. **The Content Browser v1 behaves as the [spec](../specs/content-browser.md) says**, with the
   changes in rules 6, 10, 11 and 12 of this ADR. The spec is the acceptance test list
   (its section 4); its Status becomes "Settled by ADR-0049".
2. **Not undoable.** Content commands are of kind Action: they record no undo entry and never make
   the level dirty (a path change of the open level is not a dirty change). Refused while an
   interaction is open and while a level opens; allowed during Play In Editor, except opening a
   level. The content selection is not undoable (ADR-0037 rule 11).

### Keymap: one focus context (amends ADR-0037 rule 4)

3. **ADR-0037 rule 4's bullet "Shortcuts have no per-panel context in P2" is replaced by:** the
   keymap has three contexts — Play In Editor, **Content** and Editor. Every `ith.content.*`
   command is in the Content context except `ith.content.toggle_drawer`, which is in Editor.
   While the Content Browser panel or the content drawer has the keyboard focus, the keymap
   consumer looks a chord up in Content first and then in Editor; otherwise Content chords never
   fire. Play In Editor keeps its place before both (ADR-0029 rule 7). Conflicts are checked per
   context, so one chord may be bound in Content and in Editor. A text field keeps every key, as
   today. No other panel gets a context in P2.
4. **Focus is reported, not asked.** `editor_ui` sets the model's keyboard-focus flag every frame
   from the browser's window (panel or drawer, child windows included); the keymap consumer reads
   the last frame's value, the one-frame lag the viewport rectangles already have (ADR-0042
   rule 8).
5. **Bindings** in `engine/config/Keymap.json`; `ith.content_browser.open_drawer` is renamed
   `ith.content.toggle_drawer`:

   | Chord | Command | Context |
   |---|---|---|
   | Ctrl + Space | `ith.content.toggle_drawer` | Editor |
   | F2 | `ith.content.rename` | Content |
   | Delete | `ith.content.delete` | Content |
   | Ctrl + D | `ith.content.duplicate` | Content |
   | Enter | `ith.content.open` | Content |
   | Alt + Up | `ith.content.navigate_up` | Content |
   | Ctrl + A | `ith.content.select_all` | Content |
   | Escape | `ith.content.dismiss` | Content |

6. **Three argument-free commands are added to the spec's table**, because shortcuts run commands
   without arguments: `ith.content.navigate_up` (the parent folder; nothing at the root),
   `ith.content.select_all` (every shown item) and `ith.content.dismiss` (clears the selection;
   with nothing selected, closes the drawer when it is open). Arrow keys, Home, End and Ctrl + F
   are widget keys of the asset view, not keymap bindings: they move the focus item and issue
   `ith.content.select` with arguments (3.6), or focus the search box. **Ctrl + Space does not
   reach the keymap while a text field has focus** (spec 3.16 said it would): the text field keeps
   every key, and changing that is not worth a special case in v1.

### Settled open questions

7. **Q2:** Ctrl + Space focuses the open panel and opens the drawer only when the panel is closed
   (spec 3.16).
8. **Q3:** deleting a folder deletes everything in it, with the count of files the browser doesn't
   show in the confirmation.
9. **Q4:** deletion is permanent, after the confirmation. The recycle bin is platform work after
   P2.
10. **Q5:** users are found on demand for a delete preview (rule 21): every native asset and every
    sidecar (whose overrides may hold references) is read and walked, plus the open level's current
    state. A reference to a sub-asset counts as a use of its asset. Imported files that name other
    files by path (a glTF's images) are not users in v1.
11. **Q6:** imports and reimports run synchronously on the main thread inside the command. The
    panel draws "Importing <n> files…" in the frame before it issues the command; results go to the
    status line and the log.
12. **Q7:** the Import button opens the **platform's file dialog** (rule 25), several files,
    filtered by the extensions the registered importers read and native asset extensions, with
    "All files". When the dialog can't be shown, the error is logged and the status line says to
    drop files onto the browser instead. No in-editor picker.
13. **Q8:** operating-system drops come from `platform` as events (rule 24), with the position.
14. **Q9:** a dropped folder is imported with its subfolders.
15. **Q10:** duplicates, new folders and Keep Both use `<base>_<n>`, n from 2 (spec 3.10).
16. **Q11:** sub-assets are not items in v1.
17. **Q12:** a move carries nothing beside an imported file; the next reimport names the missing
    file.
18. **Q13:** while a search or filter is active, the view shows matches from the current folder and
    all folders below it.
19. **Q14:** `Saved/Editor/ContentBrowser.json` (`ith.content_browser_state`, version 1, written
    atomically when it changes) keeps the current folder, view mode, sort, tree expansion, splitter
    position and drawer height; search, filter, selection and the drawer's open state are not
    kept.
20. **Q15 and Q16:** a native asset from outside is copied in and keeps its id unless the database
    has it; Replace overwrites the source of an imported asset with the same extension and
    reimports it, keeping its id, settings and overrides.

### Amendment of ADR-0015 rule 7

21. **Rule 7 reads, for v1:** deleting assets in the Content Browser first shows each used asset
    with its users and warns that their references will become missing; when the user confirms,
    the assets are deleted and the references load as placeholders (rule 20). Replacing references
    comes later and restores the rule's original offer. Rule 6's relinking of an orphan sidecar is
    also later; v1 offers delete, and moving the file back plus Rescan relinks it.

### Module work

22. **`asset_database` gains** (each with tests; failures are `asset.*` codes with the `fs` message):
    - **`folders()`:** every folder under `Content/` (links not followed), sorted, built by `scan`
      from the walk it already does and kept current by the operations below;
    - **`createFolder(path)`;**
    - **`moveFolder(from, to)`:** one `fs::move` of the folder, then every record below it changes
      path, keeping ids; a case-only rename of the folder itself is allowed; a target inside the
      folder, the content root, or a taken name (any entry, without ASCII case) fails with nothing
      changed;
    - **`removeFolder(path)`:** deletes every file below it, then the folders from the deepest up,
      going on after a failure and returning the paths that failed with their errors; records of
      deleted assets are erased; a sidecar left behind is reported as an orphan;
    - **`duplicate(id, newPath) -> AssetId`:** copies the file (and the sidecar) with a new random id
      in the envelope or sidecar, everything else unchanged;
    - **`copyIn(externalPath, targetPath, replace)`:** copies an outside file to `targetPath` with
      the files its importer reads (`Importer::dependencies`, same relative paths, only when
      missing); a native asset is added to the database, with a new id when its id is taken;
      `replace` overwrites the source of an imported asset with the same extension, atomically,
      and is refused otherwise. It doesn't import: the caller calls `import`;
    - **`findUsers(ids)`:** for each id, the native assets and sidecars outside `ids` whose JSON
      holds a reference to it (rule 10);
    - **`collectAssetReferences(value)`:** a free function returning the asset ids referenced in a
      JSON value (any object whose `asset` member is a non-nil uuid string); `findUsers` and the
      open-level check use it. A test pins that `AssetRef` and `SoftAssetRef` write that shape;
    - **the case-variant lookup** that `move` uses becomes public, so the browser's name rule
      ("no other entry with that name without ASCII case") and the database's are one function.

    Copies read the file and write it with `writeFileAtomic` (durable), so **`file_system` gains
    nothing** in v1; a streaming copy comes when large sources make the memory peak matter.
23. **`editor_core` gains** `ContentBrowserModel` (tree, items, natural sort, search, filter,
    selection with anchor and focus item, pending prompts, problem list, session state, keyboard
    focus flag, a generation counter), the type category table, the `ith.content.*` commands and
    the `ith.query.content`, `ith.query.content_tree` and `ith.query.content_problems` queries,
    the Content keymap context, and a document path update that follows renames, moves and Rescan
    without changing the dirty state. `EditorSessionOptions::assets` becomes a mutable
    `AssetDatabase*`. No ImGui, GPU or `platform` (ADR-0037 rule 21).
24. **`platform_events` and `platform` gain drop events**, appended to `EventType`: `DropBegin`,
    `DropPosition`, `DropFile` and `DropComplete`, each with its window; `DropPosition` and
    `DropFile` with `x`, `y` converted like mouse positions; `DropFile` with the path in `text`
    (UTF-8). They translate SDL's events one to one; dropped text is not delivered.
25. **`platform` gains the open-file dialog** of ADR-0012 rule 8:
    `Platform::showOpenFileDialog(window, {filters, defaultFolder, allowMany})`, main thread, one
    dialog at a time (a second fails with `platform.dialog_busy`). The callback copies the paths
    and posts them to the main thread through SDL's thread-safe event queue; a later pump delivers
    `FileDialogFile` (one per path, in `text`) and then `FileDialogClosed` (empty `text` for chosen
    or cancelled, the error otherwise). `platform` keeps the filter strings alive until the
    callback runs. Save and folder dialogs come when a feature needs them.
26. **`editor_ui` gains** the panel `ith.content_browser` (dock Bottom, one instance), the drawer
    (an ImGui window that is not dockable and not in layouts), inline rename, the delete and
    conflict dialogs, drag and drop between its own widgets, and `UiHost::showOpenFileDialog`.
    The **editor application** forwards drop and dialog events to `EditorUi`, which hit-tests
    drops against the browser's last-frame rectangles and issues `ith.content.import`; drops
    elsewhere are ignored.

### Slices

27. **Slice 1, browse:** `AssetDatabase::folders()`; the model, the queries and
    `navigate`, `navigate_up`, `select`, `select_all`, `dismiss`, `set_view`, `set_filter`,
    `rescan`, `open` and `toggle_drawer`; the Content keymap context and the bindings of rule 5;
    session state; the panel and the drawer; the browse part of the automation script; the
    performance benchmark (spec requirements 59 to 61). Acceptance: spec requirements 1 to 15, 50
    to 56 and 58 to 61.
28. **Slice 2, file operations and import:** first the `platform` drop events and dialog (their
    own commit, with offscreen tests of the translation), then the `asset_database` additions,
    then `new_folder`, `rename`, `move`, `duplicate`, `delete`, `import`, `reimport` and `fix`
    with their dialogs. Acceptance: the remaining spec requirements.

## Consequences

- Users manage content in the editor with the keys they know; the browser, the drawer and
  automation share one model, and every action is scripted.
- Shortcuts gain one focus context; the keymap file format doesn't change. The rebinding screen
  later shows the context next to each command.
- A delete preview reads every native asset and sidecar: fast enough for P2 projects, slow for
  large ones. References written in another shape than `AssetRef`'s would be missed; the pinning
  test guards that.
- A large import blocks the editor (Windows marks a window "not responding" after about five
  seconds without pumping events).
- The file dialog can't be tested headless; the commands behind it can. On a Linux system with
  neither the desktop portal nor zenity, Import works by drop only.
- Copies hold the whole file in memory once (imports already read it whole).
- Follow-up after acceptance: ADR-0037 and ADR-0015 list this ADR under "Amended by"; the spec gets
  the changes of rules 6, 10, 11 and 12 (sections 3.12, 3.16, 3.17, 3.19, 5 and 6); `docs/architecture.md` describes the new module
  parts; the private plan's step 12 points here.
- **Revisit when:**
  - a delete preview takes more than a second on a real project (a dependents index in the
    database);
  - an import takes long enough to be noticed (one file per frame, then the background lane);
  - another panel needs its own shortcut context, or users want Ctrl + Space inside text fields;
  - the file watcher lands after P2 (Rescan becomes a fallback);
  - meshes can be placed (sub-assets as items, dragging assets into the viewport and Details);
  - reference replacement, the recycle bin or "show in file manager" are built;
  - the Linux lab machines or the Steam Deck show the dialog misbehaving.
