#include "ith/ecs/World.h"

#include "ith/ecs/Hooks.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/foundation/Mutex.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <limits>
#include <ranges>
#include <utility>
#include <vector>

namespace ith {
namespace {

// World ids in use. Id 0 is never handed out: it marks the null handle.
struct WorldIds {
    static constexpr std::size_t kCount = 256;

    Mutex mutex;
    std::bitset<kCount> used ITH_GUARDED_BY(mutex);
    std::size_t next ITH_GUARDED_BY(mutex) = 1;
};

WorldIds& worldIds() {
    static WorldIds instance;
    return instance;
}

std::uint8_t acquireWorldId() {
    WorldIds& ids = worldIds();
    const MutexLock lock(ids.mutex);
    for (std::size_t attempt = 1; attempt < WorldIds::kCount; ++attempt) {
        const std::size_t id = ids.next;
        ids.next = id + 1 == WorldIds::kCount ? 1 : id + 1;
        if (!ids.used.test(id)) {
            ids.used.set(id);
            return static_cast<std::uint8_t>(id);
        }
    }
    ITH_FATAL("ecs: more than 255 worlds exist at once");
}

void releaseWorldId(std::uint8_t id) {
    WorldIds& ids = worldIds();
    const MutexLock lock(ids.mutex);
    ids.used.reset(id);
}

std::uint64_t hashTypes(std::span<const detail::ComponentIndex> types) noexcept {
    std::uint64_t hash = kFnv1a64Basis;
    for (const detail::ComponentIndex type : types) {
        hash = hashCombine(hash, type);
    }
    return hash;
}

} // namespace

World::World() : m_id(acquireWorldId()) {
    // The hierarchy's hooks, once per process, before any World stores ChildOf or Disabled.
    [[maybe_unused]] static const bool kHierarchyHooks = setHierarchyHooks();
    m_archetypes.push_back(std::make_unique<detail::Archetype>(std::span<const detail::ComponentIndex>()));
    m_archetypesByTypes[hashTypes({})].push_back(0);
}

World::~World() {
    ITH_ASSERT_MSG(m_iterationDepth == 0, "ecs: a World destroyed while a query iterates it");
    // No hooks run. The archetypes destroy their values, then the resources go.
    m_hookCommands.clear();
    m_archetypes.clear();
    m_resources.clear();
    releaseWorldId(m_id);
}

const World::EntityRecord* World::findRecord(Entity entity) const noexcept {
    if (entity.worldId() != m_id) {
        ITH_ASSERT_MSG(entity.isNull(), "ecs: the entity handle belongs to another world");
        return nullptr;
    }
    if (entity.index() >= m_records.size()) {
        return nullptr;
    }
    const EntityRecord& record = m_records[entity.index()];
    if (record.archetype == detail::kNoArchetype || record.generation != entity.generation()) {
        return nullptr;
    }
    return &record;
}

World::EntityRecord* World::findRecord(Entity entity) noexcept {
    return const_cast<EntityRecord*>(std::as_const(*this).findRecord(entity));
}

bool World::canChangeStructure() const noexcept {
    ITH_ASSERT_MSG(m_iterationDepth == 0,
                   "ecs: structural change while a query iterates or a system runs; record a command instead");
    return m_iterationDepth == 0;
}

Entity World::reserveEntity() {
    // Each caller takes a distinct cursor value: a free-list entry while they last, then a new
    // slot past the records. Neither the free list nor the records change until the owner thread
    // settles the reservations, so reading them here is safe.
    const std::int64_t cursor = m_freeCursor.fetch_sub(1, std::memory_order_relaxed);
    if (cursor > 0) {
        const std::uint32_t index = m_freeSlots[static_cast<std::size_t>(cursor - 1)];
        return Entity::fromParts(index, m_records[index].generation, m_id);
    }
    const std::uint64_t index = m_records.size() + static_cast<std::uint64_t>(-cursor);
    if (index >= std::numeric_limits<std::uint32_t>::max()) {
        ITH_FATAL("ecs: the world has no entity slots left");
    }
    return Entity::fromParts(static_cast<std::uint32_t>(index), 0, m_id);
}

void World::settleReservations() {
    const std::int64_t cursor = m_freeCursor.load(std::memory_order_relaxed);
    if (std::cmp_equal(cursor, m_freeSlots.size())) {
        return;
    }
    // Reserved slots are neither free nor alive until their spawn command is applied.
    if (cursor >= 0) {
        m_freeSlots.resize(static_cast<std::size_t>(cursor));
    } else {
        m_freeSlots.clear();
        m_records.resize(m_records.size() + static_cast<std::size_t>(-cursor));
    }
    m_freeCursor.store(static_cast<std::int64_t>(m_freeSlots.size()), std::memory_order_relaxed);
}

void World::abandonReserved(std::span<const Entity> entities) {
    const MutexLock lock(m_abandonedMutex);
    m_abandoned.insert(m_abandoned.end(), entities.begin(), entities.end());
}

void World::freeAbandonedReservations() {
    std::vector<Entity> abandoned;
    {
        const MutexLock lock(m_abandonedMutex);
        abandoned.swap(m_abandoned);
    }
    if (abandoned.empty()) {
        return;
    }
    settleReservations();
    for (const Entity entity : abandoned) {
        ITH_ASSERT(m_records[entity.index()].archetype == detail::kNoArchetype &&
                   m_records[entity.index()].generation == entity.generation());
        freeSlot(entity.index());
    }
}

std::pair<Entity, std::uint32_t> World::allocateEntity(detail::ArchetypeIndex archetype) {
    // The owner thread takes a slot directly, without the atomic operation of reserveEntity.
    settleReservations();
    std::uint32_t index = 0;
    if (!m_freeSlots.empty()) {
        index = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_freeCursor.store(static_cast<std::int64_t>(m_freeSlots.size()), std::memory_order_relaxed);
    } else {
        if (m_records.size() >= std::numeric_limits<std::uint32_t>::max()) {
            ITH_FATAL("ecs: the world has no entity slots left");
        }
        index = static_cast<std::uint32_t>(m_records.size());
        m_records.emplace_back();
    }
    const Entity entity = Entity::fromParts(index, m_records[index].generation, m_id);
    return {entity, placeReserved(entity, archetype)};
}

std::uint32_t World::placeReserved(Entity entity, detail::ArchetypeIndex archetype) {
    settleReservations();
    EntityRecord& record = m_records[entity.index()];
    ITH_ASSERT(record.archetype == detail::kNoArchetype && record.generation == entity.generation());
    record.archetype = archetype;
    record.row = m_archetypes[archetype]->pushRow(entity, m_clock.tick);
    ++m_entityCount;
    linkRoot(entity.index(), kNoSlot); // a ChildOf among its components takes it out again
    return record.row;
}

void World::releaseSlot(std::uint32_t index) {
    EntityRecord& record = m_records[index];
    record.archetype = detail::kNoArchetype;
    record.isRoot = false;
    record.previousRoot = kNoSlot;
    record.nextRoot = kNoSlot;
    --m_entityCount;
    freeSlot(index);
}

void World::freeSlot(std::uint32_t index) {
    settleReservations();
    EntityRecord& record = m_records[index];
    // A slot whose generation would wrap is retired, so an old handle never matches again.
    if (record.generation < Entity::kMaxGeneration) {
        ++record.generation;
        m_freeSlots.push_back(index);
        m_freeCursor.store(static_cast<std::int64_t>(m_freeSlots.size()), std::memory_order_relaxed);
    }
}

detail::ArchetypeIndex World::findOrCreateArchetype(std::span<const detail::ComponentIndex> types) {
    // Archetypes appear only between iterations, so a running query never misses one.
    ITH_ASSERT_MSG(m_iterationDepth == 0, "ecs: archetype created while a query iterates");
    std::vector<detail::ArchetypeIndex>& bucket = m_archetypesByTypes[hashTypes(types)];
    for (const detail::ArchetypeIndex index : bucket) {
        if (std::ranges::equal(m_archetypes[index]->types(), types)) {
            return index;
        }
    }
    const auto index = static_cast<detail::ArchetypeIndex>(m_archetypes.size());
    m_archetypes.push_back(std::make_unique<detail::Archetype>(types));
    bucket.push_back(index);
    return index;
}

void World::moveEntity(EntityRecord& record, detail::ArchetypeIndex target) {
    detail::Archetype& from = *m_archetypes[record.archetype];
    detail::Archetype& to = *m_archetypes[target];
    // The ticks of the values that move along are copied; a new value gets the current tick.
    const std::uint32_t row = to.pushRow(from.entities()[record.row], m_clock.tick);
    const Entity moved = from.moveRow(record.row, to, row);
    if (!moved.isNull()) {
        m_records[moved.index()].row = record.row;
    }
    record.archetype = target;
    record.row = row;
}

bool World::despawn(Entity entity) {
    if (!canChangeStructure() || findRecord(entity) == nullptr) {
        return false;
    }
    despawnTree(entity, false);
    return true;
}

bool World::despawnRecursive(Entity entity) {
    if (!canChangeStructure() || findRecord(entity) == nullptr) {
        return false;
    }
    despawnTree(entity, true);
    return true;
}

void World::despawnTree(Entity entity, bool everything) {
    const std::vector<Entity>* children = childrenOf(entity);
    if (children == nullptr || children->empty()) {
        despawnOne(entity);
        return;
    }
    // The entities that go, parents before their children; despawned in reverse, so children go
    // before their parents and each detached child is placed while its ancestors still exist.
    std::vector<Entity> order{entity};
    for (std::size_t next = 0; next < order.size(); ++next) {
        if (const std::vector<Entity>* below = childrenOf(order[next])) {
            for (const Entity child : *below) {
                const auto* link =
                    static_cast<const ChildOf*>(findComponent(child, detail::componentIndexOf<ChildOf>()));
                if (everything || (link != nullptr && link->despawnWithParent)) {
                    order.push_back(child);
                }
            }
        }
    }
    std::vector<Entity> detached;
    for (std::size_t index = order.size(); index-- > 0;) {
        const Entity current = order[index];
        if (!isAlive(current)) {
            continue; // a hook's command despawned it already
        }
        if (const std::vector<Entity>* remaining = childrenOf(current); remaining != nullptr && !remaining->empty()) {
            detached.assign(remaining->begin(), remaining->end());
            for (const Entity child : detached) {
                if (parentOf(child) == current) {
                    removeComponent(child, detail::componentIndexOf<ChildOf>());
                }
            }
        }
        despawnOne(current);
    }
}

void World::despawnOne(Entity entity) {
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return;
    }
    unlinkRoot(entity.index());
    detail::Archetype& archetype = *m_archetypes[record->archetype];
    if (archetype.hasHooks()) {
        const Entity outer = m_despawning;
        m_despawning = entity;
        runRemovalHooks(archetype, record->row); // while the entity is whole
        m_despawning = outer;
    }
    const Entity moved = archetype.removeRow(record->row);
    if (!moved.isNull()) {
        m_records[moved.index()].row = record->row;
    }
    releaseSlot(entity.index());
    flushHookCommands();
}

