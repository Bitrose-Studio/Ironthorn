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
// Handles: see ith/foundation/Entity.h. A stale handle (its entity was despawned, or the world was cleared)
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
// at a time uses it. Creating and destroying worlds is thread-safe. The one exception: jobs may
// record spawns into their own command buffers at the same time, as long as nothing changes the
// world's structure meanwhile (see Commands.h).
//
// Access by type record: has, get, add and remove also take a component type's reflection record
// (`const TypeInfo&`, e.g. from TypeRegistry::find(id)) instead of a C++ type, and
// forEachComponent and dynamicQuery (DynamicQuery.h) visit components whose types are known only
// at run time. This is how the editor, serialization and the visual-script VM reach components.
//
//   const ith::TypeInfo& health = *typeRegistry.find("game::Health");
//   if (const void* value = world.add(ship, health)) { ... } // default-constructed
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
// Deferred commands (Commands.h) record spawn, despawn, add and remove for later; apply(commands)
// carries them out in recording order. Systems (Schedule.h) change structure this way: while a
// system runs, direct structural changes are refused, as inside a query.
//
//   ith::Commands commands(world);
//   const ith::Entity bullet = commands.spawn(Position{}, Velocity{.x = 10.0f}); // reserved now
//   world.apply(commands); // alive from here
//
// Hooks (Hooks.h, ADR-0030): a component type can have onAdd, onInsert, onReplace and onRemove
// hooks, set once per process, which every World runs as it adds, replaces and removes the type's
// values; the commands they record are applied before the changing call returns.
//
// Immutable components (ITH_TYPE(Component, Immutable), ADR-0030) change only by being replaced
// through add, so their hooks always run. Everything that would hand out a writable value of one
// asserts and is refused: get<T> with a non-const T and get(entity, type) give null, a query or
// dynamic query that writes one visits nothing, and the non-const forEachComponent skips it. Read
// it with get<const T>, a const term, or the const forEachComponent.
//
// add returns a const pointer for every type: a write after add would skip the hooks that add has
// already run. Set the fields on the value before adding it, or write through get afterwards.
//
// The hierarchy (Hierarchy.h, ADR-0031): ChildOf links a child to its parent, the World keeps each
// parent's ordered Children and the ordered list of roots, and despawn follows each link's policy.
// Disabled entities and their descendants carry DisabledInHierarchy, which queries skip.
//
//   const ith::Entity wheel = world.spawn(ith::LocalTransform{}, ith::ChildOf{.parent = car});
//   world.placeBefore(wheel, firstWheel);         // reorder siblings
//   world.forEachRoot([](ith::Entity root) { }); // the roots, in order
//   world.despawnRecursive(car);                 // the car and everything below it
//
// Change detection (ADR-0014 rule 9): every component value has an added tick and a changed tick.
// The World keeps a 32-bit tick that each system run moves on by one. Spawning or adding a value
// stamps both ticks; replacing it through add stamps its changed tick; and every mutable access
// stamps the changed tick, whether the caller writes or not: get<T> with a non-const T, the
// non-const get(entity, type), the non-const forEachComponent, a written query term, and
// HookContext::value<T> with a non-const T. Reading through const access stamps nothing.
// Resources have no ticks. Systems (Schedule.h) read the ticks with the query filters ith::Added
// and ith::Changed, which keep the values added or changed since that system last ran on this
// World. Ticks are compared relative to the current tick and old ones are clamped periodically, so
// the 32-bit wraparound never makes an old value look new.
//
// Stable ids (StableId.h, ADR-0032): an entity with a StableId can be found by it with
// findByStableId; ids are unique per world. A whole world is copied into a fresh one with
// ith::copyWorld (WorldCopy.h).

#include "ith/ecs/Archetype.h"
#include "ith/ecs/Commands.h"
#include "ith/ecs/ComponentRegistry.h"
#include "ith/ecs/DynamicQuery.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Query.h"
#include "ith/ecs/StableId.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Mutex.h"
#include "ith/foundation/Uuid.h"
#include "ith/math/Types.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"

#include <algorithm>
#include <array>
#include <atomic>
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

class HookContext;
class World;
struct WorldCopyOptions;

