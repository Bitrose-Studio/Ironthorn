- `ith/ecs`: `ith::World` stores entities and their components in archetype tables. Entities are
  64-bit generational handles (`ith::Entity`) that go stale when despawned or when the world is
  cleared; `spawn`, `despawn`, `add`, `remove`, `get` and constant-time `has`; typed queries
  (`world.query<Position, const Velocity>().forEach(...)`) with cached archetype matches.
  Component types are reflected structs with the `Component` role. See `docs/development/ecs.md`.
- `ith_ecs_benchmark` measures spawning, despawning, iteration, tag churn and random access over
  100k entities.
