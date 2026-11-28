#include "ith/gameplay/GameWorld.h"

#include "ith/ecs/TransformPropagation.h"
#include "ith/foundation/Assert.h"
#include "ith/gameplay/Time.h"

#include <algorithm>
#include <cmath>

namespace ith {

Expected<void> buildGameSchedule(Schedule& schedule, std::span<const SystemDescriptor> systems) {
    schedule.add(transformPropagationSystem());
    for (const SystemDescriptor& system : systems) {
        schedule.add(system);
    }
    return schedule.build();
}

void tickGameWorld(World& world, Schedule& schedule, double seconds) {
    const bool idle = !world.isIterating();
    ITH_ASSERT_MSG(idle, "gameplay: a game world ticked while it is iterated or runs a system");
    if (!idle) {
        return;
    }
    const bool finite = std::isfinite(seconds);
    ITH_ASSERT_MSG(finite, "gameplay: a tick's real time must be finite");
    const double real = finite ? std::clamp(seconds, 0.0, kMaxTickDelta) : 0.0;

    Time* time = world.resource<Time>();
    if (time == nullptr) {
        time = world.insertResource(Time{});
    }
    const bool validDilation = std::isfinite(time->dilation) && time->dilation >= 0.0f;
    ITH_ASSERT_MSG(validDilation, "gameplay: Time::dilation must be finite and not negative");
    const double scale = validDilation && !time->paused ? static_cast<double>(time->dilation) : 0.0;
    const double game = real * scale;

    time->unscaledDelta = static_cast<float>(real);
    time->unscaledElapsed += real;
    time->delta = static_cast<float>(game);
    time->elapsed += game;
    ++time->frame;

    schedule.run(world);
}

} // namespace ith
