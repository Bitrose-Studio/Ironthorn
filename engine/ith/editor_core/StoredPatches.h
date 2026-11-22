#pragma once

// The stored form of entity patches in the undo history (ADR-0037 rule 5). A handle changes when
// undo spawns an entity again, so the history never keeps one: every entity a stored patch names,
// as its own entity, inside a value or as placeBefore, is a placeholder Entity::fromParts(i, 1, 0)
// of world id 0 whose index i is a position in a table of stable ids.
//
//   ith::StableIdTable ids;
//   std::vector<ith::EntityPatch> stored = ith::toStored(world, std::move(inverse), ids, warnings);
//   ...
//   std::vector<ith::EntityPatch> live = ith::toLive(world, stored, ids);
//   world.applyPatches(live);
//
// The same placeholders serve as World::applyPatches' Spawn placeholders: a stored Spawn keeps
// its placeholder, and the other patches of the list that name it keep it too.

#include "ith/ecs/EntityPatch.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ith {

class World;

// The stable ids that the placeholders of stored patches index.
class StableIdTable {
public:
    // The index of `id`, added at the end when the table doesn't have it.
    std::size_t indexOf(const Uuid& id);

    // The index of `id`, or nullopt.
    [[nodiscard]] std::optional<std::size_t> find(const Uuid& id) const;

    [[nodiscard]] const std::vector<Uuid>& ids() const noexcept { return m_ids; }

    [[nodiscard]] std::size_t size() const noexcept { return m_ids.size(); }

private:
    std::vector<Uuid> m_ids;
    std::unordered_map<Uuid, std::size_t> m_index;
};

// The placeholder of table index `index`.
[[nodiscard]] Entity storedEntity(std::size_t index) noexcept;

// Turns `patches`, built against `world` as World::applyPatches returns them (with Spawn
// placeholders for entities they spawn), into the stored form, adding the stable ids it needs to
// `ids`. An entity a patch targets must have a StableId, or the StableId among a Spawn's Adds;
// otherwise it fails with "editor.no_stable_id". A reference inside a value to an entity without a
// stable id, or to a dead one, becomes the null handle and adds one to `droppedReferences`.
[[nodiscard]] Expected<std::vector<EntityPatch>> toStored(const World& world, std::vector<EntityPatch> patches,
                                                          StableIdTable& ids, std::size_t& droppedReferences);

// Turns stored patches back into patches `world.applyPatches` takes: each placeholder becomes the
// live entity with that stable id, or stays a placeholder when a Spawn of the list creates it. An id
// no live entity has becomes the null handle, which applyPatches then refuses.
[[nodiscard]] std::vector<EntityPatch> toLive(const World& world, std::vector<EntityPatch> stored,
                                              const StableIdTable& ids);

} // namespace ith
