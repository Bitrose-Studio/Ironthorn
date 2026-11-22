# ADR-0032: Entity references, stable ids and world copy

- **Status:** Accepted
- **Amended by:** [ADR-0034](0034-level-documents.md),
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md)
- **Date:** 2026-11-22
- **Related:** amends [ADR-0014](0014-ecs-storage-and-world.md) (ECS) rules 3, 4 and 20,
  [ADR-0018](0018-reflection.md) (reflection) rules 5 and 9, and
  [ADR-0020](0020-serialization-rules-completed.md) (serialization);
  [ADR-0015](0015-asset-identity-and-reimport.md) rule 14 (derived ids for prefab instances),
  [ADR-0016](0016-gameplay-model-on-the-ecs.md) (relations, entity-targeted events, hooks stay
  engine invariants), [ADR-0017](0017-runtime-and-editor-separation.md) rules 10–13 (Play In
  Editor, `EditorOnly`, Keep Simulation Changes),
  [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md) (hooks),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md) (hierarchy)

## Context

The editor needs Play In Editor (ADR-0017 rule 10), which copies the editor world into a new
world, and later Keep Simulation Changes (rule 13) and level files. All three must translate the
entity handles stored inside components and resources: `ChildOf::parent`, `Children`, relations
(ADR-0016 rule 8), entity fields written by users. ADR-0014 rules 3 and 4 name a stable object
id and a world copy "through the component type records" but leave open:

- how type-erased code finds the `Entity` values inside a value;
- what the stable id is, who assigns it and how it is looked up;
- what a copy keeps (slots, hooks, ticks, flags) and what it refuses.

ADR-0018 rule 5 lists "handles" among field types, but reflection describes no entity handle
today, so `ChildOf::parent` and `Children::entities` are unreflected.

Prior art, from public documentation:

- **Unreal:** Play In Editor duplicates the editor's world into a separate play world, so the
  editor's level is never simulated.
- **Bevy:** `MapEntities` and `ReflectMapEntities` let a type's entity fields be rewritten
  through an entity mapper; a `DynamicScene` is built from one world and written into another
  through such a mapping.
- **flecs:** entity ids are 64-bit values stored in components like any other data, and its
  serializer describes entity-typed members so they can be written as names or paths.

## Options considered

Finding entity references:

