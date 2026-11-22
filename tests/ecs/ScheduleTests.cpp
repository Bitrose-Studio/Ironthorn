// The schedule (ADR-0014 rules 16 to 18): stages and sync points, ordering within a stage, the
// order commands are applied in, build errors, enforced access, and systems that record commands
// from several jobs.

#include "ith/ecs/Schedule.h"

#include "ith/ecs/World.h"
#include "ith/jobs/JobSystem.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::Access;
using ith::Commands;
using ith::Entity;
using ith::Schedule;
using ith::Stage;
using ith::SystemContext;
using ith::SystemDescriptor;
using ith::World;
using ith::test::Counter;
using ith::test::GameClock;
using ith::test::Health;
using ith::test::Name;
using ith::test::Position;
using ith::test::Velocity;

// A system that appends its name to `log` when it runs.
SystemDescriptor logging(std::string name, Stage stage, std::vector<std::string>& log,
                         std::vector<std::string> after = {}, std::vector<std::string> before = {}) {
    return {
        .name = name,
        .stage = stage,
        .before = std::move(before),
        .after = std::move(after),
        .run = [&log, name](SystemContext&) { log.push_back(name); },
    };
}

std::vector<std::string> names(const std::vector<std::string_view>& views) {
    return {views.begin(), views.end()};
}

std::string buildError(Schedule& schedule) {
    const ith::Expected<void> built = schedule.build();
    return built ? std::string() : built.error().code + ": " + built.error().message;
}

} // namespace

TEST_CASE("Schedule: stages run in order and each ends with its sync point") {
    World world;
    Schedule schedule;
    std::vector<std::string> log;
    Entity spawned;
    std::vector<bool> seenAlive;

    schedule.add({
        .name = "spawner",
        .stage = Stage::PrePhysics,
        .access = ith::accessTo<Health>(),
        .run = [&](SystemContext& context) { spawned = context.commands().spawn(Health{.value = 3}); },
    });
    const auto observe = [&](std::string name, Stage stage) {
        schedule.add({
            .name = std::move(name),
            .stage = stage,
            .access = ith::accessTo<const Health>(),
            .run = [&](SystemContext& context) { seenAlive.push_back(context.isAlive(spawned)); },
        });
    };
    observe("same stage, later", Stage::PrePhysics);
    observe("next stage", Stage::DuringPhysics);
    schedule.add(logging("extract", Stage::Extract, log));
    schedule.add(logging("input", Stage::Input, log));
    schedule.add(logging("post update", Stage::PostUpdate, log));
    schedule.add(logging("post physics", Stage::PostPhysics, log));
    REQUIRE(schedule.build());

    schedule.run(world);
    CHECK(log == std::vector<std::string>{"input", "post physics", "post update", "extract"});
    // Commands apply at the end of the stage: not before the next system of the same stage.
    CHECK(seenAlive == std::vector<bool>{false, true});
    CHECK(world.get<Health>(spawned)->value == 3);
}

TEST_CASE("Schedule: constraints order a stage; otherwise systems run in the order they were added") {
    Schedule schedule;
    std::vector<std::string> log;
    schedule.add(logging("render prep", Stage::PrePhysics, log, {"move"}));
    schedule.add(logging("ai", Stage::PrePhysics, log));
    schedule.add(logging("move", Stage::PrePhysics, log, {"steer"}));
    schedule.add(logging("steer", Stage::PrePhysics, log, {}, {"ai"}));
    schedule.add(logging("audio", Stage::PrePhysics, log));
    REQUIRE(schedule.build());

    // Among the systems whose constraints are met, the one added first runs first.
    const std::vector<std::string> expected{"steer", "ai", "move", "render prep", "audio"};
    CHECK(names(schedule.order(Stage::PrePhysics)) == expected);
    CHECK(schedule.order(Stage::Input).empty());

    World world;
    for (int frame = 0; frame < 3; ++frame) {
        log.clear();
        schedule.run(world);
        CHECK(log == expected);
    }
    // Building again gives the same order.
    REQUIRE(schedule.build());
    CHECK(names(schedule.order(Stage::PrePhysics)) == expected);
}

