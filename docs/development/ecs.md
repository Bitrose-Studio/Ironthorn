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
- Handles are runtime-only. Never save them or send them over the network; persistent identity is
  a stable id component (ADR-0014 rule 3).

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
- Code that needs a world matrix before the next propagation multiplies the local transforms of
  the entity's ancestors itself.

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
  that writes only the changed subtrees, removals included);
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