void World::runHook(HookKind kind, detail::Archetype& archetype, std::size_t column, std::uint32_t row) {
    const ComponentHooks* hooks = archetype.hooks(column);
    if (hooks == nullptr) {
        return;
    }
    ComponentHook hook = nullptr;
    switch (kind) {
        case HookKind::Add:
            hook = hooks->onAdd;
            break;
        case HookKind::Insert:
            hook = hooks->onInsert;
            break;
        case HookKind::Replace:
            hook = hooks->onReplace;
            break;
        case HookKind::Remove:
            hook = hooks->onRemove;
            break;
    }
    if (hook == nullptr) {
        return;
    }
    if (m_hookCommands.size() <= m_hookDepth) {
        m_hookCommands.push_back(std::make_unique<Commands>(*this));
    }
    HookContext context(*this, *m_hookCommands[m_hookDepth], archetype.entities()[row],
                        detail::componentType(archetype.types()[column]), archetype.column(column).at(row));
    // Direct structural changes are refused while the hook runs, so `row` stays where it is.
    const detail::IterationScope scope(m_iterationDepth);
    hook(context);
}

void World::runRemovalHooks(detail::Archetype& archetype, std::uint32_t row) {
    for (const std::uint32_t column : archetype.hookedColumns()) {
        runHook(HookKind::Replace, archetype, column, row);
    }
    for (const std::uint32_t column : archetype.hookedColumns()) {
        runHook(HookKind::Remove, archetype, column, row);
    }
}

