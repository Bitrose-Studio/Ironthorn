// ECS benchmarks (ADR-0014 rule 19). Run on a Release build, by hand or as the CTest
// `ecs_benchmark` (label "benchmark", excluded from normal test runs; CI runs it in the
// linux-clang-release job). It only prints numbers and always exits with 0.
//
//   cmake --build build/windows --config Release --target ith_ecs_benchmark
//   build/windows/bin/Release/ith_ecs_benchmark
//
// Each workload runs 11 times after a warm-up run, on a freshly set-up world each time, and the
// median is printed. The first two workloads have targets from an earlier archetype ECS
// prototype on a desktop CPU; record results together with the machine they ran on. Spawning
// through commands and the schedule's overhead per run have no target in ADR-0014 and are printed
// for reference. Transform propagation, the remaining ADR workload, arrives with the hierarchy.

#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Stopwatch.h"

#include "EcsComponents.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ith::Entity;
using ith::World;
using ith::test::Position;
using ith::test::Velocity;

constexpr std::size_t kEntities = 100'000;
constexpr int kRuns = 11;

// Results are summed into this, so the optimizer can't drop the work.
volatile float g_sink = 0.0f;

template <typename State>
double measure(const std::function<State()>& setup, const std::function<void(State&)>& work) {
    std::vector<double> samples;
    for (int run = 0; run <= kRuns; ++run) {
        State state = setup();
        const ith::Stopwatch stopwatch;
        work(state);
        const double elapsed = stopwatch.elapsedMilliseconds();
        if (run > 0) {
            samples.push_back(elapsed);
        }
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

void report(const char* name, double milliseconds, double target = 0.0) {
    if (target > 0.0) {
        std::printf("%-52s %9.3f ms   target %6.2f ms   %s\n", name, milliseconds, target,
                    milliseconds <= target ? "within target" : "ABOVE TARGET");
    } else {
        std::printf("%-52s %9.3f ms\n", name, milliseconds);
    }
}

std::pair<Position, Velocity> moving(std::size_t index) {
    const auto value = static_cast<float>(index);
    return {Position{.x = value}, Velocity{.x = 1.0f, .y = 0.5f, .z = value * 0.001f}};
}

template <typename Tag>
void tagIf(World& world, Entity entity, bool condition) {
    if (condition) {
        world.add(entity, Tag{});
    }
}

// kEntities moving entities spread evenly over `archetypes` archetypes (up to 128).
std::unique_ptr<World> spreadWorld(std::size_t archetypes) {
    auto world = std::make_unique<World>();
    for (std::size_t index = 0; index < kEntities; ++index) {
        auto [position, velocity] = moving(index);
        const Entity entity = world->spawn(position, velocity);
        const std::size_t mask = index % archetypes;
        tagIf<ith::test::Tag0>(*world, entity, (mask & 1u) != 0);
        tagIf<ith::test::Tag1>(*world, entity, (mask & 2u) != 0);
        tagIf<ith::test::Tag2>(*world, entity, (mask & 4u) != 0);
        tagIf<ith::test::Tag3>(*world, entity, (mask & 8u) != 0);
        tagIf<ith::test::Tag4>(*world, entity, (mask & 16u) != 0);
        tagIf<ith::test::Tag5>(*world, entity, (mask & 32u) != 0);
        tagIf<ith::test::Tag6>(*world, entity, (mask & 64u) != 0);
    }
    return world;
}

struct Populated {
    std::unique_ptr<World> world;
    std::vector<Entity> entities;
};

Populated populated() {
    Populated state{.world = std::make_unique<World>(), .entities = {}};
    state.entities.reserve(kEntities);
    for (std::size_t index = 0; index < kEntities; ++index) {
        auto [position, velocity] = moving(index);
        state.entities.push_back(state.world->spawn(position, velocity));
    }
    return state;
}

void integrate(World& world) {
    world.query<Position, const Velocity>().forEach([](Position& position, const Velocity& velocity) {
        constexpr float kStep = 1.0f / 60.0f;
        position.x += velocity.x * kStep;
        position.y += velocity.y * kStep;
        position.z += velocity.z * kStep;
    });
}

} // namespace

int main() {
    std::printf("Ironthorn ECS benchmarks: median of %d runs, %zu entities unless noted\n", kRuns, kEntities);
#ifdef ITH_SHIPPING
    const char* configuration = "Release";
#elif defined(ITH_DEBUG)
    const char* configuration = "Debug (numbers are not meaningful)";
#else
    const char* configuration = "development (asserts on)";
#endif
    std::printf("%s build, %u hardware threads\n\n", configuration, std::thread::hardware_concurrency());

    using WorldPtr = std::unique_ptr<World>;

    report("spawn 100k (Position, Velocity)",
           measure<WorldPtr>([] { return std::make_unique<World>(); },
                             [](WorldPtr& world) {
                                 for (std::size_t index = 0; index < kEntities; ++index) {
                                     auto [position, velocity] = moving(index);
                                     world->spawn(position, velocity);
                                 }
                             }),
           5.1);

    report("despawn 100k", measure<Populated>(populated, [](Populated& state) {
               for (const Entity entity : state.entities) {
                   state.world->despawn(entity);
               }
           }));

    for (const std::size_t archetypes : {std::size_t{1}, std::size_t{10}, std::size_t{100}}) {
        const std::string name =
            "iterate (Position&, const Velocity&), " + std::to_string(archetypes) + " archetype(s)";
        report(name.c_str(),
               measure<WorldPtr>([archetypes] { return spreadWorld(archetypes); },
                                 [](WorldPtr& world) { integrate(*world); }),
               0.34);
    }

    report("tag churn: add + remove a tag on 10k entities", measure<Populated>(populated, [](Populated& state) {
               for (std::size_t index = 0; index < state.entities.size(); index += 10) {
                   state.world->add(state.entities[index], ith::test::Tag7{});
               }
               for (std::size_t index = 0; index < state.entities.size(); index += 10) {
                   state.world->remove<ith::test::Tag7>(state.entities[index]);
               }
           }));

    report("random access: get<Position> by handle, 100k", measure<Populated>(
                                                               [] {
                                                                   Populated state = populated();
                                                                   std::ranges::shuffle(state.entities,
                                                                                        std::mt19937(7));
                                                                   return state;
                                                               },
                                                               [](Populated& state) {
                                                                   float sum = 0.0f;
                                                                   for (const Entity entity : state.entities) {
                                                                       sum += state.world->get<Position>(entity)->x;
                                                                   }
                                                                   g_sink = g_sink + sum;
                                                               }));

    report("spawn 100k through commands, then apply",
           measure<WorldPtr>([] { return std::make_unique<World>(); },
                             [](WorldPtr& world) {
                                 ith::Commands commands(*world);
                                 for (std::size_t index = 0; index < kEntities; ++index) {
                                     auto [position, velocity] = moving(index);
                                     commands.spawn(position, velocity);
                                 }
                                 world->apply(commands);
                             }));

    // Schedule overhead: 100 systems that each run one query over an empty world, so the time is
    // the executor, the access checks and 100 empty sync points; reported per run.
    {
        constexpr int kSystems = 100;
        constexpr int kFrames = 1000;
        World world;
        ith::Schedule schedule;
        for (int index = 0; index < kSystems; ++index) {
            schedule.add({
                .name = "system " + std::to_string(index),
                .stage = static_cast<ith::Stage>(index % static_cast<int>(ith::kStageCount)),
                .access = ith::accessTo<Position, const Velocity>(),
                .run =
                    [](ith::SystemContext& context) {
                        context.query<Position, const Velocity>().forEach([](Position&, const Velocity&) {});
                    },
            });
        }
        if (!schedule.build()) {
            return 0;
        }
        const double perRun = measure<int>([] { return 0; },
                                           [&](int&) {
                                               for (int frame = 0; frame < kFrames; ++frame) {
                                                   schedule.run(world);
                                               }
                                           }) /
                              kFrames;
        report("schedule run: 100 systems, empty world (per run)", perRun);
    }

    return 0;
}
