// The gameplay module (ADR-0053 decision 1), without the editor: tickGameWorld's Time advance
// (clamping, dilation, the game's pause, fixed steps, the frame count and elapsed time) on a plain
// World, buildGameSchedule's order and errors, and Time's reflection record.

#include "ith/gameplay/GameWorld.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"
#include "ith/gameplay/Time.h"
#include "ith/math/Transform.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(gameplay)

namespace {

using ith::Schedule;
using ith::Stage;
using ith::SystemDescriptor;
using ith::Time;
using ith::World;

// How many times one failed precondition asserts: none when asserts are compiled out.
#ifdef ITH_ENABLE_ASSERTS
constexpr int kAsserts = 1;
#else
constexpr int kAsserts = 0;
#endif

// A built game schedule with no game systems.
Schedule emptyGameSchedule() {
    Schedule schedule;
    REQUIRE(ith::buildGameSchedule(schedule, {}).hasValue());
    return schedule;
}

const Time& timeOf(const World& world) {
    const Time* time = world.resource<const Time>();
    REQUIRE(time != nullptr);
    return *time;
}

} // namespace

TEST_CASE("tickGameWorld: a world without a Time gets a default one, then the first tick") {
    World world;
    Schedule schedule = emptyGameSchedule();
    CHECK(world.resource<Time>() == nullptr);

    ith::tickGameWorld(world, schedule, 0.02);
    const Time& time = timeOf(world);
    CHECK(time.frame == 1);
    CHECK(time.unscaledDelta == doctest::Approx(0.02));
    CHECK(time.delta == doctest::Approx(0.02));
    CHECK(time.elapsed == doctest::Approx(0.02));
    CHECK(time.unscaledElapsed == doctest::Approx(0.02));
    CHECK(time.dilation == 1.0f);
    CHECK_FALSE(time.paused);
}

TEST_CASE("tickGameWorld: a default Time reads 0 before the first tick") {
    const Time time;
    CHECK(time.frame == 0);
    CHECK(time.delta == 0.0f);
    CHECK(time.unscaledDelta == 0.0f);
    CHECK(time.elapsed == 0.0);
    CHECK(time.unscaledElapsed == 0.0);
    CHECK(time.dilation == 1.0f);
    CHECK_FALSE(time.paused);
    CHECK(ith::kMaxTickDelta == 0.1);
}

TEST_CASE("tickGameWorld: the real time is clamped to [0, kMaxTickDelta]") {
    World world;
    Schedule schedule = emptyGameSchedule();

    ith::tickGameWorld(world, schedule, 2.0); // a stall
    CHECK(timeOf(world).unscaledDelta == static_cast<float>(ith::kMaxTickDelta));
    CHECK(timeOf(world).delta == static_cast<float>(ith::kMaxTickDelta));
    CHECK(timeOf(world).elapsed == ith::kMaxTickDelta);

    ith::tickGameWorld(world, schedule, ith::kMaxTickDelta); // the bound itself is kept
    CHECK(timeOf(world).unscaledDelta == static_cast<float>(ith::kMaxTickDelta));

    ith::tickGameWorld(world, schedule, -1.0); // a clock that went back
    CHECK(timeOf(world).unscaledDelta == 0.0f);
    CHECK(timeOf(world).delta == 0.0f);

    ith::tickGameWorld(world, schedule, 0.0);
    CHECK(timeOf(world).unscaledDelta == 0.0f);

    CHECK(timeOf(world).frame == 4); // every tick counts, also those of no time
    CHECK(timeOf(world).elapsed == 2 * ith::kMaxTickDelta);
    CHECK(timeOf(world).unscaledElapsed == 2 * ith::kMaxTickDelta);
}

TEST_CASE("tickGameWorld: a real time that isn't finite asserts and counts as 0") {
    World world;
    Schedule schedule = emptyGameSchedule();
    ith::tickGameWorld(world, schedule, 0.05);

    const ith::test::AssertCapture capture;
    ith::tickGameWorld(world, schedule, std::numeric_limits<double>::quiet_NaN());
    CHECK(capture.count() == kAsserts);
    CHECK(timeOf(world).unscaledDelta == 0.0f);
    ith::tickGameWorld(world, schedule, std::numeric_limits<double>::infinity());
    ith::tickGameWorld(world, schedule, -std::numeric_limits<double>::infinity());
    CHECK(capture.count() == 3 * kAsserts);
    CHECK(timeOf(world).delta == 0.0f);
    CHECK(timeOf(world).frame == 4);
    CHECK(timeOf(world).elapsed == 0.05);
    CHECK(timeOf(world).unscaledElapsed == 0.05);
}