void World::runSpawnHooks(Entity entity, std::span<const detail::ComponentIndex> types) {
    const EntityRecord& record = *findRecord(entity);
    detail::Archetype& archetype = *m_archetypes[record.archetype];
    m_spawning = true; // hooks run one after another, never nested: no structural change between
    for (const detail::ComponentIndex type : types) {
        runHook(HookKind::Add, archetype, archetype.columnOf(type), record.row);
    }
    for (const detail::ComponentIndex type : types) {
        runHook(HookKind::Insert, archetype, archetype.columnOf(type), record.row);
    }
    m_spawning = false;
    flushHookCommands();
}

void World::flushHookCommands() {
    if (m_hookDepth >= m_hookCommands.size() || m_hookCommands[m_hookDepth]->empty()) {
        return;
    }
    if (m_hookDepth >= kMaxHookDepth) {
        ITH_FATAL("ecs: hook commands nested too deep; hooks keep triggering each other");
    }
    Commands& commands = *m_hookCommands[m_hookDepth];
    ++m_hookDepth;
    apply(commands);
    --m_hookDepth;
}

void* World::prepareAdd(Entity entity, detail::ComponentIndex type, bool& added) {
    added = false;
    if (!canChangeStructure() || type == detail::kNoComponent) {
        return nullptr;
    }
    EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return nullptr;
    }
    detail::Archetype& current = *m_archetypes[record->archetype];
    if (const std::size_t column = current.columnOf(type); column != detail::Archetype::kNoColumn) {
        runHook(HookKind::Replace, current, column, record->row); // the old value is still there
        void* value = current.column(column).at(record->row);
        current.column(column).destroy(value);
        return value;
    }
    added = true;
    detail::ArchetypeIndex target = detail::kNoArchetype;
    if (const auto edge = current.addEdges.find(type); edge != current.addEdges.end()) {
        target = edge->second;
    } else {
        std::vector<detail::ComponentIndex> types(current.types().begin(), current.types().end());
        types.insert(std::ranges::lower_bound(types, type), type);
        target = findOrCreateArchetype(types);
        current.addEdges.emplace(type, target);
        m_archetypes[target]->removeEdges.emplace(type, record->archetype);
    }
    moveEntity(*record, target);
    detail::Archetype& destination = *m_archetypes[target];
    return destination.column(destination.columnOf(type)).at(record->row);
}

bool World::removeComponent(Entity entity, detail::ComponentIndex type) {
    if (!canChangeStructure()) {
        return false;
    }
    EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return false;
    }
    detail::Archetype& current = *m_archetypes[record->archetype];
    if (!current.has(type)) {
        return false;
    }
    const std::size_t column = current.columnOf(type);
    const bool hooked = current.hooks(column) != nullptr;
    if (hooked) {
        runHook(HookKind::Replace, current, column, record->row);
        runHook(HookKind::Remove, current, column, record->row);
    }
    detail::ArchetypeIndex target = detail::kNoArchetype;
    if (const auto edge = current.removeEdges.find(type); edge != current.removeEdges.end()) {
        target = edge->second;
    } else {
        std::vector<detail::ComponentIndex> types(current.types().begin(), current.types().end());
        types.erase(std::ranges::lower_bound(types, type));
        target = findOrCreateArchetype(types);
        current.removeEdges.emplace(type, target);
        m_archetypes[target]->addEdges.emplace(type, record->archetype);
    }
    moveEntity(*record, target);
    if (hooked) {
        flushHookCommands();
    }
    return true;
}

void World::finishAdd(Entity entity, detail::ComponentIndex type, bool added) {
    const EntityRecord& record = *findRecord(entity);
    detail::Archetype& archetype = *m_archetypes[record.archetype];
    const std::size_t column = archetype.columnOf(type);
    detail::ChangeTicks& ticks = archetype.column(column).ticks(record.row);
    ticks.changed = m_clock.tick;
    if (added) {
        ticks.added = m_clock.tick;
    }
    if (archetype.hooks(column) == nullptr) {
        return; // no hook ran, onReplace included
    }
    if (added) {
        runHook(HookKind::Add, archetype, column, record.row);
    }
    runHook(HookKind::Insert, archetype, column, record.row);
    flushHookCommands();
}

void* World::findComponent(Entity entity, detail::ComponentIndex type) const noexcept {
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return nullptr;
    }
    detail::Archetype& archetype = *m_archetypes[record->archetype];
    const std::size_t column = archetype.columnOf(type);
    return column != detail::Archetype::kNoColumn ? archetype.column(column).at(record->row) : nullptr;
}

void* World::writeComponent(Entity entity, detail::ComponentIndex type) noexcept {
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return nullptr;
    }
    detail::Archetype& archetype = *m_archetypes[record->archetype];
    const std::size_t column = archetype.columnOf(type);
    if (column == detail::Archetype::kNoColumn) {
        return nullptr;
    }
    archetype.column(column).ticks(record->row).changed = m_clock.tick;
    return archetype.column(column).at(record->row);
}

void World::markChanged(Entity entity, detail::ComponentIndex type) noexcept {
    static_cast<void>(writeComponent(entity, type));
}

void World::markPlacementChanged(Entity entity, bool keepsParent, bool keepsLocal) noexcept {
    if (keepsParent) {
        markChanged(entity, detail::componentIndexOf<ChildOf>());
        return;
    }
    if (keepsLocal && has<LocalTransform>(entity)) {
        markChanged(entity, detail::componentIndexOf<LocalTransform>());
        return;
    }
    // A root without a LocalTransform: its world matrix is the identity, as propagation would
    // write, and its children start from it.
    if (auto* world =
            static_cast<WorldTransform*>(writeComponent(entity, detail::componentIndexOf<WorldTransform>()))) {
        world->matrix = Mat4(1.0f);
    }
    if (const std::vector<Entity>* children = childrenOf(entity)) {
        for (const Entity child : *children) {
            markChanged(child, detail::componentIndexOf<ChildOf>());
        }
    }
}

