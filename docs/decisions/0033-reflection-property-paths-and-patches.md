# ADR-0033: Reflection property paths and patches

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0018](0018-reflection.md) (reflection) rule 10,
  [ADR-0020](0020-serialization-rules-completed.md) (serialization) and
  [ADR-0015](0015-asset-identity-and-reimport.md) (prefabs) rule 14;
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) rule 5 (Details),
  [ADR-0017](0017-runtime-and-editor-separation.md) rule 13 (Keep Simulation Changes),
  [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md) (hooks, immutable
  components), [ADR-0032](0032-entity-references-stable-ids-and-world-copy.md) (entity mapping)

## Context

ADR-0018 rule 10 says edits are validated, then applied, but reflection has no way to name a
value inside a type and no patch. The editor shell needs both:

- the Details panel edits one field of one component, and must know that field's attributes
  before it has a value;
- undo needs the old values of every edit, in a form that can be applied again;
- prefab overrides (ADR-0015 rule 14) are stored as property paths such as `Light.intensity`,
  `+Light` and `-Light`, and must survive renames;
- Keep Simulation Changes (ADR-0017 rule 13) copies component values from the play world back
  into the editor world as one undoable change;
- immutable components (ADR-0030) change only by being replaced, with their hooks.

Lessons from earlier engine prototypes by the same maintainer: prefab overrides recorded as
property paths, with `+` and `-` for added and removed components, worked well as a format.

Prior art, from public documentation and behaviour:

- **Unreal:** a property edit is described by a chain of properties from the outer object to the
  edited member, and a change kind (value set, array add, remove or clear, and an interactive
  kind for slider drags, committed when the drag ends). Before a change, the object records its
  state into the active transaction, and undo restores that state. An instance's overridden
  properties are found by comparing each property with the archetype's; an array property is
  overridden as a whole.
- **Bevy:** `ReflectPath` reaches a value by a string of `.field` and `[index]` steps, and
  `ParsedPath` keeps a parsed path for reuse.