// Writes the WorldTransforms of the world that changes made stale (TransformPropagation.h).
void propagateTransforms(World& world);

namespace detail {

// Propagation within the current run's tick window (TransformPropagation.cpp).
void propagateChangedTransforms(World& world);

// The steps of propagateChangedTransforms, with access to the World (TransformPropagation.cpp).
struct TransformPropagation;

// For tests only: sets the world's tick, so that tests can cross the 32-bit wraparound.
// Precondition: the world has no entities and no system has run on it.
void setChangeTickForTesting(World& world, std::uint32_t tick);

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
    // handle, and while a query iterates (asserted). Its children marked despawnWithParent go
    // first, recursively, children before parents; its other children are detached in place and
    // become roots, keeping their world placement (Hierarchy.h).
    bool despawn(Entity entity);

    // Despawns the entity and every entity below it, children before parents, whatever their
    // links' policy. Returns false as despawn does.
    bool despawnRecursive(Entity entity);

    // Moves `entity` before `sibling` among its parent's children, or among the roots when it has
    // no parent; a null `sibling` moves it to the end. Returns false, doing nothing, for a stale
    // `entity`, and for a stale `sibling`, as a command meets one. A `sibling` that is the entity
    // itself or under another parent asserts and returns false; so does a call while a query iterates.
    bool placeBefore(Entity entity, Entity sibling);

    // Calls `function(Entity)` for every root (an entity without ChildOf), disabled ones
    // included, in order: the order they became roots, changed by placeBefore. Structural changes
    // inside `function` are refused, as in a query.
    template <typename Function>
    void forEachRoot(Function&& function) const;

    // True when the handle refers to a live entity of this world.
    [[nodiscard]] bool isAlive(Entity entity) const noexcept { return findRecord(entity) != nullptr; }

    // The number of live entities.
    [[nodiscard]] std::size_t entityCount() const noexcept { return m_entityCount; }

    // The entity whose StableId is `id`, or the null handle when no live entity has it (StableId.h).
    // Constant time on average.
    [[nodiscard]] Entity findByStableId(const Uuid& id) const;

    // True when the entity is alive and has a T. Constant time. T may be written const.
    template <typename T>
    [[nodiscard]] bool has(Entity entity) const;

    // The entity's T, or null when the entity is stale or has no T. The pointer is valid until
    // the next structural change of this world. get<const T> gives read-only access; a non-const
    // T marks the value changed, and a non-const T of an immutable type asserts and gives null.
    template <typename T>
    [[nodiscard]] T* get(Entity entity);
    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const;

    // Gives the entity `value` as its T, replacing a T it already has, and returns the stored
    // value, read-only: the hooks have seen it, so writes belong before the add or through get.
    // Like spawn, the value is copied or moved into a local first, so it may be a component
    // stored in this world. Returns null, doing nothing, for a stale handle and while a query
    // iterates (asserted). Runs onReplace before an existing value is destroyed, then onAdd (for a
    // new type) and onInsert, then the commands they recorded.
    template <typename T>
        requires detail::kIsValueArgument<T>
    const std::remove_cvref_t<T>* add(Entity entity, T&& value);

    // Removes the entity's T, after its onReplace and onRemove hooks. Returns false, doing nothing,
    // when the entity is stale or has no T, and while a query iterates (asserted).
    template <typename T>
    bool remove(Entity entity);

    // Despawns every entity; their handles become stale. Archetypes and query caches stay, so
    // refilling the world is cheap. Refused (asserted) while a query iterates. Runs the remove
    // hooks of every entity first (see Hooks.h); entities spawned by their commands survive.
    void clear();

    // A query over the entities that have every component type in Terms; see Query.h. Terms
    // are distinct component types, `const T` to read and `T` to write, and filters, With<T>,
    // Without<T>, IncludeDisabled, Added<T> and Changed<T>, which the returned Query leaves out of
    // its terms. Disabled entities are skipped unless the query asks for them. A writable term of
    // an immutable type asserts, and the query visits nothing. Added and Changed work only while a
    // system runs.
    template <typename... Terms>
    [[nodiscard]] detail::QueryOf<Terms...> query();