detail::ChangeClock World::beginRun(std::uint64_t key) {
    const detail::ChangeClock outer = m_clock;
    const std::uint32_t thisRun = m_clock.tick;
    const auto found = m_lastRuns.find(key);
    // A first run starts further back than any tick can be, so it sees every value.
    m_clock.lastRun = found != m_lastRuns.end() ? found->second : thisRun - detail::kMaxChangeAge - 1;
    m_clock.inSystem = true;
    return outer;
}

void World::endRun(std::uint64_t key, const detail::ChangeClock& outer) {
    m_lastRuns[key] = m_clock.tick;
    m_clock = outer;
    if (outer.inSystem) {
        return; // nested: the outer run moves the tick on
    }
    ++m_clock.tick;
    if (m_clock.tick - m_lastTickCheck >= detail::kTickCheckInterval) {
        clampTicks();
    }
}

void World::clampTicks() {
    const std::uint32_t tick = m_clock.tick;
    for (const std::unique_ptr<detail::Archetype>& archetype : m_archetypes) {
        archetype->clampTicks(tick, detail::kMaxChangeAge);
    }
    // A last run older than every tick keeps meaning "everything is new".
    for (auto& [key, lastRun] : m_lastRuns) {
        if (tick - lastRun > detail::kMaxChangeAge + 1) {
            lastRun = tick - detail::kMaxChangeAge - 1;
        }
    }
    m_lastTickCheck = tick;
}

void World::clear() {
    if (!canChangeStructure()) {
        return;
    }
    // Every remove hook runs while the whole world is still there. The hierarchy's hooks do
    // nothing: everything they would keep goes.
    m_clearing = true;
    for (const std::unique_ptr<detail::Archetype>& archetype : m_archetypes) {
        if (archetype->hasHooks()) {
            for (std::uint32_t row = 0; row < archetype->size(); ++row) {
                runRemovalHooks(*archetype, row);
            }
        }
    }
    m_clearing = false;
    for (const std::unique_ptr<detail::Archetype>& archetype : m_archetypes) {
        for (const Entity entity : archetype->entities()) {
            releaseSlot(entity.index());
        }
        archetype->clear();
    }
    m_firstRoot = kNoSlot;
    m_lastRoot = kNoSlot;
    flushHookCommands();
}

void World::apply(Commands& commands) {
    ITH_ASSERT_MSG(commands.m_world == this, "ecs: commands applied to another world");
    if (commands.m_world != this || !canChangeStructure()) {
        return;
    }
    freeAbandonedReservations();
    // Consecutive spawns of one set of types share an archetype lookup.
    std::vector<detail::ComponentIndex> types;
    std::vector<detail::ComponentIndex> spawnTypes;
    std::vector<detail::ComponentIndex> hookOrder;
    detail::ArchetypeIndex spawnArchetype = detail::kNoArchetype;
    for (const Commands::Command& command : commands.m_commands) {
        const std::span<Commands::Value> values(commands.m_values.data() + command.firstValue, command.valueCount);
        switch (command.kind) {
            case Commands::Kind::Spawn: {
                types.clear();
                for (const Commands::Value& value : values) {
                    types.push_back(value.type);
                }
                std::ranges::sort(types);
                if (std::ranges::any_of(values, [&](const Commands::Value& value) {
                        return !acceptsValue(Entity(), value.type, value.memory);
                    })) {
                    // Refused: the values go, and the reserved handle stays stale for good.
                    for (const Commands::Value& value : values) {
                        detail::componentType(value.type).destroy(value.memory);
                    }
                    freeSlot(command.entity.index());
                    break;
                }
                if (spawnArchetype == detail::kNoArchetype || types != spawnTypes) {
                    spawnArchetype = findOrCreateArchetype(types);
                    spawnTypes = types;
                }
                const std::uint32_t row = placeReserved(command.entity, spawnArchetype);
                detail::Archetype& archetype = *m_archetypes[spawnArchetype];
                for (const Commands::Value& value : values) {
                    detail::Column& column = archetype.column(archetype.columnOf(value.type));
                    column.relocate(column.at(row), value.memory);
                }
                if (archetype.hasHooks()) {
                    // In the order the values were recorded, which is the order they were given.
                    hookOrder.clear();
                    for (const Commands::Value& value : values) {
                        hookOrder.push_back(value.type);
                    }
                    runSpawnHooks(command.entity, hookOrder);
                }
                break;
            }
            case Commands::Kind::Despawn:
                despawn(command.entity);
                break;
            case Commands::Kind::Add: {
                const Commands::Value& value = values.front();
                bool added = false;
                void* memory = nullptr;
                if (acceptsValue(command.entity, value.type, value.memory)) {
                    memory = prepareAdd(command.entity, value.type, added);
                }
                if (memory != nullptr) {
                    detail::relocateValue(detail::componentType(value.type), memory, value.memory);
                    finishAdd(command.entity, value.type, added);
                } else {
                    detail::componentType(value.type).destroy(value.memory);
                }
                break;
            }
            case Commands::Kind::Remove:
                removeComponent(command.entity, command.type);
                break;
            case Commands::Kind::DespawnRecursive:
                despawnRecursive(command.entity);
                break;
            case Commands::Kind::PlaceBefore:
                placeBefore(command.entity, command.other);
                break;
            case Commands::Kind::LinkChild:
                linkChild(command.entity);
                break;
        }
    }
    commands.reset();
}

detail::ArchetypeIndex World::spawnArchetype(std::size_t slot, std::span<const detail::ComponentIndex> types) {
    if (slot >= m_spawnArchetypes.size()) {
        m_spawnArchetypes.resize(slot + 1, detail::kNoArchetype);
    }
    detail::ArchetypeIndex& cached = m_spawnArchetypes[slot];
    if (cached == detail::kNoArchetype) {
        cached = findOrCreateArchetype(types);
    }
    return cached;
}

