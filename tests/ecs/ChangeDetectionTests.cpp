// Change detection (ADR-0014 rule 9): change ticks, the Added and Changed filters, typed and
// dynamic, what marks a value changed, the window of each system's runs per world, the 32-bit
// wraparound, and transform propagation that writes only the subtrees that changed.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ith::Entity;
using ith::Schedule;
using ith::Stage;
using ith::SystemContext;
using ith::SystemDescriptor;
using ith::World;
using ith::test::Health;
using ith::test::Position;
using ith::test::Tag0;

using Bits = std::set<std::uint64_t>;

Bits bitsOf(std::initializer_list<Entity> entities) {
    Bits bits;
    for (const Entity entity : entities) {
        bits.insert(entity.bits());
    }
    return bits;
}

// What a watching system saw in its last run.
struct Seen {
    Bits added;
    Bits changed;
};

// A system that records the entities whose Health was added, or added or changed, since its last
// run; typed, or through a dynamic query.
SystemDescriptor watcher(Seen& seen, std::string name = "test::watch", Stage stage = Stage::PostUpdate,
                         bool dynamic = false) {
    return {
        .name = std::move(name),
        .stage = stage,
        .access = ith::accessTo<const Health>(),
        .run =
            [&seen, dynamic](SystemContext& context) {
                seen = {};
                if (dynamic) {
                    const std::array health{ith::QueryTerm{.type = &ith::typeOf<Health>()}};
                    const std::array added{
                        ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Added}};
                    const std::array changed{
                        ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Changed}};
                    context.dynamicQuery({}, added).forEach(
                        [&](Entity entity, std::span<void* const>) { seen.added.insert(entity.bits()); });
                    context.dynamicQuery(health, changed).forEach([&](Entity entity, std::span<void* const>) {
                        seen.changed.insert(entity.bits());
                    });
                    return;
                }
                context.query<ith::Added<Health>>().forEach([&](Entity entity) { seen.added.insert(entity.bits()); });
                context.query<const Health, ith::Changed<Health>>().forEach(
                    [&](Entity entity, const Health&) { seen.changed.insert(entity.bits()); });
            },
    };
}

Schedule watching(Seen& seen, bool dynamic = false) {
    Schedule schedule;
    schedule.add(watcher(seen, "test::watch", Stage::PostUpdate, dynamic));
    REQUIRE(schedule.build());
    return schedule;
}

} // namespace

TEST_CASE("Change detection: a system sees what was added and changed since its last run") {
    for (const bool dynamic : {false, true}) {
        CAPTURE(dynamic);
        World world;
        Seen seen;
        Schedule schedule = watching(seen, dynamic);
        const Entity first = world.spawn(Health{});
        const Entity second = world.spawn(Health{}, Position{});
        world.spawn(Position{});

        // The first run sees every value as added and changed.
        schedule.run(world);
        CHECK(seen.added == bitsOf({first, second}));
        CHECK(seen.changed == bitsOf({first, second}));

        schedule.run(world);
        CHECK(seen.added.empty());
        CHECK(seen.changed.empty());

        // Read-only access marks nothing; mutable access marks the value, written or not.
        CHECK(world.get<const Health>(first) != nullptr);
        CHECK(std::as_const(world).get<Health>(first) != nullptr);
        schedule.run(world);
        CHECK(seen.changed.empty());
        CHECK(world.get<Health>(first) != nullptr);
        schedule.run(world);
        CHECK(seen.added.empty());
        CHECK(seen.changed == bitsOf({first}));

        // Replacing a value changes it; removing and adding it again adds it.
        world.add(second, Health{.value = 5});
        schedule.run(world);
        CHECK(seen.added.empty());
        CHECK(seen.changed == bitsOf({second}));
        CHECK(world.remove<Health>(second));
        world.add(second, Health{});
        schedule.run(world);
        CHECK(seen.added == bitsOf({second}));
        CHECK(seen.changed == bitsOf({second}));

        // Moving to another archetype keeps the ticks.
        world.add(first, Tag0{});
        world.remove<Position>(second);
        schedule.run(world);
        CHECK(seen.changed.empty());

        // New entities, spawned directly or through commands, are added.
        const Entity third = world.spawn(Health{});
        ith::Commands commands(world);
        const Entity fourth = commands.spawn(Health{}, Tag0{});
        world.apply(commands);
        schedule.run(world);
        CHECK(seen.added == bitsOf({third, fourth}));
        CHECK(seen.changed == bitsOf({third, fourth}));
    }
}

