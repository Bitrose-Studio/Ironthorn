# Editor core

`engine/ith/editor_core` is the editor without its UI
([ADR-0029](../decisions/0029-editor-ui-toolkit.md) rule 1,
[ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md)): the
document and its world, commands, transactions and undo, selection and change counters. It
includes no ImGui and opens no window, so everything here is tested headless, under sanitizers
too. `editor_ui` draws its state and calls its commands.

## The session

```cpp
#include "ith/editor_core/EditorSession.h"

ith::TypeRegistry types;
ith::generated::registerReflection_ecs(types);
ith::generated::registerReflection_editor_core(types);
// ... and the game's modules

ith::EditorSession session(types);
auto created = session.execute("ith.entity.create", args); // args: a serialization::Value object
```

An `EditorSession` owns one `EditorDocument` and the `CommandRegistry`, with the built-in
commands registered. `types` names the component types that commands may use. Everything is
main thread only.

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
| `Action` | changes editor state without its own transaction (undo, redo, selection) | while an interaction holds a transaction open |
| `Query` | reads only | never |

`canExecute` failures come back as `editor.command.disabled` with the reason. Arguments and
results are JSON objects (`serialization::Value`): entities by stable id, components by fully
qualified type name, values as `fromValue` reads them, unknown fields refused. Commands that take
`ids` use the selection when it is left out.

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
| `ith.edit.duplicate` | `ids` | Copies with new ids, each right after its original; they become the selection. Returns `{"ids"}` |
| `ith.selection.set` | `entities`, `mode` (`replace`, `add`, `toggle`, `remove`) | Change the selection |
| `ith.edit.select_all`, `ith.edit.select_none` | | Select every entity in Outliner order, or none |

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

## Tests

`tests/editor_core` holds the transaction and history tests, the command tests (every command with
undo and redo, refusals, argument errors) and a randomized model test: thousands of random
commands mixed with undo and redo, checked against the level text after each entry, plus the P2
step 7 sequence (create, rename, reparent, edit, delete, undo all, redo all) compared by level
text.