detail::QueryState& World::queryState(std::size_t slot, std::span<const detail::ComponentIndex> terms,
                                      std::span<const detail::ComponentIndex> with,
                                      std::span<const detail::ComponentIndex> without, bool includeDisabled,
                                      bool refused, std::span<const detail::ComponentIndex> added,
                                      std::span<const detail::ComponentIndex> changed) {
    if (slot >= m_queries.size()) {
        m_queries.resize(slot + 1);
    }
    std::unique_ptr<detail::QueryState>& state = m_queries[slot];
    if (state == nullptr) {
        state = std::make_unique<detail::QueryState>();
        detail::initQueryState(*state, terms, with, without, includeDisabled, added, changed);
        state->matchesNothing = state->matchesNothing || refused;
    }
    return *state;
}

bool World::has(Entity entity, const TypeInfo& type) const {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    const EntityRecord* record = findRecord(entity);
    return record != nullptr && m_archetypes[record->archetype]->has(index);
}

void* World::get(Entity entity, const TypeInfo& type) {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    if (index == detail::kNoComponent || !detail::isWritableComponent(type)) {
        return nullptr;
    }
    return writeComponent(entity, index);
}

const void* World::get(Entity entity, const TypeInfo& type) const {
    return findComponent(entity, detail::registerComponentType(type));
}

const void* World::add(Entity entity, const TypeInfo& type) {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    // The default value, for the types acceptsValue checks: a default ChildOf has no parent and a
    // default StableId has the nil id, which are both refused.
    const ChildOf noParent;
    const StableId noId;
    const void* defaultValue = index == detail::componentIndexOf<StableId>() ? static_cast<const void*>(&noId)
                                                                             : static_cast<const void*>(&noParent);
    if (!acceptsValue(entity, index, defaultValue)) {
        return nullptr;
    }
    bool added = false;
    void* memory = prepareAdd(entity, index, added);
    if (memory != nullptr) {
        // The registered record builds the value, as it destroys it later.
        detail::componentType(index).construct(memory);
        finishAdd(entity, index, added);
    }
    return memory;
}

const void* World::add(Entity entity, const TypeInfo& type, void* value) {
    return addRecordValue(entity, type, value, true);
}

const void* World::addRecordValue(Entity entity, const TypeInfo& type, void* value, bool checkCycle) {
    ITH_ASSERT_MSG(value != nullptr, "ecs: add by record without a value");
    const detail::ComponentIndex index = detail::registerComponentType(type);
    if (value == nullptr || !acceptsValue(entity, index, value, checkCycle)) {
        return nullptr;
    }
    bool added = false;
    void* memory = prepareAdd(entity, index, added);
    if (memory != nullptr) {
        // The registered record builds the value, as it destroys it later.
        detail::componentType(index).move(memory, value);
        finishAdd(entity, index, added);
    }
    return memory;
}

bool World::remove(Entity entity, const TypeInfo& type) {
    return removeComponent(entity, detail::registerComponentType(type));
}

DynamicQuery World::dynamicQuery(std::span<const QueryTerm> terms, std::span<const QueryFilter> filters) {
    DynamicQuery query(terms, m_archetypes, m_iterationDepth, m_clock);
    std::vector<detail::ComponentIndex> indices;
    indices.reserve(terms.size());
    bool writesImmutable = false;
    for (const QueryTerm& term : terms) {
        ITH_ASSERT_MSG(term.type != nullptr, "ecs: a dynamic query term without a type");
        indices.push_back(term.type != nullptr ? detail::registerComponentType(*term.type) : detail::kNoComponent);
        if (term.type != nullptr && term.access == Access::Write && !detail::isWritableComponent(*term.type)) {
            writesImmutable = true;
        }
    }
    std::vector<detail::ComponentIndex> with;
    std::vector<detail::ComponentIndex> without;
    std::vector<detail::ComponentIndex> added;
    std::vector<detail::ComponentIndex> changed;
    bool includeDisabled = false;
    for (const QueryFilter& filter : filters) {
        if (filter.kind == FilterKind::IncludeDisabled) {
            includeDisabled = true;
            continue;
        }
        ITH_ASSERT_MSG(filter.type != nullptr, "ecs: a dynamic query filter without a type");
        const detail::ComponentIndex index =
            filter.type != nullptr ? detail::registerComponentType(*filter.type) : detail::kNoComponent;
        switch (filter.kind) {
            case FilterKind::With:
                with.push_back(index);
                break;
            case FilterKind::Without:
                without.push_back(index);
                break;
            case FilterKind::Added:
                added.push_back(index);
                break;
            case FilterKind::Changed:
                changed.push_back(index);
                break;
            case FilterKind::IncludeDisabled:
                break;
        }
    }
    detail::initQueryState(query.m_state, indices, with, without, includeDisabled, added, changed);
    query.m_state.matchesNothing = query.m_state.matchesNothing || writesImmutable;
    return query;
}

DynamicQuery::DynamicQuery(std::span<const QueryTerm> terms,
                           std::vector<std::unique_ptr<detail::Archetype>>& archetypes, std::uint32_t& iterationDepth,
                           const detail::ChangeClock& clock)
    : m_terms(terms.begin(), terms.end()),
      m_archetypes(&archetypes),
      m_iterationDepth(&iterationDepth),
      m_clock(&clock) {}

namespace {

bool isResourceType(const TypeInfo& type) noexcept {
    const bool isResource = type.kind == TypeKind::Struct && type.role == TypeRole::Resource;
    ITH_ASSERT_MSG(isResource, "ecs: a resource type must be a reflected struct with ITH_TYPE(Resource)");
    return isResource;
}

} // namespace

void* World::prepareResource(const TypeInfo& type) {
    if (!isResourceType(type)) {
        return nullptr;
    }
    if (const auto found = m_resources.find(type.id); found != m_resources.end()) {
        const TypeInfo& known = *found->second.get_deleter().type;
        if (known.name != type.name || known.size != type.size || known.alignment != type.alignment) {
            ITH_FATAL("ecs: two resource types with the same id but a different name or layout");
        }
        // The value is replaced in place, so the resource keeps its address.
        std::byte* memory = found->second.get();
        known.destroy(memory);
        return memory;
    }
    std::unique_ptr<std::byte, detail::ResourceDelete> memory(
        static_cast<std::byte*>(::operator new(type.size, std::align_val_t(type.alignment))),
        detail::ResourceDelete{&type});
    std::byte* value = memory.get();
    m_resources.emplace(type.id, std::move(memory));
    return value;
}