- **Reflection describes `Entity` as a leaf, and one generic walker remaps every reflected
  `Entity` value** (chosen, the owner's decision). User types work with no extra code.
- **Remap engine components only.** User `Entity` fields would silently point into the wrong
  world after a copy.
- **Per-type mappers registered like hooks.** Gameplay code would register engine-level
  callbacks, which ADR-0016 rule 4 forbids, and a forgotten mapper fails silently.

Stable ids:

- **Opt-in `StableId` component** (chosen, the owner's decision): only entities that need one
  pay for it.
- **On every entity:** runtime spawns (projectiles, effects) would pay a Uuid and a lookup entry
  each for nothing.

## Decision

### Entity as a reflected leaf

1. **`Entity` moves to `foundation`** as `ith/foundation/Entity.h`. It is header-only and
   includes only `<cstdint>`, so the move is clean. Every include is updated; no forwarding
   header is kept. **Why:** the walker lives in `reflection`, which `ecs` depends on, so `Entity`
   must sit below both.
2. **`TypeKind::Entity`** (amends ADR-0018 rule 5). `typeOf<Entity>()` is a hand-written leaf
   record in `TypeOf.h`, named `"ith::Entity"`. The enumerator is appended last, so the values
   of existing kinds, which the binary schema hash covers (ADR-0020 rule 3), don't change.
3. **`ChildOf::parent` and `Children::entities` become reflected fields.** Every other `Entity`
   field of a component or resource is reflected too: an unreflected one is copied bit for bit
   and is not remapped, so after a copy it points into the source world.
4. **The walker:** `ith::remapEntities(const TypeInfo& type, void* value, const EntityMap& map)`
   in `reflection` replaces every `Entity` the value reaches with `map(entity)`: fields
   (Transient and EditorOnly included), nested structs, fixed arrays, vectors and optionals.
   `EntityMap` is a function pointer with a context pointer; it allocates nothing.
   - `TypeInfo` gains `containsEntities` (amends ADR-0018 rule 9): true when the type, its fields
     or its elements reach an `Entity`. It is set when the record is built (by generated code,
     the leaf traits, and container records on first use). The walker returns at once for a type
     without entities, so copying a world touches only the columns that have any.
   - It is the one walker for world copy, Keep Simulation Changes (play entity → stable id →
     editor entity), level save and load (entity ↔ stable id) and, later, replication.
5. **Serialization refuses a bare `Entity`** (adds to ADR-0020 rule 5). A read or write that
   reaches a saved `Entity` value (a root, field or container element of kind `Entity` that the
   flags rules don't skip) fails with `serialization.no_entity_mapping` unless the caller passed
   an entity mapping context in the archive options. Transient fields are never saved, so they
   need none; `Children` is Transient and needs none.
   - With a context, the value is written as the `Uuid` the context gives (the nil `Uuid` for
     the null handle; text as a `Uuid` string, binary as 16 bytes) and read back through the
     context. The schema hash encodes `Entity` with that width.
   - The level-documents ADR defines the context: it writes entities as their stable ids, and
     says what happens to an entity without one or an id not found.

### Stable ids

6. **`ith::StableId { Uuid id; }`** is a component in `ecs` (amends ADR-0014 rule 3). It is
   Immutable; it is neither Transient nor EditorOnly, because save games, networking and Keep
   Simulation Changes need it in game worlds.
7. **It is opt-in.** The editor (creating an entity), level load and prefab instantiation assign
   one; runtime spawns get none. Ids are unique per level and may be derived: prefab instances
   follow ADR-0015 rule 14.
8. **Lookup:** `World::findByStableId(const Uuid&) -> Entity`, the null handle when no entity
   has the id. Engine hooks on `StableId` keep the lookup. A second entity with an id already
   present in the world asserts and is refused before the value is stored, as ADR-0031 rule 8
   does for invalid parents.

### World copy

9. **API** (completes ADR-0014 rule 4):
   `bool ith::copyWorld(const World& source, World& target, const WorldCopyOptions& options = {})`,
   with `struct WorldCopyOptions { bool excludeEditorOnly = false; };`.
   - `World` is neither copyable nor movable, so the caller owns the target.
   - The target must be fresh: nothing was ever spawned in it. Otherwise the call asserts and is
     refused (returns false). Merging into a non-empty world is not offered.
   - **Preconditions** (asserted): the source is not being iterated. Spawns reserved but not
     applied in the source are not copied.
10. **Slots.** The target keeps the source's slot indices, generations and free list, under its
    own world id. Remapping is `Entity::fromParts(index, generation, target.id())` for a handle
    alive in the source; any other handle (stale, null, another world's) becomes null. Code
    matches entities across worlds by `StableId`, never by handle.
11. **No hooks run.** The invariants hooks keep are copied or rebuilt directly: children order,
    root order, `DisabledInHierarchy` and the stable-id lookup.
12. **What is copied.**
    - Every component, Transient ones included, with its `Entity` values remapped.
    - `EditorOnly` components and resources are dropped when `excludeEditorOnly` is set. The Play
      In Editor copy sets it (ADR-0017 rule 10).
    - Every world resource, with its `Entity` values remapped. App-scoped resources (ADR-0016
      rule 6) are not in the world and are unaffected.
    - **Change ticks:** the target's clock starts fresh and every component value is stamped
      added and changed, as after a level load. Per-system last runs are not copied.
13. **Scope.** The editor shell's first ECS step ships the walker, `StableId`, the lookup and
    `copyWorld`. The Keep Simulation Changes command comes later, on the editor's undoable
    transactions.

## Consequences

- One walker serves copy, save, load, Keep Simulation Changes and replication; user types need
  no mapping code, and a type without entities costs one flag test.
- `Entity` is a foundation type; including it no longer pulls in `ecs`.
- A level or save format can't write a raw handle by accident: the archive refuses it.
- Entities spawned at run time have no stable id, so Keep Simulation Changes skips them
  (ADR-0017 rule 13) and save games must assign ids to what they persist.
- Handles of a copy look like the source's with another world id; code that compares handles
  across worlds is a bug that the world id check (ADR-0014 rule 2) catches.
- Follow-up work: move `Entity.h` and update its includes; `TypeKind::Entity` across reflection,
  archives and the schema hash; `containsEntities` in the generator; reflect the hierarchy
  fields; `StableId` with its hooks and lookup; `copyWorld` with tests (slots, hierarchy, flags,
  ticks, a user `Entity` field in a nested vector); the level-documents ADR's mapping context.
- **Owner's answers at acceptance (2026-11-22):** `Entity` is a reflected leaf type remapped by
  one generic walker; `StableId` is an opt-in component. The rest follows the engineer's
  recommendation, accepted by the owner.
- **Revisit when:**
  - a world must be merged into a non-empty one (streaming, prefab editing in place);
  - profiles show the walker or the copy dominating Play In Editor start;
  - networking needs ids on runtime spawns.