    // Access by type record. `type` is the reflection record of a component type: a reflected
    // struct with the Component role. Any other type asserts, and the call then fails as for a
    // stale handle. Each call looks the type up in the process-wide component table under its
    // mutex; typed access doesn't.

    // True when the entity is alive and has a value of `type`. Constant time.
    [[nodiscard]] bool has(Entity entity, const TypeInfo& type) const;

    // The entity's value of `type`, or null when the entity is stale or has none. The pointer is
    // valid until the next structural change of this world. The non-const form marks the value
    // changed; it asserts and gives null for an immutable type, which is read through a const World.
    [[nodiscard]] void* get(Entity entity, const TypeInfo& type);
    [[nodiscard]] const void* get(Entity entity, const TypeInfo& type) const;

    // Gives the entity a default-constructed value of `type`, replacing one it already has, and
    // returns it, read-only as for add<T>. Returns null, doing nothing, for a stale handle and
    // while a query iterates (asserted). Hooks run as for add<T>, with the default value; to add a
    // value with its fields set, so that hooks see it, use the overload below.
    const void* add(Entity entity, const TypeInfo& type);

    // Gives the entity a value of `type` moved from `value`, which points to a valid value of
    // that type and stays a moved-from value the caller destroys. Otherwise as above. This is how
    // a loader adds a value it has read, immutable or with hooks.
    const void* add(Entity entity, const TypeInfo& type, void* value);

    // Removes the entity's value of `type`. Returns false, doing nothing, when the entity is stale
    // or has none, and while a query iterates (asserted).
    bool remove(Entity entity, const TypeInfo& type);

    // Calls `function(const TypeInfo& type, void* value)` for each component of the entity, in an
    // unspecified order that is stable while its set of component types stays the same; nothing
    // for a stale handle. Structural changes inside `function` are refused, as in a query. The
    // non-const form marks each value it visits changed and skips immutable components, asserting;
    // the const form, with
    // `function(const TypeInfo&, const void*)`, visits every component.
    template <typename Function>
    void forEachComponent(Entity entity, Function&& function);
    template <typename Function>
    void forEachComponent(Entity entity, Function&& function) const;

    // A query over the entities that have every component type in `terms`, narrowed by `filters`;
    // no terms and no filters visit every entity. See DynamicQuery.h. The query copies the terms
    // and keeps its own cache. A write term of an immutable type asserts, and the query visits
    // nothing.
    [[nodiscard]] DynamicQuery dynamicQuery(std::span<const QueryTerm> terms,
                                            std::span<const QueryFilter> filters = {});

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

    // Carries out the commands in recording order and empties the buffer, which keeps its memory
    // for reuse. Each command behaves like the World function of the same name: a command on a
    // stale handle does nothing, and a spawn makes its reserved handle alive. Refused (asserted;
    // the commands stay) while a query iterates or a system runs, and for a buffer of another
    // world (asserted). The commands that each command's hooks record are applied before the next
    // command.
    void apply(Commands& commands);

private:
    friend class Commands;
    friend class HookContext;
    friend class Schedule;
    friend void propagateTransforms(World& world);
    friend struct detail::TransformPropagation;
    friend void detail::setChangeTickForTesting(World& world, std::uint32_t tick);
    friend bool copyWorld(const World& source, World& target, const WorldCopyOptions& options);

    // The run key of propagateTransforms called directly; Schedule gives its systems keys from 1.
    static constexpr std::uint64_t kDirectPropagationKey = 0;

    // Starts a run of the system with `key`: its window is (its last run on this world, the current
    // tick]; a first run sees every tick. Returns the clock to restore. A run inside another run
    // (propagateTransforms in a system) shares the outer run's tick.
    detail::ChangeClock beginRun(std::uint64_t key);

    // Ends the run that beginRun started: records it as the system's last run, restores `outer`,
    // and, unless it was nested, moves the tick on and clamps old ticks when it is time.
    void endRun(std::uint64_t key, const detail::ChangeClock& outer);

    // Clamps every tick older than kMaxChangeAge (Query.h), last runs included.
    void clampTicks();