TEST_CASE("Schedule: commands apply system by system in run order, each in recording order") {
    World world;
    const Entity target = world.spawn(Health{.value = 0});
    Schedule schedule;
    // Added in reverse, run as "first", "second", "third" through the constraints.
    const auto writer = [&](std::string name, std::vector<std::string> after, std::vector<std::int32_t> values) {
        schedule.add({
            .name = std::move(name),
            .stage = Stage::PostPhysics,
            .access = ith::accessTo<Health, Counter>(),
            .after = std::move(after),
            .run =
                [&, values](SystemContext& context) {
                    for (const std::int32_t value : values) {
                        context.commands().add(target, Health{.value = value});
                        context.commands().spawn(Counter{.value = value});
                    }
                },
        });
    };
    writer("third", {"second"}, {30, 31});
    writer("second", {"first"}, {20});
    writer("first", {}, {10, 11, 12});
    REQUIRE(schedule.build());
    schedule.run(world);

    CHECK(world.get<Health>(target)->value == 31);
    std::vector<std::int64_t> spawned;
    world.query<const Counter>().forEach([&](const Counter& counter) { spawned.push_back(counter.value); });
    CHECK(spawned == std::vector<std::int64_t>{10, 11, 12, 20, 30, 31});
}

TEST_CASE("Schedule: build reports invalid systems before anything runs") {
    Schedule schedule;
    std::vector<std::string> log;
    const auto run = [](SystemContext&) {};

    SUBCASE("an empty name") {
        schedule.add({.run = run});
        CHECK(buildError(schedule) == "ecs.schedule.invalid_system: a system has no name");
    }
    SUBCASE("no function") {
        schedule.add({.name = "idle"});
        CHECK(buildError(schedule).starts_with("ecs.schedule.invalid_system: system 'idle'"));
    }
    SUBCASE("a name used twice") {
        schedule.add(logging("twin", Stage::Input, log));
        schedule.add(logging("twin", Stage::Extract, log));
        CHECK(buildError(schedule) == "ecs.schedule.duplicate_name: two systems are named 'twin'");
    }
    SUBCASE("an unknown system") {
        schedule.add(logging("a", Stage::Input, log, {"missing"}));
        CHECK(buildError(schedule) ==
              "ecs.schedule.unknown_system: system 'a' is ordered against an unknown system 'missing'");
    }
    SUBCASE("a system of another stage") {
        schedule.add(logging("a", Stage::Input, log));
        schedule.add(logging("b", Stage::Extract, log, {}, {"a"}));
        CHECK(buildError(schedule) ==
              "ecs.schedule.other_stage: system 'b' is ordered against 'a', which runs in another stage");
    }
    SUBCASE("a cycle") {
        schedule.add(logging("free", Stage::PostUpdate, log));
        schedule.add(logging("a", Stage::PostUpdate, log, {"c"}));
        schedule.add(logging("b", Stage::PostUpdate, log, {"a"}));
        schedule.add(logging("c", Stage::PostUpdate, log, {"b"}));
        CHECK(buildError(schedule) ==
              "ecs.schedule.cycle: the order constraints of stage PostUpdate form a cycle through 'a', 'b', 'c'");
        CHECK(schedule.order(Stage::PostUpdate).empty());
    }
    SUBCASE("a system ordered against itself") {
        schedule.add(logging("self", Stage::Input, log, {"self"}));
        CHECK(buildError(schedule).starts_with("ecs.schedule.cycle:"));
    }
    SUBCASE("access to a type that is neither a component nor a resource") {
        schedule.add({.name = "a", .access = ith::accessTo<ith::test::NotAComponent>(), .run = run});
        CHECK(buildError(schedule).starts_with("ecs.schedule.invalid_access: system 'a'"));
    }
    SUBCASE("access without a type") {
        schedule.add({.name = "a", .access = {ith::QueryTerm{}}, .run = run});
        CHECK(buildError(schedule).starts_with("ecs.schedule.invalid_access: system 'a'"));
    }
    SUBCASE("access to a type named twice") {
        schedule.add({.name = "a", .access = ith::accessTo<Health, const Health>(), .run = run});
        CHECK(buildError(schedule) == "ecs.schedule.invalid_access: system 'a' declares 'ith::test::Health' twice");
    }

    // A schedule that failed to build doesn't run.
    World world;
    const ith::test::AssertCapture capture;
    schedule.run(world);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
    CHECK(log.empty());
}

