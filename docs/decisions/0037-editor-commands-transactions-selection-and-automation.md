# ADR-0037: Editor commands, transactions, selection and automation

- **Status:** Accepted
- **Amended by:** [ADR-0041](0041-editor-amendments-after-outliner-and-details.md),
  [ADR-0042](0042-editor-viewport.md), [ADR-0048](0048-editor-viewport-translate-gizmo-as-built.md),
  [ADR-0049](0049-editor-content-browser-v1.md), [ADR-0053](0053-play-in-editor-and-simulate.md),
  [ADR-0054](0054-play-in-editor-as-built.md)
- **Date:** 2026-11-22
- **Related:** amends [ADR-0033](0033-reflection-property-paths-and-patches.md) rule 12
  (`applyPatches`), [ADR-0032](0032-entity-references-stable-ids-and-world-copy.md) (stable ids
  name entities in the undo history) and [ADR-0008](0008-editor-familiar-to-unreal-developers.md)
  rule 9 (a deliberate difference); [ADR-0017](0017-runtime-and-editor-separation.md) rules 8 and
  10–13 (editor-only data, Play In Editor, Keep Simulation Changes),
  [ADR-0029](0029-editor-ui-toolkit.md) rules 1, 2, 7, 12, 15, 18 and 19 (module boundary,
  routing, idle policy, layouts, tests),
  [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md) (hooks),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) (reparenting keeps
  the world placement), [ADR-0034](0034-level-documents.md) (deterministic level text),
  [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) (keymap, durable writes,
  main-thread queue)

## Context

P2 step 7 builds `editor_core`, the first editor module: commands, transactions and undo,
selection and the automation runner, with no ImGui (ADR-0029 rules 1 and 2). The P2 exit
criterion is "every action scripted in tests".

Accepted ADRs already give:
- `World::applyPatches` with exact inverses (ADR-0033 rule 12);
- `StableId` and `World::findByStableId` (ADR-0032 rules 6–8);
- deterministic level text (ADR-0034 rule 9);
- layered configuration with `Keymap.json`, whose command ids are provisional until this step
  (ADR-0035 rule 21);
- the main-thread queue (ADR-0035 rules 22 and 23).

Three gaps remain:
1. `applyPatches` can't spawn or despawn an entity.
2. Its inverses don't hold what hooks change. Undoing a reparent restores `LocalTransform`
   first, then the `ChildOf` hook recomputes it from the wrong parent; and even in the right
   order, a value recomputed through a matrix inverse and a decomposition isn't bit-exact.
3. Handles change when undo spawns an entity again, so a history that stores handles goes
   stale.

