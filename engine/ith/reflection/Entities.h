#pragma once

// Rewriting the entity handles inside reflected values (ADR-0032).
//
//   ith::Entity toCopy(void* context, ith::Entity entity) {
//       return static_cast<const SlotMap*>(context)->find(entity); // null when it has no copy
//   }
//
//   SlotMap slots = ...;
//   ith::remapEntities(ith::typeOf<game::Squad>(), &squad, {.function = &toCopy, .context = &slots});
//
// One walker serves every place where handles must be translated: world copy, Keep Simulation
// Changes, level save and load, and replication. It reaches every ith::Entity that the value's
// reflected fields hold, at any depth: fields (Transient and EditorOnly ones included), nested
// structs, std::array, std::vector and std::optional. An Entity in a field that isn't reflected
// is not reached, so every Entity field of a component or resource must be reflected.

#include "ith/foundation/Entity.h"
#include "ith/reflection/TypeInfo.h"

namespace ith {

// Where each handle goes: function(context, entity) returns the handle that replaces `entity`.
// A plain function and a pointer to its state, so remapping allocates nothing.
struct EntityMap {
    Entity (*function)(void* context, Entity entity) = nullptr;
    void* context = nullptr;
};

// Replaces every Entity that the value of `type` at `value` reaches with map.function(context,
// entity), null handles included, in field and element order. Returns at once for a type whose
// containsEntities is false. Precondition (asserted; nothing changes when it fails):
// map.function is set. `value` is a complete, non-const object of `type`.
void remapEntities(const TypeInfo& type, void* value, const EntityMap& map);

} // namespace ith
