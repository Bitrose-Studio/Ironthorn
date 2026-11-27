# Spec: Content Browser v1

- **Status:** Settled by [ADR-0049](../decisions/0049-editor-content-browser-v1.md) (Accepted
  2026-11-27); the browse slice (ADR-0049 rule 27) is implemented, file operations and import are
  not yet, and the Content Drawer button in the status bar (3.16) comes with them (it changes the
  shell's golden images). The ADR settles the open questions (section 6) and the spec follows it
  where it differs from the first draft (its rules 3, 6, 10, 11 and 12)
- **Date:** 2026-11-27
- **Builds on:** [ADR-0003](../decisions/0003-clean-room.md) (clean room),
  [ADR-0004](../decisions/0004-modules-and-layering.md) (layers),
  [ADR-0008](../decisions/0008-editor-familiar-to-unreal-developers.md) (rules 2, 4 and 6: bottom
  dock, Ctrl + Space content drawer, a browser that mirrors the disk),
  [ADR-0012](../decisions/0012-windowing-and-event-loop.md) (rule 8: file dialogs, editor and
  tools only),
  [ADR-0015](../decisions/0015-asset-identity-and-reimport.md) (ids, the asset database, rules 5
  to 7, case rules, broken references),
  [ADR-0024](../decisions/0024-asset-document-envelope-and-extension.md) (`.ithasset`, `.ithmap`),
  [ADR-0034](../decisions/0034-level-documents.md) (levels),
  [ADR-0035](../decisions/0035-file-system-configuration-and-main-thread-queue.md) (file system,
  no watcher in P2, `Saved/Editor/`),
  [ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md)
  (commands, Actions, the unsaved-changes answer, asset selection not undoable),
  [ADR-0040](../decisions/0040-editor-docking-panels-and-layouts.md) (panels, the
  `ith.content_browser` panel),
  [ADR-0049](../decisions/0049-editor-content-browser-v1.md) (the decisions for v1)

This is a clean-room behavioural spec (ADR-0003). It was written from public documentation and
the common conventions of desktop file managers and game editors; the sources are listed at the
end. The implementation is built from this spec and Ironthorn's architecture, never from another
engine's code.

## 1. Purpose and scope

The asset database (`asset_database`) already scans the project's `Content/` folder, imports and
reimports source files, and moves, renames and deletes assets with their sidecars while keeping
their ids. Nothing in the editor lets a user do any of that. The **Content Browser** is the panel
where a user sees the project's content as it is on disk and manages it:

- browse the `Content/` folders in a tree, and the folder's contents in a grid or a list;
- find assets by name and by type;
- create folders, and rename, move, duplicate and delete folders and assets;
- import files from outside the project (a button or a drop from the operating system), reimport
  them, and rescan the disk after changes made outside the editor;
- see what is wrong on disk (a file without its sidecar, a sidecar without its file, duplicates,
  names that differ only in case) and fix it;
- open a level by double-clicking it.

The same browser is also available as a pop-up **content drawer** on Ctrl + Space (ADR-0008
rule 4), so a user with the panel closed still reaches content quickly.

Owner decisions this spec follows (2026-11-27):

- **File operations are not undoable.** They act on disk at once and never enter the undo
  history. Level edits stay undoable as before. The Content Browser selection is not undoable
  (ADR-0037 rule 11).
- **Deleting an asset that others use** shows the users and asks. If the user confirms, the asset
  is deleted and the references to it become missing; they load as placeholders (ADR-0015
  rule 20). Replacing references comes later.
- **Every action is an `ith.content.*` command** that automation can call. The view model has no
  GPU and no ImGui and lives in `editor_core`.

### In scope for v1

- The Content Browser panel (bottom dock) and the content drawer (Ctrl + Space).
- The folder tree, the path bar, the asset view in grid and list mode, the search box and the
  type filter.
- New folder, rename, move by drag and drop, duplicate, delete, import (button and operating
  system drop), reimport and Rescan.
- Problem entries and their fixes.
- Double-click on a level opens it, with the usual unsaved-changes prompt.
- Keyboard shortcuts while the browser has focus, and per-project session state.

### Out of scope for v1

| Out of scope | Why, or where it goes |
|---|---|
| Thumbnails and previews | No renderer for assets until P3; v1 shows type icons |
| A file watcher | ADR-0035 rule 9: Rescan covers outside changes until the watcher lands after P2 |
| Dragging assets into the viewport, the Outliner or Details | No mesh rendering until P3; reference fields in Details come with that work |
| Creating assets from a menu (data assets, materials) | Each asset type brings its own create entry with its editor; a new level stays File > New Level |
| Replacing references on delete, a broken-reference view | Later (ADR-0015 rules 7 and 20); v1 shows the users and lets references go missing |
| Relinking an orphan sidecar to a renamed file | v1 offers delete; moving the file back and Rescan relinks it |
| Showing sub-assets (the meshes inside a glTF) as items | No consumer in P2 (open question 11) |
| Asset editors for non-level types | Double-click on them does nothing in v1 |
| Several Content Browser instances, back and forward history, favourites, collections | Not needed for P2; the panel is single-instance (ADR-0040 rule 3) |
| Copy, cut and paste of files; Ctrl + drag to copy | Duplicate covers copying in v1 |
| Rubber-band selection, type-to-select, tile size control | Polish after P2 |
| Moving to the operating system's recycle bin; "show in file manager" | Platform work (open question 4) |
| Search syntax (type or path prefixes, exclusions) | Plain words in v1 |
| Source control status | Comes with source control integration |

## 2. Concepts and terminology

| Ironthorn name | What it is |
|---|---|
| **Content Browser** | The panel `ith.content_browser`: folder tree, path bar, toolbar, asset view and status line |
| **Content drawer** | The same browser shown as a pop-up over the bottom of the main window, toggled with Ctrl + Space |
| **Content root** | The project's `Content/` folder, shown as **Content**; every path below is relative to it |
| **Content path** | A path relative to the content root, UTF-8, `/`-separated; the root is the empty path `""` |
| **Folder tree** | The left part: the content root and every folder below it |
| **Path bar** | The breadcrumb above the asset view: `Content › Meshes › Rocks`, one segment per folder |
| **Asset view** | The right part: the items of the current folder, or the search results |
| **Item** | One entry in the asset view: a **folder**, an **asset** (a database record) or a **problem entry** |
| **Problem entry** | A file on disk that is not in the database because something is wrong with it (3.2) |
| **Problem badge** | A mark on an asset or problem entry that has a problem, with the reason as its tooltip |
| **Label** | The name an item shows (3.2) |
| **Type** | An asset's database type: a native asset's `format` (`ith.level`) or an imported file's importer id (`gltf`) |
| **Type category** | The group a type is shown and filtered under: Level, Model, Texture, Audio, Data, Other |
| **Current folder** | The folder the asset view shows, and where new folders and imports go by default |
| **Item reference** | How commands name an item: `{"id": <uuid>}` for an asset, `{"path": <content path>}` for any item |
| **Content selection** | The selected items of the asset view; separate from the level's entity selection |
| **Primary item** | The last item added to the content selection; F2, Enter and Duplicate's rename act on it |
| **Anchor** | The item a Shift range starts from |
| **Focus item** | The item the keyboard cursor is on |
| **View mode** | `grid` (tiles) or `list` (rows with columns) |
| **Users** | The native assets and sidecars whose saved JSON refers to an asset (or one of its sub-assets) by id, plus the open level if its current state does; "dependents" in ADR-0015 (3.11) |
| **Import destination** | The folder imported files are copied into |
| **Conflict choice** | What an import does with a name already taken: `replace`, `keep_both` or `skip` |
| **Session state** | What the browser remembers per project between runs (3.12) |

"Content Browser", "content drawer", "Level" and "asset" are generic vocabulary kept for
familiarity (ADR-0008 rule 3).

## 3. Behaviour

### 3.1 Layout of the browser

From top to bottom, left to right:

- **Toolbar:** Import, Add (a menu with New Folder), the search box, the type filter menu, the
  view mode switch (grid or list), the sort menu (list mode also sorts by its column headers), and
  Rescan.
- **Folder tree** on the left, **path bar** and **asset view** on the right, divided by a
  draggable splitter (default: the tree takes 20 % of the width).
- **Status line:** `<n> items`, `<m> selected` when m > 0, and `<p> problems` when p > 0; clicking
  the problem count sets the type filter to Problems (3.5).

Empty states: an empty folder shows "This folder is empty. Drop files here or use Import."; a
search without matches shows "No items match". Without an asset database (no project open), the
browser shows "No project is open" and every `ith.content.*` command fails with
`editor.document.no_asset_database`.

### 3.2 What is shown

**Folder tree.** The content root, then every folder below it, as listed on disk at the last scan
or content command. Empty folders are shown. Links to folders are not followed and not shown
(as the database treats them). A folder is collapsed or expanded by its arrow or by double-click;
navigating to a folder expands its ancestors. The current folder is highlighted.

**Asset view.** Without a search or filter (3.5), the direct children of the current folder:

| Shown | Label | Notes |
|---|---|---|
| Folders | Folder name | Always listed before every other item |
| Native assets (`.ithasset`, `.ithmap`) | File name without that extension | `Ship.ithasset` shows `Ship` |
| Imported assets (a file with a sidecar) | Full file name | `Rock.png` and `Rock.gltf` can share a folder, so the extension stays visible |
| Problem entries | Full file name | See below |

Not shown: sidecars whose file exists (they belong to their file), files that no importer reads
and that are not native assets (ADR-0024 rule 7), links to folders.

**Problems** come from the last scan (ADR-0015 rule 6) and from content commands (3.13):

| Scan problem | Shown as | Fix offered |
|---|---|---|
| `MissingSidecar`: a file an importer reads, without a sidecar | Problem entry, label greyed, badge "Not imported" | Import (in place) |
| `OrphanSidecar`: a sidecar whose file is gone | Problem entry labelled with the sidecar's name (`Rock.png.meta`), badge "Source file missing" | Delete |
| `DuplicateId` left unresolved by a first scan | Problem entry, badge "Duplicate id of <other path>" | Assign New Id |
| `DuplicateId` that the scan resolved by giving the copy a new id | No badge; one info line in the log | none needed |
| `InvalidFile` (unreadable asset or sidecar, folder that can't be listed, leftover temporary file, name that isn't valid Unicode) | Problem entry (or a badge on the folder), with the scan's message | Delete, Rename |
| `CaseConflict`: two paths that differ only in ASCII case | Badge on both items, naming the other | Rename |
| `FormatMismatch`: the extension doesn't fit the format | Badge on the asset | Rename |

An asset with a badge is still an asset and supports every asset operation. A problem entry
supports only Delete, Rename (except orphan sidecars) and its fix; it can't be dragged, duplicated
or reimported.

**Type categories.** A table in `editor_core` maps each type to a display name and a category;
`editor_ui` chooses the icon by category (folders and problem entries have icons of their own):

| Category | Types in P2 |
|---|---|
| Level | `ith.level` |
| Model | `gltf` |
| Texture | the image importers, when they exist |
| Audio | the audio importers, when they exist |
| Data | every other native format (`ith.input_action`, `ith.input_context`, …) |
| Other | any type the table doesn't know; its display name is the raw type |

The module that brings a format or an importer adds its row.

**Grid mode** shows a tile per item: the type icon, the label below it on at most two lines,
ending in "…" when longer (the tooltip shows the full label, the type's display name and the
content path). **List mode** shows one row per item with columns Name and Type, plus Folder while
a search or filter is active.

### 3.3 Sorting

- Folders always come first, in every sort.
- **By Name** (the default), ascending or descending: *natural order*. A label is split into runs
  of digits and runs of other characters; non-digit runs compare without ASCII case, digit runs
  by numeric value (with equal values, the shorter run first, so `Rock07` follows `Rock7`), so
  `Rock2` comes before `Rock10`. Ties are broken by the bytes of the label, then of the content
  path, so the order is total and the same on every platform.
- **By Type**: the category's display name, then the type's display name, then by Name.
- Descending reverses the order of the non-folder items and of the folders, each within its
  group.

### 3.4 Navigation

- Clicking a folder in the tree, double-clicking a folder item, Enter on a folder item, or
  clicking a path bar segment makes that folder current.
- Alt + Up goes to the parent folder (`ith.content.navigate_up`; nothing at the root).
- Navigating clears the content selection and the search text; the type filter stays.
- When the current folder disappears (deleted, moved away, gone after Rescan), the current
  folder becomes its nearest existing ancestor; when it is moved by the browser, the current
  folder follows it to its new path.

### 3.5 Search and type filter

- **Search text** is split on spaces; empty text means no search. An item matches when every
  word is a substring of its label, compared without ASCII case (letters outside ASCII compare
  exactly, as content paths do, ADR-0015).
- **Type filter**: a set of categories, plus **Problems** (items with a badge or problem
  entries). An empty set means all. Categories match assets only; problem entries match only
  Problems (they have no category: `ith.query.content` gives `category` for assets only).
  Categories in the set combine with OR; search and filter combine with AND.
- **While a search or a filter is active**, the asset view shows the matching items of the
  current folder **and all its subfolders**, without folder items, with each item's folder in its
  tooltip and in list mode's Folder column (open question 13). Clearing both returns to the
  folder's own contents.
- The view updates as the user types, with no button.
- Changing the search or filter removes from the content selection the items that are no longer
  shown.

### 3.6 Content selection

The content selection is an ordered list of items without duplicates, keyed by asset id for
assets and by content path for folders and problem entries, so a renamed asset stays selected.
The last item added is the **primary item**.

| Input | Result |
|---|---|
| Click | Selects the item alone; it becomes the anchor and the focus item |
| Ctrl + click | Toggles the item; it becomes the anchor and the focus item |
| Shift + click | Selects the items from the anchor to the clicked one, in display order, replacing the selection; the anchor stays |
| Ctrl + Shift + click | Adds that range to the selection |
| Click on empty space | Clears the selection |
| Right-click on an unselected item | Selects it alone, then opens the context menu; on a selected item the selection stays |
| Arrow keys | Move the focus item and select it alone; in grid mode Left and Right move by one, Up and Down by one row |
| Shift + arrow keys | Move the focus item and select the range from the anchor to it |
| Home, End | Move the focus item to the first or last item (with Shift: range) |
| Ctrl + A | Selects every shown item (`ith.content.select_all`) |
| Escape | Clears the selection (`ith.content.dismiss`) |

- Clicks, arrow keys, Home and End are handled by the asset view itself: it moves the focus item
  and issues `ith.content.select` with arguments. They are not keymap bindings (3.17).
- Selecting never changes the level's entity selection, never records an undo entry and never
  makes the level dirty.
- Items that disappear (deleted, moved out of the shown set, gone after Rescan) leave the
  selection. With no anchor (it disappeared), Shift acts like a plain click.
- The folder tree has no multiple selection: its selected folder is the current folder.
  Keyboard commands with focus in the tree act on that folder.

### 3.7 New folder

- Add > New Folder, or New Folder in the context menu of the asset view's empty space or of a
  tree folder, creates a folder in the current folder (or in the tree folder clicked).
- Its name is `New Folder`, or the first free name of the duplicate scheme (3.10).
- The new folder is selected and its inline rename starts at once. Escape keeps the default
  name; Enter or clicking away commits the typed one (3.8).

### 3.8 Rename

- F2, or Rename in the context menu, starts an **inline rename** of the primary item (or the
  selected tree folder when the tree has focus).
- The field holds the editable part: a folder's name, a native asset's name without its
  extension, an imported file's name without its last extension. The extension is shown after
  the field and can't be changed (`asset.extension_changed` makes it part of the type).
- **Enter** commits, **Escape** cancels, **clicking away** commits a valid name and cancels an
  invalid one. An unchanged name does nothing.
- **Validation runs as the user types**; the reason shows under the field, and Enter with an
  invalid name keeps the field open. The new name is invalid when:
  - it is empty;
  - it contains `/` or `\`;
  - the resulting content path fails `isValidContentPath` (control characters, `< > : " | ? *`,
    a name ending in `.` or a space, a Windows device name such as `CON` or `com1.txt`, more than
    255 bytes, a folder name ending in `.meta`);
  - **another entry in the same folder has the same name without ASCII case**: any file on disk,
    a hidden one or a sidecar included, or any folder (`editor.content.exists`).