    // findComponent that also stamps the value's changed tick.
    [[nodiscard]] void* writeComponent(Entity entity, detail::ComponentIndex type) noexcept;

    // Stamps the changed tick of the entity's `type` value, if it has one.
    void markChanged(Entity entity, detail::ComponentIndex type) noexcept;

    // After an entity loses its parent or its LocalTransform, which leaves no tick, gives
    // propagation a tick to find: the entity's ChildOf when it `keepsParent`, else its
    // LocalTransform when it `keepsLocal` one. Otherwise the entity is now a root without a
    // LocalTransform: its WorldTransform becomes the identity, and its children's ChildOf is stamped.
    void markPlacementChanged(Entity entity, bool keepsParent, bool keepsLocal) noexcept;

    // No slot: the end of the root list.
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

    struct EntityRecord {
        std::uint32_t generation = 0;
        detail::ArchetypeIndex archetype = detail::kNoArchetype; // kNoArchetype: the slot is free
        std::uint32_t row = 0;
        // The root list (Hierarchy.h): the neighbours of a root, kNoSlot at the ends.
        std::uint32_t previousRoot = kNoSlot;
        std::uint32_t nextRoot = kNoSlot;
        bool isRoot = false;
    };

    // What reparenting or detaching an entity needs between its ChildOf hooks: its world matrix
    // before the change, and where it was among its old parent's children.
    struct Placement {
        Entity entity; // null when nothing is pending
        Mat4 world{1.0f};
        bool keepWorld = false; // false for a spawn: the local transform stays as given
        Entity oldParent;
        std::size_t oldIndex = 0;
    };

    // The record of a live entity of this world, or null.
    [[nodiscard]] const EntityRecord* findRecord(Entity entity) const noexcept;
    [[nodiscard]] EntityRecord* findRecord(Entity entity) noexcept;

    // True when structural changes are allowed; asserts when they aren't.
    [[nodiscard]] bool canChangeStructure() const noexcept;

    // Creates an entity in a new row of `archetype`, whose values the caller constructs.
    std::pair<Entity, std::uint32_t> allocateEntity(detail::ArchetypeIndex archetype);

    // Makes a reserved handle alive in a new row of `archetype` and returns the row, whose values
    // the caller constructs.
    std::uint32_t placeReserved(Entity entity, detail::ArchetypeIndex archetype);

    // Frees the slot of a live entity.
    void releaseSlot(std::uint32_t index);

    // Gives a slot that holds no entity a new generation and puts it on the free list, or retires
    // it when the generation would wrap.
    void freeSlot(std::uint32_t index);

    // Reserves the handle of an entity that a command spawns later. Safe from several threads at
    // once and next to reads of the world, but not next to structural changes.
    Entity reserveEntity();

    // Hands back handles reserved by commands that were dropped without being applied. Their slots
    // get a new generation at the next apply, so the handles stay stale. Thread-safe.
    void abandonReserved(std::span<const Entity> entities);

    // Owner thread only. settleReservations takes the slots reserved since its last call off the
    // free list and adds records for new ones; every function that changes the free list or the
    // records calls it first. freeAbandonedReservations frees the slots of abandoned reservations.
    void settleReservations();
    void freeAbandonedReservations();

    // The archetype of exactly these types (sorted, unique), created when missing.
    detail::ArchetypeIndex findOrCreateArchetype(std::span<const detail::ComponentIndex> types);

    // Moves an entity to `target`; components `target` lacks are destroyed, components it adds
    // are left uninitialized. Updates the records.
    void moveEntity(EntityRecord& record, detail::ArchetypeIndex target);

    // The memory where add() constructs the value: the old value's memory, destroyed after its
    // onReplace hook, or a new uninitialized slot after moving the entity. Null when add() must
    // fail. `added` tells whether the entity gains the type. After constructing the value, the
    // caller calls finishAdd, which runs onAdd and onInsert and applies their commands.
    void* prepareAdd(Entity entity, detail::ComponentIndex type, bool& added);
    void finishAdd(Entity entity, detail::ComponentIndex type, bool added);

    // Runs onAdd for each of a new entity's components, then onInsert for each, in the order of
    // `types` (the order they were given), then applies their commands.
    void runSpawnHooks(Entity entity, std::span<const detail::ComponentIndex> types);

