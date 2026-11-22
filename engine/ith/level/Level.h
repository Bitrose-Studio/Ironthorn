#pragma once

// Level documents (ADR-0034): a World's entities saved as an "ith.level" text document and loaded
// back, all or nothing.
//
//   std::vector<ith::serialization::Error> warnings;
//   const auto text = ith::saveLevel(world, levelId, registry, {.warnings = &warnings});
//   // write *text to Maps/Harbour.ithmap
//
//   const auto info = ith::loadLevel(fileText, world, registry, {.source = "Maps/Harbour.ithmap",
//                                                               .warnings = &warnings});
//   if (!info) { log(info.error().toString()); } // the world is unchanged
//
// The document (an ADR-0024 asset document whose id is the level's asset id):
//
//   {
//     "format": "ith.level",
//     "version": 1,
//     "id": "<level id>",
//     "data": {
//       "entities": [
//         {
//           "id": "<stable id>",
//           "components": { "<type name>": <value>, ... },
//           "editor": { "<EditorOnly type name>": <value>, ... }
//         },
//         ...
//       ]
//     }
//   }
//
// - One record per entity with a StableId, in Outliner order: each root in root order, followed
//   depth-first by its children in Children order. The record's "id" is the stable id, which is not
//   repeated in "components".
// - "components" maps each component's fully qualified type name to its full value, as
//   serialization::toValue writes it. "editor" holds the EditorOnly components the same way and is
//   left out when empty. Transient components (Children, WorldTransform, DisabledInHierarchy) are
//   not written. The parent is the ith::ChildOf component.
// - An Entity value is written as the stable id of that entity; the null handle as the nil id.
// - World resources are not saved.
//
// The same world always gives the same bytes: records in Outliner order, the keys of "components"
// and "editor" sorted bytewise, fields in declaration order, floats in their shortest round-trip
// form, two-space indentation, LF line endings and a final newline.

#include "ith/ecs/World.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/LevelFormat.h"
#include "ith/level/UnresolvedComponents.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Error.h"
#include "ith/serialization/TextArchive.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// The current version of the level format, which saveLevel writes. Version 1 is also the oldest
// one loadLevel reads; there are no migrations yet.
inline constexpr std::uint32_t kLevelVersion = 1;

// Warnings, of a save or a load, are collected only when a list is given, and at most
// kMaxLevelWarningsPerCode of each code are kept; one more warning of that code then counts the
// ones dropped.
inline constexpr std::size_t kMaxLevelWarningsPerCode = 100;

struct LevelSaveOptions {
    // Receives the warnings of a successful save, appended (see saveLevel). Null to not collect
    // them.
    std::vector<serialization::Error>* warnings = nullptr;
};

// The options of TextReadOptions that apply to a level.
struct LevelLoadOptions {
    // The file name or other source, copied into errors and warnings.
    std::string_view source;
    // Warn (the editor): unknown fields are dropped and unresolved components kept, with
    // warnings. Fail (cooking): each of them is an error.
    serialization::UnknownFields unknownFields = serialization::UnknownFields::Warn;
    // Receives the warnings of a successful load, appended (see loadLevel). Null to not collect
    // them.
    std::vector<serialization::Error>* warnings = nullptr;
    // The bytes the load may allocate, as Document.h describes: the JSON tree is checked against
    // it, and everything read from the tree (the records, every component value with its size, the
    // names of unresolved components) shares one more budget of the same size. 0 means the default
    // of serialization::allocationBudget for the size of the text. Exceeding it fails the load with
    // "serialization.too_large".
    std::size_t allocationBudget = 0;
};

// What loadLevel loaded.
struct LevelInfo {
    // The level's asset id, from the document's "id".
    Uuid id;
    // The number of entities spawned: one per record.
    std::size_t entityCount = 0;
};

// Writes the entities of `world` as a level document whose id is `levelId` (see the header
// comment). Writing the file is the caller's job.
// - An entity without a StableId is not saved; one "level.unsaved_entity" warning gives their
//   number. Its saved children are written in its place as roots, without ChildOf, each with a
//   "level.dropped_parent" warning.
// - An Entity value that refers to no saved entity (one without a StableId, a stale handle, a
//   handle of another world) is written as the nil id, with a "level.unsaved_reference" warning
//   that names the referencing entity and the component type. A reference never fails a save.
// - The entries of an entity's UnresolvedComponents are written back as they were read, except
//   those the next load would refuse: an entry whose type name the entity has as a written
//   component, or whose name `registry` now resolves (FormerName included) to StableId or to a
//   component type the record already writes.
// Fails with "serialization.invalid_id" (a nil `levelId`) and the toValue errors of a component
// value ("serialization.non_finite", "serialization.invalid_enum", ...), whose path is then
// "$.entities[<record>].components.<type name>" (".editor." for an EditorOnly type) followed by
// the path inside the value.
// Precondition (asserted; the save then fails with "level.world_busy"): `world` is not being
// iterated and no system or hook runs on it. Not thread-safe with changes to `world`.
[[nodiscard]] Expected<std::string, serialization::Error>
saveLevel(const World& world, const Uuid& levelId, const TypeRegistry& registry, const LevelSaveOptions& options = {});

// Replaces the entities of `world` with those of the level document `text`, with component types
// found in `registry`. All or nothing: everything is read and checked before the world changes,
// and a failure leaves the world as it was.
//
// Checks, which fail the load:
// - the envelope (readDocument's errors; "id" is required and is the level id), and a "data" that
//   is an object whose "entities" is an array of record objects ("serialization.wrong_type");
// - every record has an "id" that is a UUID other than nil ("level.invalid_id"), unique in the
//   document ("level.duplicate_id"), and no "ith::StableId" component ("level.invalid_id");
// - a type name given twice in a record, in one section or across both, or two names of one type
//   ("serialization.duplicate_field");
// - the ChildOf parents form no cycle, and no entity is its own parent ("level.parent_cycle");
// - with UnknownFields::Fail, an unknown key in the document, a record or a component value, and
//   every component that would be unresolved: an unknown or unsuitable type is
//   "level.unresolved_component", a value fromValue refuses fails with fromValue's error.
//
// Warnings, which don't (with UnknownFields::Warn):
// - "serialization.unknown_field": an unknown key in "data", in a record or in a component value;
// - "level.unresolved_component": a type name the registry doesn't know, a type that isn't a saved
//   component type (not a Component, or Transient), or a value fromValue refuses. The raw value
//   is kept in the entity's UnresolvedComponents and saved back;
// - "level.unknown_reference": an Entity value whose id names no record reads as the null handle;
// - "level.dropped_parent": a ChildOf whose parent is unknown or nil is dropped, and the entity
//   loads as a root.
//
// Then the world is built, which can't fail: world.clear() (generations are kept, so old handles
// stay stale; resources are kept), one entity spawned per record with its StableId in file order,
// then record by record its ChildOf first and its other components in document order through
// World::add, so hooks run, and UnresolvedComponents last. Children and the root order follow the
// file order. Every value is stamped added and changed at the world's current tick.
//
// Preconditions (asserted; the load then fails with "level.world_busy" and changes nothing):
// `world` is not being iterated and no system or hook runs on it. Not thread-safe with other uses
// of `world`; `registry` is only read. `text`, `options.source` and `options.warnings` must not
// live in storage that world.clear() frees, such as a component or a hook's state: the warnings are
// appended before the clear, and the clear destroys what the world owns.
[[nodiscard]] Expected<LevelInfo, serialization::Error>
loadLevel(std::string_view text, World& world, const TypeRegistry& registry, const LevelLoadOptions& options = {});

} // namespace ith
