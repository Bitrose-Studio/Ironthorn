#pragma once

// Setting up and ticking a game world (ADR-0053 decision 1): the code the player, the dedicated
// server and Play In Editor share.
//
//   ith::Schedule schedule;
//   if (auto built = ith::buildGameSchedule(schedule, gameSystems); !built) { report(built.error()); }
//   // once per frame, with the real seconds since the previous tick:
//   ith::tickGameWorld(world, schedule, seconds);
//
// Not thread-safe: tick a world on the thread that owns it, as Schedule::run requires.

#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Expected.h"

#include <span>

namespace ith {

// Adds the engine's game-world systems to `schedule` (transformPropagationSystem, in PostUpdate),
// then `systems` in order, then builds it. Systems already in `schedule` stay ahead of them. The
// order within a stage follows Schedule's rules: the engine's systems come first among the
// systems of their stage that don't constrain them. Fails with build()'s error ("ecs.schedule.*").
// On failure discard the schedule: it keeps every system added, so it can't run, and calling this
// again on it fails too, adding the engine's systems a second time ("ecs.schedule.duplicate_name"
// unless an error build() checks earlier remains). Use a new Schedule for the next attempt.
[[nodiscard]] Expected<void> buildGameSchedule(Schedule& schedule, std::span<const SystemDescriptor> systems);

// One tick of a game world. A world that is being iterated or runs a system (World::isIterating)
// asserts, and nothing happens: its Time stays as it was and no system runs. Otherwise, first its
// Time (Time.h) advances:
// - `seconds`, the real time since the previous tick, is clamped to [0, kMaxTickDelta]; a value that
//   isn't finite asserts and counts as 0;
// - a world without a Time gets a default one first;
// - unscaledDelta is the clamped value and unscaledElapsed grows by it;
// - delta is unscaledDelta x dilation, or 0 while paused, and elapsed grows by it; a dilation that
//   is negative or not finite asserts and counts as 0;
// - frame grows by 1.
// Then `schedule` runs on the world once. Precondition as for Schedule::run: the schedule is built
// and not running (asserted; Time has advanced, but nothing runs).
void tickGameWorld(World& world, Schedule& schedule, double seconds);

} // namespace ith
