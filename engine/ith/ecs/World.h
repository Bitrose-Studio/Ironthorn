#pragma once

// ith::World: entities and their components (ADR-0014).
//
//   ith::World world;
//   const ith::Entity ship = world.spawn(Position{}, Velocity{.x = 1.0f});
//   world.add(ship, Health{.value = 100});
//   if (Health* health = world.get<Health>(ship)) { health->value -= 10; }
//   world.query<Position, const Velocity>().forEach([](Position& p, const Velocity& v) { p.x += v.x; });
//   world.remove<Health>(ship);
//   world.despawn(ship);
//
// A component type is a reflected struct with the Component role (ITH_TYPE(Component), see
// docs/development/reflection.md). Its identity is reflection's stable type id; the World builds,
// moves and destroys values through the type's reflection record.
//
// Entities with the same set of component types share an archetype table, one column per
// component type, so queries walk contiguous arrays. Adding or removing a component moves the
// entity to another archetype; pointers and references to its components, and to the
// components of the entity that fills its old row, are invalidated by every structural change
// (spawn, despawn, add, remove, clear). Handles stay valid until the entity is despawned.
//
// Handles: see Entity.h. A stale handle (its entity was despawned, or the world was cleared)
// fails every lookup: isAlive and has return false, get returns null, add, remove and despawn
// do nothing. A handle created by another World is a programmer error: it asserts and fails the
// same way. Generations are never reset, not even by clear(), so a handle from before a level
// change never refers to a later entity.
//
// Each World takes a process-wide world id from 1 to 255 for its lifetime; more than 255 live
// worlds are fatal. Ids are handed out round-robin, so an id is reused as late as possible: a
// handle kept after its world is destroyed can alias an entity of a later world only after
// about 255 more world lifetimes, and only if index and generation match too. Don't keep handles
// of destroyed worlds.
//
// A World has a stable address: it can't be copied or moved. It is not thread-safe: one thread
// at a time uses it. Creating and destroying worlds is thread-safe.
//
// Access by type record: has, get, add and remove also take a component type's reflection record
// (`const TypeInfo&`, e.g. from TypeRegistry::find(id)) instead of a C++ type, and
// forEachComponent and dynamicQuery (DynamicQuery.h) visit components whose types are known only
// at run time. This is how the editor, serialization and the visual-script VM reach components.
//
//   const ith::TypeInfo& health = *typeRegistry.find("game::Health");
//   if (void* value = world.add(ship, health)) { ... } // default-constructed
//
// Resources are typed singletons of a world (time, input state, settings): one value per resource
// type, a reflected struct with the Resource role (ITH_TYPE(Resource)), kept apart from entities.
//
//   world.insertResource(game::MatchRules{.scoreLimit = 10});
//   if (game::MatchRules* rules = world.resource<game::MatchRules>()) { ... }
//
// A resource's address stays the same until it is removed or the world is destroyed, also when
// insertResource replaces its value. Inserting and removing resources is not a structural change:
// it is allowed while a query iterates. clear() keeps the resources.
//
// Not yet here (later ADR-0014 slices): deferred commands and the schedule with declared access to
// components and resources, hooks, hierarchy and change ticks.

#include "ith/ecs/Archetype.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/DynamicQuery.h"
#include "ith/ecs/Entity.h"
#include "ith/ecs/Query.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ith {

namespace detail {

// Destroys a resource value through its record and frees its aligned memory.
struct ResourceDelete {
    const TypeInfo* type = nullptr;
    void operator()(std::byte* memory) const noexcept;
};

// True for T that a World's typed component and resource functions take as a value; a
// TypeInfo selects the functions that take a type record.
template <typename T>
inline constexpr bool kIsValueArgument = !std::is_same_v<std::remove_cvref_t<T>, TypeInfo>;

} // namespace detail

class World {
public:
    // An empty world with a new world id. More than 255 live worlds are fatal.
    World();

    // Destroys every component value and gives the world id back.
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    // The id stored in this world's handles, from 1 to 255.
    [[nodiscard]] std::uint8_t id() const noexcept { return m_id; }

