#pragma once

// The hierarchy, enabled state and transforms of entities (ADR-0014 rules 12 to 14, ADR-0031).
//
//   const ith::Entity ship = world.spawn(ith::LocalTransform{}, ith::WorldTransform{});
//   const ith::Entity turret = world.spawn(ith::LocalTransform{.value = {.translation = {0, 2, 0}}},
//                                          ith::WorldTransform{}, ith::ChildOf{.parent = ship});
//   world.add(ship, ith::Disabled{});    // queries skip the ship and the turret from here on
//   world.remove<ith::ChildOf>(turret);  // detached: a root again, where it was in the world
//
// - ChildOf links an entity to its parent. It is immutable: reparent by adding another ChildOf,
//   detach by removing it. A parent that is the entity itself, one of its descendants, a dead
//   entity or an entity of another world asserts and is refused before anything is stored.
// - Reparenting and detaching keep the entity's world placement: its LocalTransform is set so that
//   it stays where it was, computed from its ancestors' local transforms (shear is dropped, see
//   ith::decompose). To keep the local transform instead, add LocalTransform after the ChildOf. An
//   entity spawned with a ChildOf keeps the local transform it was spawned with.
// - Children is the ordered list of a parent's children, kept by the World: never add, remove or
//   write it. Children are appended when they gain the parent; World::placeBefore reorders them.
//   A parent whose children are all gone keeps an empty list.
// - Every entity without a ChildOf is a root. The World keeps the roots in order too (spawn order,
//   then placeBefore); World::forEachRoot visits them in that order. This is the Outliner's order.
// - Despawning a parent first despawns its children marked despawnWithParent, children before
//   parents; its other children are detached and become roots, keeping their world placement.
//   World::despawnRecursive despawns the whole subtree.
// - Disabled turns an entity and everything below it off: the World keeps DisabledInHierarchy on
//   the disabled entity and each descendant, and every query skips entities with
//   DisabledInHierarchy unless it names Disabled or DisabledInHierarchy, or includes
//   ith::IncludeDisabled (Query.h). Never add or remove DisabledInHierarchy yourself.
// - WorldTransform is the entity's placement in the world, written by transform propagation
//   (TransformPropagation.h): parent's world matrix times the local transform. An entity in the
//   hierarchy without a LocalTransform counts as the identity.

#include "ith/ecs/Entity.h"
#include "ith/math/Transform.h"
#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <vector>

namespace ith {

// The entity's placement relative to its parent, or to the world for a root. Like ChildOf and
// Disabled, it carries engine hooks (its onRemove tells propagation), so its hooks can't be set.
struct LocalTransform {
    ITH_TYPE(Component)
    ITH_FIELD()
    Transform value;
};

// The entity's placement in the world: its parent's WorldTransform times its local transform.
// Written only by transform propagation, never saved. A matrix, because a rotated parent with a
// non-uniform scale shears its children, which a Transform can't express.
struct WorldTransform {
    ITH_TYPE(Component, Transient)
    ITH_FIELD()
    Mat4 matrix{1.0f};
};

// The link from a child to its parent. Immutable: change it by adding a new value.
struct ChildOf {
    ITH_TYPE(Component, Immutable)
    // The parent, an entity of the same world. A runtime handle: saving maps it to the parent's
    // stable id.
    Entity parent;
    // True for a part of the parent, despawned with it; false (the default) for a child that is
    // detached in place when the parent is despawned.
    ITH_FIELD()
    bool despawnWithParent = false;
};

// The ordered children of a parent, kept by the World (see the header comment).
struct Children {
    ITH_TYPE(Component, Immutable, Transient)
    std::vector<Entity> entities;
};

// Turns the entity and its descendants off: queries skip them (see the header comment).
struct Disabled {
    ITH_TYPE(Component)
};

// On every entity that is Disabled or below a Disabled one; kept by the World.
struct DisabledInHierarchy {
    ITH_TYPE(Component, Transient)
};

} // namespace ith
