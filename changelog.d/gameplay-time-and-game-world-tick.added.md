- The new `gameplay` module (framework layer, runtime) is the runtime home of game worlds:
  `ith::Time`, a reflected world resource with `delta`, `unscaledDelta`, `elapsed`,
  `unscaledElapsed`, `frame`, `dilation` and `paused`; `ith::buildGameSchedule`, which adds the
  engine's game-world systems (transform propagation) and then the game's to a schedule and builds
  it; and `ith::tickGameWorld(world, schedule, seconds)`, which clamps the real time to
  `ith::kMaxTickDelta` (0.1 s), advances `Time` with the game's dilation and pause, and runs the
  schedule once. The player, the dedicated server and Play In Editor tick game worlds through it.
- `ith::World::structureGeneration()` is a counter that moves on every structural change (spawn,
  despawn, add of a new type, remove, reparenting, `placeBefore`, `clear`, world copy), directly
  or by command, and never on value-only writes, so views can follow a running world without
  walking it. Its values never repeat across worlds, so a cached value can't match a new world.
