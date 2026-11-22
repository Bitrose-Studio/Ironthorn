#include "ith/editor_core/StoredPatches.h"

#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/reflection/TypeOf.h"

#include <cstdint>
#include <string>
#include <utility>

namespace ith {

namespace {

// The StableId among a Spawn's Adds, or null.
const Uuid* spawnedId(const EntityPatch& patch) {
    for (const ComponentChange& change : patch.changes) {
        if (change.kind == ComponentChange::Kind::Add && change.type != nullptr &&
            change.type->id == typeOf<StableId>().id) {
            return &change.value.as<StableId>()->id;
        }
    }
    return nullptr;
}

struct StoreContext {
    const World* world = nullptr;
    StableIdTable* ids = nullptr;
    // A Spawn placeholder of the patches (by bits) and the table index of its stable id.
    std::unordered_map<std::uint64_t, std::size_t> spawns;
    std::size_t dropped = 0;
};

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity toStoredEntity(void* context, Entity entity) {
    auto& store = *static_cast<StoreContext*>(context);
    if (entity.isNull()) {
        return entity;
    }
    if (entity.worldId() == 0) {
        if (const auto found = store.spawns.find(entity.bits()); found != store.spawns.end()) {
            return storedEntity(found->second);
        }
        ++store.dropped;
        return {};
    }
    const StableId* id = entity.worldId() == store.world->id() ? store.world->get<const StableId>(entity) : nullptr;
    if (id == nullptr) {
        ++store.dropped;
        return {};
    }
    return storedEntity(store.ids->indexOf(id->id));
}

struct LiveContext {
    const World* world = nullptr;
    const StableIdTable* ids = nullptr;
    // The table indices the list spawns.
    std::vector<bool> spawned;
};

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMap::function has
Entity toLiveEntity(void* context, Entity entity) {
    const auto& live = *static_cast<const LiveContext*>(context);
    if (entity.isNull() || entity.worldId() != 0) {
        return entity;
    }
    const std::size_t index = entity.index();
    if (index >= live.ids->size()) {
        return {};
    }
    if (live.spawned[index]) {
        return entity;
    }
    return live.world->findByStableId(live.ids->ids()[index]);
}

} // namespace

std::size_t StableIdTable::indexOf(const Uuid& id) {
    const auto [found, inserted] = m_index.try_emplace(id, m_ids.size());
    if (inserted) {
        m_ids.push_back(id);
    }
    return found->second;
}

std::optional<std::size_t> StableIdTable::find(const Uuid& id) const {
    const auto found = m_index.find(id);
    if (found == m_index.end()) {
        return std::nullopt;
    }
    return found->second;
}

Entity storedEntity(std::size_t index) noexcept {
    return Entity::fromParts(static_cast<std::uint32_t>(index), 1, 0);
}

Expected<std::vector<EntityPatch>> toStored(const World& world, std::vector<EntityPatch> patches, StableIdTable& ids,
                                            std::size_t& droppedReferences) {
    StoreContext store{.world = &world, .ids = &ids};
    for (const EntityPatch& patch : patches) {
        if (patch.op != EntityOp::Spawn) {
            continue;
        }
        const Uuid* id = spawnedId(patch);
        if (id == nullptr) {
            return makeError("editor.no_stable_id", "an entity the undo history spawns has no StableId");
        }
        store.spawns.emplace(patch.entity.bits(), ids.indexOf(*id));
    }
    for (const EntityPatch& patch : patches) {
        const bool targetsWorld = patch.op != EntityOp::Spawn;
        const bool stable = patch.entity.worldId() == world.id() && world.get<const StableId>(patch.entity) != nullptr;
        const bool spawned = patch.entity.worldId() == 0 && store.spawns.contains(patch.entity.bits());
        if (targetsWorld && !stable && !spawned) {
            return makeError("editor.no_stable_id", "the editor changes only entities that have a StableId");
        }
    }
    const EntityMap map{.function = &toStoredEntity, .context = &store};
    for (EntityPatch& patch : patches) {
        remapEntities(patch, map);
    }
    droppedReferences += store.dropped;
    return patches;
}

std::vector<EntityPatch> toLive(const World& world, std::vector<EntityPatch> stored, const StableIdTable& ids) {
    LiveContext live{.world = &world, .ids = &ids, .spawned = std::vector<bool>(ids.size(), false)};
    for (const EntityPatch& patch : stored) {
        if (patch.op == EntityOp::Spawn && patch.entity.worldId() == 0 && patch.entity.index() < ids.size()) {
            live.spawned[patch.entity.index()] = true;
        }
    }
    const EntityMap map{.function = &toLiveEntity, .context = &live};
    for (EntityPatch& patch : stored) {
        remapEntities(patch, map);
    }
    return stored;
}

} // namespace ith