TEST_CASE("Schedule: adding a system needs a new build") {
    World world;
    Schedule schedule;
    std::vector<std::string> log;
    schedule.add(logging("a", Stage::Input, log));
    REQUIRE(schedule.build());
    schedule.add(logging("b", Stage::Input, log));
    CHECK(schedule.order(Stage::Input).empty());
    {
        const ith::test::AssertCapture capture;
        schedule.run(world);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
    }
    CHECK(log.empty());
    REQUIRE(schedule.build());
    schedule.run(world);
    CHECK(log == std::vector<std::string>{"a", "b"});
    CHECK(schedule.systemCount() == 2);
}

TEST_CASE("Schedule: systems reach declared data through their context") {
    World world;
    world.insertResource(GameClock{.seconds = 0.5});
    const Entity mover = world.spawn(Position{}, Velocity{.x = 2.0f});
    Schedule schedule;
    schedule.add({
        .name = "integrate",
        .access = ith::accessTo<Position, const Velocity, const GameClock>(),
        .run =
            [&](SystemContext& context) {
                CHECK(std::string(context.systemName()) == "integrate");
                const auto delta = static_cast<float>(context.resource<const GameClock>()->seconds);
                context.query<Position, const Velocity>().forEach(
                    [&](Position& position, const Velocity& velocity) { position.x += velocity.x * delta; });
                CHECK(context.get<const Velocity>(mover)->x == 2.0f);
                CHECK(context.has<Health>(mover) == false); // has needs no access
            },
    });
    schedule.add({
        .name = "tick",
        .stage = Stage::PostUpdate,
        .access = ith::accessTo<GameClock, Position>(),
        .run =
            [&](SystemContext& context) {
                ++context.resource<GameClock>()->frame;
                const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Position>(), .access = Access::Write}};
                context.dynamicQuery(terms).forEach([](Entity, std::span<void* const> components) {
                    static_cast<Position*>(components[0])->y += 1.0f;
                });
            },
    });
    REQUIRE(schedule.build());
    schedule.run(world);
    schedule.run(world);
    CHECK(world.get<Position>(mover)->x == 2.0f);
    CHECK(world.get<Position>(mover)->y == 2.0f);
    CHECK(world.resource<GameClock>()->frame == 2);
}

TEST_CASE("Schedule: query filters need no declared access") {
    World world;
    const Entity moving = world.spawn(Position{}, Velocity{});
    const Entity frozen = world.spawn(Position{}, Velocity{}, Health{});
    Schedule schedule;
    schedule.add({
        .name = "push",
        .access = ith::accessTo<Position>(),
        .run =
            [](SystemContext& context) {
                context.query<Position, ith::With<Velocity>, ith::Without<Health>>().forEach(
                    [](Position& position) { position.x += 1.0f; });
                const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Position>(), .access = Access::Write}};
                const std::array filters{
                    ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Without}};
                context.dynamicQuery(terms, filters).forEach([](Entity, std::span<void* const> components) {
                    static_cast<Position*>(components[0])->y += 1.0f;
                });
            },
    });
    REQUIRE(schedule.build());
    const ith::test::AssertCapture capture;
    schedule.run(world);
    CHECK(capture.count() == 0);
    CHECK(world.get<Position>(moving)->x == 1.0f);
    CHECK(world.get<Position>(moving)->y == 1.0f);
    CHECK(world.get<Position>(frozen)->x == 0.0f);
    CHECK(world.get<Position>(frozen)->y == 0.0f);
}

