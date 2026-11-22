# Editor core

`engine/ith/editor_core` is the editor without its UI
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 1,
[ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md)): the
document and its world, commands, transactions and undo, selection, change counters, level files,
the keymap and automation. It includes no ImGui and opens no window, so everything here is
tested headless, under sanitizers too. `editor_ui` draws its state and calls its commands.

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
| `ith.entity.attach`, `ith.entity.detach` | `ids`, `parent` | Reparent or make roots, keeping the world placement |
| `ith.entity.move_before` | `id`, `before` (null: the end) | Reorder among siblings |
| `ith.entity.add_component`, `ith.entity.remove_component` | `ids`, `type`, `value` | Add (or replace) or remove a component |
| `ith.entity.set_property` | `ids`, `path`, `value` | Set one value by component path (`ith::LocalTransform.value.translation.x`); read-only fields are refused with `editor.command.read_only` |
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
`structure` (spawns, despawns, parents, order, component sets, labels), `values` (with
`takeTouched()`, the stable ids whose values changed), `selection` and `document`. A gizmo drag
bumps only `values`, so the Outliner doesn't rebuild.

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
runs the bound command of a key press; letter keys follow the user's layout, so Ctrl + Z is the key
that prints Z. While an interaction holds a transaction open, Escape cancels it
(`Transactions::cancelOpen`) and other keys go on to the consumers below.

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

- the transaction and history tests, and the command tests (every command with undo and redo,
  refusals, argument errors);
- the file tests (save and reopen with and without a job system, failed writes and opens, unsaved
  changes, busy refusals, opening by asset id) and the query tests;
- the keymap tests: chords, layers and conflicts, the engine's `Keymap.json` (every id registered
  or listed as coming with a later step), and the consumer;
- the automation tests: the matcher, script errors, every script in `tests/editor_core/scripts`
  run in a fresh project, and the **coverage test**, which fails when a registered command is run
  by no script. A new command therefore comes with a script step;
- a randomized model test: thousands of random commands mixed with undo and redo, checked against
  the level text after each entry, plus the P2 step 7 sequence (create, rename, reparent, edit,
  delete, undo all, redo all) compared by level text.