- **A case-only rename of the same item is allowed** (`rock.png` to `Rock.png`); it is a
  rename on disk, not a conflict.
- Renaming an asset keeps its id; its sidecar is renamed with it. Renaming a folder renames the
  folder on disk; every asset inside keeps its id and its database path changes.
- Renaming the open level's file, or a folder that contains it, changes the open document's path
  to match; the level stays open, with the same dirty state.
- A rename that would fix a problem (a case conflict, a format mismatch) removes the badges it
  fixed.
- Content root: can't be renamed (`editor.content.root`).

### 3.9 Move by drag and drop

- Dragging starts after the pointer moves a few pixels with the button held. Dragging a selected
  item drags the whole selection; dragging an unselected item selects it alone first. A tree
  folder can be dragged too.
- **Drop targets** are folders only: folder items in the asset view, folders in the tree and
  path bar segments. Hovering a valid target highlights it; any other place shows a "not
  allowed" cursor and a drop there does nothing.
- **Refused targets:** a dragged folder itself or any folder below it; a drag that contains a
  problem entry or the content root. The tooltip says why.
- **A drop on the folder that already holds every dragged item** does nothing.
- **All or nothing before anything moves:** if any dragged item's name is taken in the target
  (the same rule as rename: any entry, without ASCII case), nothing moves and the browser lists
  the conflicting names.