void* World::findResource(const TypeInfo& type) const noexcept {
    if (!isResourceType(type)) {
        return nullptr;
    }
    const auto found = m_resources.find(type.id);
    return found != m_resources.end() ? found->second.get() : nullptr;
}

void* World::insertResource(const TypeInfo& type) {
    void* memory = prepareResource(type);
    if (memory != nullptr) {
        // The stored record destroys the value later, so it builds it too.
        m_resources.find(type.id)->second.get_deleter().type->construct(memory);
    }
    return memory;
}

void* World::resource(const TypeInfo& type) {
    return findResource(type);
}

const void* World::resource(const TypeInfo& type) const {
    return findResource(type);
}

bool World::removeResource(const TypeInfo& type) {
    if (!isResourceType(type)) {
        return false;
    }
    return m_resources.erase(type.id) == 1;
}

// The hierarchy (ADR-0031).

namespace {

// The entity's local transform as a matrix; identity without a LocalTransform.
Mat4 localMatrix(const LocalTransform* local) noexcept {
    return local != nullptr ? toMatrix(local->value) : Mat4(1.0f);
}

} // namespace

void World::linkRoot(std::uint32_t index, std::uint32_t before) {
    EntityRecord& record = m_records[index];
    if (record.isRoot) {
        return;
    }
    record.isRoot = true;
    if (before == kNoSlot) {
        record.previousRoot = m_lastRoot;
        record.nextRoot = kNoSlot;
        if (m_lastRoot != kNoSlot) {
            m_records[m_lastRoot].nextRoot = index;
        } else {
            m_firstRoot = index;
        }
        m_lastRoot = index;
        return;
    }
    EntityRecord& next = m_records[before];
    record.previousRoot = next.previousRoot;
    record.nextRoot = before;
    if (next.previousRoot != kNoSlot) {
        m_records[next.previousRoot].nextRoot = index;
    } else {
        m_firstRoot = index;
    }
    next.previousRoot = index;
}

void World::unlinkRoot(std::uint32_t index) {
    EntityRecord& record = m_records[index];
    if (!record.isRoot) {
        return;
    }
    if (record.previousRoot != kNoSlot) {
        m_records[record.previousRoot].nextRoot = record.nextRoot;
    } else {
        m_firstRoot = record.nextRoot;
    }
    if (record.nextRoot != kNoSlot) {
        m_records[record.nextRoot].previousRoot = record.previousRoot;
    } else {
        m_lastRoot = record.previousRoot;
    }
    record.isRoot = false;
    record.previousRoot = kNoSlot;
    record.nextRoot = kNoSlot;
}

Entity World::parentOf(Entity entity) const {
    const auto* link = static_cast<const ChildOf*>(findComponent(entity, detail::componentIndexOf<ChildOf>()));
    return link != nullptr ? link->parent : Entity();
}

std::vector<Entity>* World::childrenOf(Entity entity) const {
    auto* children = static_cast<Children*>(findComponent(entity, detail::componentIndexOf<Children>()));
    return children != nullptr ? &children->entities : nullptr;
}

Mat4 World::worldMatrixOf(Entity entity) const {
    // Walk up to the root, then compose downwards: world = parent * local.
    std::vector<Entity> chain;
    for (Entity current = entity; isAlive(current); current = parentOf(current)) {
        chain.push_back(current);
    }
    Mat4 world(1.0f);
    for (const Entity current : std::views::reverse(chain)) {
        world = world * localMatrix(get<const LocalTransform>(current));
    }
    return world;
}

bool World::acceptsValue(Entity entity, detail::ComponentIndex type, const void* value, bool checkCycle) const {
    if (type == detail::componentIndexOf<StableId>()) {
        if (!entity.isNull() && (entity.worldId() != m_id || !isAlive(entity))) {
            return true; // stale: add refuses it anyway, without asserting
        }
        const Uuid& id = static_cast<const StableId*>(value)->id;
        const bool valid = !id.isNil();
        ITH_ASSERT_MSG(valid, "ecs: a StableId can't be the nil id");
        const Entity owner = findByStableId(id);
        const bool unique = owner.isNull() || owner == entity;
        ITH_ASSERT_MSG(unique, "ecs: another entity of the world already has this StableId");
        return valid && unique;
    }
    if (type != detail::componentIndexOf<ChildOf>()) {
        return true;
    }
    // A stale entity: add refuses it anyway, without asserting, as any command on a stale handle.
    const bool spawning = entity.isNull();
    if (!spawning && (entity.worldId() != m_id || !isAlive(entity))) {
        return true;
    }
    const Entity parent = static_cast<const ChildOf*>(value)->parent;
    const bool alive = !parent.isNull() && parent.worldId() == m_id && isAlive(parent);
    ITH_ASSERT_MSG(alive, "ecs: a ChildOf's parent must be a live entity of the same world");
    if (!alive) {
        return false;
    }
    if (spawning || !checkCycle) {
        return true; // a new entity has no descendants; addAcyclic's caller proved there is no cycle
    }
    for (Entity ancestor = parent; !ancestor.isNull(); ancestor = parentOf(ancestor)) {
        if (ancestor == entity) {
            ITH_ASSERT_MSG(false, "ecs: a ChildOf's parent can't be the entity itself or one of its descendants");
            return false;
        }
    }
    return true;
}

