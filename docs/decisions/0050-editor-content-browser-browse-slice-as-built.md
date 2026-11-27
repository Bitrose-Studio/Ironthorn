# ADR-0050: Editor Content Browser browse slice as built (amends 0015, 0029, 0034, 0049)

- **Status:** Accepted
- **Amended by:** [ADR-0052](0052-editor-content-browser-file-operations-as-built.md)
- **Date:** 2026-11-27
- **Related:** amends [ADR-0049](0049-editor-content-browser-v1.md) rules 2, 3, 4, 6, 7, 22 and 27,
  [ADR-0029](0029-editor-ui-toolkit.md) rule 7 (what the ImGui consumer consumes),
  [ADR-0015](0015-asset-identity-and-reimport.md) rule 6 (scan problems) and
  [ADR-0034](0034-level-documents.md) rule 1 (the level id); the
  [Content Browser spec](../specs/content-browser.md) sections 3.5, 3.14, 3.16 and 3.17;
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rule 4 (`ith.file.save_as`)

## Context

ADR-0049's first slice (rule 27) landed `AssetDatabase::folders()`, the `ContentBrowserModel`, the
browse commands and queries, the Content keymap context, the panel and the drawer
(`asset_database/AssetDatabase`, `editor_core/{ContentBrowserModel, ContentCommands, Keymap,
EditorSession}`, `editor_ui/{ContentBrowserPanel, ImGuiPlatform}`). Building and testing it showed
gaps in the rules as written:

- with the focus in the browser, End moved the asset view's focus item and also ran its Editor
  binding, Snap to Floor;
- a click into the browser followed by Delete in the same batch of events still saw last frame's
  focus, so Delete reached `ith.edit.delete` and deleted level entities;
- Escape in an open menu also ran a shortcut;
- the scan's recursion and the search text had no bound;
- a Save As of a level the database knows keeps the level's id, so two files share one id.

Items 1 to 8 restate the affected rules as built. Item 9 is a new decision, implemented in a
follow-up commit after acceptance. Rules not listed stand.

## Options considered

The focus a key sees when a click precedes it in the same batch:

- **Last frame's value only** (ADR-0049 rule 4 as written). The key goes to the wrong context; with
  Delete that destroys data. Rejected.
- **Route keys one frame late.** Queues input in the UI and adds a frame of latency to every
  shortcut. Rejected.
- **Hit-test the press against last frame's rectangles at once (chosen).** The UI consumer sees the
  press before later events are routed; the rectangles are a frame old, as the viewport's already
  are (ADR-0042 rule 8).

The level id after Save As (item 9):

- **Keep the id (as built).** Two files with one id: the next Rescan gives one of them a new id
  (ADR-0015 rule 6), which may be the file the open document is, so the document then saves under
  an id the database gives to the other file, and references to the original may move to the copy.
  Rejected.
- **The copy gets a new id; the open document keeps the original id and path** ("save a copy").
  Ids stay correct, but the command's contract is that the new file becomes the document's file
  (`ith.file.save_as`: "which becomes its file"). A separate command if ever wanted.
- **The copy gets a new id and the open document takes it (chosen).** The original file and its
  references are untouched; the copy is a new level, as a new file is a new asset under ADR-0015
  rule 1. It matches Unreal's observed Save Current Level As: the editor goes on editing the new
  map and the original stays as it was.

When Save As replaces another level file that already exists, the saved level takes over the
replaced file's id (item 9). A new id there would leave every reference to the replaced level
pointing at an id that no file has any more.

## Decision

1. **ADR-0049 rules 3 and 6, the widget keys.** While the browser has the keyboard focus, the
   asset view's widget chords (`kContentWidgetChords`: Up, Down, Left, Right, Home and End, alone
   and with Shift, and Ctrl + F) are consumed by the keymap consumer without running anything;
   they never fall back to the Editor context. F2, Delete and Ctrl + D are bound in the Content
   context (`ith.content.rename`, `.delete`, `.duplicate`). Until slice 2 registers those commands,
   the keymap consumer consumes these three chords (`kContentReservedChords`) while the browser has
   the keyboard focus, without running anything and whatever the keymap binds: a user layer that
   unbinds them can't let them reach the Editor context and its entity commands. *Reason:* a key the
   browser uses must not also act on the level.