TEST_CASE("tickGameWorld: fixed steps give exact frames and elapsed time") {
    World world;
    Schedule schedule = emptyGameSchedule();

    // Requirement 13 of the Play In Editor spec: two steps of 0.02 s.
    ith::tickGameWorld(world, schedule, 0.02);
    ith::tickGameWorld(world, schedule, 0.02);
    CHECK(timeOf(world).frame == 2);
    CHECK(timeOf(world).elapsed == 0.04);
    CHECK(timeOf(world).unscaledElapsed == 0.04);
    CHECK(timeOf(world).delta == 0.02f);
    CHECK(timeOf(world).unscaledDelta == 0.02f);

    for (int step = 0; step < 998; ++step) {
        ith::tickGameWorld(world, schedule, 1.0 / 60.0);
    }
    CHECK(timeOf(world).frame == 1000);
    CHECK(timeOf(world).elapsed == doctest::Approx(0.04 + 998.0 / 60.0));
}

TEST_CASE("tickGameWorld: dilation scales delta and elapsed but not the unscaled values") {
    World world;
    world.insertResource(Time{.dilation = 0.5f});
    Schedule schedule = emptyGameSchedule();

    ith::tickGameWorld(world, schedule, 0.02);
    CHECK(timeOf(world).delta == 0.01f);
    CHECK(timeOf(world).unscaledDelta == 0.02f);
    CHECK(timeOf(world).elapsed == doctest::Approx(0.01));
    CHECK(timeOf(world).unscaledElapsed == doctest::Approx(0.02));

    world.resource<Time>()->dilation = 0.0f; // frozen game time
    ith::tickGameWorld(world, schedule, 0.02);
    CHECK(timeOf(world).delta == 0.0f);
    CHECK(timeOf(world).elapsed == doctest::Approx(0.01));
    CHECK(timeOf(world).unscaledElapsed == doctest::Approx(0.04));

    world.resource<Time>()->dilation = 2.0f; // faster than real time, after the clamp
    ith::tickGameWorld(world, schedule, 1.0);
    CHECK(timeOf(world).unscaledDelta == static_cast<float>(ith::kMaxTickDelta));
    CHECK(timeOf(world).delta == doctest::Approx(0.2));
}

TEST_CASE("tickGameWorld: a dilation set by a system applies from the next tick on") {
    World world;
    Schedule schedule;
    std::vector<float> seen;
    const std::vector<SystemDescriptor> systems{{
        .name = "test::slow_motion",
        .stage = Stage::PrePhysics,
        .access = ith::accessTo<Time>(),
        .run =
            [&seen](ith::SystemContext& context) {
                Time* time = context.resource<Time>();
                seen.push_back(time->delta);
                time->dilation = 0.5f;
            },
    }};
    REQUIRE(ith::buildGameSchedule(schedule, systems).hasValue());

    ith::tickGameWorld(world, schedule, 0.02);
    ith::tickGameWorld(world, schedule, 0.02);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == 0.02f);
    CHECK(seen[1] == 0.01f);
    CHECK(timeOf(world).unscaledDelta == 0.02f);
}

TEST_CASE("tickGameWorld: while the game is paused, delta is 0 and the systems still run") {
    World world;
    world.insertResource(Time{.paused = true});
    Schedule schedule;
    int runs = 0;
    const std::vector<SystemDescriptor> systems{{
        .name = "test::count",
        .access = ith::accessTo<const Time>(),
        .run = [&runs](ith::SystemContext&) { ++runs; },
    }};
    REQUIRE(ith::buildGameSchedule(schedule, systems).hasValue());

    ith::tickGameWorld(world, schedule, 0.05);
    ith::tickGameWorld(world, schedule, 0.05);
    CHECK(runs == 2);
    CHECK(timeOf(world).frame == 2);
    CHECK(timeOf(world).delta == 0.0f);
    CHECK(timeOf(world).elapsed == 0.0);
    CHECK(timeOf(world).unscaledDelta == 0.05f);
    CHECK(timeOf(world).unscaledElapsed == 0.1);

    world.resource<Time>()->paused = false; // resume: game time goes on from where it stopped
    ith::tickGameWorld(world, schedule, 0.05);
    CHECK(timeOf(world).delta == 0.05f);
    CHECK(timeOf(world).elapsed == 0.05);
    CHECK(runs == 3);
}