bool World::placeBefore(Entity entity, Entity sibling) {
    if (!canChangeStructure() || !isAlive(entity)) {
        return false;
    }
    const Entity parent = parentOf(entity);
    if (!sibling.isNull()) {
        if (!isAlive(sibling)) {
            return false; // like any command on a stale handle
        }
        const bool valid = sibling != entity && parentOf(sibling) == parent;
        ITH_ASSERT_MSG(valid, "ecs: placeBefore needs a sibling under the same parent, other than the entity");
        if (!valid) {
            return false;
        }
    }
    if (parent.isNull()) {
        unlinkRoot(entity.index());
        linkRoot(entity.index(), sibling.isNull() ? kNoSlot : sibling.index());
        return true;
    }
    std::vector<Entity>* children = childrenOf(parent);
    if (children == nullptr) {
        return false;
    }
    const auto same = [](Entity wanted) { return [wanted](Entity candidate) { return candidate == wanted; }; };
    if (const auto found = std::ranges::find_if(*children, same(entity)); found != children->end()) {
        children->erase(found);
    }
    const auto position = sibling.isNull() ? children->end() : std::ranges::find_if(*children, same(sibling));
    children->insert(position, entity);
    markChanged(parent, detail::componentIndexOf<Children>());
    return true;
}

void World::linkChild(Entity child) {
    const Entity parent = parentOf(child);
    if (!isAlive(parent)) {
        return;
    }
    if (std::vector<Entity>* children = childrenOf(parent)) {
        if (std::ranges::find_if(*children, [child](Entity entry) { return entry == child; }) == children->end()) {
            children->push_back(child);
            markChanged(parent, detail::componentIndexOf<Children>());
        }
        return;
    }
    add(parent, Children{.entities = {child}});
}

void World::refreshDisabled(Commands& commands, Entity entity, bool parentDisabled, bool ownDisabled) const {
    // Every entity below one whose state doesn't change already matches it, so the walk stops
    // there.
    struct Pending {
        Entity entity;
        bool parentDisabled = false;
    };

    std::vector<Pending> pending{{.entity = entity, .parentDisabled = parentDisabled}};
    while (!pending.empty()) {
        const Pending current = pending.back();
        pending.pop_back();
        const bool own = current.entity == entity ? ownDisabled : has<Disabled>(current.entity);
        const bool disabled = current.parentDisabled || own;
        if (disabled == has<DisabledInHierarchy>(current.entity)) {
            continue;
        }
        if (disabled) {
            commands.add(current.entity, DisabledInHierarchy{});
        } else {
            commands.remove<DisabledInHierarchy>(current.entity);
        }
        if (const std::vector<Entity>* children = childrenOf(current.entity)) {
            for (const Entity child : *children) {
                pending.push_back({.entity = child, .parentDisabled = disabled});
            }
        }
    }
}

void World::onChildOfAdd(HookContext& context) {
    World& world = *context.m_world;
    const Entity entity = context.entity();
    world.unlinkRoot(entity.index());
    if (!world.m_spawning) {
        // The entity was a root, so its world placement is its local transform.
        world.m_placement = {
            .entity = entity, .world = localMatrix(world.get<const LocalTransform>(entity)), .keepWorld = true};
    }
}

void World::onChildOfReplace(HookContext& context) {
    World& world = *context.m_world;
    if (world.m_clearing) {
        return;
    }
    const Entity entity = context.entity();
    const Entity oldParent = static_cast<const ChildOf*>(context.value())->parent;
    std::size_t oldIndex = 0;
    if (std::vector<Entity>* children = world.childrenOf(oldParent)) {
        const auto found = std::ranges::find_if(*children, [entity](Entity entry) { return entry == entity; });
        oldIndex = static_cast<std::size_t>(found - children->begin());
        if (found != children->end()) {
            children->erase(found);
            world.markChanged(oldParent, detail::componentIndexOf<Children>());
        }
    }
    if (world.m_despawning == entity) {
        return;
    }
    world.m_placement = {.entity = entity,
                         .world = world.worldMatrixOf(entity), // the old link is still in place
                         .keepWorld = true,
                         .oldParent = oldParent,
                         .oldIndex = oldIndex};
}

void World::onChildOfInsert(HookContext& context) {
    World& world = *context.m_world;
    const Entity entity = context.entity();
    const Entity parent = static_cast<const ChildOf*>(context.value())->parent;
    const Placement placement = world.m_placement.entity == entity ? world.m_placement : Placement{};
    world.m_placement = {};
    if (std::vector<Entity>* children = world.childrenOf(parent)) {
        const bool sameParent = placement.oldParent == parent && placement.oldIndex <= children->size();
        if (sameParent) {
            children->insert(children->begin() + static_cast<std::ptrdiff_t>(placement.oldIndex), entity);
        } else if (children->empty() || children->back() != entity) {
            // A new link, or a move from another parent, whose replace hook took the entity out of the
            // old list: the entity is in no list of this parent, so appending needs no search, and a
            // wide parent fills in linear time. The back check covers a LinkChild that already ran.
            children->push_back(entity);
        }
        world.markChanged(parent, detail::componentIndexOf<Children>());
    } else {
        // Creating the list changes the parent's structure, so it waits until the hooks are done.
        context.commands().m_commands.push_back({.kind = Commands::Kind::LinkChild, .entity = entity});
    }
    if (placement.keepWorld && placement.oldParent != parent) {
        if (auto* local = static_cast<LocalTransform*>(
                world.writeComponent(entity, detail::componentIndexOf<LocalTransform>()))) {
            local->value = decompose(glm::inverse(world.worldMatrixOf(parent)) * placement.world);
        }
    }
    world.refreshDisabled(context.commands(), entity, world.has<DisabledInHierarchy>(parent),
                          world.has<Disabled>(entity));
}

void World::onChildOfRemove(HookContext& context) {
    World& world = *context.m_world;
    const Entity entity = context.entity();
    if (world.m_clearing || world.m_despawning == entity) {
        return;
    }
    const Placement placement = world.m_placement.entity == entity ? world.m_placement : Placement{};
    world.m_placement = {};
    world.linkRoot(entity.index(), kNoSlot);
    if (placement.keepWorld) {
        if (auto* local = static_cast<LocalTransform*>(
                world.writeComponent(entity, detail::componentIndexOf<LocalTransform>()))) {
            local->value = decompose(placement.world);
        }
    }
    world.markPlacementChanged(entity, false, true); // losing ChildOf leaves no tick of its own
    world.refreshDisabled(context.commands(), entity, false, world.has<Disabled>(entity));
}