- **What moves:** an asset with its sidecar, through the database, keeping its id; a folder as
  one move on disk, with everything inside it (files that the browser doesn't show included).
  Nothing else moves with an imported file (open question 12): a `.gltf` moved without the
  `.bin` it reads will fail its next reimport with `asset.not_found` naming the missing file.
- Items are moved in selection order. If one fails on disk, the move stops; the result lists what
  moved and what didn't, and the browser refreshes the folders involved.
- The moved items stay selected only if they are still shown; the current folder doesn't change
  (except under 3.4's rule).
- Moving the open level (or a folder that contains it) changes the document's path as rename
  does.

### 3.10 Duplicate

- Ctrl + D, or Duplicate in the context menu, duplicates every selected asset into its own
  folder. Folders and problem entries can't be duplicated in v1 (the command reports them as
  skipped).
- **Naming:** take the editable part of the name; if it ends in `_<digits>`, drop that suffix to
  get the base. The copy is `<base>_<n>` plus the extension, with `n` the smallest integer from 2
  whose name is free in the folder under the rename rule (no entry with that name without ASCII
  case). `Rock.png` gives `Rock_2.png`; duplicating `Rock_2.png` gives `Rock_3.png` if free. The
  same scheme names new folders (`New Folder`, `New Folder_2`) and imports kept with Keep Both.
- **A native asset** is copied with a new random asset id in its envelope; its data is copied as
  it is. A duplicated level keeps its entities' stable ids, which is harmless (ADR-0034 rule 15).
- **An imported asset** is copied with its sidecar; the copy's sidecar gets a new asset id and
  keeps the importer, settings, sub-asset table and overrides. Files it reads beside it (a
  `.bin`) are not copied; the copy, in the same folder, reads the same ones.
- **The open level with unsaved changes:** the saved file is copied, and the log says the unsaved
  changes are not in the copy.
- The copies are selected afterwards (the originals are not).

### 3.11 Delete

- Delete, or Delete in the context menu, acts on the selection (or the selected tree folder when
  the tree has focus). It always asks first, because it can't be undone.
- **What is deleted:**
  - an asset: its file and its sidecar;
  - a problem entry: its file (an orphan sidecar: the sidecar; a file without a sidecar: the file);
  - a folder: **the folder with everything in it**, including files the browser doesn't show
    (open question 3).
- **Refused:** the content root (`editor.content.root`); the open level's file or a folder that
  contains it (`editor.content.level_open`: "open another level or a new level first").
- **Users.** For every asset that would be deleted (inside deleted folders too), the browser finds
  its users that are **not** themselves being deleted, on demand when the delete is previewed
  (ADR-0049 rule 10):
  - every native asset and every sidecar (whose overrides may hold references) is read and its
    JSON walked for the shape a reference is written in: an object whose `asset` member is the
    asset's id as a uuid string (`{"asset": <uuid>, "subAsset": <uuid>}`, as `AssetRef` and
    `SoftAssetRef` write it). This finds references in components whose types aren't registered
    too;
  - the open level's current, possibly unsaved, state is walked the same way;
  - a reference to a sub-asset counts as a use of its asset;
  - imported files that name other files by path (a glTF's images) are not users in v1.
- **The confirmation dialog** shows:
  - the title "Delete <n> items?";
  - the items by label and content path, at most 20, then "and <k> more";
  - for folders, the number of assets inside them and, when there are any, "and <j> other files
    not shown in the Content Browser";
  - when there are users: a list of each used asset with its users (label and content path), and
    the warning that the users' references will become missing and load as placeholders;
  - "This can't be undone. The files are deleted from disk.";
  - buttons **Delete** and **Cancel**; Cancel has the keyboard focus, and Escape cancels.
- **Deleting** removes the files permanently (no recycle bin, open question 4), then folders from
  the deepest up. A failure on one item doesn't stop the others; the result lists the items that
  failed with their errors. A sidecar that can't be deleted after its file was becomes an orphan
  sidecar entry (as `AssetDatabase::remove` reports).
- References in users are not changed: they keep the deleted id, and the level or asset loads a
  placeholder for it (ADR-0015 rule 20).
- Deleted items leave the selection; the current folder moves up if it was deleted (3.4).

### 3.12 Import

**Starting an import:**

- **Import button** (and Import… in the empty-space context menu) opens the **platform's file
  dialog** (ADR-0012 rule 8, ADR-0049 rule 12): the system's dialog on Windows, the desktop portal
  (or zenity) on Linux. It selects several files, filtered by the extensions the registered
  importers read and the native asset extensions, with an "All files" choice. The dialog is
  asynchronous: the editor keeps drawing, and the chosen paths arrive later as events, which the
  browser passes to `ith.content.import`. The destination is the current folder. There is no
  in-editor file picker.
- **When the dialog can't be shown** (on Linux, neither the portal nor zenity is there; or a
  dialog is already open), the error is logged and the status line says to drop files onto the
  browser instead.
