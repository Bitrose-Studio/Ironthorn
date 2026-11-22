#pragma once

// Entity patches: changes to the components of several entities, and the spawns and despawns of
// whole entities, applied as one step with World::applyPatches (ADR-0033, ADR-0037).
//
//   const ith::TypeInfo& light = ith::typeOf<game::Light>();
//   ith::Patch brighter(light);
//   brighter.set(*ith::PropertyPath::parse(light, "intensity"), ith::OwnedValue::of(4.0f));
//
//   ith::EntityPatch patch{.entity = lamp};
//   patch.changes.push_back(ith::ComponentChange::makePatch(std::move(brighter)));
//   patch.changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(game::Flicker{})));
//   patch.changes.push_back(ith::ComponentChange::makeRemove(ith::typeOf<game::Glow>()));
//
//   ith::Expected<std::vector<ith::EntityPatch>> undo = world.applyPatches(std::span(&patch, 1));
//   ...
//   static_cast<void>(world.applyPatches(*undo)); // back to what it was
//
// A new entity and a child of it, in one call:
//
//   const ith::Entity lampSlot = ith::Entity::fromParts(0, 1, 0); // a placeholder
//   std::vector<ith::EntityPatch> patches(2);
//   patches[0] = {.op = ith::EntityOp::Spawn, .entity = lampSlot};
//   patches[0].changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::StableId{.id = lampId})));
//   patches[1] = {.op = ith::EntityOp::Spawn, .entity = ith::Entity::fromParts(1, 1, 0)};
//   patches[1].changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::ChildOf{.parent = lampSlot})));
//
// The Details panel, undo, prefab overrides and Keep Simulation Changes describe their edits this
// way. Patches hold entity handles of the world they are applied to; translate them for another
// world with ith::remapEntities (below).
//
// Plain values; not thread-safe.

#include "ith/foundation/Entity.h"
#include "ith/reflection/Entities.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/TypeInfo.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ith {

// One change to one component of an entity.
struct ComponentChange {
    enum class Kind : std::uint8_t {
        Patch,  // apply `patch` to the component the entity has
        Add,    // add `value`, replacing the component the entity has, if any
        Remove, // remove the component the entity has
    };

    Kind kind = Kind::Patch;
    // The component type. Set by the make functions.
    const TypeInfo* type = nullptr;
    // Kind::Patch: the edits, on `type`.
    ith::Patch patch;
    // Kind::Add: the new value, of `type`.
    OwnedValue value;

    // A Patch change of the patch's root type.
    [[nodiscard]] static ComponentChange makePatch(ith::Patch edits) {
        const TypeInfo* root = edits.rootType();
        return ComponentChange{.kind = Kind::Patch, .type = root, .patch = std::move(edits)};
    }

    // An Add change of the value's type.
    [[nodiscard]] static ComponentChange makeAdd(OwnedValue newValue) {
        const TypeInfo* valueType = newValue.type();
        return ComponentChange{.kind = Kind::Add, .type = valueType, .value = std::move(newValue)};
    }

    // A Remove change.
    [[nodiscard]] static ComponentChange makeRemove(const TypeInfo& componentType) {
        return ComponentChange{.kind = Kind::Remove, .type = &componentType};
    }
};

// What an EntityPatch does to its entity (ADR-0037 rule 8).
enum class EntityOp : std::uint8_t {
    Modify,  // apply `changes` to a live entity
    Spawn,   // create an entity with the Adds in `changes`
    Despawn, // destroy a live entity that has no children; `changes` is empty
};

// The changes to one entity, applied in order.
//
// Spawn: `entity` is a placeholder, a handle of world id 0 that isn't null (for example
// Entity::fromParts(n, 1, 0)), distinct from the other Spawns of the call. Every handle of the call
// equal to it, in the entities, values and placeBefore of the patches, stands for the new entity.
// The World spawns it, adds its ChildOf first and then the other components in order, then places
// it. Its changes are Adds only.
//
// Despawn: the entity must have no children (a delete detaches or removes them first). Its inverse
// is a Spawn of every component the World doesn't keep itself, Transient ones included, placed
// before the sibling that followed it.
struct EntityPatch {
    EntityOp op = EntityOp::Modify;
    Entity entity;
    std::vector<ComponentChange> changes;
    // Modify and Spawn: when set, the entity is placed before this sibling among its parent's
    // children, or among the roots, after its changes; a null sibling places it at the end.
    std::optional<Entity> placeBefore;
};

// Replaces every Entity an entity patch holds, its entity and placeBefore included, with
// map.function(context, entity), as ith::remapEntities does for one value. Precondition
// (asserted): map.function is set.
void remapEntities(EntityPatch& patch, const EntityMap& map);

} // namespace ith
