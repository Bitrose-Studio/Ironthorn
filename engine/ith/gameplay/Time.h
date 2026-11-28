#pragma once

// ith::Time: the game world's clock, a world resource (ADR-0053 decision 1, ADR-0016 rule 12).
//
//   const ith::Time& time = *context.resource<const ith::Time>(); // in a system that declared it
//   position.x += speed * time.delta;
//
// ith::tickGameWorld (GameWorld.h) advances it once per tick, before any system runs, so every
// system of a tick reads the same, final values. Games write `dilation` and `paused`; the other
// fields are written only by the tick, and are ReadOnly in reflection, so editors show but don't
// edit them.

#include "ith/reflection/Annotations.h"

#include <cstdint>

namespace ith {

// The longest real time one tick accounts for, in seconds: a longer gap (a stall, a breakpoint, a
// window drag) counts as this much.
inline constexpr double kMaxTickDelta = 0.1;

struct Time {
    ITH_TYPE(Resource)

    // Seconds of game time this tick: unscaledDelta x dilation, 0 while paused.
    ITH_FIELD(ReadOnly)
    float delta = 0.0f;

    // Real seconds this tick, clamped to [0, kMaxTickDelta].
    ITH_FIELD(ReadOnly)
    float unscaledDelta = 0.0f;

    // The sums of the deltas of every tick so far; 0 before the first tick.
    ITH_FIELD(ReadOnly)
    double elapsed = 0.0;
    ITH_FIELD(ReadOnly)
    double unscaledElapsed = 0.0;

    // The ticks run so far: 0 before the first tick, 1 during it.
    ITH_FIELD(ReadOnly)
    std::uint64_t frame = 0;

    // The game's time scale, finite and not negative; it applies from the next tick on.
    ITH_FIELD(Min = 0)
    float dilation = 1.0f;

    // The game's own pause (a pause menu): the ticks go on and the systems still run, with `delta`
    // 0. It applies from the next tick on. Not the editor's Pause, which runs no tick at all.
    ITH_FIELD()
    bool paused = false;
};

} // namespace ith
