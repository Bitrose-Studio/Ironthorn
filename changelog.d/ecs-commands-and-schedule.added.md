- `ith/ecs/Commands.h`: deferred commands. An `ith::Commands` buffer records `spawn` (which
  returns the new entity's reserved handle at once), `despawn`, `add` and `remove`, and
  `world.apply(commands)` carries them out in recording order. Jobs may record into their own
  buffers at the same time and `append` them in a fixed order, so the result doesn't depend on
  the number of workers.
- `ith/ecs/Schedule.h`: systems and the schedule. A system is a descriptor with a unique name, a
  stage (`Input`, `PrePhysics`, `DuringPhysics`, `PostPhysics`, `PostUpdate`, `Extract`), the
  component and resource types it reads and writes (`ith::accessTo<A, const B>()`) and
  `before`/`after` constraints. `Schedule::build()` reports unknown names, duplicates, cycles and
  invalid access as an `ith::Error` before anything runs; `run(world)` runs the stages in order
  on a serial, deterministic executor and applies each stage's commands at its end, system by
  system in run order. Systems reach data through `ith::SystemContext`, which asserts on access
  they didn't declare.
- Direct structural changes (spawn, despawn, add, remove, clear) are refused while a system runs,
  as inside a query; record commands instead. See `docs/development/ecs.md`.
