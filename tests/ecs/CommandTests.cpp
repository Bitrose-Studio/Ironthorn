// Deferred commands (ADR-0014 rule 8): reserved handles, application in recording order, stale
// handles, dropped buffers, append, and buffers recorded from several jobs at once.

#include "ith/ecs/World.h"
#include "ith/jobs/JobSystem.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using ith::Commands;
using ith::Entity;
using ith::World;
using ith::test::Counter;
using ith::test::Health;
using ith::test::Name;
using ith::test::Position;

// A name long enough to live on the heap, so leaks and double frees show under the sanitizers.
std::string longName(std::size_t index) {
    return "entity number " + std::to_string(index) + " with a name that does not fit in place";
}

// The Counter values of the world in query order.
std::vector<std::int64_t> counters(World& world) {
    std::vector<std::int64_t> values;
    world.query<const Counter>().forEach([&](const Counter& counter) { values.push_back(counter.value); });
    return values;
}

} // namespace

TEST_CASE("Commands: a spawn reserves its handle, which comes alive when the buffer is applied") {
    World world;
    Commands commands(world);
    const Entity entity = commands.spawn(Position{.x = 1.0f}, Name{.text = longName(1)});
    CHECK_FALSE(entity.isNull());
    CHECK(entity.worldId() == world.id());
    CHECK_FALSE(world.isAlive(entity));
    CHECK(world.get<Position>(entity) == nullptr);
    CHECK(world.entityCount() == 0);
    CHECK(commands.size() == 1);

    world.apply(commands);
    CHECK(commands.empty());
    REQUIRE(world.isAlive(entity));
    CHECK(world.get<Position>(entity)->x == 1.0f);
    CHECK(world.get<Name>(entity)->text == longName(1));
    CHECK(world.entityCount() == 1);
}

TEST_CASE("Commands: later commands may use a reserved handle, here or in a buffer applied later") {
    World world;
    Commands first(world);
    Commands second(world);
    const Entity entity = first.spawn(Health{.value = 5});
    first.add(entity, Name{.text = longName(2)});
    second.remove<Health>(entity);
    world.apply(first);
    world.apply(second);
    CHECK(world.get<Name>(entity)->text == longName(2));
    CHECK_FALSE(world.has<Health>(entity));
}

TEST_CASE("Commands: apply in recording order") {
    World world;
    const Entity entity = world.spawn(Health{.value = 1});
    Commands commands(world);

    SUBCASE("add then remove leaves nothing") {
        commands.add(entity, Name{.text = longName(3)});
        commands.remove<Name>(entity);
        world.apply(commands);
        CHECK_FALSE(world.has<Name>(entity));
    }
    SUBCASE("remove then add leaves the value") {
        commands.remove<Health>(entity);
        commands.add(entity, Health{.value = 7});
        world.apply(commands);
        CHECK(world.get<Health>(entity)->value == 7);
    }
    SUBCASE("the last add wins") {
        commands.add(entity, Health{.value = 2});
        commands.add(entity, Health{.value = 3});
        world.apply(commands);
        CHECK(world.get<Health>(entity)->value == 3);
    }
    SUBCASE("nothing reaches an entity after its despawn") {
        commands.despawn(entity);
        commands.add(entity, Name{.text = longName(4)});
        commands.remove<Health>(entity);
        world.apply(commands);
        CHECK_FALSE(world.isAlive(entity));
        CHECK(world.entityCount() == 0);
    }
    SUBCASE("spawns get rows in recording order") {
        for (std::int64_t value = 0; value < 10; ++value) {
            commands.spawn(Counter{.value = value});
        }
        world.apply(commands);
        CHECK(counters(world) == std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    }
}

TEST_CASE("Commands: commands on stale handles do nothing and destroy their values") {
    World world;
    const Entity stale = world.spawn(Health{});
    world.despawn(stale);
    Commands commands(world);
    commands.add(stale, Name{.text = longName(5)});
    commands.remove<Health>(stale);
    commands.despawn(stale);
    commands.despawn(Entity());
    world.apply(commands);
    CHECK(world.entityCount() == 0);
    CHECK(commands.empty());
}

TEST_CASE("Commands: a dropped buffer destroys its values and its reserved handles stay stale") {
    World world;
    Entity reserved;
    {
        Commands commands(world);
        reserved = commands.spawn(Name{.text = longName(6)});
        commands.add(world.spawn(Health{}), Name{.text = longName(7)});
    }
    CHECK_FALSE(world.isAlive(reserved));

    // The next apply frees the abandoned slot with a new generation: the slot is reused, the old
    // handle never matches.
    Commands commands(world);
    world.apply(commands);
    const Entity reused = world.spawn(Health{});
    CHECK(reused.index() == reserved.index());
    CHECK(reused.generation() != reserved.generation());
    CHECK_FALSE(world.isAlive(reserved));
    CHECK(world.isAlive(reused));
}

TEST_CASE("Commands: direct spawns and reservations never share a slot") {
    World world;
    std::vector<Entity> despawned;
    for (int index = 0; index < 4; ++index) {
        despawned.push_back(world.spawn(Health{}));
    }
    for (const Entity entity : despawned) {
        world.despawn(entity);
    }
    Commands commands(world);
    std::set<std::uint64_t> handles;
    std::set<std::uint32_t> slots;
    for (int index = 0; index < 6; ++index) {
        const Entity reserved = commands.spawn(Counter{.value = index});
        const Entity direct = world.spawn(Counter{.value = 100 + index});
        handles.insert(reserved.bits());
        handles.insert(direct.bits());
        slots.insert(reserved.index());
        slots.insert(direct.index());
    }
    CHECK(handles.size() == 12);
    CHECK(slots.size() == 12);
    world.apply(commands);
    CHECK(world.entityCount() == 12);
    for (const std::uint64_t bits : handles) {
        CHECK(world.isAlive(Entity::fromBits(bits)));
    }
}

TEST_CASE("Commands: append moves the commands in order and leaves the other buffer empty") {
    World world;
    Commands first(world);
    Commands second(world);
    first.spawn(Counter{.value = 0}, Name{.text = longName(0)});
    second.spawn(Counter{.value = 1}, Name{.text = longName(1)});
    second.spawn(Counter{.value = 2});
    first.append(second);
    CHECK(second.empty());
    CHECK(first.size() == 3);
    first.spawn(Counter{.value = 3});
    world.apply(first);
    CHECK(counters(world) == std::vector<std::int64_t>{0, 1, 2, 3});

    std::vector<std::string> names;
    world.query<const Name>().forEach([&](const Name& name) { names.push_back(name.text); });
    CHECK(names == std::vector<std::string>{longName(0), longName(1)});

    // The emptied buffer can be used again.
    second.spawn(Counter{.value = 4});
    world.apply(second);
    CHECK(world.entityCount() == 5);
}

TEST_CASE("Commands: misuse asserts and changes nothing") {
    World world;
    World other;
    Commands commands(world);
    Commands foreign(other);
    const Entity entity = commands.spawn(Health{});
    const ith::test::AssertCapture capture;

    commands.append(foreign);
    commands.append(commands);
    other.apply(commands);
    world.spawn(Health{});
    world.query<const Health>().forEach([&](const Health&) { world.apply(commands); });
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 4);
#endif
    CHECK(commands.size() == 1);
    world.apply(commands);
    CHECK(world.isAlive(entity));
    CHECK(other.entityCount() == 0);
}