    // Creates an entity with the given components, each of a distinct component type, and
    // returns its handle. The arguments are copied or moved into locals before storage changes,
    // so passing a component stored in this world (`*world.get<Name>(other)`) is safe. Refused
    // (asserts, returns the null handle) while a query iterates.
    template <typename... Components>
    Entity spawn(Components&&... components);

    // Destroys an entity and its components. Returns false, doing nothing, for a stale or null
    // handle, and while a query iterates (asserted).
    bool despawn(Entity entity);

    // True when the handle refers to a live entity of this world.
    [[nodiscard]] bool isAlive(Entity entity) const noexcept { return findRecord(entity) != nullptr; }

    // The number of live entities.
    [[nodiscard]] std::size_t entityCount() const noexcept { return m_entityCount; }

    // True when the entity is alive and has a T. Constant time. T may be written const.
    template <typename T>
    [[nodiscard]] bool has(Entity entity) const;

    // The entity's T, or null when the entity is stale or has no T. The pointer is valid until
    // the next structural change of this world. get<const T> gives read-only access.
    template <typename T>
    [[nodiscard]] T* get(Entity entity);
    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const;

    // Gives the entity `value` as its T, replacing a T it already has, and returns the stored
    // value. Like spawn, the value is copied or moved into a local first, so it may be a
    // component stored in this world. Returns null, doing nothing, for a stale handle and while a
    // query iterates (asserted).
    template <typename T>
        requires detail::kIsValueArgument<T>
    std::remove_cvref_t<T>* add(Entity entity, T&& value);

    // Removes the entity's T. Returns false, doing nothing, when the entity is stale or has no
    // T, and while a query iterates (asserted).
    template <typename T>
    bool remove(Entity entity);

    // Despawns every entity; their handles become stale. Archetypes and query caches stay, so
    // refilling the world is cheap. Refused (asserted) while a query iterates.
    void clear();

    // A query over the entities that have every component type in Terms; see Query.h. Terms
    // are distinct component types, `const T` to read and `T` to write.
    template <typename... Terms>
    [[nodiscard]] Query<Terms...> query();

    // Access by type record. `type` is the reflection record of a component type: a reflected
    // struct with the Component role. Any other type asserts, and the call then fails as for a
    // stale handle. Each call looks the type up in the process-wide component table under its
    // mutex; typed access doesn't.

    // True when the entity is alive and has a value of `type`. Constant time.
    [[nodiscard]] bool has(Entity entity, const TypeInfo& type) const;

    // The entity's value of `type`, or null when the entity is stale or has none. The pointer is
    // valid until the next structural change of this world.
    [[nodiscard]] void* get(Entity entity, const TypeInfo& type);
    [[nodiscard]] const void* get(Entity entity, const TypeInfo& type) const;

    // Gives the entity a default-constructed value of `type`, replacing one it already has, and
    // returns it. Returns null, doing nothing, for a stale handle and while a query iterates
    // (asserted).
    void* add(Entity entity, const TypeInfo& type);

    // Removes the entity's value of `type`. Returns false, doing nothing, when the entity is stale
    // or has none, and while a query iterates (asserted).
    bool remove(Entity entity, const TypeInfo& type);

    // Calls `function(const TypeInfo& type, void* value)` for each component of the entity, in an
    // unspecified order that is stable while its set of component types stays the same; nothing
    // for a stale handle. Structural changes inside `function` are refused, as in a query.
    template <typename Function>
    void forEachComponent(Entity entity, Function&& function);

    // A query over the entities that have every component type in `terms`; no terms visit every
    // entity. See DynamicQuery.h. The query copies the terms and keeps its own cache.
    [[nodiscard]] DynamicQuery dynamicQuery(std::span<const QueryTerm> terms);

    // Resources. A resource type is a reflected struct with the Resource role; any other type
    // asserts, and the call then fails as for a missing resource.

    // Stores `value` as the world's resource of its type, replacing the value it has, and returns
    // the stored value. Like add, the value is copied or moved into a local first, so it may be
    // the resource's current value.
    template <typename T>
        requires detail::kIsValueArgument<T>
    std::remove_cvref_t<T>* insertResource(T&& value);

    // The world's T resource, or null when it has none. resource<const T> gives read-only access.
    template <typename T>
    [[nodiscard]] T* resource();
    template <typename T>
    [[nodiscard]] const T* resource() const;