- **A drop from the operating system** onto the browser or the drawer (open question 8). The
  editor hit-tests the drop position against the browser's rectangles of the last frame; drops
  elsewhere are ignored, and dropped text is not delivered. The destination is the folder item,
  tree folder or path bar segment under the pointer, else the current folder.
- **A dropped folder** is imported with its subfolders, which are created in the destination
  with the same names (open question 9).

**Per file:**

1. **A file already inside the content root** is not copied: it is imported in place (a file
   without a sidecar gets one; a file with one is reimported). The destination doesn't apply.
2. **A file that no importer reads** and that is not a native asset is skipped, listed in the
   result as "no importer for `.<ext>`".
3. **The destination name** is the file's name in the destination folder. When it is taken
   under the rename rule (any entry, without ASCII case), the file has a **conflict**, resolved by
   the conflict choice:
   - `replace`: allowed only when the taken name is an imported asset with the same extension.
     The new content is written over its source file (atomically) and the asset is reimported,
     so it keeps its id, settings, overrides and the ids of matching sub-assets. Otherwise
     `replace` is refused for that file and it is skipped with the reason;
   - `keep_both`: the file gets the next free name of the duplicate scheme (3.10);
   - `skip`: the file is left out.
4. **Copy.** The file is copied to its destination. For a source file, so are the files its
   importer says it reads (`Importer::dependencies`, such as a `.gltf`'s `.bin`), to the same
   paths relative to it, when they exist beside the original and are not there yet.
5. **Import** through the database. Then the files the import reports as referenced (the images
   of a glTF's materials), when they exist beside the original, are copied to the same relative
   paths if missing, and imported themselves if an importer reads them.
6. **A native asset from outside** (`.ithasset`, `.ithmap`) is copied as it is; it keeps its id
   unless the database already has that id, in which case the copy gets a new one (the duplicate
   rule of ADR-0015 rule 6).

**Conflicts in the UI:** before copying anything, the browser lists every conflict. A dialog asks,
per file, Replace (when allowed) / Keep Both / Skip, with "Apply to all remaining" and Cancel
(which imports nothing). Automation passes the choice (4.2).

**While it runs** (ADR-0049 rule 11): imports and reimports run synchronously on the main thread,
inside the command. No frame is drawn during the import, so there is no live progress and no
Cancel: the panel draws the notice "Importing <n> files…" in the frame before it issues the
command. Results go to the status line and the log.

**Afterwards:** the destination becomes the current folder, the imported assets are selected,
and the log has one line per file (imported, reimported, from cache, skipped and why, failed and
why). A copy whose import failed stays on disk and shows as a "Not imported" entry, so the user
can retry or delete it. Each file is limited to `kMaxSourceFileSize`, with its dependencies
counted (as the database enforces).

### 3.13 Reimport and fixes

- **Reimport** (context menu) reruns the importer with the saved settings for every selected
  imported asset (ADR-0015 rule 9). Native assets in the selection are skipped; with only native
  assets selected, the command is disabled ("only imported files can be reimported").
- **Import** on a "Not imported" entry imports the file in place; the entry becomes an asset.
- **Assign New Id** on an unresolved duplicate gives the file a new id (`assignNewId`); the entry
  becomes an asset.
- The result and the log say, per asset: reimported, unchanged (from the cache), sub-assets
  that are now missing (by source key, as warnings), or the error.
- **Problems after a command:** each content command updates the problem list for the paths it
  touched: entries it fixed are removed (an imported file, a deleted orphan, a renamed case
  conflict on both sides) and problems it caused are added (a sidecar left behind by a failed
  delete). Everything else waits for Rescan.

### 3.14 Rescan

- **Rescan** (toolbar) rebuilds the database from disk (`AssetDatabase::scan`), lists the folders
  again and replaces the problem list. The editor also scans once when a project opens.
- The current folder stays, or becomes its nearest existing ancestor. The selection keeps the
  items that still exist (assets by id, so an asset renamed outside the editor stays selected).
  Tree expansion stays for folders that still exist.
- **The open level** (never a dirty change): when the database has a record at the level's path,
  the document takes that record's id (a scan gives a copy of a known level a new one) and keeps
  its path. When its file is there but not in the database (left out as a duplicate id or an
  invalid file), nothing changes. Only when its file is gone does the id find it: moved outside the
  editor, the document's path changes to the file's new path; not found, the level stays open with
  its path, and a warning says that saving will write it again.
- The log reports the number of assets and problems found.
- Rescan is refused while the open level is being saved (`editor.document.save_in_progress`).
- A level saved into the content root to a file the database doesn't know under the level's id
  shows at once: the save rescans. That is a new level's first save, a Save As to a new path (the
  copy gets a new level id, the original keeps its own), and a Save As over a level file the
  database hasn't scanned yet. A Save, or a Save As over a level the database already knows (the
  saved level takes that file's id), needs no Rescan (ADR-0050 item 9).

### 3.15 Opening a level

- Double-click or Enter on a level asset runs `ith.content.open`, which opens it through
  `ith.file.open_level` by id.
- With unsaved changes, the usual Save / Don't Save / Cancel prompt appears (ADR-0037 rule 15),
  and the command runs again with the answer.
- Refused during Play In Editor (`editor.command.playing`), as Open Level is.
- Double-click or Enter on any other asset does nothing in v1; on a folder it navigates.
- When the open succeeds from the drawer, the drawer closes.

### 3.16 The content drawer

- **Ctrl + Space** (anywhere in the editor), or the Content Drawer button in the status bar,
  toggles the drawer (`ith.content.toggle_drawer`). **While a text field has the keyboard focus,
  Ctrl + Space does nothing:** the text field keeps every key, as for every other shortcut
  (ADR-0049 rule 6).
- **When the Content Browser panel is open**, Ctrl + Space brings its tab to the front and gives
  it the keyboard focus instead of opening the drawer, so the two never show at once (open
  question 2). When the panel is closed, the drawer opens.
- **Place and size:** over the bottom of the main window, the width of the dock space, above
  docked panels and the status bar. Its height starts at 40 % of the main window and is changed
  by dragging its top edge (at least 150 pixels at scale 1, at most 80 % of the window); the
  height is session state.
- It is not a dock panel: it is not in layouts, Open Panel doesn't list it and it never changes
  the layout.
- **On open**, the keyboard focus goes to the asset view; Ctrl + F moves it to the search box.
- **It closes** on Ctrl + Space; on Escape when nothing in the drawer uses that Escape (an inline
  rename, an open menu or dialog and a focused text field take it first; then
  `ith.content.dismiss` clears a non-empty selection, and closes the drawer only when nothing is
  selected); when the user
  clicks anywhere outside it; after a level opens from it. A dialog it opened (delete, import
  conflicts) doesn't close it.
- **Shared state:** the drawer and the panel show the same browser: current folder, selection,
  search, filter and view mode.
- It starts closed in every run.

### 3.17 Keyboard shortcuts

**Keymap contexts** (ADR-0049 rule 3, amending ADR-0037 rule 4). The keymap has three contexts:
Play In Editor, **Content** and Editor. A command's context is chosen by its id, as for the Play
In Editor commands; `Keymap.json` stays one map from command id to chords, with no `content`
section. Every `ith.content.*` command is in the Content context except
`ith.content.toggle_drawer`, which is in Editor.

- While the browser (panel or drawer, child windows included) has the keyboard focus, a chord is
  looked up in Content first, then in Editor. Otherwise Content chords never fire.
- Play In Editor keeps its place before both.
- Conflicts are checked per context, so one chord may be bound in Content and in Editor. F2,
  Delete, Ctrl + D, Ctrl + A and Escape are also the level's entity commands (`ith.edit.*`); with
  the focus in the browser they reach the content commands, elsewhere the entity commands.
- `editor_ui` reports the browser's focus to the model every frame; the keymap reads the last
  frame's value (ADR-0049 rule 4). Between frames the focus follows at once: a mouse press inside
  the browser's last-frame rectangles (panel or drawer) gives it the focus and one outside takes
  it, and `ith.content.toggle_drawer` gives it to the drawer or the panel it opens (and takes it
  when it closes the drawer), so a key later in the same batch of events reaches the right
  context.
- While the browser has the focus, the widget keys of the asset view (below) are consumed by the
  keymap without running anything, so End never also runs its Editor binding (Snap to Floor) and
  an arrow never reaches the level.
- Until the file operations register `ith.content.rename`, `ith.content.delete` and
  `ith.content.duplicate`, F2, Delete and Ctrl + D are consumed the same way while the browser has
  the focus, whatever the keymap binds (`kContentReservedChords`), so unbinding a content command in
  a user layer can't let Delete reach the level's `ith.edit.delete`. The file operations replace
  this with their real commands.
- While an ImGui popup is open (a menu, the unsaved-changes question, later dialogs), it keeps
  every key, as a text field does: Escape closes the menu and runs no shortcut.
- A text field (search, inline rename) keeps every key, as today. No other panel has a context
  in P2.

Shortcuts run their command without arguments. The bindings in `engine/config/Keymap.json`
(ADR-0049 rule 5):

| Chord | Action | Command | Context |
|---|---|---|---|
| Ctrl + Space | Toggle the drawer (3.16) | `ith.content.toggle_drawer` | Editor |
| F2 | Rename the primary item | `ith.content.rename` | Content |
| Delete | Delete the selection (asks) | `ith.content.delete` | Content |
| Ctrl + D | Duplicate the selection | `ith.content.duplicate` | Content |
| Enter, Keypad Enter | Open the primary item (level: open it; folder: go into it) | `ith.content.open` | Content |
| Alt + Up | Parent folder | `ith.content.navigate_up` | Content |
| Ctrl + A | Select every shown item | `ith.content.select_all` | Content |
| Escape | Clear the selection; with nothing selected, close the drawer when it is open | `ith.content.dismiss` | Content |

**Widget keys of the asset view**, not keymap bindings and not remappable in v1:

| Keys | Action |
|---|---|
| Arrows, Home, End, with or without Shift | Move the focus item and select (3.6), through `ith.content.select` with arguments |
| Ctrl + F | Focus the search box (UI only) |

The keymap consumer holds this list as `kContentWidgetChords` (`Keymap.h`).

### 3.18 Session state

Kept per project in `<project>/Saved/Editor/ContentBrowser.json` (ADR-0035 rule 13), a document
`ith.content_browser_state` version 1, written when it changes (atomic, not durable: it can be
rebuilt), read at start-up:

- the current folder;
- the view mode, the sort key and direction;
- the expanded tree folders;
- the tree splitter position and the drawer height.

Not kept: the search text, the type filter, the selection and whether the drawer is open (open
question 14). A saved folder that no longer exists falls back to its nearest existing ancestor;
an unreadable file is ignored with one warning.

### 3.19 Commands and queries

Every action is a registered command (ADR-0037 rules 1 and 20). File operations are of kind
**Action**: they record no undo entry, are refused while an interaction is open
(`editor.command.interaction_open`) and while a level opens (`editor.document.busy`), and are
allowed during Play In Editor (except opening a level). They never make the level dirty; a path
change of the open level is not a dirty change (ADR-0049 rule 2). Commands that take `items` act
on the content selection when `items` is absent, so the keymap binds them without arguments.
`navigate_up`, `select_all` and `dismiss` exist because shortcuts run commands without arguments
(ADR-0049 rule 6); they are the only way to go up, select everything or clear: `navigate` always
takes a folder and `select` always takes items and a mode.

**A command that needs a user's answer** (a rename without a name, a delete without
confirmation, an import with unresolved conflicts) changes nothing on disk, starts the matching
prompt in the view model (inline rename, confirmation dialog, conflict dialog) and returns what
the prompt shows. The UI runs it again with the answer; automation passes the answer at once.
This follows the unsaved-changes pattern of `ith.file.open_level`.