TEST_CASE("Commands: a buffer applied every frame keeps working") {
    World world;
    Commands commands(world);
    for (int frame = 0; frame < 20; ++frame) {
        for (int index = 0; index < 500; ++index) {
            commands.spawn(Counter{.value = index}, Name{.text = longName(static_cast<std::size_t>(index))});
        }
        world.apply(commands);
        world.query<const Counter>().forEach([&](Entity entity, const Counter&) { commands.despawn(entity); });
        world.apply(commands);
        CHECK(world.entityCount() == 0);
    }
}

TEST_CASE("Commands: buffers recorded by several jobs give the same world for any worker count") {
    constexpr std::size_t kItems = 2000;
    constexpr std::size_t kBatch = 32;

    const auto build = [&](std::uint32_t workers) {
        ith::JobSystem jobs(ith::JobSystemConfig{.foregroundWorkers = workers, .backgroundWorkers = 0});
        World world;
        // Free slots, so reservations take free-list entries first and new slots after.
        std::vector<Entity> old;
        for (std::size_t index = 0; index < 300; ++index) {
            old.push_back(world.spawn(Health{}));
        }
        for (const Entity entity : old) {
            world.despawn(entity);
        }

        const std::size_t ranges = (kItems + kBatch - 1) / kBatch;
        std::vector<Commands> perRange;
        perRange.reserve(ranges);
        for (std::size_t range = 0; range < ranges; ++range) {
            perRange.emplace_back(world);
        }
        std::vector<Entity> handles(kItems);
        jobs.parallelFor("record", kItems, kBatch, [&](std::size_t begin, std::size_t end) {
            Commands& commands = perRange[begin / kBatch];
            for (std::size_t item = begin; item < end; ++item) {
                handles[item] =
                    commands.spawn(Counter{.value = static_cast<std::int64_t>(item)}, Name{.text = longName(item)});
                if (item % 3 == 0) {
                    commands.add(handles[item], Position{.x = static_cast<float>(item)});
                }
            }
        });
        Commands merged(world);
        for (Commands& commands : perRange) {
            merged.append(commands);
        }
        world.apply(merged);

        std::set<std::uint64_t> distinct;
        for (std::size_t item = 0; item < kItems; ++item) {
            distinct.insert(handles[item].bits());
            CHECK(world.get<Counter>(handles[item])->value == static_cast<std::int64_t>(item));
        }
        CHECK(distinct.size() == kItems);
        CHECK(world.entityCount() == kItems);

        // The world's contents in iteration order, which handles don't influence.
        std::vector<std::string> contents;
        world.query<const Counter, const Name>().forEach([&](Entity entity, const Counter& counter, const Name& name) {
            contents.push_back(std::to_string(counter.value) + name.text + (world.has<Position>(entity) ? "p" : ""));
        });
        return contents;
    };

    const std::vector<std::string> inline0 = build(0);
    CHECK(inline0.size() == kItems);
    CHECK(build(4) == inline0);
    CHECK(build(4) == inline0);
}