    // Destroys the world's T resource. Returns false when it has none.
    template <typename T>
    bool removeResource();

    // The same by type record: insertResource default-constructs the value.
    void* insertResource(const TypeInfo& type);
    [[nodiscard]] void* resource(const TypeInfo& type);
    [[nodiscard]] const void* resource(const TypeInfo& type) const;
    bool removeResource(const TypeInfo& type);

    // The number of resources the world has.
    [[nodiscard]] std::size_t resourceCount() const noexcept { return m_resources.size(); }

private:
    struct EntityRecord {
        std::uint32_t generation = 0;
        detail::ArchetypeIndex archetype = detail::kNoArchetype; // kNoArchetype: the slot is free
        std::uint32_t row = 0;
    };

    // The record of a live entity of this world, or null.
    [[nodiscard]] const EntityRecord* findRecord(Entity entity) const noexcept;
    [[nodiscard]] EntityRecord* findRecord(Entity entity) noexcept;

    // True when structural changes are allowed; asserts when they aren't.
    [[nodiscard]] bool canChangeStructure() const noexcept;

    // Creates an entity in a new row of `archetype`, whose values the caller constructs.
    std::pair<Entity, std::uint32_t> allocateEntity(detail::ArchetypeIndex archetype);

    // Makes a slot free: a new generation, or retirement when the generation would wrap.
    void releaseSlot(std::uint32_t index);

    // The archetype of exactly these types (sorted, unique), created when missing.
    detail::ArchetypeIndex findOrCreateArchetype(std::span<const detail::ComponentIndex> types);

    // Moves an entity to `target`; components `target` lacks are destroyed, components it adds
    // are left uninitialized. Updates the records.
    void moveEntity(EntityRecord& record, detail::ArchetypeIndex target);

    // The memory where add() constructs the value: the old value's memory, destroyed, or a new
    // uninitialized slot after moving the entity. Null when add() must fail.
    void* prepareAdd(Entity entity, detail::ComponentIndex type);

    bool removeComponent(Entity entity, detail::ComponentIndex type);
    [[nodiscard]] void* findComponent(Entity entity, detail::ComponentIndex type) const noexcept;
    // The archetype for a spawn signature: `types` (sorted) cached under `slot`.
    detail::ArchetypeIndex spawnArchetype(std::size_t slot, std::span<const detail::ComponentIndex> types);
    detail::QueryState& queryState(std::size_t slot, std::span<const detail::ComponentIndex> terms);

    // The memory where insertResource constructs the value: the old value's memory, destroyed, or
    // new uninitialized memory. Null when `type` is not a resource type.
    void* prepareResource(const TypeInfo& type);
    [[nodiscard]] void* findResource(const TypeInfo& type) const noexcept;

    std::uint8_t m_id;
    std::uint32_t m_iterationDepth = 0;
    std::size_t m_entityCount = 0;
    std::vector<EntityRecord> m_records;
    std::vector<std::uint32_t> m_freeSlots;
    // Index 0 is the archetype without components.
    std::vector<std::unique_ptr<detail::Archetype>> m_archetypes;
    // Archetypes by a hash of their type list; a bucket holds the rare collisions.
    std::unordered_map<std::uint64_t, std::vector<detail::ArchetypeIndex>> m_archetypesByTypes;
    // Query caches, by type list slot (detail::typeListSlot).
    std::vector<std::unique_ptr<detail::QueryState>> m_queries;
    // The archetype of each spawn signature, by type list slot; kNoArchetype when not cached yet.
    std::vector<detail::ArchetypeIndex> m_spawnArchetypes;
    // Resource values by type id; each deleter keeps the value's type record.
    std::unordered_map<std::uint64_t, std::unique_ptr<std::byte, detail::ResourceDelete>> m_resources;
};