| Id | Args | Result |
|---|---|---|
| `ith.content.navigate` | `{folder}` | `{folder}` |
| `ith.content.navigate_up` | `{}` | `{folder}`: the parent folder; nothing changes at the root |
| `ith.content.select` | `{items, mode?}` with `mode` `replace` (the default), `add`, `toggle`, `remove`, `range` (Shift + click: from the anchor to the one item) or `add_range` (Ctrl + Shift + click); `replace` with no items clears the selection | `{selection, primary}` |
| `ith.content.select_all` | `{}` | `{selection, primary}`: every shown item |
| `ith.content.dismiss` | `{}` | `{selection, primary, drawerOpen}`: clears the selection; with nothing selected, closes the drawer when it is open |
| `ith.content.set_view` | `{mode?, sortBy?: "name" \| "type", descending?}` | the view settings |
| `ith.content.set_filter` | `{search?, categories?}` | `{shown}`: the number of items shown |
| `ith.content.new_folder` | `{parent?, name?}` | `{path}`; without `name`, also starts its inline rename |
| `ith.content.rename` | `{item?, name?}` | `{path}`; without `name`, starts the inline rename |
| `ith.content.move` | `{items?, folder}` | `{moved, failed}` |
| `ith.content.duplicate` | `{items?}` | `{created, skipped}` |
| `ith.content.delete` | `{items?, confirmed?: false}` | without `confirmed`: `{deleted: false, preview}`; with it: `{deleted, failed}` |
| `ith.content.import` | `{files, folder?, conflict?: "replace" \| "keep_both" \| "skip"}` | `{imported, skipped, failed}`, or `{done: false, conflicts}` when conflicts exist and `conflict` is absent |
| `ith.content.reimport` | `{items?}` | `{reimported, skipped, failed}` |
| `ith.content.fix` | `{item}` | `{path, id}`: Import for a "Not imported" entry, Assign New Id for a duplicate |
| `ith.content.rescan` | `{}` | `{assets, problems}` |
| `ith.content.open` | `{item?, unsaved?}` | as `ith.file.open_level` for a level; `{folder}` for a folder |
| `ith.content.toggle_drawer` | `{open?}` | `{open, target}`: `target` is `drawer` or `panel` (3.16); `open: false` always closes the drawer |

- `files` in `ith.content.import` are absolute paths, UTF-8, from the file dialog or the drop;
  `folder` is a content path (the current folder by default). Automation calls it with paths, so
  importing is tested headless without the dialog. The command runs the imports synchronously
  (3.12).
- `preview` lists the items, the files and folders to remove, the counts of 3.11 and the users
  per asset. `moved`, `created`, `deleted`, `imported` and the others list item references with
  their content paths; `failed` adds each error's code and message.
- Error codes: `editor.content.not_found`, `editor.content.invalid_name` (the reason in the
  message), `editor.content.exists`, `editor.content.invalid_target` (not a folder, a folder into
  itself or below itself, a problem entry dragged), `editor.content.root`,
  `editor.content.level_open`, `editor.content.not_imported`, `editor.content.no_importer`,
  `editor.command.invalid_args`, `editor.document.no_asset_database`, and the database's and file
  system's codes (`asset.*`, `fs.*`) inside `failed`.
