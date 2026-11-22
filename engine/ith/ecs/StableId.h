#pragma once

// ith::StableId: the persistent identity of an entity (ADR-0014 rule 3, ADR-0032).
//
//   const ith::Entity door = world.spawn(ith::StableId{.id = ith::Uuid::generate()}, Door{});
//   const ith::Entity found = world.findByStableId(id); // the null handle when no entity has it
//
// A handle (ith::Entity) is runtime-only. Files, the editor's undo history and selection, Keep
// Simulation Changes and networking name entities by their stable id instead.
//
// - Opt-in: the editor gives one to every entity it creates, level load and prefab instantiation
//   assign theirs, and entities spawned at run time have none.
// - Unique per world: a second entity with an id the world already has, or the nil id, asserts and
//   is refused before anything is stored, as an invalid ChildOf is. Ids may be derived rather than
//   random (prefab instances, ADR-0015 rule 14).
// - Immutable: change it by adding a new value. Like ChildOf, it carries engine hooks, which keep
//   the world's lookup (World::findByStableId), so its hooks can't be set.
// - A world copy (WorldCopy.h) keeps every stable id, so an entity of the copy has the id of the
//   entity it was copied from.

#include "ith/foundation/Uuid.h"
#include "ith/reflection/Annotations.h"

namespace ith {

struct StableId {
    ITH_TYPE(Component, Immutable)
    // Not nil.
    ITH_FIELD()
    Uuid id;
};

} // namespace ith