#ifdef _MSC_VER
// MSVC warns (C4324) that the tuple of arguments is padded when a component is over-aligned;
// keeping that alignment is intended.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename... Components>
Entity World::spawn(Components&&... components) {
    static_assert(detail::kDistinctTypes<std::remove_cvref_t<Components>...>,
                  "spawn takes at most one component of each type");
    if (!canChangeStructure()) {
        return {};
    }
    // An argument may refer to a value in this world, which the new row can move: take the values
    // into locals before storage changes.
    [[maybe_unused]] std::tuple<std::remove_cvref_t<Components>...> values{std::forward<Components>(components)...};
    constexpr std::size_t kCount = sizeof...(Components);
    const std::array<detail::ComponentIndex, kCount> types{
        detail::componentIndexOf<std::remove_cvref_t<Components>>()...};
    if (std::ranges::find(types, detail::kNoComponent) != types.end()) {
        return {}; // a type without the Component role; registering it asserted
    }
    std::array<detail::ComponentIndex, kCount> sorted = types;
    std::ranges::sort(sorted);
    const detail::ArchetypeIndex archetypeIndex =
        spawnArchetype(detail::typeListSlot<detail::SpawnList, std::remove_cvref_t<Components>...>(), sorted);
    const std::pair<Entity, std::uint32_t> created = allocateEntity(archetypeIndex);
    if constexpr (kCount > 0) {
        const std::uint32_t row = created.second;
        detail::Archetype& archetype = *m_archetypes[archetypeIndex];
        std::size_t term = 0;
        // The archetype's columns are the sorted types, so a type's column is its rank.
        const auto construct = [&]<typename T>(T& component) {
            const auto column =
                static_cast<std::size_t>(std::ranges::lower_bound(sorted, types[term++]) - sorted.begin());
            ::new (archetype.column(column).at(row)) T(std::move(component));
        };
        std::apply([&](auto&... value) { (construct(value), ...); }, values);
    }
    return created.first;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

template <typename T>
bool World::has(Entity entity) const {
    const EntityRecord* record = findRecord(entity);
    return record != nullptr &&
           m_archetypes[record->archetype]->has(detail::componentIndexOf<std::remove_const_t<T>>());
}

template <typename T>
T* World::get(Entity entity) {
    return static_cast<T*>(findComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>()));
}

template <typename T>
const T* World::get(Entity entity) const {
    return static_cast<const T*>(findComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>()));
}

template <typename T>
    requires detail::kIsValueArgument<T>
std::remove_cvref_t<T>* World::add(Entity entity, T&& value) {
    using Component = std::remove_cvref_t<T>;
    Component local(std::forward<T>(value)); // see spawn
    void* memory = prepareAdd(entity, detail::componentIndexOf<Component>());
    return memory != nullptr ? ::new (memory) Component(std::move(local)) : nullptr;
}

template <typename T>
bool World::remove(Entity entity) {
    return removeComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>());
}

template <typename... Terms>
Query<Terms...> World::query() {
    const std::array<detail::ComponentIndex, sizeof...(Terms)> terms{
        detail::componentIndexOf<std::remove_const_t<Terms>>()...};
    return Query<Terms...>(queryState(detail::typeListSlot<Terms...>(), terms), m_archetypes, m_iterationDepth);
}

template <typename Function>
void World::forEachComponent(Entity entity, Function&& function) {
    static_assert(std::is_invocable_v<Function&, const TypeInfo&, void*>,
                  "the function must take (const ith::TypeInfo&, void*)");
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return;
    }
    detail::Archetype& archetype = *m_archetypes[record->archetype];
    const std::uint32_t row = record->row;
    const detail::IterationScope scope(m_iterationDepth);
    const std::span<const detail::ComponentIndex> types = archetype.types();
    for (std::size_t column = 0; column < types.size(); ++column) {
        function(detail::componentType(types[column]), archetype.column(column).at(row));
    }
}

template <typename T>
    requires detail::kIsValueArgument<T>
std::remove_cvref_t<T>* World::insertResource(T&& value) {
    using Resource = std::remove_cvref_t<T>;
    Resource local(std::forward<T>(value)); // see spawn
    void* memory = prepareResource(typeOf<Resource>());
    return memory != nullptr ? ::new (memory) Resource(std::move(local)) : nullptr;
}

template <typename T>
T* World::resource() {
    return static_cast<T*>(findResource(typeOf<std::remove_const_t<T>>()));
}

template <typename T>
const T* World::resource() const {
    return static_cast<const T*>(findResource(typeOf<std::remove_const_t<T>>()));
}

template <typename T>
bool World::removeResource() {
    return removeResource(typeOf<std::remove_const_t<T>>());
}

} // namespace ith
