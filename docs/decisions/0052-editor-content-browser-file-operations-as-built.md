# ADR-0052: Editor Content Browser file operations as built (amends 0049, 0050)

- **Status:** Accepted
- **Date:** 2026-11-27
- **Related:** amends [ADR-0049](0049-editor-content-browser-v1.md) rules 2, 6, 14, 22 and 28 and
  [ADR-0050](0050-editor-content-browser-browse-slice-as-built.md) item 1; the
  [Content Browser spec](../specs/content-browser.md) sections 3.12, 3.16, 3.17, 3.19 and 3.20;
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) (every action is a
  command); [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) rule 4 (file
  operations)

## Context

Slice 2 (ADR-0049 rule 28) lands the file operations: `new_folder`, `rename`, `move`,
`duplicate`, `delete`, `import`, `reimport` and `fix`
(`editor_core/{ContentFileCommands, ContentImportCommands, ContentCommandSupport, ContentPaths,
ContentCommands, ContentBrowserModel, EditorSession, Keymap}`, `file_system/FileSystem`,
`AssetDatabase::importers()`). Building and testing them showed gaps in the rules as written:

- ADR-0050 item 1 said the reserved chords (F2, Delete, Ctrl + D) stop being consumed once their
  commands exist. Then a user layer that unbinds `ith.content.delete` sends Delete in the browser to
  `ith.edit.delete`, which deletes level entities the user can't see.
- Spec 3.20 refuses only operations that touch the open level's file during a save. A Save As
  writes a path that is not yet the document's, so a rename, move or import onto that path races
  the background write.
- An import walks dropped folders with no bound on the number of entries, and follows links out of
  the dropped tree.
- The panel's Cancel on a prompt (inline rename, delete confirmation, import conflicts) had no
  command, so it could not be scripted, and `ith.content.dismiss` ignored a pending prompt.
- Whether a path is inside `Content/`, and whether an operation touches the open level, was decided
  by comparing path bytes. The same file can be spelled differently: drive-letter case, 8.3 short
  names and junctions on Windows, a project reached through a symlink on POSIX.
- Slice 2 as one commit would hold the commands, every UI path to them and a golden-image update.

Items 1 to 6 restate the affected rules as built, items 3 and 5 as new decisions; rules not
listed stand. The spec also records behaviour that needed no decision: 3.8 (rename by full name,
and the refusal when a sidecar is in the way), 3.11 (a problem entry's sidecar moves and is deleted
with it), 3.12 (folders merged in the plan and on disk, and the bounds on references) and 3.13
(`editor.content.not_imported`).

## Options considered

F2, Delete and Ctrl + D in the browser (item 1):

- **Drop the guard once the commands exist** (ADR-0050 item 1 as written). A rebound or unbound
  content command lets its chord fall back to the Editor context. Rejected: Delete then deletes
  entities.
- **Consume every chord the Content lookup misses.** Also blocks Ctrl + S, Ctrl + Space and every
  other editor-wide shortcut while the browser has focus. Rejected.
- **Keep a fixed list of owned chords, consumed after the Content lookup misses (chosen).** User
  layers can still rebind or unbind the content commands; only the three chords that would hit
  entity commands are held back.

Content operations during a save (item 2):

- **Refuse only operations touching the in-flight target** (track the Save As target path, its
  folder and its id). Exact, but every operation must test its sources, targets and folders against
  that path, and a missed case loses a file or gives two files one id.
- **Queue operations until the save completes.** Commands become asynchronous and automation must
  wait for them. Rejected.
- **Refuse every content file operation while any save is in flight (chosen).** The window is the
  length of one background write; one check, no tracking.

The import walk (item 3):

- **No bound.** A drop of a home folder or a drive stalls the main thread without end. Rejected.
- **A bound on the bytes copied.** Each file is already limited (`kMaxSourceFileSize`); the walk
  itself, the stats and the plan are what grow. Not enough alone.
- **A bound on the entries walked, top-level paths included (chosen).** The same kind of bound as
  ADR-0050 items 6 and 7, checked before anything is copied.
- **Follow links with cycle detection.** Copies files from outside the tree the user dropped, and
  needs a visited set of resolved paths. Rejected: links inside a dropped folder are skipped and
  reported; a link the user passes directly is imported.

Cancelling a prompt (item 4):

- **The panel clears the prompt itself.** Cancel then can't be scripted or tested headless, against
  ADR-0037. Rejected.
- **A separate `ith.content.cancel` command.** Escape then needs two bindings or a special case to
  choose between it and `dismiss`.
- **`ith.content.dismiss` clears a pending prompt first (chosen).** One command follows spec 3.16's
  Escape order, and Escape and Cancel issue the same thing.

Comparing paths (item 5):

- **Bytes after lexical normalisation** (as first built). Misses every case in the context list;
  an import from `c:\…` of a file inside `C:\…\Content` would copy it into itself.
- **Lexical normalisation plus case folding on Windows.** Fixes drive letters only; 8.3 names,
  junctions and symlinks still differ. Rejected.
- **A boolean "same or inside" test in `file_system`.** Answers whether a path is in `Content/`,
  but the caller still needs the content path, and comparing the open level's path with an
  operation's paths would need a resolve per pair. Rejected.
- **Map every path to a content path once, then compare content paths (chosen).**
  `fs::pathInside` resolves both paths and returns the rest as a content path spelled as on disk;
  content paths then compare with the database's spelling. Platform code stays in `file_system`;
  a resolve per path is cheap at the rate commands run.

Landing slice 2 (item 6):

