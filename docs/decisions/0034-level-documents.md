# ADR-0034: Level documents

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** completes [ADR-0015](0015-asset-identity-and-reimport.md) rule 18 (levels),
  [ADR-0024](0024-asset-document-envelope-and-extension.md) (`.ithmap`),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) rule 6 (`Children`
  rebuilt from saved order) and [ADR-0032](0032-entity-references-stable-ids-and-world-copy.md)
  rule 5 (entity mapping context); [ADR-0010](0010-serialization.md) and
  [ADR-0020](0020-serialization-rules-completed.md) (envelope, stable output, migrations),
  [ADR-0014](0014-ecs-storage-and-world.md) rule 4 (load clears and refills; a failed load leaves
  the world as it was), [ADR-0017](0017-runtime-and-editor-separation.md) rule 9 (`editor`
  section), [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md) (hooks),
  [ADR-0033](0033-reflection-property-paths-and-patches.md) (override records, `toValue` and
  `fromValue`)

## Context

The editor shell must build, save, reload and play a scene without anyone touching files by hand.
`.ithmap` and the format name `ith.level` exist, but the level has no schema, reader or writer.
Accepted ADRs leave three points to this one:

- how `Children` is rebuilt from the saved order (ADR-0031 rule 6);
- what the entity mapping context writes (ADR-0032 rule 5);
- what happens to an entity without a stable id, and to an id that is not found.

Prior art, from public documentation:

- **Unreal:** a level is one binary package (`.umap`). With World Partition, One File Per Actor
  saves each actor to its own external file, so editing an actor doesn't need the level file
  checked out. Its documented costs are encoded file names in changelists and partial submits
  that can leave dangling references. Actors are embedded back into their level when cooked
  ([One File Per Actor](https://dev.epicgames.com/documentation/en-us/unreal-engine/one-file-per-actor-in-unreal-engine)).
- **Bevy:** a `DynamicScene` holds a list of resources and a list of entities, with components
  identified by type path. Writing it into a world first spawns an entity for each scene entity,
  then remaps the entity references inside components through an entity map
  ([dynamic_scene.rs](https://docs.rs/bevy_scene/latest/src/bevy_scene/dynamic_scene.rs.html)).
- **Godot `.tscn`:** a text format meant to be readable and easy for version control. Each node
  names its parent by path, properties at their default value are not stored, and from 4.6 nodes
  have a `unique_id` that survives moves and renames
  ([TSCN file format](https://docs.godotengine.org/en/stable/engine_details/file_formats/tscn.html)).

## Options considered

Document shape:

- **Ordered entity records, components keyed by type name** (chosen). The file order is the
  Outliner order, and each record is self-contained, so a later split into one file per entity
  can store the same record.
- **Entities keyed by id in an object.** The order is lost, and the Outliner order would need a
  second structure.
- **A dedicated `parent` key per record**, as in Godot. It duplicates `ChildOf` (its
  `despawnWithParent` too) and needs special code on both sides.

Component values:

- **Full values through `toValue`** (chosen, the owner's decision). No new code, and changing a
  default in C++ never silently changes existing levels.
- **Only the fields that differ from the default**, as Unreal and Godot do. Smaller files, and a
  new field doesn't touch every entity on the next resave. But a default changed in code changes
  every level, and saving needs a default value and a comparison per type.

Files:

- **One file per level** (chosen for P2). Text JSON merges in git, the editor has one save path,
  and there are no partial commits.
- **One file per entity**, like One File Per Actor. Its main gain, avoiding exclusive checkout of
  a binary level, doesn't apply to text in git. It costs a folder format, a scan and validation
  of partial commits.

Loading:

- **Through `add`, with hooks, after full validation** (chosen). Engine modules build Transient
  state in hooks (ADR-0016 rule 4); that state isn't saved, so a load without hooks would miss it.
- **Without hooks, like `copyWorld`.** A copy carries the Transient state; a load can't.

Entities without a stable id:

- **Not saved, with a warning** (chosen, the owner's decision). By ADR-0032 rule 7 they are
  runtime-only: the editor, level load and prefab instantiation always assign ids.
- **Assign one at save time.** Saving would mutate the world it reads, and an id not written back
  would be new on every save, which changes the file each time.
- **Refuse the save.** A helper entity would block saving the user's work.

Unknown component types:

- **Kept as raw data and written back** (chosen, the owner's decision), as a missing prefab's
  overrides are kept (ADR-0015 rule 17). A disabled plugin or a rename without `FormerName` must
  not erase a component from a whole level on the next save.
- **Warned and dropped**, as unknown fields are (ADR-0010 rule 13). Acceptable for a field; a
  whole component lost from every entity is not.

## Decision

### Schema

1. **Envelope** (ADR-0024): `{"format": "ith.level", "version": 1, "id": <level asset id>,
   "data": {"entities": [...]}}`. The level id is created with the level and never regenerated.
2. **Entity record:** `{"id": <stable id>, "components": {...}, "editor": {...}}`.
   - `components` maps a component's fully qualified type name to its full value as `toValue`
     writes it.
   - `editor` holds the components whose type is EditorOnly, in the same form (ADR-0017 rule 9),
     and is left out when empty. The cooker drops it without knowing its types.
   - `ith::StableId` is the record's `id` and is not repeated in `components`.
   - Transient components are not written (`Children`, `WorldTransform`, `DisabledInHierarchy`).
     `ith::Disabled` is an ordinary component.
3. **Hierarchy.** The parent is the `ith::ChildOf` component, whose `parent` is written as a
   stable id (rule 7). Records are in Outliner order: each root in root order, followed
   depth-first by its children in `Children` order. Loading rebuilds `Children` and the root order
   by appending (rule 12), which confirms ADR-0031 rule 6.
4. **World resources are not saved** in version 1. They are application or session state (time,
   input, network role, match rules registered by game modules; ADR-0016).
5. **Prefab instances** (after P2) will be records in the same `entities` array, so the Outliner
   order holds. Such a record has a `prefab` key (the prefab's `AssetRef`), the instance root's
   components, `overrides` keyed as ADR-0033 rule 15 says, the added entities and the removed
   prefab entity ids. Instance entities get derived stable ids (ADR-0015 rule 14), so the entity
   mapping doesn't change. Adding instances **bumps the format to version 2**, so an older editor
   refuses such a file instead of dropping the unknown key on save (ADR-0010 rules 12 and 15).

### Entity mapping context (ADR-0032 rule 5)

6. **An entity without a `StableId` is not saved.** The save reports one `level.unsaved_entity`
   warning with their number. Its saved children are written in its place as roots, without
   `ChildOf`, with a `level.dropped_parent` warning.
7. **Saving writes an entity as its stable id.** A reference to an entity that is not saved (no
   stable id, stale, of another world) is written as the nil id, with a `level.unsaved_reference`
   warning naming the referencing entity and the component path. A reference never fails a save.
8. **Loading maps an id to the entity of that record.** An id that names no record of the document
   reads as the null handle, with a `level.unknown_reference` warning. A `ChildOf` whose parent is
   unknown or nil is dropped and the entity loads as a root, with a `level.dropped_parent` warning.

### Text, binary and stable output

9. **Source levels are text JSON**, written and read through `writeDocument` and `readDocument`.
   **The same world always gives the same bytes:**
   - records in Outliner order;
   - keys in `components` and `editor` sorted by type name, bytewise;
   - fields in declaration order;
   - floats in the shortest round-trip form (ADR-0020 rule 4);
   - two-space indentation, LF line endings, a final newline;
   - no timestamps, user names or paths (ADR-0015 rule 19).
10. **Cooked levels are binary,** defined with the cooker: entity records with each component's
    type id and its binary-archive value, schema hashes over every component type, no `editor`
    section, and every unresolved component (rule 13) a cook error. P2 reads and writes text only;
    Play In Editor copies the editor world and reads no file.

### Module and API

11. **The `level` module** (layer `core`, kind `runtime`) depends on `foundation`, `reflection`,
    `serialization` and `ecs`, so `ecs` stays free of serialization and `serialization` free of
    `ecs`. `kLevelFormat` moves into it from `asset_database`, which then depends on `level`. The
    cooked reader joins this module later.
    - `ith::saveLevel(const World&, const Uuid& levelId, const LevelSaveOptions&)
      -> Expected<std::string, serialization::Error>`. The options take a warnings list. Writing
      the file stays with the caller.
    - `ith::loadLevel(std::string_view text, World&, const TypeRegistry&, const LevelLoadOptions&)
      -> Expected<LevelInfo, serialization::Error>`. The options are those of `TextReadOptions`
      that apply: `source`, `unknownFields`, `warnings` and `allocationBudget`. `LevelInfo` holds
      the level id and the number of entities.
    - Preconditions (asserted): the world is not being iterated and no system runs.
12. **Loading is all-or-nothing.**
    - **Validate.** Read and migrate the envelope. Check that every record has an id, not nil and
      unique (`level.invalid_id`, `level.duplicate_id`), and that the parents form no cycle and no
      entity is its own parent (`level.parent_cycle`). Read every component into an `OwnedValue`
      through `fromValue`, with a mapping that gives document-local placeholder handles. These
      problems are errors and leave the world untouched; data from a file never reaches an ECS
      assert.
    - **Build,** which can't fail:
      - `world.clear()` (ADR-0014 rule 4). The world needn't be fresh: generations are kept, so
        old handles stay stale, and its resources are kept;
      - spawn every record with its `StableId`, in file order;
      - `remapEntities` (ADR-0032 rule 4) turns the placeholders into the new handles;
      - record by record, `ChildOf` first, then the other components in document order, through
        `add(entity, type, value)`. Children are appended in file order, so `Children` and the
        root order match the saved order; hooks run (ADR-0030); a `LocalTransform` added after
        `ChildOf` keeps its saved value (ADR-0031 rule 3).
    - **Change ticks:** every value is stamped added and changed at the world's current tick, as
      after a world copy.
13. **Unresolved components are kept.** A type name the registry doesn't know, a name that isn't
    a component type, or a value `fromValue` refuses is a `level.unresolved_component` warning.
    The raw value is kept on the entity in a Transient `ith::UnresolvedComponents` component owned
    by `level`, and `saveLevel` writes it back under its key and in its section. Once the type is
    known again, the next load reads it. With `UnknownFields::Fail` (cooking) each one is an
    error. Unknown fields inside a known component are still warned about and dropped (ADR-0010
    rule 13).
14. **Versioning:** `ith.level` version 1, minimum version 1, no migrations. Component types change
    through what reflection offers: `FormerName` for renames, defaults for new fields, and rule 13
    for values that no longer read.

### Asset database

15. An `.ithmap` file is a native asset: no sidecar, its envelope id is its `AssetId`, and the scan
    reads only the header, as it does today. Components refer to assets through ordinary
    `AssetRef` and `SoftAssetRef` fields, saved as ids (ADR-0015 rule 4). The dependents index for
    levels, which walks those fields through the type registry, comes with the dependents view.
    Stable ids are unique per level; duplicating a level copies them, which is harmless while one
    level is loaded into a world.

## Consequences

- A level is built, saved and reloaded entirely through the editor. Its diffs are stable and
  merge well, and a later split into one file per entity reuses the record.
- Runtime-only entities, and references to them, leave saves with a warning, never an error.
- A missing component type never destroys level data; cooking catches it.
- Adding a field to a component changes every entity that has it on the next resave, once.
- Follow-up work: the `level` module with tests: a 10k-entity level saved, loaded and saved again
  to the same bytes; hierarchy and root order rebuilt; references to records later in the file;
  every validation error leaving the world unchanged; unknown and unreadable components written
  back; unsaved entities and references; hooks running, `ChildOf` before `LocalTransform`; the
  `editor` sections stripped and the level loaded with no editor types registered. A fuzz target
  for `loadLevel`; `kLevelFormat` moved; a level guide in `docs/development`.
- **Owner's answers at acceptance (2026-11-22):** components are saved as full values;
  entities without a `StableId` are skipped on save with a warning; unknown or unreadable
  components are kept and written back. No world resources in version 1 and one file per level in
  P2 follow the engineer's recommendation, accepted by the owner, as does the rest.
- **Revisit when:**
  - level files become merge-conflict hot spots, or the open-world ADR needs cells (one file per
    entity);
  - the first per-level setting is needed (resources saved with the level);
  - a component needs a breaking change in content that has shipped (per-type versions and
    migrations);
  - several levels are loaded into one world (stable ids scoped by level);
  - profiles show loading level text on a path that matters.