TEST_CASE("Change detection: written query terms and mutable access by record mark values changed") {
    World world;
    Seen seen;
    Schedule schedule = watching(seen);
    const Entity moving = world.spawn(Health{}, Position{});
    const Entity still = world.spawn(Health{});
    schedule.run(world);

    // A written term marks every value it hands out, a read term none.
    world.query<Health, const Position>().forEach([](Health&, const Position&) {});
    schedule.run(world);
    CHECK(seen.changed == bitsOf({moving}));
    world.query<const Health>().forEach([](const Health&) {});
    schedule.run(world);
    CHECK(seen.changed.empty());

    const std::array written{ith::QueryTerm{.type = &ith::typeOf<Health>(), .access = ith::Access::Write}};
    world.dynamicQuery(written).forEach([](Entity, std::span<void* const>) {});
    schedule.run(world);
    CHECK(seen.changed == bitsOf({moving, still}));
    const std::array read{ith::QueryTerm{.type = &ith::typeOf<Health>()}};
    world.dynamicQuery(read).forEach([](Entity, std::span<void* const>) {});
    schedule.run(world);
    CHECK(seen.changed.empty());

    // By record: the non-const get and forEachComponent mark; their const forms don't.
    CHECK(std::as_const(world).get(still, ith::typeOf<Health>()) != nullptr);
    std::as_const(world).forEachComponent(moving, [](const ith::TypeInfo&, const void*) {});
    schedule.run(world);
    CHECK(seen.changed.empty());
    CHECK(world.get(still, ith::typeOf<Health>()) != nullptr);
    world.forEachComponent(moving, [](const ith::TypeInfo&, void*) {});
    schedule.run(world);
    CHECK(seen.changed == bitsOf({moving, still}));
}

TEST_CASE("Change detection: each system has its own window, per world") {
    World world;
    Seen before;
    Seen after;
    Seen self;
    Entity spawned;
    Schedule schedule;
    schedule.add(watcher(before, "test::before", Stage::PrePhysics));
    schedule.add({
        .name = "test::writer",
        .stage = Stage::PrePhysics,
        .access = ith::accessTo<Health>(),
        .after = {"test::before"},
        .run =
            [&](SystemContext& context) {
                // What changed since this system's last run, before it writes anything.
                self = {};
                context.query<const Health, ith::Changed<Health>>().forEach(
                    [&](Entity entity, const Health&) { self.changed.insert(entity.bits()); });
                context.query<ith::Added<Health>>().forEach([&](Entity entity) { self.added.insert(entity.bits()); });
                context.query<Health, ith::Without<Tag0>>().forEach([](Health& health) { ++health.value; });
                if (spawned.isNull()) {
                    spawned = context.commands().spawn(Health{}, Tag0{});
                }
            },
    });
    schedule.add(watcher(after, "test::after", Stage::PrePhysics)); // added last: runs after the writer
    REQUIRE(schedule.build());
    const Entity entity = world.spawn(Health{});

    schedule.run(world);
    CHECK(self.changed == bitsOf({entity}));
    CHECK(after.changed == bitsOf({entity}));

    // The writer doesn't see its own writes, but sees the spawn its commands made; a system after
    // it in the same stage sees its writes; one before it sees them on its next run.
    schedule.run(world);
    CHECK(self.added == bitsOf({spawned}));
    CHECK(self.changed == bitsOf({spawned}));
    CHECK(before.changed == bitsOf({entity, spawned}));
    CHECK(after.added == bitsOf({spawned}));
    CHECK(after.changed == bitsOf({entity, spawned}));

    // The same schedule on another world: a first run there, then a window of its own.
    World other;
    const Entity there = other.spawn(Health{}, Tag0{});
    schedule.run(other);
    CHECK(after.added == bitsOf({there}));
    schedule.run(other);
    CHECK(after.added.empty());
    CHECK(after.changed.empty());
    CHECK(world.get<Health>(entity) != nullptr);
    schedule.run(world);
    CHECK(self.changed == bitsOf({entity}));
}

TEST_CASE("Change detection: change filters work only in systems, with declared access") {
    World world;
    world.spawn(Health{});
    {
        const ith::test::AssertCapture capture;
        int visits = 0;
        world.query<const Health, ith::Changed<Health>>().forEach([&](const Health&) { ++visits; });
        const std::array filters{ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Added}};
        world.dynamicQuery({}, filters).forEach([&](Entity, std::span<void* const>) { ++visits; });
        CHECK(visits == 0);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 2);
#endif
    }

    // A change filter reads the type's ticks: the system must declare the type.
    Schedule schedule;
    schedule.add({
        .name = "test::undeclared",
        .access = ith::accessTo<const Position>(),
        .run =
            [](SystemContext& context) {
                context.query<ith::Changed<Health>>().forEach([](Entity) {});
                const std::array filters{
                    ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Added}};
                context.dynamicQuery({}, filters).forEach([](Entity, std::span<void* const>) {});
            },
    });
    REQUIRE(schedule.build());
    const ith::test::AssertCapture capture;
    schedule.run(world);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 2);
#endif
}