2. **ADR-0049 rule 4, the focus between frames.** `editor_ui` still reports the focus at the end of
   every UI frame, together with the panel's and the drawer's rectangles
   (`ContentBrowserModel::setScreenRects`). A mouse press is hit-tested against those rectangles
   as soon as the UI consumer sees it (`pointerPressed`): inside gives the browser the focus,
   outside takes it, before any later event in the batch is routed. `ith.content.toggle_drawer`
   gives the focus to the drawer it opens and takes it when it closes it; `ith.content.dismiss`
   closing the drawer, and a level opened from the drawer, take it. *Reason:* click then Delete in
   one batch deleted level entities.
3. **ADR-0029 rule 7, popups keep their keys.** The ImGui consumer also consumes key presses while
   any ImGui popup is open (a menu, a context menu, a modal), editor-wide, not only for the
   browser. ImGui still gets every key; the Play In Editor keys sit above the ImGui consumer and
   are unaffected. *Reason:* Escape that closes a menu must not also clear a selection, and a
   modal's Enter must not open an asset.
4. **ADR-0049 rule 7, one browser on screen.** When the panel is in the layout (opened by any
   route, not only Ctrl + Space), the drawer closes the same frame; the two are never drawn
   together. *Reason:* Open Panel and layout loads bypass `toggle_drawer`.
5. **ADR-0049 rule 2 and spec 3.14, saves and Rescan.** A level saved under `Content/` to a path
   and with an id the database doesn't have (a new level's first save) shows in the browser at
   once: the save's completion rescans. Rescan is refused while a save is in flight
   (`editor.document.save_in_progress`) and, as every Action, while a level opens. *Reason:* a
   first save must not need a manual Rescan, and a scan made before the write lands misses the file
   and races the save's own update of the document path.
6. **ADR-0015 rule 6 and ADR-0049 rule 22, scan depth.** The scan enters at most
   `kMaxFolderDepth` (64) nested folders below `Content/`. A deeper folder is an `InvalidFile`
   problem, not entered and not in `folders()`. *Reason:* everything that walks the folders (the
   tree, search, the recursive scan itself) stays bounded on a malformed or hostile tree.
7. **Spec 3.5, search text.** The search text is clamped to `kMaxContentSearchSize` (255 bytes),
   cut at a UTF-8 character boundary. *Reason:* a bounded matching cost per item, and no broken
   characters from a cut.
8. **ADR-0049 rule 27 and spec 3.16, the status-bar button.** The Content Drawer button in the
   status bar moves to slice 2, since it changes the shell's golden images; Ctrl + Space and the
   command work in slice 1. *Reason:* one golden-image update with slice 2's toolbar changes.
9. **ADR-0034 rule 1, Save As (new).** The level id that `ith.file.save_as` gives the saved level
   depends on the target path:
   - **a new path:** the copy gets a **new random level id**; the original file keeps its id and
     its references;
   - **another level file that already exists** (after the overwrite prompt): the saved level
     **takes over that file's id**, so references to the overwritten level keep working and now
     get the new content; the document's original file keeps its own id;
   - **the document's own path:** a Save; the id is unchanged.

   A level that has never been saved keeps its id on a new path (it is the level's first file).
   The open document takes the resulting id and the path when the write succeeds; a failed write
   leaves the id and the path unchanged. The level's viewport cameras follow the resulting id.
   Entity stable ids are copied, which ADR-0034 rule 15 already allows. This is not a regeneration
   in the sense of ADR-0015 rule 19 and ADR-0034 rule 1: a new path is a new asset, and an
   overwritten file keeps its identity. *Reason:* one id per file without a Rescan having to guess,
   and references keep pointing at the file they were made to.

## Consequences

- ADR-0049's keymap and focus rules, and the spec, now match the code and the tests
  (`tests/editor_core/{ContentBrowserTests, KeymapTests, AutomationTests}`,
  `tests/editor_ui/ContentBrowserPanelTests`).
- Every popup now blocks shortcuts while open, including the unsaved-changes question and menus;
  a shortcut needs the popup closed first.
- The focus rectangles are a frame old: a press on a window that moved in the last frame may land
  in the wrong context once.
- Folders nested deeper than 64 levels are invisible in the browser until flattened; the problem
  list names them.
- Item 9 needs a follow-up commit: the id change in `EditorSession` and `ith.file.save_as`, the
  camera entry carried over, and tests (Save As of a known level, then Rescan, finds no duplicate;
  Save As over another level takes its id; a failed write keeps the id). Until then a Save As of a known level waits for a Rescan, which may
  give either file a new id.
- ADR-0015, ADR-0029, ADR-0034 and ADR-0049 list this ADR under "Amended by".
- **Revisit when:** another panel gets a keymap context (the widget-chord list generalises per
  panel); a popup needs a shortcut while open; real projects nest folders deeper than 64; a "Save
  a Copy" command is wanted (item 9's second option).