TEST_CASE("tickGameWorld: a dilation that is negative or not finite asserts and counts as 0") {
    World world;
    Schedule schedule = emptyGameSchedule();
    world.insertResource(Time{.dilation = -1.0f});

    const ith::test::AssertCapture capture;
    ith::tickGameWorld(world, schedule, 0.05);
    CHECK(capture.count() == kAsserts);
    CHECK(timeOf(world).delta == 0.0f);
    CHECK(timeOf(world).unscaledDelta == 0.05f);

    world.resource<Time>()->dilation = std::numeric_limits<float>::quiet_NaN();
    ith::tickGameWorld(world, schedule, 0.05);
    CHECK(capture.count() == 2 * kAsserts);
    CHECK(timeOf(world).delta == 0.0f);
    CHECK(timeOf(world).elapsed == 0.0);
    CHECK(timeOf(world).frame == 2);
}

TEST_CASE("tickGameWorld: the systems read this tick's Time") {
    World world;
    Schedule schedule;
    std::vector<std::uint64_t> frames;
    const std::vector<SystemDescriptor> systems{
        {
            .name = "test::early",
            .stage = Stage::Input,
            .access = ith::accessTo<const Time>(),
            .run = [&frames](ith::SystemContext& context) { frames.push_back(context.resource<const Time>()->frame); },
        },
        {
            .name = "test::late",
            .stage = Stage::Extract,
            .access = ith::accessTo<const Time>(),
            .run = [&frames](ith::SystemContext& context) { frames.push_back(context.resource<const Time>()->frame); },
        },
    };
    REQUIRE(ith::buildGameSchedule(schedule, systems).hasValue());

    ith::tickGameWorld(world, schedule, 0.01);
    ith::tickGameWorld(world, schedule, 0.01);
    CHECK(frames == std::vector<std::uint64_t>{1, 1, 2, 2});
}

TEST_CASE("buildGameSchedule: transform propagation first, then the game systems in order") {
    World world;
    const ith::Entity parent = world.spawn(ith::LocalTransform{}, ith::WorldTransform{});
    const ith::Entity child = world.spawn(ith::LocalTransform{}, ith::WorldTransform{}, ith::ChildOf{.parent = parent});
    Schedule schedule;
    std::vector<std::string> runs;
    float seenX = -1.0f;
    const auto record = [&runs](std::string name) {
        return [&runs, name](ith::SystemContext&) { runs.push_back(name); };
    };
    const std::vector<SystemDescriptor> systems{
        {.name = "test::first", .stage = Stage::PostUpdate, .run = record("first")},
        {.name = "test::second", .stage = Stage::PostUpdate, .run = record("second")},
        {
            .name = "test::reads_transforms",
            .stage = Stage::PostUpdate,
            .access = ith::accessTo<const ith::WorldTransform>(),
            .run =
                [&seenX, child](ith::SystemContext& context) {
                    seenX = context.get<const ith::WorldTransform>(child)->matrix[3][0];
                },
        },
        {.name = "test::gameplay", .stage = Stage::PrePhysics, .run = record("gameplay")},
    };
    REQUIRE(ith::buildGameSchedule(schedule, systems).hasValue());
    CHECK(schedule.systemCount() == 5);

    const std::vector<std::string_view> postUpdate = schedule.order(Stage::PostUpdate);
    REQUIRE(postUpdate.size() == 4);
    CHECK(postUpdate[0] == "ith::propagate_transforms");
    CHECK(postUpdate[1] == "test::first");
    CHECK(postUpdate[2] == "test::second");
    CHECK(postUpdate[3] == "test::reads_transforms");
    CHECK(schedule.order(Stage::PrePhysics) == std::vector<std::string_view>{"test::gameplay"});

    // The game systems of PostUpdate see this tick's world transforms.
    world.add(parent, ith::LocalTransform{.value = ith::Transform{.translation = {3.0f, 0.0f, 0.0f}}});
    ith::tickGameWorld(world, schedule, 0.01);
    CHECK(runs == std::vector<std::string>{"gameplay", "first", "second"});
    CHECK(seenX == 3.0f);
}

