# Levels

A level is a `World`'s entities saved as one text document, an `.ithmap` file. The `level`
module writes and reads it; the editor saves and reloads scenes through it. The design is
[ADR-0034](../decisions/0034-level-documents.md); this page shows how to use it.

## Saving and loading

```cpp
#include "ith/level/Level.h"

std::vector<ith::serialization::Error> warnings;
const auto text = ith::saveLevel(world, levelId, {.warnings = &warnings});
if (!text) {
    log(text.error().toString());
}
// Writing the file is the caller's job.

const auto info = ith::loadLevel(fileText, world, registry,
                                 {.source = "Maps/Harbour.ithmap", .warnings = &warnings});
if (!info) {
    log(info.error().toString()); // the world is unchanged
}
```

- `levelId` is the level's asset id. It is created with the level and never regenerated; the
  asset database reads it from the document's header (an `.ithmap` file needs no sidecar).
- `registry` must know every component type the level uses: register the reflection of the
  `ecs` module and of each game and plugin module at startup
  ([reflection guide](reflection.md)). A type it doesn't know is kept, not lost (see
  [Unresolved components](#unresolved-components)).
- Both functions take the world between frames: not while a query iterates it, a system runs or
  a hook runs. That is asserted, and the call then fails with `level.world_busy`.
- Saving reads the world and never changes it. Loading replaces all of the world's entities.

## The document

```json
{
  "format": "ith.level",
  "version": 1,
  "id": "6c0d5b0e-8a51-4c33-9e7f-3f6f0a1b2c3d",
  "data": {
    "entities": [
      {
        "id": "00000000-0000-0000-0000-000000000001",
        "components": {
          "game::Light": {
            "intensity": 2.5
          }
        },
        "editor": {
          "game::EditorNote": {
            "text": "by the door"
          }
        }
      },
      {
        "id": "00000000-0000-0000-0000-000000000002",
        "components": {
          "ith::ChildOf": {
            "parent": "00000000-0000-0000-0000-000000000001",
            "despawnWithParent": false
          }
        }
      }
    ]
  }
}
```

- **One record per entity**, with its `ith::StableId` as `id`. The id isn't repeated in
  `components`.
- **Records are in Outliner order:** each root in root order, followed depth-first by its
  children in `Children` order.
- **`components`** maps each component's fully qualified type name to its full value, written as
  the text archive writes it ([serialization guide](serialization.md)). Every field is written,
  also when it has its default value, so changing a default in code never changes existing levels.
- **`editor`** holds the `EditorOnly` components the same way, and is left out when there are
  none. The cooker drops the section without reading it, so a game never needs the editor's types.
- **Not saved:** Transient components (`Children`, `WorldTransform`, `DisabledInHierarchy`) and
  world resources. `Children` and the order of the roots are rebuilt from the record order;
  `ith::Disabled` is an ordinary component.
- **The parent** is the `ith::ChildOf` component.

### Stable output

The same world always gives the same bytes, so level files diff and merge well in git:

- records in Outliner order;
- keys in `components` and `editor` sorted bytewise by type name;
- fields in declaration order;
- floats in their shortest round-trip form;
- two-space indentation, LF line endings and a newline at the end;
- no timestamps, user names or paths.

Adding a field to a component type changes every entity that has it on the next save, once.

## Entities and references

An `ith::Entity` value inside a component (a `ChildOf` parent, a target, a list of members) is
saved as the stable id of the entity it refers to, and read back as the entity loaded for that
record. Reflect every `Entity` field ([reflection guide](reflection.md#entity-fields)).

| Situation | Saving | Loading |
|---|---|---|
| An entity without a `StableId` | Not saved; one `level.unsaved_entity` warning counts them. Its saved children are written in its place as roots, each with `level.dropped_parent`. | |
| A reference to an entity that isn't saved (no `StableId`, stale, another world's) | Written as the nil id, with `level.unsaved_reference` | |
| An id that names no record of the level | | Reads as the null entity, with `level.unknown_reference` |
| A `ChildOf` whose parent is unknown or nil | | Dropped: the entity loads as a root, with `level.dropped_parent` |

References may name records anywhere in the file, later ones included. Entities the editor,
level loading and prefab instantiation create always have a `StableId`; entities spawned at run
time don't, so a helper entity never blocks saving the level.

## How loading works

Loading is all or nothing. First the whole document is read and checked, without touching the
world:

- the envelope: format `ith.level`, a version this engine reads, and the level id;
- every record has an `id` that is a UUID other than nil (`level.invalid_id`), unique in the
  level (`level.duplicate_id`), and no `ith::StableId` component (`level.invalid_id`);
- no type given twice in a record, also not under its `FormerName` or in both sections
  (`serialization.duplicate_field`);
- the parents form no cycle (`level.parent_cycle`);
- every component value is read.

Any of these is an error, the world is left exactly as it was, and data from a file never reaches
an ECS assert. Then the world is built, which can't fail:

1. `world.clear()`. Generations are kept, so handles from before the load stay stale, and the
   world's resources are kept.
2. One entity is spawned per record, with its `StableId`, in file order.
3. Entity references are mapped to the new entities.
4. Record by record, `ChildOf` is added first, then the other components in document order,
   through `World::add`. Hooks run, so engine modules build their Transient state as for any
   other add. Children are appended in file order, which rebuilds `Children` and the root order,
   and a `LocalTransform` added after its `ChildOf` keeps its saved value.
5. Every value is stamped added and changed at the world's current tick.

Loading a level into the world it came from gives the same level back: saving it again gives the
same bytes.

## Unresolved components

A component that can't be read is kept, never dropped, so a disabled plugin or a rename without
`FormerName` doesn't erase a component from a whole level on the next save:

- a type name the registry doesn't know;
- a name that isn't a saved component type (not a `Component`, or `Transient`);
- a value that doesn't read (a field of the wrong JSON type, an unknown enumerator).

Each is a `level.unresolved_component` warning. Its raw value is kept on the entity in the
Transient component `ith::UnresolvedComponents` (`ith/level/UnresolvedComponents.h`), and
`saveLevel` writes it back under its type name, in the section it came from. Once the type is
known again, the next load reads it. If the entity gets a real component with the same type name,
the real one is saved instead.

Unknown fields inside a known component are different: they are warned about
(`serialization.unknown_field`) and dropped, as in every text document. So are unknown keys in a
record or in `data`.

## Cooking

Pass `UnknownFields::Fail` to turn every warning about data into an error: unknown fields and
keys fail with `serialization.unknown_field`, an unknown or unsuitable component type with
`level.unresolved_component`, and a value that doesn't read with its own error. A level that
loads this way contains nothing stale. Cooked levels will be binary, written by the cooker; P2
reads and writes text only, and Play In Editor copies the editor world (`ith::copyWorld`) instead
of reading a file.

## Errors and warnings

Every error and warning is a `serialization::Error` with the source name and the path of the
problem, such as `$.entities[3].components.game::Light.intensity`.

| Code | Kind | Meaning |
|---|---|---|
| `level.invalid_id` | error | A record without an id, an id that isn't a UUID or is nil, or a `StableId` component |
| `level.duplicate_id` | error | Two records with one id |
| `level.parent_cycle` | error | An entity is its own parent or ancestor |
| `level.world_busy` | error | The world is iterated, or a system or hook runs (asserted) |
| `serialization.*` | error | The envelope, the shape of the document, a duplicate key; a value that can't be written when saving |
| `level.unresolved_component` | warning (error when cooking) | A component kept raw (see above) |
| `level.unknown_reference` | warning | An id that names no record reads as the null entity |
| `level.dropped_parent` | warning | A parent that isn't saved, or isn't in the level: the entity is a root |
| `level.unsaved_entity` | warning | Entities without a `StableId` were left out |
| `level.unsaved_reference` | warning | A reference to an entity that isn't saved was written as the nil id |
| `serialization.unknown_field` | warning (error when cooking) | An unknown key or field, dropped |

## Versions

The format is `ith.level` version 1, with no migrations. Component types change through what
reflection offers: `FormerName` for renames, defaults for new fields, and unresolved components
for values that no longer read. Prefab instances will be records with a `prefab` key; adding them
bumps the format to version 2, so an older editor refuses such a level instead of dropping the key.