Prior art, from public documentation and observed behaviour:
- **Unreal:** editor transactions are named and listed in an Undo History panel; scoped
  transactions nest, and only the outermost makes an entry; the transaction buffer is limited by
  memory ([`UTransBuffer`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/UTransBuffer));
  Python editor scripting records named transactions in the same history
  ([Scripting the Unreal Editor Using Python](https://dev.epicgames.com/documentation/en-us/unreal-engine/scripting-the-unreal-editor-using-python));
  selecting actors is itself an undoable transaction.
- **Esoterica** (MIT; ideas only, no code): each undo action stores the selection before and
  after; RAII transaction scopes nest; Details edits a working copy; change events are split
  into structure, selection and values; the gizmo is a pure function.

## Options considered

Spawn and despawn in undo:
- **Spawn and Despawn operations in `applyPatches`** (chosen): one validator keeps every change
  all-or-nothing, and the inverse comes from the ECS like every other one.
- **Steps sequenced by the editor** (`spawn`, `add`, `placeBefore` outside `applyPatches`):
  smaller in `ecs`, but the editor repeats the ECS checks, and a failure halfway leaves a
  half-built world.

Undo entries:
- **Inverse patches naming entities by stable id** (chosen).
- **Entity snapshots:** exact, but they reverse ADR-0033's choice and copy whole entities for a
  one-field edit.

Hook side effects:
- **The ECS captures and restores what hooks change on the same entity** (chosen).
- **The editor writes `LocalTransform` explicitly after `ChildOf`:** fails, because an inverse
  runs in reverse order, so the `ChildOf` hook runs last and overwrites the value.

Selection and undo:
- **Selection rides along with each entry** (chosen, the owner's decision), as in Esoterica:
  clicks don't fill the history, and undo still restores the selection.
- **Selection changes are entries of their own**, as in Unreal.

Change notification:
- **Generation counters polled each frame** (chosen), as `Config` does (ADR-0035 rule 19).
- **Callbacks:** reentrancy and lifetime hazards, for no gain.

Automation format:
- **A C++ API plus JSON scripts run by one runner** (chosen).
- **C++ only:** the editor's `--automation` run and a later MCP adapter would have no data form.
- **JSON only:** view-model and interaction tests become awkward.

## Decision

### Commands

1. **Registry.** `CommandRegistry` belongs to one editor session and is used on the main thread
   only (asserted). A `CommandDesc` has:
   - `id`: stable, `ith.<area>.<verb>` in lower snake case, never reused;
   - `displayName` and `tooltip`, in ADR-0008 vocabulary, and `category`, its menu;
   - `readOnly`: a query, which never records and is allowed during Play In Editor;
   - `canExecute(EditorContext&) -> Expected<void>`; the error is the disabled reason, shown as
     the tooltip and returned to automation;
   - `execute(EditorContext&, const serialization::Value& args) -> Expected<serialization::Value>`.

   Menus, the toolbar, shortcuts, panels and automation all call `registry.execute(id, args)`;
   nothing else changes the editor world. `editor_ui` registers its commands (layouts, Open
   Panel) in the same registry.
2. **Arguments and results are JSON values** (`serialization::Value`) at the boundary.
   - `registerCommand<Args, Result>` converts reflected structs with `fromValue` and `toValue`
     (ADR-0033 rule 7).
   - Entities are named by stable id and assets by `AssetId`; a handle or pointer never appears
     in arguments or results.
   - A value whose type is known only at run time (Set Property's `value`) is read raw and
     converted to the leaf type of the parsed path.
   - Errors are `ith::Error`s with codes `editor.command.unknown`, `editor.command.disabled`,
     `editor.command.invalid_args`, `editor.command.playing` and
     `editor.command.interaction_open`, or the failing layer's code.
3. **Each execution is a transaction scope** named by `displayName` (rules 5 and 6). An error
   from `execute` cancels the scope, so a failed command changes nothing. A command that records
   nothing leaves no entry. Undo, Redo and read-only commands open no scope.
4. **Command ids.** The 13 ids in `Keymap.json` are confirmed. Added, with chords:

   | Id | Chord | | Id | Chord |
   |---|---|---|---|---|
   | `ith.edit.undo` | Ctrl+Z | | `ith.file.new_level` | Ctrl+N |
   | `ith.edit.redo` | Ctrl+Y | | `ith.file.open_level` | Ctrl+O |
   | `ith.edit.delete` | Delete | | `ith.file.save` | Ctrl+S |
   | `ith.edit.rename` | F2 | | `ith.file.save_as` | Ctrl+Alt+S |
   | `ith.edit.select_all` | Ctrl+A | | `ith.play.play` | Alt+P |
   | `ith.edit.select_none` | Escape | | `ith.play.simulate` | Alt+S |

   Registered without a chord: `ith.edit.delete_with_children`; `ith.entity.create`, `.attach`,
   `.detach`, `.move_before`, `.add_component`, `.remove_component` and `.set_property`;
   `ith.selection.set`; `ith.play.keep_simulation_changes`; ADR-0029 rule 15's layout commands;
   the queries of rule 17.
   - A keymap entry naming an unknown command warns `editor.keymap.unknown_command`.
   - A chord bound to two commands warns; the higher layer wins, and within a layer the lower id.
   - A test checks that the engine `Keymap.json` names only registered commands.
   - Shortcuts have no per-panel context in P2. In Play In Editor, Escape reaches `ith.play.stop`
     first (ADR-0029 rule 7).

### Transactions and undo

5. **An undo entry** is `{name, ids, patches, selectionBefore, selectionAfter, bytes}`.
   - In the stored patches every entity, the patch's own and every `Entity` inside a value, is a
     world-id-0 placeholder indexing `ids`, a list of stable ids. Values are converted with
     ADR-0032's `remapEntities`.
   - An entity without a `StableId` is refused as a target (`editor.no_stable_id`); a reference
     to one is stored as null, with a warning. The editor world has such entities only when hooks
     make helpers, and the same hooks make them again.
   - Applying an entry maps each placeholder through `findByStableId`, or to the ECS placeholder
     of a Spawn in the same entry (rule 8).
   - An entry stores one direction: undo applies it and keeps the inverse that `applyPatches`
     returns, for redo, and redo does the reverse.
6. **One transaction per interaction.**
   - `Transactions::begin(name)` returns a move-only RAII `TransactionScope`. `apply(patches)`
     applies at once (live preview) and merges the inverse in (rule 7); `commit()` ends it;
     `cancel()`, or destroying the scope without `commit()`, applies the collected inverse and
     records nothing.
   - **Nested scopes join the outermost.** An inner `commit()` does nothing; an inner `cancel()`
     cancels the whole transaction.
   - While an interaction is open, the keymap consumer turns Escape into `cancel()` and ignores
     other chords, and `execute` refuses commands that change the world with
     `editor.command.interaction_open`.
   - Details drags and gizmo drags (started, manipulating, stopped) begin, update and commit one
     transaction. An Alt + drag duplicate is a nested scope inside the drag's transaction.
   - Every interaction has a one-shot command (`ith.entity.set_property`, `ith.edit.duplicate`)
     that records the same entry.
   - A rollback that fails is a bug: it asserts, logs `editor.undo.failed` and clears the
     history.
7. **Merging, within one transaction only.** A newer inverse is put in front of the older ones;
   then a `set` whose entity, component and path an older `set` also covers is dropped, which
   keeps the first inverse and the last patch (ADR-0033 consequences). Separate commands are
   never merged by time in P2.
8. **ECS additions** (amend ADR-0033 rule 12):
   - **(a) `EntityPatch::op`** is `Modify` (today's behaviour), `Spawn` or `Despawn`.
     - **Spawn:** its `entity` is a world-id-0 placeholder that the other patches of the call may
       name; its changes are Adds only; `placeBefore` names a sibling or end. The World spawns,
       adds `ChildOf` first, then the other components in order, places the entity, and remaps
       the call's placeholders to the new handle.
     - **Despawn:** refused with `ecs.patch.has_children` while the entity has children. Its
       inverse is a Spawn with every component the World doesn't keep itself (Transient ones
       included, so `UnresolvedComponents` survives) and its next sibling.
   - **(b) Hook side effects on the same entity.** For an entity whose changes include an Add, a
     Remove or a patch of an immutable component, `applyPatches` first copies its components that
     the changes don't name. After the commit, the inverse gains an Add of the old value, or a
     Remove, for each such component that hooks changed (by its changed tick), added or removed.
     These come after the inverses of the named changes, so on undo they run after the hooks.
   - **(c) Contracts.** Hooks that change non-Transient components of **other** entities make
     undo inexact: engine hooks don't, and components the editor edits must not. Systems that run
     in the editor world write only Transient data.
9. **History limits.** At most 500 entries and 256 MiB, both Editor Preferences settings; the
   oldest entries are dropped first. An entry's size is estimated through reflection (type
   sizes, container element counts, string lengths). The newest entry is always kept, with a
   warning when it alone is over the budget. Committing after an undo drops the redo entries.
10. **Lifetime of the history.**
    - **Save keeps it.**
    - **New Level, Open Level and reloading the level clear it.**
    - **Play In Editor:** the editor history is kept. Nothing is undoable while playing: undo and
      redo are refused, edits to the play world are not recorded, and commands that change the
      editor world are refused with `editor.command.playing`, except Keep Simulation Changes,
      which records one entry in the editor history (ADR-0033 rule 16).

### Selection

11. **`Selection`** is an ordered list of entity stable ids without duplicates.
    - The **last is primary**: the gizmo pivot and the Details focus.
    - It changes through `ith.selection.set {entities, mode}` (`replace`, `add`, `toggle`,
      `remove`), `ith.edit.select_all` and `ith.edit.select_none`.
    - Ids that no longer resolve are pruned when a transaction commits and after undo and redo.
    - Component selection (`{entity, component type}`) comes with the Details step. Asset
      selection belongs to the Content Browser's view model and is not undoable.
12. **Selection rides along with undo entries.** Each entry records the selection before and
    after; undo restores before, redo restores after. Changing the selection alone makes no
    entry.

### Change notification

13. **`EditorChanges`** keeps four generation counters, polled once per frame:
    - `structure`: spawn, despawn, parent, order, the set of components, the Outliner label;
    - `values`: component value edits, with the stable ids touched since the last poll;
    - `selection`;
    - `document`: path, dirty state, load.

    Commits, undo and redo bump them by the kind of each patch: Spawn, Despawn, `ChildOf`, Add
    and Remove count as `structure`; a Patch counts as `values`. There are no ECS observers.
    View models rebuild only on their counter, so a gizmo drag never rebuilds the Outliner.
    Details refreshes its working copy when `values` names one of its entities and it has no
    interaction open. Any change requests a frame (ADR-0029 rule 12).

### The editor document

14. **`EditorDocument`** owns the editor `World`, the open level's `AssetId` and path, the
    history, the selection and the save state. One level is open at a time in P2. The Outliner
    label is `ith::EditorLabel { std::string name; }`, `EditorOnly`, declared in `editor_core`
    (ADR-0017 rule 8).
15. **Dirty state.** The document is dirty when the history position differs from the saved
    marker, so undoing back to the saved point makes it clean. A saved marker that was dropped
    from the history keeps the document dirty until the next save. New Level, Open Level and
    Quit on a dirty document fail with `editor.document.unsaved_changes` unless the arguments
    carry `unsaved: "save"` or `"discard"`. The UI asks Save / Don't Save / Cancel and issues the
    command again; automation passes the argument.
16. **Save and open.**
    - `saveLevel` runs on the main thread, which owns the world, and the saved marker moves to
      the current position. `writeFileAtomic` (durable) runs on the background lane, and its
      result returns through the main-thread queue (ADR-0035 rule 8).
    - A failed write restores the marker and is logged and returned.
    - A save while another is writing fails with `editor.document.save_in_progress`; Quit waits
      for the write.
    - Open reads the file on the background lane and runs `loadLevel` on the main thread.
      Opening by `AssetId` resolves the path through `AssetDatabase`.

### Automation

17. **Queries** are read-only commands: `ith.query.document`, `ith.query.selection`,
    `ith.query.entity {id}` (components as JSON through `toValue`, entities as stable ids),
    `ith.query.find {label}`, `ith.query.history` (names, position, saved marker) and
    `ith.query.world_hash` (a hash of `saveLevel`'s deterministic text).
18. **C++ API.** `EditorSession` builds a document, registry and configuration from explicit
    directories, headless. `Automation::run(id, args)` runs a command or query;
    `Automation::waitIdle()` drains the main-thread queue until no editor job is pending.
    View-model and interaction tests use this API.
19. **JSON scripts.** An `ith.automation_script` document, version 1, in the ADR-0010 envelope.
    - `steps` holds `{command, args, expect, expectError}` (the last two optional) and
      `{wait: "idle"}`. `expect` matches a subset of the result: object keys, with arrays and
      scalars compared exactly.
    - There are no variables: `ith.entity.create` takes an optional `id`, so scripts are
      deterministic.
    - The runner stops at the first failure and reports the step index, the JSON path and the
      error; it exits 0 or 1.
    - The same runner serves the headless `editor_core` tests in CTest and the editor's
      `--automation <script>` run (ADR-0029 rule 18). A later MCP adapter calls
      `Automation::run`.
20. **"Every action scripted":** every action is a registered command; a test fails when a
    registered command id appears in no automation test script; the step 7 suite spawns,
    renames, reparents, edits, deletes, undoes all and redoes all, comparing `world_hash`, plus a
    randomized do, undo and redo model test.

### Module

21. **`editor_core`**, layer `editor`, kind `editor`, depends on `foundation`, `math`,
    `reflection`, `serialization`, `ecs`, `level`, `config`, `file_system`, `jobs`, `input` and
    `platform_events` (the keymap consumer is an `InputConsumer`), `assets` and `asset_database`
    (`tools` kind, allowed for `editor`). It doesn't depend on `platform`, `rhi`, `render_graph`
    or ImGui, so it builds and tests headless and under sanitizers (ADR-0029 rule 19). Later
    steps add the tree view model, Details view models, the gizmo as a pure function and the Play
    In Editor world copy.

### Deliberate difference from Unreal

22. **ADR-0008 rule 9 gains a row:**

    | Area | Unreal Engine | Ironthorn | Why |
    |---|---|---|---|
    | Selection and undo | Selecting is its own undo step | Selection is restored with each undo step, never undone alone | Clicks don't fill the history |

## Consequences

- One path changes the editor world, so undo, scripting and the later MCP adapter can't drift
  apart. Selection and undo survive despawn and respawn.
- `ecs` gains Spawn and Despawn operations and hook side-effect capture in `applyPatches`, with
  tests: a reparent undone and redone bit for bit, Despawn refused with children, placeholders
  across one call.
- Structural edits copy the entity's other components once; value edits cost what they cost
  today.
- Hooks that write non-Transient data of other entities are a documented undo hazard.
- Edits made while playing can't be undone, and separate commands never merge.
- Follow-up work: the `ecs` additions; `editor_core` with the registry, transactions, selection,
  document, keymap consumer, queries and runner; the added chords in `engine/config/Keymap.json`
  with its header comment updated; the command-coverage test; the ADR-0008 rule 9 row.
- **Owner's answers at acceptance (2026-11-22):** selection is not undoable on its own and rides
  along with each entry, a documented difference from Unreal; nothing is undoable while playing;
  `applyPatches` gains Spawn and Despawn and hook side-effect capture; the history is limited to
  500 entries and 256 MiB as Editor Preferences; the keymap additions are accepted. The rest
  follows the engineer's recommendation, accepted by the owner.
- **Revisit when:**
  - a command needs merging across separate executions (nudge keys);
  - edits made while playing need undo;
  - several levels are open in one world;
  - shortcuts need per-panel contexts;
  - the history's size estimate proves far from real memory use.
