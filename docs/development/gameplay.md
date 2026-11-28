# Gameplay

The `gameplay` module is the runtime home of game worlds: the `Time` world resource, and the
setup and tick of a game world's schedule that the player, the dedicated server and Play In Editor
share. The design is [ADR-0053](../decisions/0053-play-in-editor-and-simulate.md) decision 1; the
behaviour of time in a Play In Editor session is in the
[Play In Editor spec](../specs/play-in-editor.md) (3.5). It builds on the [ECS](ecs.md).

`gameplay` is a `runtime` module in the `framework` layer that depends on `foundation`,
`reflection` and `ecs` only. Later it also holds the lifecycle events and the game-world system
list of the gameplay framework (ADR-0016).

## The parts

| Name | Header | What it is |
|---|---|---|
| `Time` | `Time.h` | The game world's clock, a reflected world resource |
| `kMaxTickDelta` | `Time.h` | The longest real time one tick accounts for: 0.1 s |
| `buildGameSchedule` | `GameWorld.h` | Adds the engine's game-world systems, then the game's, and builds the schedule |
| `tickGameWorld` | `GameWorld.h` | Advances the world's `Time`, then runs the schedule once |

## Use

```cpp
#include "ith/gameplay/GameWorld.h"
#include "ith/gameplay/Time.h"

ith::Schedule schedule;
if (ith::Expected<void> built = ith::buildGameSchedule(schedule, gameSystems); !built) {
    report(built.error()); // an "ecs.schedule.*" error
}

// Once per frame, with the real seconds since the previous tick:
ith::tickGameWorld(world, schedule, seconds);
```

A system reads the time like any resource it declares:

```cpp
schedule.add({
    .name = "game::move",
    .access = ith::accessTo<Position, const Velocity, const ith::Time>(),
    .run = [](ith::SystemContext& context) {
        const float delta = context.resource<const ith::Time>()->delta;
        context.query<Position, const Velocity>().forEach([&](Position& p, const Velocity& v) {
            p.x += v.x * delta;
        });
    },
});
```

Register the module's types at startup like any reflected module's (the [reflection
guide](reflection.md#reading-the-records)):

```cpp
ITH_DECLARE_REFLECTION_MODULE(gameplay)

ith::generated::registerReflection_gameplay(registry);
```

## Time

| Field | Type | Meaning |
|---|---|---|
| `delta` | `float` | Seconds of game time this tick: `unscaledDelta × dilation`, 0 while `paused` |
| `unscaledDelta` | `float` | Real seconds this tick, clamped to [0, `kMaxTickDelta`] |
| `elapsed`, `unscaledElapsed` | `double` | The sums of the two deltas over every tick so far |
| `frame` | `std::uint64_t` | The ticks run so far: 0 before the first, 1 during it |
| `dilation` | `float` | The game's time scale, default 1; finite and not negative |
| `paused` | `bool` | The game's own pause (a pause menu): `delta` is 0, the systems still run |

`tickGameWorld(world, schedule, seconds)` advances `Time` before any system runs, so every system
of a tick reads the same, final values:

- A world that is being iterated or runs a system (`World::isIterating`) asserts, and nothing
  happens: `Time` stays as it was and no system runs.
- `seconds` is clamped to [0, `kMaxTickDelta`]: a stall, a breakpoint or a window drag counts as
  0.1 s, and a clock that went back as 0. A value that isn't finite asserts and counts as 0.
- A world without a `Time` gets a default one first.
- `delta` is computed from `dilation` and `paused` as they are at the start of the tick, so a
  system that changes them changes the next tick. A `dilation` that is negative or not finite
  asserts and counts as 0.
- `elapsed` and `unscaledElapsed` are summed in `double`: two ticks of 0.02 s give exactly the
  `double` 0.04.
- `frame` grows by one per tick, also for a tick of no time.
- Then `schedule.run(world)` runs once, under its precondition: the schedule is built and not
  running.

Only `dilation` and `paused` are the game's to write. The other fields are written by the tick and
are `ReadOnly` in reflection, so editors show them without offering to edit them.

The editor's Pause is not `Time::paused`: a paused Play In Editor session runs no tick at all, and
its frame step calls `tickGameWorld` with a fixed `seconds`.

## The game schedule

`buildGameSchedule(schedule, systems)` adds the engine's game-world systems, which in P2 are
`transformPropagationSystem()` (`ith::propagate_transforms`, `PostUpdate`), then `systems` in
order, then calls `build()`. The engine's systems therefore run first among the systems of their
stage that don't constrain them, and game systems in `PostUpdate` see this tick's
`WorldTransform`s. Its errors are `build()`'s (`ecs.schedule.duplicate_name` for a game system
named like an engine system, `ecs.schedule.invalid_system`, `ecs.schedule.unknown_system`,
`ecs.schedule.cycle`, ...). **Discard the schedule on failure:** it keeps every system it added, so
it can't run, and a second call on it fails too, adding the engine's systems again
(`ecs.schedule.duplicate_name`, unless an error `build()` checks earlier remains). Try again with
a new `Schedule`.

## Tests

`tests/gameplay/GameWorldTests.cpp` runs on a plain `World`, without the editor: the default
`Time`, clamping, values that aren't finite, fixed steps with exact frames and elapsed time,
dilation (also set by a system, which applies from the next tick), the game's pause, the values
the systems of different stages read, a tick refused while the world is iterated, the schedule's
order and errors (a cycle and a second call on a failed schedule included), and `Time`'s
reflection record with its read-only fields.