    enum class HookKind : std::uint8_t {
        Add,
        Insert,
        Replace,
        Remove,
    };

    // Runs one hook, when the column's type has it, for the value in `row`. Structural changes are
    // refused meanwhile; the hook records its commands in the current hook buffer.
    void runHook(HookKind kind, detail::Archetype& archetype, std::size_t column, std::uint32_t row);

    // Runs onReplace, then onRemove, for every hooked component of the entity in `row`, in the
    // order of Archetype::hookedColumns.
    void runRemovalHooks(detail::Archetype& archetype, std::uint32_t row);

    // Applies the commands that hooks recorded at the current depth, if any (Hooks.h).
    void flushHookCommands();

    bool removeComponent(Entity entity, detail::ComponentIndex type);
    [[nodiscard]] void* findComponent(Entity entity, detail::ComponentIndex type) const noexcept;
    // The archetype for a spawn signature: `types` (sorted) cached under `slot`.
    detail::ArchetypeIndex spawnArchetype(std::size_t slot, std::span<const detail::ComponentIndex> types);
    // The cached state of a typed query; `refused` makes a new state match nothing.
    detail::QueryState& queryState(std::size_t slot, std::span<const detail::ComponentIndex> terms,
                                   std::span<const detail::ComponentIndex> with,
                                   std::span<const detail::ComponentIndex> without, bool includeDisabled, bool refused,
                                   std::span<const detail::ComponentIndex> added,
                                   std::span<const detail::ComponentIndex> changed);

    // The memory where insertResource constructs the value: the old value's memory, destroyed, or
    // new uninitialized memory. Null when `type` is not a resource type.
    void* prepareResource(const TypeInfo& type);
    [[nodiscard]] void* findResource(const TypeInfo& type) const noexcept;

    // The hierarchy (Hierarchy.h, World.cpp).

    // Puts a live entity's slot into the root list before `before` (kNoSlot: at the end), or takes
    // it out. Both do nothing when the slot already is, or isn't, in the list.
    void linkRoot(std::uint32_t index, std::uint32_t before);
    void unlinkRoot(std::uint32_t index);

    // False, asserting, when `value` is a ChildOf whose parent `entity` may not have (the entity
    // itself, a descendant, a dead entity or one of another world), or a StableId whose id is nil or
    // already another live entity's. True for every other type.
    // `entity` is null for an entity being spawned.
    [[nodiscard]] bool acceptsValue(Entity entity, detail::ComponentIndex type, const void* value) const;

    // The parent of a live entity, or null.
    [[nodiscard]] Entity parentOf(Entity entity) const;

    // The children list of a live entity, writable whatever its immutability, or null.
    [[nodiscard]] std::vector<Entity>* childrenOf(Entity entity) const;

    // The entity's world matrix from its ancestors' local transforms; identity for a dead entity.
    [[nodiscard]] Mat4 worldMatrixOf(Entity entity) const;

    // Despawns an entity and the children that go with it: those marked despawnWithParent, or all
    // of them when `everything` is set. The other children are detached first.
    void despawnTree(Entity entity, bool everything);

    // Despawns one entity, which has no children left that despawn with it.
    void despawnOne(Entity entity);

    // Appends `child` to its parent's children, creating the list (the LinkChild command).
    void linkChild(Entity child);

    // Queues adding or removing DisabledInHierarchy below and on `entity`, so that it matches
    // Disabled and the ancestors. `parentDisabled` is the state above `entity`; `ownDisabled` is
    // whether `entity` counts as Disabled.
    void refreshDisabled(Commands& commands, Entity entity, bool parentDisabled, bool ownDisabled) const;

    // The engine's hooks for ChildOf and Disabled, set once per process by the first World.
    static void onChildOfAdd(HookContext& context);
    static void onChildOfInsert(HookContext& context);
    static void onChildOfReplace(HookContext& context);
    static void onChildOfRemove(HookContext& context);
    static void onDisabledAdd(HookContext& context);
    static void onDisabledRemove(HookContext& context);
    static void onLocalTransformRemove(HookContext& context);
    static void onStableIdInsert(HookContext& context);
    static void onStableIdReplace(HookContext& context);
    static bool setHierarchyHooks();

