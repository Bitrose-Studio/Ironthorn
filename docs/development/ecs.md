# ECS

The `ecs` module stores the game world as entities with components, in archetype tables. The
design is [ADR-0014](../decisions/0014-ecs-storage-and-world.md); this page shows how to use what
exists today, which is the ADR's first slice: entities, archetype tables, typed queries and `has`.
Deferred commands, the schedule, resources, dynamic queries, hooks, the hierarchy and change
detection follow in later slices.

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
- `const T` declares read access and `T` write access. The schedule will order systems by these
  declarations, and change detection will mark written values as changed.
- Naming a type twice is a compile error.
- Each world caches the archetypes that match each query type, and adds new archetypes as they
  appear. A query object can be kept and reused while the world lives.
- **No structural changes inside `forEach`.** They assert and are refused. Collect the entities
  and change them after the loop; deferred commands come with the schedule.

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
removing a tag on 10k entities; and random access by handle. Spawning and iteration are compared
with the targets from ADR-0014 (5.1 ms and 0.34 ms). Record results with the machine they ran on.

## Tests

`tests/ecs` holds the unit tests (`WorldTests.cpp`) and a randomized model test
(`ModelTests.cpp`) that runs thousands of random operations against a simple reference model and
checks every handle, value and query result.