#ifdef ITH_ENABLE_ASSERTS
TEST_CASE("Schedule: touching undeclared data asserts") {
    World world;
    world.insertResource(GameClock{});
    const Entity entity = world.spawn(Position{}, Velocity{}, Health{});
    Schedule schedule;
    int asserts = 0;
    const auto expectAssert = [&](std::string name, std::vector<ith::QueryTerm> access,
                                  std::function<void(SystemContext&)> touch) {
        schedule.add({
            .name = std::move(name),
            .access = std::move(access),
            .run =
                [&asserts, touch](SystemContext& context) {
                    const ith::test::AssertCapture capture;
                    touch(context);
                    asserts += capture.count();
                },
        });
    };
    expectAssert("query undeclared", ith::accessTo<Position>(),
                 [](SystemContext& context) { (void)context.query<Position, const Velocity>(); });
    expectAssert("query writes a read", ith::accessTo<const Position>(),
                 [](SystemContext& context) { (void)context.query<Position>(); });
    expectAssert("get undeclared", {}, [entity](SystemContext& context) { (void)context.get<const Health>(entity); });
    expectAssert("get writes a read", ith::accessTo<const Health>(),
                 [entity](SystemContext& context) { (void)context.get<Health>(entity); });
    expectAssert("resource undeclared", {}, [](SystemContext& context) { (void)context.resource<const GameClock>(); });
    expectAssert("resource writes a read", ith::accessTo<const GameClock>(),
                 [](SystemContext& context) { (void)context.resource<GameClock>(); });
    expectAssert("dynamic query writes a read", ith::accessTo<const Health>(), [](SystemContext& context) {
        const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Health>(), .access = Access::Write}};
        (void)context.dynamicQuery(terms);
    });
    REQUIRE(schedule.build());
    schedule.run(world);
    CHECK(asserts == 7);
}
#endif

TEST_CASE("Schedule: direct structural changes are refused while a system runs") {
    World world;
    const Entity entity = world.spawn(Health{});
    Schedule schedule;
    int asserts = 0;
    schedule.add({
        .name = "rogue",
        .access = ith::accessTo<Health>(),
        .run =
            [&](SystemContext&) {
                const ith::test::AssertCapture capture;
                CHECK(world.spawn(Health{}).isNull());
                CHECK_FALSE(world.despawn(entity));
                CHECK(world.add(entity, Name{}) == nullptr);
                CHECK_FALSE(world.remove<Health>(entity));
                Commands commands(world);
                world.apply(commands);
                schedule.run(world); // re-entrant
                asserts = capture.count();
            },
    });
    REQUIRE(schedule.build());
    schedule.run(world);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(asserts == 7); // the re-entrant run asserts twice: running, and the world is iterated
#endif
    CHECK(world.entityCount() == 1);
    CHECK(world.has<Health>(entity));
}

TEST_CASE("Schedule: systems that share written data never run at the same time") {
    // With the serial executor this always holds; the test guards any executor that replaces it.
    World world;
    world.spawn(Health{}, Position{});
    Schedule schedule;
    std::atomic<int> healthUsers{0};
    std::atomic<int> overlaps{0};
    const auto user = [&](std::string name, std::vector<ith::QueryTerm> access) {
        schedule.add({
            .name = std::move(name),
            .access = std::move(access),
            .run =
                [&](SystemContext&) {
                    if (healthUsers.fetch_add(1) != 0) {
                        overlaps.fetch_add(1);
                    }
                    for (int spin = 0; spin < 1000; ++spin) {
                        if (healthUsers.load() != 1) {
                            overlaps.fetch_add(1);
                        }
                    }
                    healthUsers.fetch_sub(1);
                },
        });
    };
    for (int index = 0; index < 4; ++index) {
        user("writer " + std::to_string(index), ith::accessTo<Health>());
        user("reader " + std::to_string(index), ith::accessTo<const Health, Position>());
    }
    REQUIRE(schedule.build());
    for (int frame = 0; frame < 10; ++frame) {
        schedule.run(world);
    }
    CHECK(overlaps.load() == 0);
}

