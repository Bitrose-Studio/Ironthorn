# ECS

The `ecs` module stores the game world as entities with components, in archetype tables. The
design is [ADR-0014](../decisions/0014-ecs-storage-and-world.md); this page shows how to use what
exists today, which is the ADR's six slices: entities, archetype tables, typed queries and `has`;
component access and queries by reflection record, and resources; deferred commands and the
schedule of systems; component hooks and immutable components
([ADR-0030](../decisions/0030-ecs-hooks-registration-and-immutable-components.md)); the
hierarchy, enabled state and transform propagation
([ADR-0031](../decisions/0031-ecs-hierarchy-enabled-state-and-transform-propagation.md)); and
[change detection](#change-detection) with the `Added` and `Changed` filters.

## Components

A component type is a reflected struct with the `Component` role
([reflection guide](reflection.md)):

```cpp
#include "ith/reflection/Annotations.h"

namespace game {

struct Position {
    ITH_TYPE(Component)
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;
};

struct Velocity {
    ITH_TYPE(Component)
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;
};

struct Frozen {
    ITH_TYPE(Component) // a tag: a component without data
};

} // namespace game
```

- A component's identity is its reflection type id, which is the same on every compiler and
  platform. The World moves and destroys values through the type's reflection record, so any
  reflected field type works, including `std::string` and `std::vector`.
- Using a type without the `Component` role as a component asserts.
- Components are plain data. Logic lives in systems ([ADR-0016](../decisions/0016-gameplay-model-on-the-ecs.md)).

## The world

```cpp
#include "ith/ecs/World.h"

ith::World world;

const ith::Entity ship = world.spawn(game::Position{}, game::Velocity{.x = 1.0f});
world.add(ship, game::Frozen{});

if (game::Position* position = world.get<game::Position>(ship)) {
    position->y = 10.0f;
}
const bool frozen = world.has<game::Frozen>(ship); // constant time

world.remove<game::Frozen>(ship);
world.despawn(ship);
```

| Call | Result |
|---|---|
| `spawn(components...)` | A new entity with these components, each of a distinct type |
| `despawn(entity)` | Destroys the entity, following its children's [despawn policy](#the-hierarchy-and-transforms); `false` for a stale handle |
| `despawnRecursive(entity)` | Destroys the entity and everything below it |
| `isAlive(entity)` | Whether the handle refers to a live entity of this world |
| `has<T>(entity)` | Whether the entity is alive and has a `T` |
| `get<T>(entity)` | A pointer to the entity's `T`, or null |
| `add(entity, value)` | Adds `value`, or replaces the `T` the entity has; returns the stored value, read-only, or null |
| `remove<T>(entity)` | Removes the entity's `T`; `false` when it has none |
| `clear()` | Despawns every entity |
| `entityCount()` | The number of live entities |
| `placeBefore(entity, sibling)`, `forEachRoot(function)` | Order siblings; visit the roots in order ([hierarchy](#the-hierarchy-and-transforms)) |

- **Pointers from `get` and `add`** are valid until the next structural change (spawn, despawn,
  add, remove, clear). Keep handles, not pointers.
- **`add` hands out a const pointer**, for every type: [hooks](#hooks) have already seen the value,
  and a write through it would skip them. Set the fields before adding, or write through `get`.
- **A World can't be copied or moved**, so its address is stable. One thread uses it at a time.
- Up to 255 worlds exist at once (an editor world and a Play In Editor world are two).

## Entity handles

`ith::Entity` is a 64-bit handle: a 32-bit slot index, a 24-bit generation and an 8-bit world id.

- When an entity is despawned, its slot's generation goes up, so old handles become **stale**.
  A stale handle fails every lookup: `isAlive` and `has` return `false`, `get` returns null, and
  `add`, `remove` and `despawn` do nothing. It never refers to a later entity in the same slot.
- `clear()` never resets generations, so handles kept across a level change stay stale.
- A slot whose generation would wrap is retired.
- A handle from another world asserts.
- The default `Entity()` is the null handle; it fails lookups without asserting.
- Handles are runtime-only. Never save their bits or send them over the network; persistent identity is
  the `StableId` component (see below). The handle type lives in `ith/foundation/Entity.h`.
- Reflect every `Entity` field of a component or resource (`ITH_FIELD()`): a world copy remaps
  only reflected handles, and the archives write them only through an entity mapping
  ([reflection guide](reflection.md#entity-fields)).

## Stable ids

```cpp
#include "ith/ecs/StableId.h"

const ith::Entity door = world.spawn(ith::StableId{.id = ith::Uuid::generate()}, Door{});
const ith::Entity found = world.findByStableId(id); // null when no entity has it
```

`ith::StableId` ([ADR-0032](../decisions/0032-entity-references-stable-ids-and-world-copy.md)) is
an entity's persistent identity: level files, the editor's undo and selection, Keep Simulation
Changes and networking name entities by it.

- **Opt-in.** The editor gives one to every entity it creates, and level load and prefab
  instantiation assign theirs. Entities spawned at run time have none.
- **Unique per world.** A second entity with an id the world already has, or the nil id, asserts
  and is refused before anything is stored: `spawn` returns the null handle, `add` returns null, a
  spawn command is dropped. Ids may be derived rather than random (prefab instances).
- **Immutable**, with engine hooks that keep the world's lookup, so its hooks can't be set.
  `findByStableId` takes constant time on average.

## World copy

```cpp
#include "ith/ecs/WorldCopy.h"

ith::World play;
ith::copyWorld(editorWorld, play, {.excludeEditorOnly = true}); // what Play In Editor does
```

`ith::copyWorld(source, target, options)` copies a whole world into a fresh one (ADR-0032):

- **Same slots.** Each entity keeps its slot and generation under the target's world id:
  `Entity::fromParts(e.index(), e.generation(), target.id())`. The free slots are the same too.
  Match entities across worlds by `StableId`, never by handle.
- **Every value**, Transient ones included, copied through its reflection record, with every
  reflected `Entity` remapped to the copy's entity. A handle that isn't alive in the source
  (stale, null, of another world) becomes null. An unreflected `Entity` is copied as it is.
- **The hierarchy as it is:** parents, children and roots in order, `Disabled` and
  `DisabledInHierarchy`, and the stable-id lookup.
- **Every resource**, copied and remapped the same way.
- `excludeEditorOnly` leaves out every `EditorOnly` component and resource; the entities stay.
- **No hooks run**, and **ticks start over**: every value is stamped added and changed at the
  target's current tick, so its systems see everything as `Added`. System runs aren't copied.
- **Refused** (asserts, returns false, changes nothing): copying a world into itself, a source
  being iterated or running a hook, and a target that isn't fresh. Fresh means as the
  constructor left it: nothing ever spawned or reserved, no resources, no system run.

## Structure generation

```cpp
const std::uint64_t seen = world.structureGeneration();
// ... later
if (world.structureGeneration() != seen) {
    rebuildTree(world); // entities, component sets or the hierarchy changed
}
```

`World::structureGeneration()` is a 64-bit counter that moves on with every structural change
(ADR-0053 decision 2), so a view such as the editor's Outliner can follow a running game world
without walking it every frame:

- **It moves** on every spawn and despawn, every add that gives an entity a new type, every
  remove, every `ChildOf` added or replaced (reparenting), `placeBefore`, `clear` (also of an
  empty world), whether directly, by command or through `applyPatches`, and in the target of
  `copyWorld`.
- **It never moves** on value-only writes (`get`, writable queries, `forEachComponent`, an `add`
  that replaces a value of a type other than `ChildOf`, patches written in place, resources), nor
  on calls and commands that change nothing, such as those on stale handles.
- It only grows, possibly by more than one per call, so compare values for equality only.
- **Values never repeat across worlds.** Each `World` starts 2^32 above the one created before it,
  so a cached value can't match a new world that reuses the old one's `id()`, unless a world makes
  2^32 structural changes.

## Queries

```cpp
world.query<game::Position, const game::Velocity>().forEach(
    [](game::Position& position, const game::Velocity& velocity) {
        position.x += velocity.x;
    });

world.query<const game::Position>().forEach([](ith::Entity entity, const game::Position& position) {
    // the entity handle comes first when the function asks for it
});

// Filters: With<T> requires a T, Without<T> skips entities that have one.
world.query<game::Position, ith::Without<game::Frozen>>().forEach([](game::Position& position) { ... });
world.query<ith::With<game::Frozen>>().forEach([](ith::Entity entity) { ... });
```

- A query visits every entity that has all its component types.
- `const T` declares read access and `T` write access. Inside a system they must match what the
  system declared ([the schedule](#the-schedule)). A `T` term marks every value it hands out as
  changed, whether the function writes it or not ([change detection](#change-detection)): use
  `const T` for values you only read.
- **Filters** `ith::With<T>` and `ith::Without<T>` may stand anywhere among the terms. They hand
  out no values, so the function doesn't take them, and a system needs no access to their types,
  as for `has`. A query of filters only passes the entity, or nothing.
- **Change filters** `ith::Added<T>` and `ith::Changed<T>` keep the entities whose `T` was added,
  or added or changed, since the running system last ran ([change detection](#change-detection)).
  They require a `T` like `With<T>`, need `T` declared inside a system, and work only in systems.
- Naming a type twice is a compile error, filters included. A change filter may name the type of a
  term: `query<const Health, ith::Changed<Health>>()`.
- **Disabled entities are skipped**: every query leaves out entities with `ith::DisabledInHierarchy`
  ([enabled state](#enabled-state)) unless it names `Disabled` or `DisabledInHierarchy` as a term
  or filter, or includes the filter `ith::IncludeDisabled`, as the editor and level saving do.
- Each world caches the archetypes that match each query type, and adds new archetypes as they
  appear. A query object can be kept and reused while the world lives.
- **No structural changes inside `forEach`.** They assert and are refused. Record them as
  [commands](#deferred-commands) and apply them after the loop.

## Access by type record

The editor, serialization and the visual-script VM know component types only at run time, as
reflection records (`const ith::TypeInfo&`, for example from `TypeRegistry::find(id)`). The world
takes them in place of a C++ type:

```cpp
const ith::TypeInfo& health = *typeRegistry.find("game::Health");

if (world.has(ship, health)) { ... }
void* value = world.get(ship, health);   // null when the entity has none
const void* added = world.add(ship, health); // default-constructed; replaces an existing value
world.add(ship, health, &loadedValue);   // moved from a value the caller built (a loader)
world.remove(ship, health);

world.forEachComponent(ship, [](const ith::TypeInfo& type, void* value) {
    // every mutable component of the entity, with its record
});
std::as_const(world).forEachComponent(ship, [](const ith::TypeInfo& type, const void* value) {
    // every component, immutable ones included, read-only (saving a level)
});
```

- These behave like the typed functions: the same stale-handle rules, the same refusal while a
  query iterates, and the same pointer lifetime.
- The record must be a reflected struct with the `Component` role; anything else asserts and the
  call fails as for a stale handle. A record of the same type from another binary works: the
  component id is the record's type id.
- `add(entity, type)` builds the value with the record's default constructor; its hooks see that
  default value. `add(entity, type, value)` moves the value from one the caller built, which stays
  a moved-from value the caller destroys: use it when hooks must see the real value, and for
  immutable components, which can't be written after the add. Both return `const void*`.
- `forEachComponent` visits in an unspecified order, stable while the entity's set of types stays
  the same. Sort by `type.id` or `type.name` where a fixed order matters, such as in saved files.
- Each call looks the type up in the process-wide component table under a mutex. Typed access
  doesn't; prefer it in systems.

### Dynamic queries

```cpp
#include "ith/ecs/DynamicQuery.h" // included by World.h

const std::array terms{
    ith::QueryTerm{.type = &position, .access = ith::Access::Write},
    ith::QueryTerm{.type = &velocity, .access = ith::Access::Read},
};
ith::DynamicQuery query = world.dynamicQuery(terms);
query.forEach([](ith::Entity entity, std::span<void* const> components) {
    // components[0] is the Position, components[1] the Velocity
});

// Filters, as With and Without are for a typed query:
const std::array filters{ith::QueryFilter{.type = &frozen, .kind = ith::FilterKind::Without}};
ith::DynamicQuery moving = world.dynamicQuery(terms, filters);
```

- A dynamic query matches archetypes with the same code as a typed query and visits the same
  entities in the same order.
- Disabled entities are skipped as for a typed query; `FilterKind::IncludeDisabled` (a filter
  without a type) visits them too. **A query without terms visits every enabled entity**, and
  with only an `IncludeDisabled` filter every entity, which is how a level is saved.
- Each term declares read or write access. Inside a system it must match the system's declared
  access. A write term marks every value it hands out as changed; don't write through a read
  term's pointer, which `Changed` would miss.
- Filters (`FilterKind::With`, `FilterKind::Without`) hand out no values, so they are not in
  `components`, and need no declared access. `FilterKind::Added` and `FilterKind::Changed` are the
  change filters, with the rules of their typed forms: they need their type declared, may name the
  type of a term, and work only in systems.
- A null type, a type that isn't a component, or a type named twice (terms and filters together)
  asserts, and the query then visits nothing.
- The query keeps its own cache of matching archetypes. Keep the `DynamicQuery` object to reuse
  it; it's valid while the world lives.
- Structural changes inside `forEach` are refused, as for typed queries.

### Patches

`World::applyPatches` changes the components of several entities as one step and returns the
patches that undo it ([ADR-0033](../decisions/0033-reflection-property-paths-and-patches.md),
`ith/ecs/EntityPatch.h`). The Details panel, undo, prefab overrides and Keep Simulation Changes
use it; the property paths and patches themselves are in the
[reflection guide](reflection.md#property-paths-and-patches).

```cpp
const ith::TypeInfo& light = ith::typeOf<game::Light>();
ith::Patch brighter(light);
brighter.set(*ith::PropertyPath::parse(light, "intensity"), ith::OwnedValue::of(4.0f));

std::vector<ith::EntityPatch> patches(1);
patches[0].entity = lamp;
patches[0].changes.push_back(ith::ComponentChange::makePatch(std::move(brighter)));
patches[0].changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(game::Flicker{})));
patches[0].changes.push_back(ith::ComponentChange::makeRemove(ith::typeOf<game::Glow>()));

ith::Expected<std::vector<ith::EntityPatch>> undo = world.applyPatches(patches);
if (undo) {
    static_cast<void>(world.applyPatches(*undo)); // back to what it was
}
```

- **Everything is checked first**, as if the changes were made one after another, without
  touching the world: live entities, component types, components present for a patch or a
  remove, every patch prepared on a copy, and every new `ChildOf` and `StableId` as `add` would
  check it, against the state the earlier changes leave (two reparents that would make a cycle
  together are refused). On failure the world is unchanged.
- **Then the changes are made in order.** A patch of a mutable component is written in place:
  its changed tick is stamped and no hook runs, as for a write through `get`. A patch of an
  immutable component and an `Add` go through `add`, so the hooks and their commands run; a
  `Remove` goes through `remove`.
- The returned patches list the entities in reverse order, each with its changes in reverse
  order. Applying them undoes the step; applying their result redoes it.
- `Children` and `DisabledInHierarchy` are refused (`ecs.patch.world_owned`): the World keeps
  them. Hooks must not remove a component that a later change of the same step patches.
- Error codes (`ecs.patch.*` and the patch errors) are listed at `World::applyPatches`; messages
  name the entity patch and the change.

#### Spawns, despawns and placement

An entity patch also creates or destroys whole entities, and moves an entity among its siblings
([ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md) rule 8),
so the editor's undo covers create and delete as one step:

```cpp
const ith::Entity door = ith::Entity::fromParts(0, 1, 0); // placeholders: world id 0, not null
const ith::Entity hinge = ith::Entity::fromParts(1, 1, 0);
std::vector<ith::EntityPatch> patches{
    {.op = ith::EntityOp::Spawn, .entity = door},
    {.op = ith::EntityOp::Spawn, .entity = hinge},
    {.op = ith::EntityOp::Despawn, .entity = oldDoor},
};
patches[0].changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::StableId{.id = doorId})));
patches[1].changes.push_back(ith::ComponentChange::makeAdd(ith::OwnedValue::of(ith::ChildOf{.parent = door})));
patches[1].placeBefore = ith::Entity(); // at the end among the door's children
```

- **`EntityOp::Spawn`**: `entity` is a placeholder, a handle of world id 0 that isn't null and
  that no other Spawn of the call uses. Every equal handle in the call (entities, values,
  `placeBefore`) stands for the new entity. Its changes are Adds only; `ChildOf` is added first,
  then the others in order. Its inverse is a Despawn.
- **`EntityOp::Despawn`**: refused with `ecs.patch.has_children` while the entity has children,
  counting the call's earlier changes, so a delete detaches or removes them first in the same call.
  Its inverse is a Spawn, under a placeholder, of every component the World doesn't keep itself
  (Transient ones included), placed before the sibling that followed it.
- **`placeBefore`** (Modify and Spawn): after its changes the entity goes before that sibling under
  the same parent, or among the roots; a null sibling means the end (`ecs.patch.invalid_place`
  otherwise). A Modify that moved its entity, by a placement or a `ChildOf` change, is undone back
  into its old place.
- **Hook side effects on the same entity are undone exactly.** When the hooks of an Add or Remove
  change other non-Transient components of that entity (reparenting recomputes `LocalTransform`),
  the inverse restores their old values after undoing the change, so undo and redo give the same
  bits every time. The inverse also drops replaces that a later one in it makes pointless, so it
  doesn't grow over undo and redo.

**Contracts for exact undo.** A hook that changes non-Transient components of *other* entities
makes an inverse inexact; engine hooks don't, and components the editor edits must not. References
to a despawned entity held by other entities stay stale after its inverse spawns it again. Systems
that run in the editor world write only Transient data (`WorldTransform`), so the world changes
only through patches the editor's undo history records.

## Resources

Resources are typed singletons of a world: time, input state, match rules, settings. A resource
type is a reflected struct with the `Resource` role:

```cpp
struct MatchRules {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::int32_t scoreLimit = 10;
};

world.insertResource(MatchRules{.scoreLimit = 20}); // inserts or replaces
if (MatchRules* rules = world.resource<MatchRules>()) {
    rules->scoreLimit += 5;
}
world.removeResource<MatchRules>();

// By record, for tools: insertResource(type) default-constructs.
void* value = world.insertResource(rulesType);
```

| Call | Result |
|---|---|
| `insertResource(value)` | Stores the value, replacing the current one; returns the stored value |
| `resource<T>()` | The world's `T`, or null; `resource<const T>()` for read-only access |
| `removeResource<T>()` | Destroys the resource; `false` when there is none |
| `insertResource(type)`, `resource(type)`, `removeResource(type)` | The same by reflection record |
| `resourceCount()` | The number of resources |

- **A resource keeps its address** until it is removed or the world is destroyed, also when
  `insertResource` replaces its value.
- Resources are not entities: inserting and removing them is not a structural change, so it's
  allowed inside a query. `clear()` keeps them.
- Each world has its own resources. Resources that live across worlds (ADR-0016's app scope)
  belong to the application, not to a world.
- Using a type without the `Resource` role asserts, and nothing is stored.
- Systems declare their resource access like their component access ([the schedule](#the-schedule)).

## Deferred commands

Structural changes that can't happen at once, inside a query or a system, are recorded in a command
buffer and applied later:

```cpp
#include "ith/ecs/Commands.h" // included by World.h

ith::Commands commands(world);
world.query<const game::Health>().forEach([&](ith::Entity entity, const game::Health& health) {
    if (health.value <= 0) {
        commands.despawn(entity);
        commands.spawn(game::Position{}, game::Explosion{}); // returns its handle at once
    }
});
world.apply(commands); // in recording order; the buffer is empty afterwards
```

| Call | Records |
|---|---|
| `spawn(components...)` | An entity with these components; returns its reserved handle |
| `despawn(entity)` | Despawning the entity |
| `add(entity, value)` | Giving the entity `value`, replacing a value of that type |
| `remove<T>(entity)` | Removing the entity's `T` |
| `despawnRecursive(entity)`, `placeBefore(entity, sibling)` | As the World functions of the same name |
| `append(other)` | Moves `other`'s commands to the end, in order; `other` is left empty |

- **`world.apply(commands)` runs the commands in recording order.** Each behaves like the World
  function of the same name: a command on a stale handle does nothing, so nothing reaches an
  entity after its despawn, and the last `add` of a type wins.
- **A spawn's handle is reserved at once.** It fails every lookup until the spawn is applied, but
  later commands may already use it, in the same buffer or in one applied afterwards.
- **A buffer destroyed without being applied drops its commands**: their values are destroyed and
  their reserved handles stay stale for good.
- Values are copied or moved into the buffer when recorded. A buffer keeps its memory after
  `apply`, so one reused every frame stops allocating.
- `apply` is refused (asserted, the commands stay) while a query iterates or a system runs.
- **Several jobs** may record into their own buffers at the same time, spawns included, while
  nothing changes the world's structure. Give each job, or each range of a `parallelFor`, its own
  buffer and `append` them in index order: the world then ends up the same whatever the number of
  workers. Only the handle values depend on timing, and handles never decide the order of anything.
- A buffer must not outlive its world. Commands by type record (for tools and scripts) come later.

## The schedule

Systems hold the logic. A system is a descriptor with a unique name, a stage, the component and
resource types it reads and writes, and its order relative to other systems of its stage:

```cpp
#include "ith/ecs/Schedule.h"

ith::Schedule schedule;
schedule.add({
    .name = "game::integrate",
    .stage = ith::Stage::PrePhysics,
    .access = ith::accessTo<game::Position, const game::Velocity, const game::Time>(),
    .after = {"game::steer"},
    .run = [](ith::SystemContext& context) {
        const float delta = context.resource<const game::Time>()->delta;
        context.query<game::Position, const game::Velocity>().forEach(
            [&](game::Position& position, const game::Velocity& velocity) { position.x += velocity.x * delta; });
    },
});
if (const ith::Expected<void> built = schedule.build(); !built) {
    // built.error().code and .message name the problem and the systems
}
schedule.run(world); // once per frame
```

- **Stages** run in this order, each ending with a sync point:
  `Input → PrePhysics → DuringPhysics → PostPhysics → PostUpdate → Extract`. Ordinary gameplay goes
  in `PrePhysics` (the default).
- **Within a stage**, every `before`/`after` constraint holds; among the systems whose constraints
  are met, the one added first runs first. Constraints name systems of the same stage.
- **At the sync point** the commands the stage's systems recorded are applied: system by system in
  run order, each system's commands in recording order. Entities spawned in a stage exist from the
  next stage on.
- **The executor is serial and deterministic**: the same systems, added in the same order, run in
  the same order every time. A parallel executor over the declared access may follow without an
  API change, once a profile shows the gain (ADR-0014 rule 18).
- **`build()` checks everything before anything runs** and fails with an `ith::Error`:

  | Code | Cause |
  |---|---|
  | `ecs.schedule.invalid_system` | An empty name or no function |
  | `ecs.schedule.duplicate_name` | Two systems with one name |
  | `ecs.schedule.unknown_system` | A constraint names an unknown system |
  | `ecs.schedule.other_stage` | A constraint names a system of another stage |
  | `ecs.schedule.cycle` | The constraints of a stage form a cycle |
  | `ecs.schedule.invalid_access` | An access entry that is not a component or resource type, a type named twice, or write access to an [immutable](#immutable-components) component |

  Adding a system needs `build()` again; `run` on a schedule that isn't built asserts and does
  nothing.

### What a system can touch

A system reaches the world only through its `ith::SystemContext`:

| Call | Needs |
|---|---|
| `query<Terms...>()`, `dynamicQuery(terms, filters)` | Each term declared; a written term declared as written. `With` and `Without` need nothing; `Added<T>` and `Changed<T>` need `T` declared, read or write |
| `get<T>(entity)` | `T` declared; written unless `T` is const |
| `resource<T>()` | `T` declared; written unless `T` is const |
| `has<T>(entity)`, `isAlive(entity)` | Nothing: they read no values |
| `commands()` | The system's buffer, applied at the end of its stage |
| `makeCommands()` | A new buffer, e.g. one per job, to `append` to `commands()` |

- `ith::accessTo<A, const B>()` builds the access list from types: `const` reads, plain writes.
  Tools and scripts fill `access` with `ith::QueryTerm` records instead.
- **Touching undeclared data asserts**, and so does writing data declared as read-only.
- **The world's structure can't change directly while a system runs** (asserted and refused):
  record commands.
- A system may split its work over jobs: gather what it needs with a query, then run a
  `parallelFor` whose ranges record into their own buffers, and append those in range order.
- Run a schedule on the thread that owns the world. One schedule may run several worlds.

## Hooks

Hooks keep engine invariants, such as the hierarchy, as components come and go
([ADR-0030](../decisions/0030-ecs-hooks-registration-and-immutable-components.md)). Gameplay
doesn't use them: it reacts to changes in systems ([ADR-0016](../decisions/0016-gameplay-model-on-the-ecs.md)
rule 4).

```cpp
#include "ith/ecs/Hooks.h"

void onOwnerRemoved(ith::HookContext& context) {
    const auto* owner = context.value<const game::Owner>();
    if (auto* log = context.resource<game::OwnershipLog>()) {
        log->released.push_back(owner->id);
    }
    context.commands().add(context.entity(), game::Unowned{}); // applied right after the change
}

// Once, at startup, before any world stores the type:
ith::setComponentHooks<game::Owner>({.onRemove = &onOwnerRemoved});
```

- **Once per process, per component type.** Every World runs the hooks, Play In Editor copies
  included. Setting a type's hooks twice, or after a World stored the type, asserts and changes
  nothing. A hook is a plain function; state it needs lives in a world resource. `ChildOf`,
  `Disabled` and `LocalTransform` have the engine's hooks, which the first World sets, so their hooks
  can't be set ([hierarchy](#the-hierarchy-and-transforms)).
- **When they run:**

  | Hook | Runs | The value |
  |---|---|---|
  | `onAdd` | The entity gains a type it didn't have | The new value |
  | `onInsert` | After every add or replace, after `onAdd` | The new value |
  | `onReplace` | Before a value is overwritten or removed | The old value, still in place |
  | `onRemove` | Before a value is removed, also by despawn and `clear()` | The value, still in place |

- **Order.** A spawn runs `onAdd` for each component, then `onInsert` for each, in the order the
  components were given (the arguments, or the values a spawn command recorded). Despawn runs
  `onReplace` for every component, then `onRemove` for every one, while the entity is still whole,
  sorted by stable type id. `clear()` does the same for every entity before anything is destroyed;
  entities that its hooks' commands spawn survive it. The World's destructor runs no hooks.
- **What a hook may do.** `ith::HookContext` gives the entity, the type record, the value
  (`value()`, or `value<T>()`, where `T` must be the hook's type), `get`, `has`, `isAlive`,
  `resource` and `commands()`. Direct structural changes are refused while a hook runs, as inside
  a query.
- **Hook commands apply right after the change** that ran the hook, before the call that made the
  change returns. Commands that those commands' hooks record apply after them in turn,
  depth-first; nesting deeper than `ith::kMaxHookDepth` (64) is fatal, because it means hooks that
  keep triggering each other. In `World::apply` and at a schedule's sync point, each command's hook
  commands apply before the next command.

## Immutable components

A component type marked `Immutable` changes only by being replaced, so its hooks always run:

```cpp
struct Owner {
    ITH_TYPE(Component, Immutable)
    ITH_FIELD()
    std::uint64_t id = 0;
};

world.add(entity, Owner{.id = 7});                 // add or replace: allowed
const Owner* owner = world.get<const Owner>(entity); // reading: allowed
commands.add(entity, Owner{.id = 8});              // replacing later: allowed
```

| Path | For an immutable type |
|---|---|
| `get<T>` with a non-const `T`, non-const `get(entity, type)` | Asserts, gives null |
| A query with a writable term (`query<T>`, a dynamic `Access::Write` term) | Asserts, visits nothing |
| Non-const `forEachComponent` | Asserts and skips it; the const form visits it |
| A system declaring write access | `build()` fails with `ecs.schedule.invalid_access` |
| `HookContext::value<T>` with a non-const `T` | Asserts, gives null |
| `add`, `add(entity, type, value)`, `Commands::add` | Allowed: they replace the value |

`add` returns a const pointer to the stored value, for immutable types as for every other. Saving
and loading are unaffected: a loader adds the value it read with `add(entity, type, value)`.

## The hierarchy and transforms

Entities form ordered trees, the Outliner's view of the world
([ADR-0031](../decisions/0031-ecs-hierarchy-enabled-state-and-transform-propagation.md)). The
components are in `ith/ecs/Hierarchy.h`, included by `World.h`:

| Component | Holds | Written by |
|---|---|---|
| `LocalTransform` | `ith::Transform value`: the placement relative to the parent, or to the world for a root | You |
| `WorldTransform` | `ith::Mat4 matrix`: the placement in the world. Transient | Propagation |
| `ChildOf` | `Entity parent`, `bool despawnWithParent`. Immutable | You, by adding or removing it |
| `Children` | `std::vector<Entity> entities`: the parent's children, in order. Immutable, Transient | The World only |
| `Disabled` | A tag | You |
| `DisabledInHierarchy` | A tag on a disabled entity and everything below it. Transient | The World only |

```cpp
const ith::Entity car = world.spawn(ith::LocalTransform{}, ith::WorldTransform{});
const ith::Entity wheel = world.spawn(ith::LocalTransform{.value = {.translation = {1, 0, 2}}},
                                      ith::WorldTransform{},
                                      ith::ChildOf{.parent = car, .despawnWithParent = true});
const ith::Entity driver = world.spawn(ith::LocalTransform{});
world.add(driver, ith::ChildOf{.parent = car});   // reparent: the driver stays where it is
world.remove<ith::ChildOf>(driver);               // detach: a root again, where it was
world.placeBefore(wheel, otherWheel);             // reorder siblings (or roots)
world.forEachRoot([](ith::Entity root) { ... });  // the roots, in order
world.despawn(car);                               // the wheel goes too; the driver would be detached
```

- **Links.** An entity with `ChildOf` is a child of its parent; every other entity is a root.
  Change the parent by adding a new `ChildOf`, detach by removing it. A parent that is the entity
  itself, one of its descendants, a dead entity or an entity of another world asserts, and nothing
  is stored (`add` returns null, a spawn gives the null handle or a reserved handle that stays
  stale).
- **Order.** Children are appended to their parent's `Children` when they gain the parent, and
  roots are appended to the world's root list when they spawn or are detached. `placeBefore(entity,
  sibling)` moves an entity before a sibling, or to the end with a null `sibling`; a `sibling` under
  another parent asserts. Replacing a `ChildOf` with the same parent keeps the place. Never add,
  remove or write `Children` yourself.
- **World placement.** Reparenting and detaching keep the entity's world placement: the World sets
  its `LocalTransform` so that it doesn't move, computed from its ancestors' local transforms (a
  shear from a rotated, non-uniformly scaled parent is dropped, see
  [`ith::decompose`](math.md)). To keep the local transform instead, add `LocalTransform` after
  the `ChildOf`, in the same command buffer. A spawn with a `ChildOf` keeps the local transform it
  was given. An entity without `LocalTransform` counts as the identity.
- **Despawning a parent** first despawns its children marked `despawnWithParent`, recursively,
  children before their parents; its other children are detached in place and become roots.
  `despawnRecursive` despawns the whole subtree, children first, whatever the links say. The
  editor's Delete is `despawn`; its "Delete with children" is `despawnRecursive`.
- **Commands** record `despawnRecursive` and `placeBefore` like the other commands. `ChildOf` is
  added and removed with `add` and `remove`.
- Removing a child from its parent's list takes time proportional to the number of siblings.

### Enabled state

`Disabled` turns an entity and everything below it off. The World keeps `DisabledInHierarchy` on
the disabled entity and on each descendant, also as entities are reparented, detached and
despawned, so systems never walk the hierarchy to check:

- Every query, typed or dynamic, skips entities with `DisabledInHierarchy`, unless it names
  `Disabled` or `DisabledInHierarchy` as a term or filter, or includes `ith::IncludeDisabled`
  (`FilterKind::IncludeDisabled` for a dynamic query).
- `get`, `has`, `forEachComponent` and `forEachRoot` see disabled entities as usual.
- Never add or remove `DisabledInHierarchy` yourself.

### Transform propagation

```cpp
#include "ith/ecs/TransformPropagation.h"

schedule.add(ith::transformPropagationSystem()); // "ith::propagate_transforms", in PostUpdate
ith::propagateTransforms(world);                 // or directly
```

- Each root's `WorldTransform` is its local transform; each child's is its parent's world matrix
  times its own local transform (`world = parent * local`,
  [ADR-0006](../decisions/0006-math-and-coordinate-conventions.md)). An entity without a
  `WorldTransform` is skipped, but its children still get theirs; disabled entities are
  propagated too.
- **Only what changed is written** ([change detection](#change-detection)): the subtrees below
  entities whose `LocalTransform` was added, written or removed, whose `ChildOf` was added,
  replaced or removed, or whose `WorldTransform` was added. The matrices are the same as a full
  propagation gives. When at least half of the entities changed, as on the first propagation of a
  world, it propagates from every root instead, which is cheaper.
- Each `WorldTransform` written is marked changed, so `Changed<WorldTransform>` finds what moved.
  Writing `WorldTransform` yourself isn't seen by propagation and is overwritten at the next change
  above it; it is propagation's output.
- The system and direct calls each keep their own last run per world. Use one of them per world:
  both work, but each repeats the work of changes the other already wrote.
- Losing a parent or a `LocalTransform` leaves no tick, so the World stamps one instead: the
  entity's `ChildOf` or `LocalTransform` if it keeps one; otherwise it is now a root counted as the
  identity, so its `WorldTransform` is set to the identity at once and its children's `ChildOf` is
  stamped. `Changed<ChildOf>` can therefore report a child whose link didn't change.
- Code that needs a world matrix before the next propagation calls `world.worldMatrixOf(entity)`:
  the matrix `WorldTransform` will hold, composed from the local transforms up the hierarchy.

## Change detection

Systems find what was added or changed without anyone marking it
([ADR-0014](../decisions/0014-ecs-storage-and-world.md) rule 9). Replication, render extraction and
the editor's refresh build on this.

```cpp
schedule.add({
    .name = "game::on_damage",
    .access = ith::accessTo<const game::Health>(),
    .run = [](ith::SystemContext& context) {
        context.query<const game::Health, ith::Changed<game::Health>>().forEach(
            [](ith::Entity entity, const game::Health& health) { /* changed since my last run */ });
        context.query<ith::Added<game::Health>>().forEach([](ith::Entity entity) { /* new since my last run */ });
    },
});
```

**Ticks.** Every component value has an *added* tick and a *changed* tick. The World keeps a 32-bit
tick that each system run moves on by one.

| What happens | Added tick | Changed tick |
|---|---|---|
| Spawn, or `add` of a type the entity didn't have (also by commands and by record) | Stamped | Stamped |
| `add` replacing a value | Kept | Stamped |
| Mutable access: `get<T>` with a non-const `T`, non-const `get(entity, type)`, non-const `forEachComponent`, `HookContext::value<T>` with a non-const `T` | Kept | Stamped, written or not |
| A query's written term (`T`, or `Access::Write`) | Kept | Stamped on every value it hands out |
| Read-only access: `get<const T>`, a const World, `const T` terms, `Access::Read` terms | Kept | Kept |
| Moving to another archetype (adding or removing other types) | Kept | Kept |

- Mutable access counts as a change even when nothing is written. That over-reports, but a write
  can't be missed; consumers that care, such as replication's delta compression, compare values.
  Prefer `const` access for reading.
- **Resources have no ticks.** Change detection covers component values only.

**The filters.** `ith::Added<T>` keeps entities whose `T` was added since the running system last
ran; `ith::Changed<T>` keeps those whose `T` was added or changed since then. The dynamic forms are
`FilterKind::Added` and `FilterKind::Changed`. "Since the system last ran" means:

- **Per system and per world.** Each World keeps the last run of every system that ran on it, so
  one schedule may run several worlds, and two systems never share a window.
- **A run's window** goes from the system's previous run on this world, exclusive, to its current
  run, inclusive. Its own writes are stamped with its current run, so it doesn't see them on its
  next run. It does see everything after them: later systems, the commands applied at sync points,
  its own included, and code outside the schedule.
- A system later in the run order sees the writes of the earlier ones in the same `Schedule::run`.
- **A system's first run on a world sees every value as added and changed.**
- **Only inside systems.** A change filter outside a running system asserts and the query visits
  nothing: outside the schedule there is no "last run". Editor tools that need change detection run
  as systems of the editor world's schedule. If a reader outside the schedule ever needs it, the
  planned extension is an explicit cursor: the reader keeps a tick (`world.changeTick()`) and asks
  for changes since it, like event readers keep their own cursor. It doesn't exist yet.
- Inside a system, a change filter needs its type declared, read or write: it reads the type's
  ticks, which a writer changes.

**Wraparound.** Ticks are 32-bit and compared relative to the current tick, so the order holds
across the wrap. Every 2^29 ticks the World clamps every tick, and every system's last run, that is
older than about 3.2 billion ticks to that age, so an old tick never looks new. A system that hasn't
run for that long sees everything as changed on its next run. At 100 systems and 60 frames per
second, the tick wraps about every eight days; clamping runs about once a day.

## How storage works

- Entities with the same set of component types share an **archetype**: one table with a column
  per component type. Columns are contiguous and aligned to at least 64 bytes, so a query walks
  plain arrays.
- Adding or removing a component **moves the entity** to the archetype with the new set; the
  transitions are cached. Despawning moves the archetype's last row into the hole.
- Each archetype keeps a bitset of its types, which makes `has` constant time.
- Each column keeps the change ticks of its values in an array beside them, 8 bytes per value, which
  moves and swap-removes with the values.
- Values that are trivially copyable are moved with `memcpy`. Others go through their reflection
  record's move and destroy functions.

## Benchmark

`tests/ecs/EcsBenchmark.cpp` measures the workloads of ADR-0014 rule 19 that exist so far. Run it
on a Release build:

```powershell
./scripts/dev.ps1 build -Config Release
./build/windows/bin/Release/ith_ecs_benchmark.exe
# or through CTest:
ctest --test-dir build/windows -C Release -L benchmark --verbose
```

The CTest entry `ecs_benchmark` carries the `benchmark` label. Normal test runs
(`scripts/dev.ps1 test`, `verify` and the CMake test presets) exclude that label, and CI runs it as
a separate step of the `linux-clang-release` job. It never fails on its numbers.

It prints the median of 11 runs for: spawning 100k entities with two components; despawning
them; iterating two components over 100k entities in 1, 10 and 100 archetypes; adding and
removing a tag on 10k entities; random access by handle; spawning 100k entities through a command
buffer; transform propagation over 111,100 entities four levels deep (100 roots with 10 children
per entity), first on a new world, where every entity is written, then after a first propagation
with no change, with 100 leaves moved and with one root moved; and one run of a schedule of 100
systems over an empty world, which is the executor's overhead. Spawning, iteration and propagation
are compared with the targets from ADR-0014 and ADR-0031 (5.1 ms, 0.34 ms and 5.2 ms); the command
spawn and the schedule have no target. Record results with the machine they ran on.

## Tests

`tests/ecs` holds:

- the unit tests: `WorldTests.cpp`, `DynamicAccessTests.cpp` (access and queries by record),
  `ResourceTests.cpp`, `CommandTests.cpp` (deferred commands, including buffers recorded by several
  jobs and checked to give the same world for any worker count), `ScheduleTests.cpp` (stages,
  order, sync points, build errors, enforced access, systems that record from jobs) and
  `HookTests.cpp` (hook timing and order, hook commands, refused structural changes, registration
  rules, and every refused write of an immutable component) and `HierarchyTests.cpp` (links and
  order, `placeBefore`, world placement on reparent and detach, the despawn policy and
  `despawnRecursive`, invalid parents, enabled state and propagation) and `ChangeDetectionTests.cpp`
  (what stamps a tick, typed and dynamic `Added` and `Changed`, each system's window per world,
  refused use outside systems and without declared access, the 32-bit wraparound, and propagation
  that writes only the changed subtrees, removals included) and `StableIdTests.cpp` (the lookup
  through every change, duplicate and nil ids refused) and `StructureGenerationTests.cpp` (what
  moves the structure generation and what doesn't: value writes, refused calls, commands that
  change nothing and schedule runs without structural commands; values unique across worlds) and
  `WorldCopyTests.cpp` (values, slots, hierarchy and order, remapped entities in components and
  resources, EditorOnly left out, fresh ticks, no hooks, refusals) and `EntityOpTests.cpp` (Spawns
  under placeholders, Despawns refused with children and undone into the old place, placements, a
  spawn, reparent and delete in one call applied all or not at all, and a reparent undone and
  redone bit for bit without the inverse growing) and `EntityPatchTests.cpp` (`applyPatches`: writes in place without
  hooks, replaces with hooks, adds and removes, inverses that undo, reparenting and its undo,
  refused values and cycles across entities, every error, and failures that change nothing);
- a randomized model test (`ModelTests.cpp`) that runs thousands of random operations, typed and
  by record, against a simple reference model and checks every handle, value, resource and query
  result, and that the hooks of an immutable component ran as often, and with the values, the
  model expects. The model also tracks which `Health` values were added or changed since an
  observing system last ran; random schedule runs, with a writer system that writes and spawns
  through commands, check the observer's `Added` and `Changed` queries, typed and dynamic, against
  it. A second model test runs random hierarchy operations, directly and through commands, and
  checks after every step that parent and child links agree both ways, that the root list is the
  entities without `ChildOf` in order, that `DisabledInHierarchy` matches the ancestors, that
  reparenting kept the world matrix, and that propagation, which writes only what changed, by the
  system or by a direct call, matches a reference;
- `ConcurrencyTests.cpp`: worlds and component types created from several threads, worlds built
  by record in parallel jobs, and a world handed from job to job. CI runs it under
  ThreadSanitizer;
- `DynamicAccessFuzz.cpp`: a libFuzzer target (`ith_ecs_dynamic_fuzz`) that reads its input as
  operations on the record-based paths and checks them against a model. It is built in the Clang
  sanitizer builds, and CTest runs a short smoke run (label `fuzz`).
