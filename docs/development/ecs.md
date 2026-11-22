# ECS

The `ecs` module stores the game world as entities with components, in archetype tables. The
design is [ADR-0014](../decisions/0014-ecs-storage-and-world.md); this page shows how to use what
exists today, which is the ADR's first three slices: entities, archetype tables, typed queries and
`has`; component access and queries by reflection record, and resources; then deferred commands
and the schedule of systems. Hooks, the hierarchy and change detection follow in later slices.

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
| `despawn(entity)` | Destroys the entity; `false` for a stale handle |
| `isAlive(entity)` | Whether the handle refers to a live entity of this world |
| `has<T>(entity)` | Whether the entity is alive and has a `T` |
| `get<T>(entity)` | A pointer to the entity's `T`, or null |
| `add(entity, value)` | Adds `value`, or replaces the `T` the entity has; returns the stored value or null |
| `remove<T>(entity)` | Removes the entity's `T`; `false` when it has none |
| `clear()` | Despawns every entity |
| `entityCount()` | The number of live entities |

- **Pointers from `get` and `add`** are valid until the next structural change (spawn, despawn,
  add, remove, clear). Keep handles, not pointers.
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
```

- A query visits every entity that has all its component types.
- `const T` declares read access and `T` write access. Inside a system they must match what the
  system declared ([the schedule](#the-schedule)), and change detection will mark written values
  as changed.
- Naming a type twice is a compile error.
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
void* added = world.add(ship, health);   // default-constructed; replaces an existing value
world.remove(ship, health);

world.forEachComponent(ship, [](const ith::TypeInfo& type, void* value) {
    // every component of the entity, with its record
});
```

- These behave like the typed functions: the same stale-handle rules, the same refusal while a
  query iterates, and the same pointer lifetime.
- The record must be a reflected struct with the `Component` role; anything else asserts and the
  call fails as for a stale handle. A record of the same type from another binary works: the
  component id is the record's type id.
- `add` builds the value with the record's default constructor. Write fields through the pointer
  afterwards (the serializer reads into it).
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
```

- A dynamic query matches archetypes with the same code as a typed query and visits the same
  entities in the same order.
- **A query without terms visits every entity**, which is how a level is saved.
- Each term declares read or write access. Inside a system it must match the system's declared
  access, and change detection will use it; don't write through a read term's pointer.
- A null type, a type that isn't a component, or a type named twice asserts, and the query then
  visits nothing.
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
  | `ecs.schedule.invalid_access` | An access entry that is not a component or resource type, or a type named twice |

  Adding a system needs `build()` again; `run` on a schedule that isn't built asserts and does
  nothing.

### What a system can touch

A system reaches the world only through its `ith::SystemContext`:

| Call | Needs |
|---|---|
| `query<Terms...>()`, `dynamicQuery(terms)` | Each term declared; a written term declared as written |
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

## How storage works

- Entities with the same set of component types share an **archetype**: one table with a column
  per component type. Columns are contiguous and aligned to at least 64 bytes, so a query walks
  plain arrays.
- Adding or removing a component **moves the entity** to the archetype with the new set; the
  transitions are cached. Despawning moves the archetype's last row into the hole.
- Each archetype keeps a bitset of its types, which makes `has` constant time.
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
buffer; and one run of a schedule of 100 systems over an empty world, which is the executor's
overhead. Spawning and iteration are compared with the targets from ADR-0014 (5.1 ms and 0.34 ms);
the last two have no target. Record results with the machine they ran on.

## Tests

`tests/ecs` holds:

- the unit tests: `WorldTests.cpp`, `DynamicAccessTests.cpp` (access and queries by record),
  `ResourceTests.cpp`, `CommandTests.cpp` (deferred commands, including buffers recorded by several
  jobs and checked to give the same world for any worker count) and `ScheduleTests.cpp` (stages,
  order, sync points, build errors, enforced access, systems that record from jobs);
- a randomized model test (`ModelTests.cpp`) that runs thousands of random operations, typed and
  by record, against a simple reference model and checks every handle, value, resource and query
  result;
- `ConcurrencyTests.cpp`: worlds and component types created from several threads, worlds built
  by record in parallel jobs, and a world handed from job to job. CI runs it under
  ThreadSanitizer;
- `DynamicAccessFuzz.cpp`: a libFuzzer target (`ith_ecs_dynamic_fuzz`) that reads its input as
  operations on the record-based paths and checks them against a model. It is built in the Clang
  sanitizer builds, and CTest runs a short smoke run (label `fuzz`).