- **Queries:** `ith.query.content` → `{folder, search, categories, view, items: [{kind, path, id?,
  label, type, category, problems: [{kind, message, otherPath?}]}], problems, selection, primary,
  anchor, focus, prompt, drawerOpen}` (items in display order, `problems` the number of shown items
  with a problem, `focus` the focus item, `prompt` a pending rename or dialog, null when none);
  `ith.query.content_tree` → `{current, folders: [{path, name, depth, hasChildren, expanded}]}`, the
  rows of the folder tree as shown (the root first; folders below a collapsed one left out);
  `ith.query.content_problems` → `{problems: [{kind, path, otherPath?, message}]}`, every
  current problem.
- The provisional keymap id `ith.content_browser.open_drawer` in `engine/config/Keymap.json` has
  become `ith.content.toggle_drawer`.

### 3.20 Failure behaviour

- A content command never leaves the view out of step with the disk: after any failure it
  refreshes the folders it touched from the database and the file system.
- An I/O failure in the middle of a move or a duplicate stops it; one in a delete or an import
  skips that item and goes on. Both report per item (3.19).
- A move whose file moved but whose sidecar could neither follow nor be moved back fails with the
  database's `asset.io` error naming both places; the item then shows at its new path, and the
  message suggests Rescan.
- Operations touching the open level's file are refused while it is being saved
  (`editor.document.save_in_progress`).
- Nothing in the browser asserts on what it finds on disk; every surprise is a problem entry, a
  badge or an error.

### 3.21 Multiplayer and runtime

None. The Content Browser is editor-only, works on the local project folder and replicates
nothing. It never runs in a player or a server.

## 4. Requirements

Each requirement is an acceptance test. Unless stated otherwise, tests are headless: an
`EditorSession` on a temporary project folder (explicit directories, ADR-0035 rule 12), the
commands and queries through `Automation::run`, and the disk checked with `file_system`.

### 4.1 What is shown

1. A project with folders `Levels/`, `Meshes/Rocks/` (empty) and files `Levels/Main.ithmap`,
   `Meshes/Ship.gltf`, its sidecar `Meshes/Ship.gltf.meta` and `Meshes/notes.txt`: the tree
   lists Content, Levels, Meshes and Rocks; `Meshes` shows `Rocks`, then `Ship.gltf`; `notes.txt`
   and the sidecar are not shown; `Levels` shows `Main` with type `ith.level`, category Level.
2. Each scan problem of 3.2 appears as specified: a glTF without a sidecar as a "Not imported"
   entry, an orphan sidecar under its own name, an unresolved duplicate as an entry naming the
   other path, a case conflict as a badge on both assets, a format mismatch as a badge, an
   invalid file and a leftover temporary file as entries with the scan's message.
3. The Problems filter shows exactly the items with badges and the problem entries, from the
   current folder down; the status line counts them.
4. A type the category table doesn't know shows category Other and its raw type.

### 4.2 Sorting, search and filter

5. Natural order: `Rock10`, `rock2`, `Rock1`, `Rock07`, `Rock7`, `a` sort as `a`, `Rock1`, `rock2`,
   `Rock7`, `Rock07`, `Rock10`; folders come first ascending and descending.
6. Sort by Type orders by category, then type, then name.
7. Search `ro sh` in a folder tree matches `RockShard.png` in a subfolder and not `Rock.png`; the
   result has no folder items; clearing the text shows the folder's own contents again.