    std::uint8_t m_id;
    mutable std::uint32_t m_iterationDepth = 0; // also counted by the const forEachComponent
    std::size_t m_entityCount = 0;
    std::vector<EntityRecord> m_records;
    // Free slots. Only the owner thread changes the list. reserveEntity hands out its entries from
    // the back through m_freeCursor and, once they run out, new slots past the end of m_records.
    std::vector<std::uint32_t> m_freeSlots;
    // How many entries of m_freeSlots are not reserved; below zero by the number of new slots
    // reserved. Equals m_freeSlots.size() when no reservation is pending.
    std::atomic<std::int64_t> m_freeCursor{0};
    Mutex m_abandonedMutex;
    std::vector<Entity> m_abandoned ITH_GUARDED_BY(m_abandonedMutex);
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
    // The buffers hooks record into, one per nesting depth, created when first needed.
    std::vector<std::unique_ptr<Commands>> m_hookCommands;
    std::uint32_t m_hookDepth = 0;
    // The root list's ends (slots), kNoSlot when there are no roots.
    std::uint32_t m_firstRoot = kNoSlot;
    std::uint32_t m_lastRoot = kNoSlot;
    // What the hierarchy hooks need to know about the change that runs them.
    Entity m_despawning;     // the entity whose despawn runs its removal hooks
    bool m_clearing = false; // clear() runs removal hooks
    bool m_spawning = false; // a spawn runs its hooks
    Placement m_placement;   // between a ChildOf's onAdd or onReplace and the hook after it
    // Change detection: the clock, the tick of the last clamp, and each system's last run by key.
    detail::ChangeClock m_clock;
    std::uint32_t m_lastTickCheck = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> m_lastRuns;
    // The entity of each stable id, kept by the StableId hooks.
    std::unordered_map<Uuid, Entity> m_stableIds;
};

template <typename Function>
void World::forEachRoot(Function&& function) const {
    static_assert(std::is_invocable_v<Function&, Entity>, "the function must take (ith::Entity)");
    const detail::IterationScope scope(m_iterationDepth);
    for (std::uint32_t index = m_firstRoot; index != kNoSlot;) {
        const EntityRecord& record = m_records[index];
        const std::uint32_t next = record.nextRoot;
        function(Entity::fromParts(index, record.generation, m_id));
        index = next;
    }
}

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
    if constexpr ((std::is_same_v<std::remove_cvref_t<Components>, ChildOf> || ...)) {
        if (!acceptsValue(Entity(), detail::componentIndexOf<ChildOf>(), &std::get<ChildOf>(values))) {
            return {};
        }
    }
    if constexpr ((std::is_same_v<std::remove_cvref_t<Components>, StableId> || ...)) {
        if (!acceptsValue(Entity(), detail::componentIndexOf<StableId>(), &std::get<StableId>(values))) {
            return {};
        }
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
        if (archetype.hasHooks()) {
            runSpawnHooks(created.first, types);
        }
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
    if constexpr (!std::is_const_v<T>) {
        if (detail::isImmutableComponent<T>() && !detail::isWritableComponent(typeOf<T>())) {
            return nullptr;
        }
        return static_cast<T*>(writeComponent(entity, detail::componentIndexOf<T>()));
    } else {
        return static_cast<T*>(findComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>()));
    }
}

template <typename T>
const T* World::get(Entity entity) const {
    return static_cast<const T*>(findComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>()));
}

template <typename T>
    requires detail::kIsValueArgument<T>
const std::remove_cvref_t<T>* World::add(Entity entity, T&& value) {
    using Component = std::remove_cvref_t<T>;
    Component local(std::forward<T>(value)); // see spawn
    const detail::ComponentIndex type = detail::componentIndexOf<Component>();
    if constexpr (std::is_same_v<Component, ChildOf> || std::is_same_v<Component, StableId>) {
        if (!acceptsValue(entity, type, &local)) {
            return nullptr;
        }
    }
    bool added = false;
    void* memory = prepareAdd(entity, type, added);
    if (memory == nullptr) {
        return nullptr;
    }
    const auto* stored = ::new (memory) Component(std::move(local));
    finishAdd(entity, type, added);
    return stored; // hooks can't change structure, so the value is still where it was built
}

