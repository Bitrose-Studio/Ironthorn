#pragma once

// Entity patches: changes to the components of several entities, applied as one step with
// World::applyPatches (ADR-0033).
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
// The Details panel, undo, prefab overrides and Keep Simulation Changes describe their edits this
// way. Patches hold entity handles of the world they are applied to; translate them for another
// world with ith::remapEntities (Patch.h) on each component patch and value.
//
// Plain values; not thread-safe.

#include "ith/foundation/Entity.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/TypeInfo.h"

#include <cstdint>
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

// The changes to one entity, applied in order.
struct EntityPatch {
    Entity entity;
    std::vector<ComponentChange> changes;
};

} // namespace ith
