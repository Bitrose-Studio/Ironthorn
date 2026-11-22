#include "ith/ecs/World.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"
#include "ith/foundation/Mutex.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <limits>
#include <utility>

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
    m_archetypes.push_back(std::make_unique<detail::Archetype>(std::span<const detail::ComponentIndex>()));
    m_archetypesByTypes[hashTypes({})].push_back(0);
}

World::~World() {
    ITH_ASSERT_MSG(m_iterationDepth == 0, "ecs: a World destroyed while a query iterates it");
    // The archetypes destroy their values, then the resources go.
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
    record.row = m_archetypes[archetype]->pushRow(entity);
    ++m_entityCount;
    return record.row;
}

void World::releaseSlot(std::uint32_t index) {
    m_records[index].archetype = detail::kNoArchetype;
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
    const std::uint32_t row = to.pushRow(from.entities()[record.row]);
    const Entity moved = from.moveRow(record.row, to, row);
    if (!moved.isNull()) {
        m_records[moved.index()].row = record.row;
    }
    record.archetype = target;
    record.row = row;
}

bool World::despawn(Entity entity) {
    if (!canChangeStructure()) {
        return false;
    }
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return false;
    }
    const Entity moved = m_archetypes[record->archetype]->removeRow(record->row);
    if (!moved.isNull()) {
        m_records[moved.index()].row = record->row;
    }
    releaseSlot(entity.index());
    return true;
}

void* World::prepareAdd(Entity entity, detail::ComponentIndex type) {
    if (!canChangeStructure() || type == detail::kNoComponent) {
        return nullptr;
    }
    EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return nullptr;
    }
    detail::Archetype& current = *m_archetypes[record->archetype];
    if (const std::size_t column = current.columnOf(type); column != detail::Archetype::kNoColumn) {
        void* value = current.column(column).at(record->row);
        current.column(column).destroy(value);
        return value;
    }
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
    return true;
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

void World::clear() {
    if (!canChangeStructure()) {
        return;
    }
    for (const std::unique_ptr<detail::Archetype>& archetype : m_archetypes) {
        for (const Entity entity : archetype->entities()) {
            releaseSlot(entity.index());
        }
        archetype->clear();
    }
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
                break;
            }
            case Commands::Kind::Despawn:
                despawn(command.entity);
                break;
            case Commands::Kind::Add: {
                const Commands::Value& value = values.front();
                if (void* memory = prepareAdd(command.entity, value.type); memory != nullptr) {
                    detail::relocateValue(detail::componentType(value.type), memory, value.memory);
                } else {
                    detail::componentType(value.type).destroy(value.memory);
                }
                break;
            }
            case Commands::Kind::Remove:
                removeComponent(command.entity, command.type);
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

detail::QueryState& World::queryState(std::size_t slot, std::span<const detail::ComponentIndex> terms) {
    if (slot >= m_queries.size()) {
        m_queries.resize(slot + 1);
    }
    std::unique_ptr<detail::QueryState>& state = m_queries[slot];
    if (state == nullptr) {
        state = std::make_unique<detail::QueryState>();
        detail::initQueryState(*state, terms);
    }
    return *state;
}

bool World::has(Entity entity, const TypeInfo& type) const {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    const EntityRecord* record = findRecord(entity);
    return record != nullptr && m_archetypes[record->archetype]->has(index);
}

void* World::get(Entity entity, const TypeInfo& type) {
    return findComponent(entity, detail::registerComponentType(type));
}

const void* World::get(Entity entity, const TypeInfo& type) const {
    return findComponent(entity, detail::registerComponentType(type));
}

void* World::add(Entity entity, const TypeInfo& type) {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    void* memory = prepareAdd(entity, index);
    if (memory != nullptr) {
        // The registered record builds the value, as it destroys it later.
        detail::componentType(index).construct(memory);
    }
    return memory;
}

bool World::remove(Entity entity, const TypeInfo& type) {
    return removeComponent(entity, detail::registerComponentType(type));
}

DynamicQuery World::dynamicQuery(std::span<const QueryTerm> terms) {
    DynamicQuery query(terms, m_archetypes, m_iterationDepth);
    std::vector<detail::ComponentIndex> indices;
    indices.reserve(terms.size());
    for (const QueryTerm& term : terms) {
        ITH_ASSERT_MSG(term.type != nullptr, "ecs: a dynamic query term without a type");
        indices.push_back(term.type != nullptr ? detail::registerComponentType(*term.type) : detail::kNoComponent);
    }
    detail::initQueryState(query.m_state, indices);
    return query;
}

DynamicQuery::DynamicQuery(std::span<const QueryTerm> terms,
                           std::vector<std::unique_ptr<detail::Archetype>>& archetypes, std::uint32_t& iterationDepth)
    : m_terms(terms.begin(), terms.end()), m_archetypes(&archetypes), m_iterationDepth(&iterationDepth) {}

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

namespace detail {

void ResourceDelete::operator()(std::byte* memory) const noexcept {
    type->destroy(memory);
    ::operator delete(memory, std::align_val_t(type->alignment));
}

void initQueryState(QueryState& state, std::span<const ComponentIndex> terms) {
    state.terms.assign(terms.begin(), terms.end());
    for (const ComponentIndex term : terms) {
        if (term == kNoComponent) {
            state.matchesNothing = true; // registering the type asserted
            continue;
        }
        const bool repeated = state.required[term];
        ITH_ASSERT_MSG(!repeated, "ecs: a query names a component type twice");
        state.matchesNothing = state.matchesNothing || repeated;
        state.required[term] = true;
    }
}

void updateQueryState(QueryState& state, std::span<const std::unique_ptr<Archetype>> archetypes) {
    for (std::size_t index = state.archetypesSeen; index < archetypes.size(); ++index) {
        const Archetype& archetype = *archetypes[index];
        if (state.matchesNothing || (archetype.mask() & state.required) != state.required) {
            continue;
        }
        state.archetypes.push_back(static_cast<ArchetypeIndex>(index));
        for (const ComponentIndex term : state.terms) {
            state.columns.push_back(static_cast<std::uint32_t>(archetype.columnOf(term)));
        }
    }
    state.archetypesSeen = archetypes.size();
}

std::size_t nextTypeListSlot() noexcept {
    static std::atomic<std::size_t> nextSlot{0};
    return nextSlot.fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail
} // namespace ith