template <typename T>
bool World::remove(Entity entity) {
    return removeComponent(entity, detail::componentIndexOf<std::remove_const_t<T>>());
}

template <typename... Terms>
detail::QueryOf<Terms...> World::query() {
    static_assert(sizeof...(Terms) > 0, "a query needs at least one component type or filter");
    static_assert((!std::is_reference_v<Terms> && ...), "query terms are component types, not references");
    static_assert(detail::kDistinctTypes<detail::TermComponentT<Terms>...>,
                  "a query names each component type once, filters included");
    constexpr std::size_t kCount = sizeof...(Terms);
    std::array<detail::ComponentIndex, kCount> terms{};
    std::array<detail::ComponentIndex, kCount> with{};
    std::array<detail::ComponentIndex, kCount> without{};
    std::array<detail::ComponentIndex, kCount> added{};
    std::array<detail::ComponentIndex, kCount> changed{};
    std::size_t termCount = 0;
    std::size_t withCount = 0;
    std::size_t withoutCount = 0;
    std::size_t addedCount = 0;
    std::size_t changedCount = 0;
    // A writable term of an immutable type refuses the query. Immutability never changes, so the
    // cached state of this query type matches nothing for good.
    bool writesImmutable = false;
    const auto sort = [&]<typename Term> {
        if constexpr (detail::kIsIncludeDisabled<Term>) {
            return; // names no component
        } else if constexpr (detail::kIsAdded<Term>) {
            added[addedCount++] = detail::componentIndexOf<typename detail::ChangeFilterComponent<Term>::Type>();
        } else if constexpr (detail::kIsChanged<Term>) {
            changed[changedCount++] = detail::componentIndexOf<typename detail::ChangeFilterComponent<Term>::Type>();
        } else if constexpr (detail::kIsWith<Term>) {
            with[withCount++] = detail::componentIndexOf<detail::TermComponentT<Term>>();
        } else if constexpr (detail::kIsWithout<Term>) {
            without[withoutCount++] = detail::componentIndexOf<detail::TermComponentT<Term>>();
        } else {
            using Component = std::remove_const_t<Term>;
            terms[termCount++] = detail::componentIndexOf<Component>();
            if constexpr (!std::is_const_v<Term>) {
                writesImmutable = writesImmutable || (detail::isImmutableComponent<Component>() &&
                                                      !detail::isWritableComponent(typeOf<Component>()));
            }
        }
    };
    (sort.template operator()<Terms>(), ...);
    constexpr bool kIncludeDisabled = (detail::kIsIncludeDisabled<Terms> || ...);
    detail::QueryState& state =
        queryState(detail::typeListSlot<Terms...>(), std::span(terms.data(), termCount),
                   std::span(with.data(), withCount), std::span(without.data(), withoutCount), kIncludeDisabled,
                   writesImmutable, std::span(added.data(), addedCount), std::span(changed.data(), changedCount));
    return detail::QueryOf<Terms...>(state, m_archetypes, m_iterationDepth, m_clock);
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
        const TypeInfo& type = detail::componentType(types[column]);
        if (detail::isWritableComponent(type)) {
            archetype.column(column).ticks(row).changed = m_clock.tick;
            function(type, archetype.column(column).at(row));
        }
    }
}

template <typename Function>
void World::forEachComponent(Entity entity, Function&& function) const {
    static_assert(std::is_invocable_v<Function&, const TypeInfo&, const void*>,
                  "the function must take (const ith::TypeInfo&, const void*)");
    const EntityRecord* record = findRecord(entity);
    if (record == nullptr) {
        return;
    }
    detail::Archetype& archetype = *m_archetypes[record->archetype];
    const std::uint32_t row = record->row;
    const detail::IterationScope scope(m_iterationDepth);
    const std::span<const detail::ComponentIndex> types = archetype.types();
    for (std::size_t column = 0; column < types.size(); ++column) {
        const void* value = archetype.column(column).at(row);
        function(detail::componentType(types[column]), value);
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