- **One commit** (ADR-0049 rule 28 as written). The largest diff of the step, with the commands,
  the dialogs and the golden images reviewed together. Rejected.
- **Commands first, then the UI (chosen).** The commands are reviewed and tested headless on their
  own; the UI commit only calls them.

## Decision

1. **ADR-0050 item 1, owned chords.** F2, Delete and Ctrl + D (`kContentOwnedChords`) are consumed
   permanently while the browser has the keyboard focus. The consumer checks them after the
   Content lookup misses: a chord bound in Content runs its command; an owned chord that Content
   doesn't bind (a user layer rebound or unbound the command) runs nothing and never reaches the
   Editor context. `kContentReservedChords` and its "until slice 2" wording are gone. *Reason:* a
   key the browser uses must not also act on the level (ADR-0050 item 1's own reason); without the
   guard, unbinding `ith.content.delete` sends Delete to `ith.edit.delete` and deletes entities the
   user can't see.
2. **ADR-0049 rule 2 and spec 3.20, saving (widened).** While any save of the level is in flight
   (Save or Save As), every content file operation (`new_folder`, `rename`, `move`, `duplicate`,
   `delete`, `import`, `reimport`, `fix`) is refused with `editor.document.save_in_progress`, not
   only those touching the open level's file. Rescan stays refused as ADR-0050 item 5 says.
   *Reason:* a Save As writes a target other than the document's current path, so a rename, move or
   import onto that target could be overwritten by the background write, or two files could end up
   with one id; the window is short, and one rule is simpler than tracking the target.
3. **ADR-0049 rule 14 and spec 3.12, import bounds (new).** One `ith.content.import` walks at most
   `kMaxImportEntries` (4096) entries: every path in `files`, including paths that don't exist, and
   every file and folder found below a dropped folder. Past the bound the import fails with
   `editor.content.too_many_files` and nothing is copied. Links found while walking a dropped
   folder, to files or to folders, are not followed or copied; each is listed in `skipped` with the
   reason. A link passed directly in `files` (dropped or chosen on its own) is imported like the
   file it points to. *Reason:* bounded work on the main thread, the same kind of bound as ADR-0050
   items 6 and 7, and no copying of files from outside the dropped tree through links; a link the
   user picked or dropped on its own is the file the user chose explicitly.
4. **ADR-0049 rule 6, dismiss.** `ith.content.dismiss` clears a pending prompt (inline rename,
   delete confirmation, import conflicts) if there is one; otherwise it clears a non-empty
   selection; otherwise it closes the drawer when it is open. Each call does one of these. The
   panel's Cancel buttons and Escape issue it. *Reason:* every action is a command (ADR-0037), so
   Cancel can be scripted; the order is spec 3.16's Escape order.
5. **ADR-0049 rule 22, path comparison (new).** Whether a path is inside `Content/`, and whether an
   operation touches the open level's file or a folder holding it, is not decided by comparing
   path bytes. `fs::pathInside(path, folder)` resolves both paths: the longest existing prefix is
   resolved (the final path on Windows, `realpath` on POSIX; `.` is the last prefix tried for a
   relative path) and the remaining segments are appended lexically, case-folded on Windows, so
   destinations that don't exist yet compare correctly. It returns the rest as a content path
   spelled as on disk, and content paths are then compared with the database's spelling. The open
   level's file is mapped the same way (`EditorSession::levelContentPath`, computed before an
   operation). A dangling link resolves as its parent plus its name, and `..` is collapsed
   lexically, as in every `fs` call. `file_system` gains two functions, `pathInside` and
   `hasUnusableName` (a path from outside the engine whose name no `fs` function can address, such
   as a name holding `\` on POSIX), where ADR-0049 rule 22 said it gains nothing in v1.
   *Reason:* drive-letter case, 8.3 names and junctions on Windows, and symlinked projects on
   POSIX, spell one file in several ways.
6. **ADR-0049 rule 28, slice 2 in two commits.** The commands land first, tested headless. The
   panel's inline rename field, the delete and conflict dialogs, drag-and-drop moves, the menu and
   toolbar entries, the handling of operating-system drops and of the open-file dialog, and the
   status-bar Content Drawer button (ADR-0050 item 8) follow in the next commit. *Reason:* the
   commands are the contract every UI path calls (ADR-0037), and the UI commit makes the one
   golden-image update.

## Consequences

- The keymap, the save rule, the import walk, dismiss and the path checks match the code and the
  tests (`tests/editor_core/{ContentFileOperationTests, ContentBrowserTests, KeymapTests}`, the
  `content_browser` automation script).
- F2, Delete and Ctrl + D can never act on the level while the browser has focus, even with their
  content commands unbound. Binding an Editor command to one of them works only outside the
  browser.
- A rename or an import started during a save fails with `editor.document.save_in_progress` and
  must be repeated after it.
- A drop of more than 4096 entries imports nothing; the user drops smaller folders. Linked content
  in a dropped folder must be copied by hand.
- Each path check touches the disk once; fine per command, not to be called per frame or per item
  in the view.
- The spec's sections 3.12, 3.16, 3.17, 3.19 and 3.20 follow these items; ADR-0049 and
  ADR-0050 are not edited now. On acceptance they get an "Amended by" line naming this ADR and their
  README rows list it.
- **Revisit when:** a real project's imports need more than 4096 entries (one file per frame, then
  the background lane, as ADR-0049 says); saves get long enough that refusing operations is noticed
  (track the target); a user wants the owned chords for Editor commands inside the browser; the
  file watcher lands and paths arrive from outside the editor's commands; the Save As own-file
  check (`ith.file.save_as`) and the import's `choices` keys still compare resolved paths by bytes.