void World::onDisabledAdd(HookContext& context) {
    const World& world = *context.m_world;
    if (world.m_clearing) {
        return;
    }
    const Entity entity = context.entity();
    world.refreshDisabled(context.commands(), entity, world.has<DisabledInHierarchy>(world.parentOf(entity)), true);
}

void World::onDisabledRemove(HookContext& context) {
    const World& world = *context.m_world;
    const Entity entity = context.entity();
    if (world.m_clearing || world.m_despawning == entity) {
        return;
    }
    world.refreshDisabled(context.commands(), entity, world.has<DisabledInHierarchy>(world.parentOf(entity)), false);
}

bool World::setHierarchyHooks() {
    const bool childOf = setComponentHooks<ChildOf>({
        .onAdd = &onChildOfAdd,
        .onInsert = &onChildOfInsert,
        .onReplace = &onChildOfReplace,
        .onRemove = &onChildOfRemove,
    });
    const bool disabled = setComponentHooks<Disabled>({.onAdd = &onDisabledAdd, .onRemove = &onDisabledRemove});
    const bool local = setComponentHooks<LocalTransform>({.onRemove = &onLocalTransformRemove});
    // onReplace also runs before a removal, so it is the only hook that drops an id.
    const bool stableId = setComponentHooks<StableId>({.onInsert = &onStableIdInsert, .onReplace = &onStableIdReplace});
    return childOf && disabled && local && stableId;
}

Entity World::findByStableId(const Uuid& id) const {
    const auto found = m_stableIds.find(id);
    return found != m_stableIds.end() ? found->second : Entity();
}

void World::onStableIdInsert(HookContext& context) {
    // acceptsValue refused a duplicate before the value was stored.
    context.m_world->m_stableIds[context.value<const StableId>()->id] = context.entity();
}

void World::onStableIdReplace(HookContext& context) {
    World& world = *context.m_world;
    const auto found = world.m_stableIds.find(context.value<const StableId>()->id);
    if (found != world.m_stableIds.end() && found->second == context.entity()) {
        world.m_stableIds.erase(found);
    }
}

void World::onLocalTransformRemove(HookContext& context) {
    World& world = *context.m_world;
    const Entity entity = context.entity();
    if (world.m_clearing || world.m_despawning == entity) {
        return;
    }
    world.markPlacementChanged(entity, world.has<ChildOf>(entity), false); // it now counts as the identity
}

namespace detail {

void ResourceDelete::operator()(std::byte* memory) const noexcept {
    type->destroy(memory);
    ::operator delete(memory, std::align_val_t(type->alignment));
}

void initQueryState(QueryState& state, std::span<const ComponentIndex> terms, std::span<const ComponentIndex> with,
                    std::span<const ComponentIndex> without, bool includeDisabled,
                    std::span<const ComponentIndex> added, std::span<const ComponentIndex> changed) {
    state.terms.assign(terms.begin(), terms.end());
    // Change filters require their type, and may name the type of a term or a With filter.
    for (const auto& [types, isAdded] : {std::pair{added, std::uint8_t{1}}, std::pair{changed, std::uint8_t{0}}}) {
        for (const ComponentIndex type : types) {
            if (type == kNoComponent) {
                state.matchesNothing = true; // registering the type asserted
                continue;
            }
            state.changes.push_back(type);
            state.added.push_back(isAdded);
        }
    }
    const auto mark = [&state](std::span<const ComponentIndex> types, ComponentMask& mask) {
        for (const ComponentIndex type : types) {
            if (type == kNoComponent) {
                state.matchesNothing = true; // registering the type asserted
                continue;
            }
            const bool repeated = state.required[type] || state.excluded[type];
            ITH_ASSERT_MSG(!repeated, "ecs: a query names a component type twice");
            state.matchesNothing = state.matchesNothing || repeated;
            mask[type] = true;
        }
    };
    mark(terms, state.required);
    mark(with, state.required);
    mark(without, state.excluded);
    for (const ComponentIndex type : state.changes) {
        state.required[type] = true;
    }
    // Disabled entities are skipped unless the query asks for them (ADR-0031 rule 9).
    const ComponentIndex inHierarchy = componentIndexOf<DisabledInHierarchy>();
    const ComponentIndex disabled = componentIndexOf<Disabled>();
    const bool namesDisabled = state.required[inHierarchy] || state.excluded[inHierarchy] || state.required[disabled] ||
                               state.excluded[disabled];
    if (!includeDisabled && !namesDisabled) {
        state.excluded[inHierarchy] = true;
    }
}

void updateQueryState(QueryState& state, std::span<const std::unique_ptr<Archetype>> archetypes) {
    for (std::size_t index = state.archetypesSeen; index < archetypes.size(); ++index) {
        const Archetype& archetype = *archetypes[index];
        if (state.matchesNothing || (archetype.mask() & state.required) != state.required ||
            (archetype.mask() & state.excluded).any()) {
            continue;
        }
        state.archetypes.push_back(static_cast<ArchetypeIndex>(index));
        for (const ComponentIndex term : state.terms) {
            state.columns.push_back(static_cast<std::uint32_t>(archetype.columnOf(term)));
        }
        for (const ComponentIndex type : state.changes) {
            state.columns.push_back(static_cast<std::uint32_t>(archetype.columnOf(type)));
        }
    }
    state.archetypesSeen = archetypes.size();
}

bool changeFiltersAllowed(const ChangeClock& clock) noexcept {
    ITH_ASSERT_MSG(clock.inSystem, "ecs: Added and Changed filters work only while a system runs");
    return clock.inSystem;
}

const void* addAcyclic(World& world, Entity entity, const TypeInfo& type, void* value) {
    return world.addRecordValue(entity, type, value, false);
}

void setChangeTickForTesting(World& world, std::uint32_t tick) {
    ITH_ASSERT(world.entityCount() == 0 && world.m_lastRuns.empty());
    world.m_clock.tick = tick;
    world.m_lastTickCheck = tick;
}

std::size_t nextTypeListSlot() noexcept {
    static std::atomic<std::size_t> nextSlot{0};
    return nextSlot.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail
} // namespace ith