8. Search compares ASCII without case and other letters exactly (`é` doesn't match `É`).
9. Filter {Model, Level} with search `main` shows only models and levels whose label contains
   `main`, from the current folder down.
10. Navigating clears the search and the selection and keeps the filter.

### 4.3 Selection

11. Click, Ctrl + click, Shift + click, Ctrl + Shift + click and click on empty space give the
    selections and anchors of 3.6, through `ith.content.select` and in the panel through injected
    input.
12. Arrow keys in grid mode with 4 columns move the focus by 1 and by 4; Shift extends from the
    anchor; Home and End go to the ends.
13. The primary item is the last added; F2 renames it.
14. Selecting assets records no undo entry, leaves the entity selection and the level's dirty
    state unchanged (`ith.query.history`, `ith.query.selection`, `ith.query.document`).
15. A selected asset stays selected after its rename and after a Rescan that finds it renamed
    outside the editor; a deleted one leaves the selection.

### 4.4 New folder and rename

16. New Folder twice in one folder creates `New Folder` and `New Folder_2`, each selected with the
    inline rename pending (`prompt`).
17. Rename refuses, with `editor.content.invalid_name` and nothing changed on disk: an empty name,
    `a/b`, `a\b`, `a:b`, `name.`, `name `, `CON`, `com1.txt`, a name of 256 bytes, a folder named
    `x.meta`.
18. Rename refuses with `editor.content.exists` a name taken in the folder by a file, a folder,
    a hidden file (`notes.txt`) or a sidecar, in any ASCII case.
19. A case-only rename of the same asset (`rock.png` to `Rock.png`) succeeds on Windows and
    Linux; the file and the sidecar have the new case and the id is unchanged.
20. Renaming an asset keeps its id and renames its sidecar; renaming `Ship` (native) can't change
    its extension; renaming a folder keeps the ids of every asset inside and updates their paths.
21. Renaming the open level, or a folder holding it, updates `ith.query.document`'s path and
    keeps the dirty state; a save then writes the new path.
22. Renaming one asset of a case conflict to a distinct name removes both badges.

### 4.5 Move

23. Moving two assets onto a folder moves their files and sidecars; ids are unchanged.
24. Moving a folder into itself or below itself fails with `editor.content.invalid_target`;
    dropping onto an asset or a problem entry is not a valid target in the panel; dragging a
    problem entry is refused.
25. A move with one name already taken in the target (any case) moves nothing and lists the
    conflict.
26. A folder move carries files the browser doesn't show (`notes.txt`) and subfolders.
27. A drop on the folder that already holds the items changes nothing.
28. With a fault injected on the second of three moves, the first stays moved, the others don't,
    and the result reports both; the view matches the disk.

### 4.6 Duplicate

29. Duplicating `Rock.png` gives `Rock_2.png`; again gives `Rock_3.png`; duplicating
    `Rock_2.png` with `Rock_3.png` taken gives `Rock_4.png`.
30. A duplicated native asset has a new envelope id and otherwise identical data; a duplicated
    imported asset has a new id in its sidecar and the same importer, settings, sub-asset table
    and overrides; a Rescan reports no duplicate id.
31. Duplicating folders or problem entries reports them as skipped; the copies are selected.
32. Duplicating the open level with unsaved changes copies the saved file and logs that the
    changes are not in the copy.

### 4.7 Delete

33. `ith.content.delete` without `confirmed` deletes nothing and returns the preview; with it,
    the asset's file and sidecar are gone.
34. The preview lists, for an asset used by a level and by a native asset outside the deleted
    set, both users; users inside the deleted set are not listed; the open level is listed when
    only its unsaved state refers to the asset.
35. After a confirmed delete of a used asset, loading the user gives a placeholder for the
    reference and a logged broken reference (ADR-0015 rule 20); the user's file is unchanged.
36. Deleting a folder removes it with every file in it; the preview counts the files not shown.
37. Deleting the content root fails with `editor.content.root`; deleting the open level or a
    folder holding it fails with `editor.content.level_open`; nothing is deleted.
38. Deleting an orphan sidecar entry removes the sidecar and the entry.
39. Deleting the current folder makes its parent current.
40. The confirmation dialog in the panel shows the contents listed in 3.11, has the focus on
    Cancel, and Escape cancels.

### 4.8 Import, reimport, fixes and Rescan

41. Importing an outside `.glb` into `Meshes/` copies it, creates its sidecar, selects it and
    makes `Meshes` current.
42. Importing an outside `.gltf` copies its `.bin` (its importer's dependency) and the images it
    references, keeping their relative paths, and imports the images when an importer reads them.
43. A conflict without `conflict` imports nothing and returns `{done: false, conflicts}`;
    `keep_both` gives the next free name; `skip` leaves it out; `replace` on an imported asset of
    the same extension overwrites its source and reimports it, keeping its id and sub-asset ids
    for matching keys; `replace` on a native asset or another extension skips the file with the
    reason.
44. A file no importer reads is skipped with "no importer"; a file already inside the content
    root is imported in place, not copied.
45. A dropped folder is imported with its subfolders.
46. An outside native asset whose id the database already has gets a new id; one with a new id
    keeps it.
47. A failed import leaves the copy as a "Not imported" entry and reports the importer's error.
48. Reimport skips native assets, reports unchanged sources as from the cache and lists missing
    sub-assets; with only native assets selected the command is disabled with its reason.
49. `ith.content.fix` on a "Not imported" entry imports it in place; on an unresolved duplicate it
    assigns a new id; both entries become assets.
50. Rescan after files are added, renamed and deleted outside the editor shows the new state, keeps
    the current folder (or its nearest ancestor) and the selection of assets that still exist, and
    updates the open level's path when its file was moved outside the editor.

### 4.9 Opening levels and the drawer

51. `ith.content.open` on a level opens it by id; with a dirty document it fails with
    `editor.document.unsaved_changes`, and succeeds with `unsaved: "save"` or `"discard"`; during
    Play In Editor it fails with `editor.command.playing`.
52. With the panel closed, `ith.content.toggle_drawer` opens the drawer and a second call closes
    it; with the panel open it returns `target: "panel"` and the panel has the focus.
53. In the panel test (ImGui null backend), the drawer closes on Escape with nothing selected, on a
    click outside it and after a level opens from it; it stays open while its delete dialog is
    shown; its height is clamped to 150 pixels and 80 % of the window.
54. The drawer is never in a saved layout, and opening or closing it doesn't change
    `ith.query.layout`.

### 4.10 Shortcuts, commands and session

55. With the focus in the browser, F2, Delete, Ctrl + D, Ctrl + A and Escape run the content
    commands; with the focus in the Outliner they run the `ith.edit.*` commands; inside the search
    box they edit text.
56. Every `ith.content.*` command and content query appears in an automation script
    (`tests/editor_core/scripts/content_browser.json`), which runs headless in CTest and in the
    editor's `--automation` run (ADR-0037 rule 20).
57. No content command records an undo entry or changes the level's dirty state, except through
    the document path update of 3.8 (a path change, not a dirty change).
58. Session state: after changing the folder, view mode, sort, tree expansion and drawer height,
    a new session on the same project restores them; search, filter and selection start empty; a
    saved folder that was deleted falls back to its nearest ancestor.

### 4.11 Performance

59. With 10 000 assets in 500 folders, building the view model of a folder of 1 000 items takes at
    most 5 ms, and updating a search over all 10 000 items at most 5 ms per keystroke, on the Low
    tier reference machine (ADR-0005), measured by a benchmark.
60. The panel draws only the visible rows or tiles: with 10 000 shown items, the number of items
    drawn per frame is bounded by the visible area.
61. With no content change and no input, the browser rebuilds nothing per frame (it polls a
    generation counter, as ADR-0037 rule 13's view models do).

## 5. Fit with Ironthorn

### Layer and modules (ADR-0004, ADR-0017)

The module work is fixed by ADR-0049 rules 22 to 26:

- **`editor_core`** (layer `editor`): the view model `ContentBrowserModel` (tree, items, natural
  sort, search, filter, selection with anchor and focus item, pending prompts, problem list,
  session state, keyboard focus flag, a generation counter), the type category table, the
  `ith.content.*` commands and the content queries, the Content keymap context, and a document
  path update that follows renames, moves and Rescan without changing the dirty state.
  `EditorSessionOptions::assets` becomes a mutable `AssetDatabase*`. No ImGui, no GPU, no
  `platform`; it uses `asset_database` and `file_system`, which it may (ADR-0037 rule 21).
- **`editor_ui`**: the panel (`ith.content_browser`, dock `Bottom`, one instance, ADR-0040 rule 3),
  the drawer (an ImGui window that is not dockable and not in layouts), inline rename fields, the
  delete and conflict dialogs, drag and drop between its own widgets, and
  `UiHost::showOpenFileDialog`. The editor application forwards drop and dialog events to
  `EditorUi`, which hit-tests drops against the browser's last-frame rectangles and issues
  `ith.content.import`.
- **`asset_database`** gains the operations the browser can't build from the outside without
  duplicating its rules:
  - `folders()`: every folder under `Content/`, built by `scan` and kept current by the
    operations below;
  - `createFolder`, `moveFolder` (one move on disk, records below it keep their ids) and
    `removeFolder` (deletes everything below it, goes on after a failure and reports what
    failed);
  - `duplicate(id, newPath)`: a copy with a new id in its envelope or sidecar;
  - `copyIn(externalPath, targetPath, replace)`: an outside file with the files its importer
    reads; it doesn't import;
  - `findUsers(ids)` and the free function `collectAssetReferences(value)`, which returns the
    asset ids referenced in a JSON value (3.11); a test pins that `AssetRef` and `SoftAssetRef`
    write the shape it looks for;
  - the case-variant lookup `move` already uses becomes public, so the browser's name rule and
    the database's are one function. `move` already accepts a case-only rename of the asset
    itself (requirement 19).
- **`platform_events` and `platform`** gain, behind the existing boundary:
  - drop events `DropBegin`, `DropPosition`, `DropFile` and `DropComplete`, each with its
    window, with the position converted like mouse positions and the path in UTF-8;
  - the open-file dialog of ADR-0012 rule 8, `Platform::showOpenFileDialog`, through SDL: main
    thread, one dialog at a time (a second fails with `platform.dialog_busy`); the chosen paths
    arrive as `FileDialogFile` events, then `FileDialogClosed` (with the error, if any). Save and
    folder dialogs come when a feature needs them.
- **`file_system`** gains nothing in v1: copies read the file and write it with
  `writeFileAtomic`, and folder deletes are done by `asset_database` file by file.

### Mapping onto the ECS

The Content Browser is not ECS data: it is editor state over files. It touches the ECS in two
places only:

| Concept | Where |
|---|---|
| Opening a level | `ith.file.open_level` by id, as today |
| The open level as a user of an asset | Its current state in JSON form, walked by `collectAssetReferences` for the `{"asset": <uuid>}` shape, the same walk used for saved assets and sidecars (ADR-0049 rule 10) |

### Why this design is the simple one

- One view model serves the panel, the drawer and automation; the drawer is the same browser in
  another place, not a second one.
- Every operation is a database or file system call the engine already has, or a small addition
  to it; nothing is undoable, so there is no file-level undo machinery.
- Problems are the scan's own list, shown where the files are, with one fix each.
- A command that needs an answer returns a prompt instead of blocking, the pattern the File
  commands already use.
- Names follow one rule (`isValidContentPath` plus "no other entry with that name without ASCII
  case"), and one numbering scheme serves duplicates, new folders and Keep Both.
- Thumbnails, the watcher, sub-asset items and reference replacement wait until something needs
  them.

## 6. Open questions (resolved)

Every question is resolved by [ADR-0049](../decisions/0049-editor-content-browser-v1.md). Each
kept its draft recommendation below, followed by the resolution. Where the ADR chose differently
(questions 1, 5, 6 and 7), the sections above follow the ADR.

1. **Shortcuts that depend on the focused panel.** F2, Delete, Ctrl + D, Ctrl + A, Escape and
   Enter mean one thing in the Outliner and viewport and another in the browser, and ADR-0037
   rule 4 has no per-panel shortcuts in P2. *Recommendation:* amend rule 4 with one focus
   context: `Keymap.json` gains a `content` section whose chords win while the browser (panel or
   drawer) has the keyboard focus; the global chords apply otherwise.
   **Resolved by ADR-0049 rule 3, differently:** a third keymap context, Content, chosen per
   command id like the Play In Editor one; `Keymap.json` keeps its format (3.17).
2. **The drawer while the panel is open.** *Recommendation:* Ctrl + Space focuses the open panel
   and opens the drawer only when the panel is closed (3.16). Alternatives: always open the drawer
   with the shared state, or give the drawer its own state as a second browser.
   **Resolved by ADR-0049 rule 7:** as recommended.
3. **Deleting a folder with files the browser doesn't show.** *Recommendation:* delete the whole
   folder, as a file manager does, with the count of such files in the confirmation (3.11).
   Alternative: delete only assets and leave folders that still hold other files.
   **Resolved by ADR-0049 rule 8:** as recommended.
4. **Permanent delete or the recycle bin.** *Recommendation:* permanent in v1, with the
   confirmation; the recycle bin (Windows shell, the freedesktop trash on Linux) is platform work
   for after P2. Projects are expected to be under version control.
   **Resolved by ADR-0049 rule 9:** as recommended.
5. **Finding the users of an asset.** *Recommendation:* on demand when a delete is previewed, by
   reading every native asset and walking its reference fields through reflection, plus the open
   level's world; an index kept by the database comes when that proves slow. Imported files that
   refer to other files by path (a glTF's images) don't count as users in v1, because those paths
   are not stored in the sidecar.
   **Resolved by ADR-0049 rule 10, differently:** on demand, but by walking the raw JSON of every
   native asset, every sidecar and the open level's current state for the `{"asset": <uuid>}`
   shape, not through reflection, so references in unregistered component types are found too
   (3.11).
6. **Imports on the main thread.** *Recommendation:* synchronous on the main thread in v1, with a
   modal progress line and the log; `AssetDatabase` is single-threaded. Running importers on the
   background lane comes when large imports block the editor too long.
   **Resolved by ADR-0049 rule 11, differently:** synchronous on the main thread, but no frame is
   drawn during the import, so there is no live progress line: a notice "Importing <n> files…" is
   drawn the frame before (3.12).
7. **The file dialog.** Windows has a native dialog; Linux needs the desktop portal (D-Bus) or a
   helper program. *Recommendation:* an in-editor file picker built on `file_system` for both
   systems in v1 (one implementation, testable headless); native dialogs later.
   **Resolved by ADR-0049 rule 12, differently:** the platform's file dialog through SDL in
   `platform` (ADR-0012 rule 8); when no dialog can be shown, the status line says to drop files
   instead. No in-editor picker (3.12).
8. **Operating system drops.** `platform` has no drop events yet. *Recommendation:* add them
   (Win32 drop target; XDND on X11; the data device on Wayland when that backend exists) as a
   prerequisite step, with the drop position.
   **Resolved by ADR-0049 rule 13:** as recommended; the drop events are SDL's, translated by
   `platform` (rule 24), and SDL already implements the drop targets on Windows, X11 and Wayland.
9. **Dropped folders.** *Recommendation:* import them with their subfolders (3.12). Alternative:
   files only. **Resolved by ADR-0049 rule 14:** as recommended.
10. **Duplicate naming.** *Recommendation:* `<base>_<n>` from 2 (3.10). Alternative: `<name> Copy`.
    **Resolved by ADR-0049 rule 15:** as recommended.
11. **Sub-assets as items.** *Recommendation:* not in v1; an imported file is one item. Revisit
    when the viewport can place meshes (P3). **Resolved by ADR-0049 rule 16:** as recommended.
12. **Moving an imported file without the files it reads.** *Recommendation:* no special handling
    in v1; the next reimport reports the missing file. Alternatives: move them together, or warn
    before the move. **Resolved by ADR-0049 rule 17:** as recommended.
13. **Search scope.** *Recommendation:* the current folder and its subfolders while a search or
    filter is active (3.5). Alternatives: the current folder only, or a toggle.
    **Resolved by ADR-0049 rule 18:** as recommended.
14. **Session state.** *Recommendation:* current folder, view settings, tree expansion and sizes
    persist in `Saved/Editor/ContentBrowser.json`; search, filter and selection don't.
    **Resolved by ADR-0049 rule 19:** as recommended (3.18).
15. **Native assets from outside the project.** *Recommendation:* copy them in, keeping their id
    unless it is taken (3.12). Alternative: refuse them.
    **Resolved by ADR-0049 rule 20:** as recommended.
16. **Replace on import.** *Recommendation:* overwrite the existing source and reimport, keeping
    its id, settings and overrides, and only for an imported asset with the same extension.
    **Resolved by ADR-0049 rule 20:** as recommended.

## 7. Sources

Public documentation and common desktop conventions only. Epic Games' editor is named here only
to identify the inspiration.

**Unreal Engine (Epic Games):**

- [Content Browser](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-in-unreal-engine):
  what the browser is for (browsing, importing, organizing content), the content drawer on
  Ctrl + Space and its closing when it loses focus, docking the drawer as a panel, view styles.
- [Content Browser interface](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-interface-in-unreal-engine):
  the areas of the browser (folder sources, path bar, filters, search, asset view, settings).
- [Sources panel reference](https://dev.epicgames.com/documentation/unreal-engine/sources-panel-reference-in-unreal-engine):
  the folder tree beside the asset view.
- [Content drawer station](https://dev.epicgames.com/documentation/unreal-engine/2-16-content-drawer):
  opening the drawer from the bottom toolbar.
- [Content Browser, 4.27](https://docs.unrealengine.com/4.27/Basics/ContentBrowser): the browser
  before the drawer existed.

**Godot Engine documentation:**

- [File system](https://docs.godotengine.org/en/stable/tutorials/scripting/filesystem.html):
  doing moves, renames and deletes inside the editor so references survive; files moved outside
  the editor need fixing.

**Unity documentation:**

- [Project window reference](https://docs.unity.com/en-us/engine/6000.3/manual/unity-editor/editor-windows-views-reference/project-view):
  a folder tree and an asset view, search with a type filter, several types combining.
- [Classic search](https://docs.unity3d.com/Manual/search-classic.html): search by name with type
  and label filters in the Project window.

**Operating system conventions:**

- [Windows keyboard shortcuts](https://support.microsoft.com/en-us/help/12445/windows-keyboard-shortcuts):
  Alt + Up for the parent folder, and the other File Explorer keys.
- [Naming files, paths, and namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file):
  reserved characters and device names, names ending in a period or a space.
- The selection conventions of desktop file managers (click, Ctrl + click, Shift + click range,
  F2 rename, Delete with confirmation), as observed in common use.

**Ironthorn:** ADR-0003, ADR-0004, ADR-0005, ADR-0008, ADR-0012, ADR-0015, ADR-0017, ADR-0024,
ADR-0034, ADR-0035, ADR-0037, ADR-0040, ADR-0041 and ADR-0049 in [docs/decisions](../decisions/README.md).