- **JSON Pointer and JSON Patch** ([RFC 6901](https://www.rfc-editor.org/rfc/rfc6901),
  [RFC 6902](https://www.rfc-editor.org/rfc/rfc6902)): a patch is an ordered list of operations,
  each applied to the result of the one before, and the whole patch fails if any operation
  fails.

## Options considered

Path text:

- **Dots and brackets, the syntax of ADR-0020's error paths** (chosen): `items[3].name`. One
  syntax across the engine; a serialization error path turns into a property path by dropping
  `$`. Field names are identifiers, so nothing needs escaping.
- **JSON Pointer** (`/items/3/name`): needs escaping, can't tell an index from a key without the
  schema, and differs from the error paths people already see.

Patch values:

- **A typed value of the target's reflected type** (chosen, `OwnedValue`): exact, applied
  without parsing, and its `Entity` values can be remapped in memory.
- **A JSON value tree:** parsed again on every apply, and it can't hold an entity handle without
  an entity mapping. JSON stays the stored form, converted at the edges.

Undo capture:

- **An inverse patch per edit** (chosen): records exactly what changed, and is the same data
  prefab overrides and their Details markings use.
- **Snapshots of whole objects:** simple, but they lose which property changed and cost a full
  copy per edit in the history.

Applying:

- **Prepare on a copy of each touched value, then move it into place** (chosen): validation is
  the apply itself, so the commit can't fail. Costs one copy of each touched component.
- **Validate in place, then apply:** inserts and erases shift later indices, so validating means
  simulating them anyway.

Prefab overrides inside `std::vector`:

- **The whole vector is one override** (chosen, the owner's decision), as in Unreal: stored
  overrides never hold an index that shifts when elements are added or removed.
- **Index paths:** break when the prefab's vector changes.
- **Elements keyed by an id field:** right for per-element overrides, but nothing needs them yet.

## Decision

### Paths

1. **A property path** addresses a value inside a reflected value:
   - text: `name`, then any number of `.name` and `[index]` steps, e.g. `value.translation.x`,
     `items[3].name`. Names are C identifiers; indices are decimal, without sign or leading
     zeros. The empty path is the whole value;
   - `std::optional` is transparent, as in serialization: `shield.amount` steps through an
     optional `shield`, and `shield` is the whole optional;
   - an `Entity` is a leaf: a path never steps through it.
2. **A component path** is the component's fully qualified type name, optionally followed by
   `.` and a property path: `ith::LocalTransform.value.translation.x`, or `game::Light` for the
   whole component. Type names contain no `.` or `[`, so the name ends at the first of them.
3. **`ith::PropertyPath`** is the compiled form: the root type's id and a list of steps (a field
   by its index in `TypeInfo::fields`, an element index, or an optional's value). Parsing
   (`PropertyPath::parse(root, text)`, and a component form through the `TypeRegistry`) checks
   names, kinds and fixed-array bounds, so the leaf type and its last `FieldInfo` (attributes,
   `Min`, `Max`, tooltip) are known without a value. Resolving checks only what depends on the
   value: a vector index and an empty optional. Paths compare equal and test `isPrefixOf`.
4. **Renames:** parsing finds types and fields by their current names and their `FormerName`
   aliases (ADR-0018 rule 8), and `toString` prints current names, so a stored path is rewritten
   when it is saved again. A field that moves or changes shape needs a migration of the document
   that stores the path (ADR-0020 rule 7).

### Values and patches

5. **`ith::OwnedValue`** is a value of any reflected type in its own aligned memory, with copy
   and move. It moves from serialization's private temporary value into `reflection`, and
   serialization uses it.
6. **Reading:** `resolve(path, root)` returns `PropertyRef { type, value, field, chainFlags }`,
   where `chainFlags` is the OR of the flags of every field on the path. `read<T>` checks the
   leaf type; `readValue` copies the leaf into an `OwnedValue`.
7. **Value trees** (amends ADR-0020): `serialization` gains public `toValue(type, object,
   entities)` and `fromValue(value, type, target, options)`, which convert a reflected value to
   and from a `serialization::Value` without the document envelope, with the same rules,
   limits and entity mapping as the text archive. Automation arguments and stored overrides
   become `OwnedValue`s through them.
8. **`ith::Patch`** is an ordered list of entries on one root type, each applied to the result
   of the ones before:
   - `set(path, value)` replaces the value at the path; the empty path replaces the whole root;
   - `insert(path, value)` and `erase(path)` take a path ending in `[i]` on a `std::vector`;
     `insert` accepts `i` up to the size, which appends. Fixed arrays and optionals take `set`
     only. There is no resize: `set` on the whole vector covers it.

   Each value's type must be the path's leaf type (the element type for `insert`).
9. **Patch then apply** (completes ADR-0018 rule 10):
   - `preparePatch(patch, root)` copies the root once, applies every entry to the copy and builds
     the inverse patch: `set` becomes `set` with the old value, `insert i` becomes `erase i`,
     `erase i` becomes `insert i` with the old element, in reverse order. It returns the new
     value and the inverse, or the first error; the target is never touched;
   - committing moves the new value into place and can't fail;
   - `applyPatch(patch, root)` does both and returns the inverse.
10. **What a patch doesn't check:** `Min` and `Max` (the Details panel clamps, ADR-0008 rule 5,
    and loading doesn't enforce them either) and `ReadOnly`. **The editor's command layer
    refuses edits whose `chainFlags` include `ReadOnly`**; Keep Simulation Changes and prefab
    loading write whole components that may contain such fields, so the patch itself allows it.
11. **Entities in patches:** values hold handles of the world they were built against.
    `remapEntities(patch, map)` runs ADR-0032's walker over every entry's value; stored patches
    go through rule 7 with an entity mapping, so a raw handle is never written.

### The world

12. **`World::applyPatches(std::span<const EntityPatch>)`** in `ecs` applies changes to several
    entities as one step. An `EntityPatch` is an entity and a list of `ComponentChange`s:
    - `Patch`: a `Patch` on a component the entity has;
    - `Add`: a component value, added or replacing the current one;
    - `Remove`: a component the entity has.

    It validates everything first: live entities, component types, components present, every
    patch prepared, every new value accepted by the hierarchy and stable-id checks (ADR-0031,
    ADR-0032). Components the World keeps (`Children`, `DisabledInHierarchy`) are refused. Then
    it commits in order and returns the inverse, entities in reverse order (`Add` of a new
    component becomes `Remove`; a replacing `Add` and a `Remove` become `Add` with the old
    value). On failure nothing changes. While a query iterates it asserts and is refused, like
    other structural changes.
13. **How a change is committed:**
    - a patch on a mutable component writes the value in place, stamping its changed tick
      without running hooks, as a write through `get` does;
    - a patch on an immutable component, and every `Add`, goes through `add(entity, type,
      value)`: the value is replaced and the hooks run (ADR-0030);
    - `Remove` goes through `remove`.

### Errors

14. Failures are `ith::Error`s whose message names the path, the entry and, for syntax, the byte
    offset:
    - `reflection.path.syntax`, `reflection.path.unknown_type`, `reflection.path.unknown_field`,
      `reflection.path.not_a_struct` (a field step on another kind),
      `reflection.path.not_indexable` (an index step on something other than an array),
      `reflection.path.index_out_of_range`, `reflection.path.empty_optional`;
    - `reflection.patch.wrong_type`, `reflection.patch.not_resizable`;
    - `ecs.patch.stale_entity`, `ecs.patch.not_a_component`, `ecs.patch.world_owned`,
      `ecs.patch.missing_component`, `ecs.patch.refused_value`; a failed component patch passes
      its reflection error on, naming the entity and the type.

    Applying a patch to a root of another type is a programmer error and asserts.

### Prefab overrides and Keep Simulation Changes

15. **Override records** (amends ADR-0015 rule 14, which wrote short type names): the key is the
    entity key, `:`, then a component path or a `+` or `-` and a fully qualified type name:
    - `"<entity>:game::Light.intensity"` is a component patch with one `set`;
    - `"<entity>:+game::Light"` is an `Add` with the stored value;
    - `"<entity>:-game::Light"` is a `Remove`.

    The entity key is a Uuid, or Uuids joined by `/` through nested prefabs (rule 15); a Uuid
    holds no `:`, so the first single `:` ends the key. Values are stored through rule 7 with an
    entity mapping. **Inside a `std::vector`, the override is the whole vector:** the path stops
    at the first dynamic-array step, so stored overrides hold no shifting index. An override
    whose path no longer parses is reported and kept (ADR-0015 rules 10 and 17); the others are
    applied as one `applyPatches`. Details marks a property overridden when an override path is
    a prefix of it.
16. **Keep Simulation Changes** builds one `applyPatches` call: for each selected play entity
    with a `StableId` found in the editor world, a whole-component `set` for each component on
    both (Transient ones skipped), an `Add` for each only in play and a `Remove` for each only in
    the editor, EditorOnly ones excepted. Entity values are remapped from play to editor through
    `StableId`, null where there is none. The returned inverse is the undo step.

## Consequences

- One mechanism serves Details edits, undo, prefab overrides and Keep Simulation Changes, and
  the Details panel gets a field's attributes from the path alone.
- An edit costs one copy of each touched component, which is small for editor edits and grows
  with components that hold large vectors.
- A slider drag can become one undo step by keeping the first inverse and the last patch; the
  transactions ADR decides this.
- Inverses don't hold what hooks changed on other components (the `LocalTransform` that
  reparenting recomputes); undo replaces the same components, so the same hooks recompute it.
- Stored paths never index a dynamic array; a prefab instance that changes one element overrides
  the whole vector.
- `TypeInfo::containsEntities` (ADR-0032 rule 4) is conservative for structs: a field whose type
  is another reflected struct counts as reaching an `Entity`, so `false` is exact and `true` may
  cost one extra walk. Patch remapping inherits this.
- Follow-up work: `PropertyPath`, `OwnedValue`, `Patch` and their tests in `reflection`;
  `toValue` and `fromValue` in `serialization`; `World::applyPatches` in `ecs`; the reflection
  and ECS guides. Tests: parse and print round trips (FormerName names printed as current
  names), every error with its offset, a path to every reachable field of every reflected test
  type, a random patch and its inverse restoring every reflected type, a failed patch leaving
  the target unchanged, the insert and erase index rules, immutable components replaced with
  their hooks, change ticks, atomicity across entities, entity remapping, and a fuzz target for
  the parser.
- **Owner's answers at acceptance (2026-11-22):** patch values are `OwnedValue`s, with JSON only
  at the edges; a prefab override inside a `std::vector` covers the whole vector; `ReadOnly` is
  enforced by the editor's command layer, not by patches; component paths use fully qualified
  type names. The rest follows the engineer's recommendation, accepted by the owner.
- **Revisit when:**
  - copying components dominates the cost of edits;
  - content needs per-element overrides inside vectors (keyed elements);
  - a hook's side effects on other components break undo.