TEST_CASE("buildGameSchedule: fails with the schedule's error") {
    SUBCASE("a game system named like an engine system") {
        Schedule schedule;
        const std::vector<SystemDescriptor> systems{
            {.name = "ith::propagate_transforms", .stage = Stage::PostUpdate, .run = [](ith::SystemContext&) {}}};
        const ith::Expected<void> built = ith::buildGameSchedule(schedule, systems);
        REQUIRE_FALSE(built.hasValue());
        CHECK(built.error().code == "ecs.schedule.duplicate_name");
    }
    SUBCASE("a game system without a function") {
        Schedule schedule;
        const std::vector<SystemDescriptor> systems{{.name = "test::empty"}};
        const ith::Expected<void> built = ith::buildGameSchedule(schedule, systems);
        REQUIRE_FALSE(built.hasValue());
        CHECK(built.error().code == "ecs.schedule.invalid_system");
    }
    SUBCASE("a constraint on an unknown system") {
        Schedule schedule;
        const std::vector<SystemDescriptor> systems{
            {.name = "test::late", .after = {"test::missing"}, .run = [](ith::SystemContext&) {}}};
        const ith::Expected<void> built = ith::buildGameSchedule(schedule, systems);
        REQUIRE_FALSE(built.hasValue());
        CHECK(built.error().code == "ecs.schedule.unknown_system");
    }
    SUBCASE("two game systems that each run after the other") {
        Schedule schedule;
        const std::vector<SystemDescriptor> systems{
            {.name = "test::a", .after = {"test::b"}, .run = [](ith::SystemContext&) {}},
            {.name = "test::b", .after = {"test::a"}, .run = [](ith::SystemContext&) {}},
        };
        const ith::Expected<void> built = ith::buildGameSchedule(schedule, systems);
        REQUIRE_FALSE(built.hasValue());
        CHECK(built.error().code == "ecs.schedule.cycle");
    }
}

TEST_CASE("buildGameSchedule: a schedule that failed is discarded, not built again") {
    Schedule schedule;
    // A system with a dangling constraint: the schedule keeps it, so it keeps failing.
    const std::vector<SystemDescriptor> broken{
        {.name = "test::late", .after = {"test::missing"}, .run = [](ith::SystemContext&) {}}};
    REQUIRE_FALSE(ith::buildGameSchedule(schedule, broken).hasValue());
    CHECK(schedule.systemCount() == 2);

    // A second call adds the engine's systems again, which now collide with the first ones.
    const ith::Expected<void> again = ith::buildGameSchedule(schedule, {});
    REQUIRE_FALSE(again.hasValue());
    CHECK(again.error().code == "ecs.schedule.duplicate_name");
    CHECK(schedule.systemCount() == 3);

    Schedule fresh;
    CHECK(ith::buildGameSchedule(fresh, {}).hasValue());
}

TEST_CASE("tickGameWorld: a world that is being iterated asserts and is left as it was") {
    World world;
    world.spawn(ith::LocalTransform{});
    world.insertResource(Time{.frame = 7, .dilation = 0.5f});
    Schedule schedule;
    int runs = 0;
    const std::vector<SystemDescriptor> systems{
        {.name = "test::count", .run = [&runs](ith::SystemContext&) { ++runs; }}};
    REQUIRE(ith::buildGameSchedule(schedule, systems).hasValue());

    const ith::test::AssertCapture capture;
    world.forEachRoot([&](ith::Entity) { ith::tickGameWorld(world, schedule, 0.05); });
    CHECK(capture.count() == kAsserts);
    CHECK(runs == 0);
    const Time& time = timeOf(world);
    CHECK(time.frame == 7);
    CHECK(time.delta == 0.0f);
    CHECK(time.unscaledDelta == 0.0f);
    CHECK(time.elapsed == 0.0);
    CHECK(time.unscaledElapsed == 0.0);
    CHECK(time.dilation == 0.5f);

    ith::tickGameWorld(world, schedule, 0.05); // outside the iteration it ticks
    CHECK(timeOf(world).frame == 8);
    CHECK(runs == 1);
}

TEST_CASE("Time: a reflected world resource registered by the gameplay module") {
    ith::TypeRegistry registry;
    ith::generated::registerReflection_gameplay(registry);
    const ith::TypeInfo* type = registry.find("ith::Time");
    REQUIRE(type != nullptr);
    CHECK(type->id == ith::typeOf<Time>().id);
    CHECK(type->role == ith::TypeRole::Resource);

    std::vector<std::string_view> names;
    std::vector<std::string_view> readOnly;
    for (const ith::FieldInfo& field : type->fields) {
        names.emplace_back(field.name);
        if (ith::hasFlags(field.flags, ith::ReflectionFlags::ReadOnly)) {
            readOnly.emplace_back(field.name);
        }
    }
    CHECK(names == std::vector<std::string_view>{"delta", "unscaledDelta", "elapsed", "unscaledElapsed", "frame",
                                                 "dilation", "paused"});
    // The tick writes these; the game writes dilation and paused.
    CHECK(readOnly == std::vector<std::string_view>{"delta", "unscaledDelta", "elapsed", "unscaledElapsed", "frame"});
}