TEST_CASE("Change detection: tick comparisons hold across the 32-bit wraparound") {
    using ith::detail::isNewer;
    using ith::detail::kMaxChangeAge;
    CHECK(isNewer(10, 5, 10));
    CHECK_FALSE(isNewer(5, 5, 10));
    CHECK_FALSE(isNewer(4, 5, 10));
    CHECK(isNewer(2, 0xFFFFFFF0u, 5));           // written after the wrap, last run before it
    CHECK(isNewer(0xFFFFFFF8u, 0xFFFFFFF0u, 5)); // both before the wrap
    CHECK_FALSE(isNewer(0xFFFFFFF0u, 0xFFFFFFF0u, 5));
    // A first run (last run kMaxChangeAge + 1 ago) sees even ticks older than kMaxChangeAge.
    const std::uint32_t now = 100;
    CHECK(isNewer(now - kMaxChangeAge, now - kMaxChangeAge - 1, now));
    CHECK(isNewer(now - kMaxChangeAge - 1000, now - kMaxChangeAge - 1, now));

    World world;
    ith::detail::setChangeTickForTesting(world, 0xFFFFFFF0u);
    Seen seen;
    Schedule schedule = watching(seen);
    const Entity entity = world.spawn(Health{});
    schedule.run(world);
    CHECK(seen.added == bitsOf({entity}));
    for (int run = 0; run < 40; ++run) {
        CAPTURE(run);
        const bool write = run == 20;
        if (write) {
            CHECK(world.get<Health>(entity) != nullptr);
        }
        schedule.run(world);
        CHECK(seen.added.empty());
        CHECK(seen.changed == (write ? bitsOf({entity}) : Bits{}));
    }
}

namespace {

using ith::ChildOf;
using ith::LocalTransform;
using ith::Vec3;
using ith::WorldTransform;

LocalTransform moved(float x, float y = 0.0f) {
    return {.value = {.translation = Vec3(x, y, 0.0f)}};
}

Vec3 translationOf(const World& world, Entity entity) {
    return Vec3(world.get<const WorldTransform>(entity)->matrix[3]);
}

} // namespace

TEST_CASE("Change detection: propagation writes only the subtrees that changed") {
    World world;
    Bits written;
    Schedule schedule;
    schedule.add(ith::transformPropagationSystem());
    schedule.add({
        .name = "test::written",
        .stage = Stage::Extract,
        .access = ith::accessTo<const WorldTransform>(),
        .run =
            [&written](SystemContext& context) {
                written.clear();
                context.query<ith::Changed<WorldTransform>>().forEach(
                    [&](Entity entity) { written.insert(entity.bits()); });
            },
    });
    REQUIRE(schedule.build());

    const Entity root = world.spawn(moved(1.0f), WorldTransform{});
    const Entity child = world.spawn(moved(2.0f), WorldTransform{}, ChildOf{.parent = root});
    const Entity grandchild = world.spawn(moved(4.0f), WorldTransform{}, ChildOf{.parent = child});
    const Entity bare = world.spawn(WorldTransform{}, ChildOf{.parent = root}); // no LocalTransform
    const Entity below = world.spawn(moved(0.0f, 8.0f), WorldTransform{}, ChildOf{.parent = bare});
    const Entity other = world.spawn(moved(16.0f), WorldTransform{});

    schedule.run(world);
    CHECK(written == bitsOf({root, child, grandchild, bare, below, other}));
    CHECK(translationOf(world, grandchild) == Vec3(7.0f, 0.0f, 0.0f));
    CHECK(translationOf(world, below) == Vec3(1.0f, 8.0f, 0.0f));
    schedule.run(world);
    CHECK(written.empty());

    // A changed LocalTransform propagates its subtree only.
    world.get<LocalTransform>(child)->value.translation.x = 3.0f;
    schedule.run(world);
    CHECK(written == bitsOf({child, grandchild}));
    CHECK(translationOf(world, grandchild) == Vec3(8.0f, 0.0f, 0.0f));

    // Detaching an entity without a LocalTransform makes it the identity, and its children follow.
    CHECK(world.remove<ChildOf>(bare));
    schedule.run(world);
    CHECK(written == bitsOf({bare, below}));
    CHECK(translationOf(world, bare) == Vec3(0.0f));
    CHECK(translationOf(world, below) == Vec3(0.0f, 8.0f, 0.0f));

    // Removing a LocalTransform makes the entity the identity too.
    CHECK(world.remove<LocalTransform>(root));
    schedule.run(world);
    CHECK(written == bitsOf({root, child, grandchild}));
    CHECK(translationOf(world, grandchild) == Vec3(7.0f, 0.0f, 0.0f));

    // Reparenting keeps the world placement; the subtree is written again.
    world.add(other, ChildOf{.parent = grandchild});
    schedule.run(world);
    CHECK(written == bitsOf({other}));
    CHECK(glm::length(translationOf(world, other) - Vec3(16.0f, 0.0f, 0.0f)) < 1e-4f);

    // A direct call keeps a window of its own: its first call writes everything.
    ith::propagateTransforms(world);
    schedule.run(world);
    CHECK(written == bitsOf({root, child, grandchild, bare, below, other}));
    ith::propagateTransforms(world);
    schedule.run(world);
    CHECK(written.empty());
}