TEST_CASE("Schedule: a system that records from several jobs gives the same world for any worker count") {
    constexpr std::size_t kItems = 1500;
    constexpr std::size_t kBatch = 64;

    const auto simulate = [&](std::uint32_t workers) {
        ith::JobSystem jobs(ith::JobSystemConfig{.foregroundWorkers = workers, .backgroundWorkers = 0});
        World world;
        for (std::size_t item = 0; item < kItems; ++item) {
            world.spawn(Counter{.value = static_cast<std::int64_t>(item)});
        }
        Schedule schedule;
        schedule.add({
            .name = "split",
            .stage = Stage::PrePhysics,
            .access = ith::accessTo<const Counter, Health, Name>(),
            .run =
                [&](SystemContext& context) {
                    // Gather on the system's thread, then record from jobs, one buffer per range.
                    std::vector<std::pair<Entity, std::int64_t>> items;
                    context.query<const Counter>().forEach(
                        [&](Entity entity, const Counter& counter) { items.emplace_back(entity, counter.value); });
                    std::vector<Commands> perRange;
                    for (std::size_t range = 0; range < (items.size() + kBatch - 1) / kBatch; ++range) {
                        perRange.push_back(context.makeCommands());
                    }
                    jobs.parallelFor("split", items.size(), kBatch, [&](std::size_t begin, std::size_t end) {
                        Commands& commands = perRange[begin / kBatch];
                        for (std::size_t index = begin; index < end; ++index) {
                            const auto [entity, value] = items[index];
                            if (value % 2 == 0) {
                                commands.despawn(entity);
                                commands.spawn(Counter{.value = value * 10}, Name{.text = std::to_string(value)});
                            } else {
                                commands.add(entity, Health{.value = static_cast<std::int32_t>(value)});
                            }
                        }
                    });
                    for (Commands& commands : perRange) {
                        context.commands().append(commands);
                    }
                },
        });
        REQUIRE(schedule.build());
        schedule.run(world);

        std::vector<std::string> contents;
        world.query<const Counter>().forEach([&](Entity entity, const Counter& counter) {
            const Name* name = world.get<Name>(entity);
            const Health* health = world.get<Health>(entity);
            contents.push_back(std::to_string(counter.value) + "/" + (name != nullptr ? name->text : "-") + "/" +
                               (health != nullptr ? std::to_string(health->value) : "-"));
        });
        CHECK(world.entityCount() == kItems);
        return contents;
    };

    const std::vector<std::string> serial = simulate(0);
    REQUIRE(serial.size() == kItems);
    CHECK(simulate(1) == serial);
    CHECK(simulate(4) == serial);
    CHECK(simulate(4) == serial);
}

TEST_CASE("Schedule: one schedule runs on two worlds") {
    World first;
    World second;
    Schedule schedule;
    schedule.add({
        .name = "spawn one",
        .access = ith::accessTo<Health>(),
        .run = [](SystemContext& context) { context.commands().spawn(Health{}); },
    });
    REQUIRE(schedule.build());
    schedule.run(first);
    schedule.run(second);
    schedule.run(first);
    CHECK(first.entityCount() == 2);
    CHECK(second.entityCount() == 1);
}

TEST_CASE("Schedule: stage names") {
    CHECK(std::string(ith::stageName(Stage::Input)) == "Input");
    CHECK(std::string(ith::stageName(Stage::PrePhysics)) == "PrePhysics");
    CHECK(std::string(ith::stageName(Stage::DuringPhysics)) == "DuringPhysics");
    CHECK(std::string(ith::stageName(Stage::PostPhysics)) == "PostPhysics");
    CHECK(std::string(ith::stageName(Stage::PostUpdate)) == "PostUpdate");
    CHECK(std::string(ith::stageName(Stage::Extract)) == "Extract");
}
