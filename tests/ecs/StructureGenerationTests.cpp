// World::structureGeneration (ADR-0053 decision 2): it moves on every structural change (spawn,
// despawn, add of a new type, remove, ChildOf writes, placeBefore, clear, directly, by command or
// by applyPatches, and copyWorld), never on value-only writes or calls that change nothing, and
// its values never repeat across worlds.

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/ecs/WorldCopy.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using ith::ChildOf;
using ith::ComponentChange;
using ith::Entity;
using ith::EntityOp;
using ith::EntityPatch;
using ith::OwnedValue;
using ith::World;
using ith::test::GameClock;
using ith::test::Health;
using ith::test::Position;
using ith::test::Velocity;

// Runs `change` and tells whether it moved the world's structure generation.
template <typename Change>
bool moves(World& world, Change&& change) {
    const std::uint64_t before = world.structureGeneration();
    std::forward<Change>(change)();
    return world.structureGeneration() != before;
}

ComponentChange settingX(float x) {
    ith::Patch patch(ith::typeOf<Position>());
    ith::Expected<ith::PropertyPath> path = ith::PropertyPath::parse(ith::typeOf<Position>(), "x");
    REQUIRE(path.hasValue());
    patch.set(*path, OwnedValue::of(x));
    return ComponentChange::makePatch(std::move(patch));
}

} // namespace

TEST_CASE("structureGeneration: values never repeat across worlds, also when a world id is reused") {
    // The same changes on a replaced world give another value, so a cached value can't match it.
    const auto changeOnce = [](World& world) { world.spawn(Position{}); };
    std::uint64_t first = 0;
    std::uint8_t firstId = 0;
    {
        World world;
        changeOnce(world);
        first = world.structureGeneration();
        firstId = world.id();
    }
    World replacement;
    changeOnce(replacement);
    CHECK(replacement.structureGeneration() != first);

    // A world created by copying gets a value of its own as well.
    World copy;
    REQUIRE(ith::copyWorld(replacement, copy, {}));
    CHECK(copy.structureGeneration() != replacement.structureGeneration());
    CHECK(copy.structureGeneration() != first);

    // Even a world that took over the old world's id.
    std::vector<std::unique_ptr<World>> worlds;
    bool reused = false;
    for (int attempt = 0; attempt < 600 && !reused; ++attempt) {
        auto world = std::make_unique<World>();
        if (world->id() == firstId) {
            reused = true;
            changeOnce(*world);
            CHECK(world->structureGeneration() != first);
            continue;
        }
        worlds.push_back(std::move(world)); // kept alive, so ids move on round-robin
        if (worlds.size() > 200) {
            worlds.clear(); // stay below the 255 live worlds
        }
    }
    CHECK(reused);
}

TEST_CASE("structureGeneration: spawn, despawn, add of a new type and remove move it") {
    World world;
    Entity entity;
    CHECK(moves(world, [&] { entity = world.spawn(Position{}); }));
    CHECK(moves(world, [&] { world.spawn(); })); // an entity without components
    CHECK(moves(world, [&] { world.add(entity, Velocity{}); }));
    CHECK(moves(world, [&] { world.add(entity, ith::typeOf<Health>()); }));
    CHECK(moves(world, [&] { world.remove<Velocity>(entity); }));
    CHECK(moves(world, [&] { world.remove(entity, ith::typeOf<Health>()); }));
    CHECK(moves(world, [&] { world.despawn(entity); }));

    const Entity recursive = world.spawn(Position{});
    CHECK(moves(world, [&] { world.despawnRecursive(recursive); }));
}

TEST_CASE("structureGeneration: value-only writes never move it") {
    World world;
    const Entity entity = world.spawn(Position{}, Velocity{});
    world.insertResource(GameClock{});

    CHECK_FALSE(moves(world, [&] { world.get<Position>(entity)->x = 1.0f; }));
    CHECK_FALSE(moves(world, [&] { static_cast<Position*>(world.get(entity, ith::typeOf<Position>()))->y = 2.0f; }));
    CHECK_FALSE(moves(world, [&] { world.add(entity, Position{.x = 3.0f}); })); // replaces the value
    CHECK_FALSE(moves(world, [&] { world.add(entity, ith::typeOf<Velocity>()); }));
    CHECK_FALSE(moves(world, [&] {
        world.query<Position, const Velocity>().forEach([](Position& p, const Velocity& v) { p.x += v.x; });
    }));
    CHECK_FALSE(moves(world, [&] { world.forEachComponent(entity, [](const ith::TypeInfo&, void*) {}); }));
    CHECK_FALSE(moves(world, [&] { world.resource<GameClock>()->frame = 5; }));
    CHECK_FALSE(moves(world, [&] { world.insertResource(GameClock{.seconds = 1.0}); }));
    CHECK_FALSE(moves(world, [&] { world.removeResource<GameClock>(); }));
    CHECK_FALSE(moves(world, [&] { ith::propagateTransforms(world); }));
}

TEST_CASE("structureGeneration: calls that change nothing don't move it") {
    World world;
    const Entity entity = world.spawn(Position{});
    world.despawn(entity); // `entity` is stale from here
    const Entity alive = world.spawn(Position{});

    CHECK_FALSE(moves(world, [&] { world.despawn(entity); }));
    CHECK_FALSE(moves(world, [&] { world.add(entity, Velocity{}); }));
    CHECK_FALSE(moves(world, [&] { world.remove<Position>(entity); }));
    CHECK_FALSE(moves(world, [&] { world.remove<Velocity>(alive); })); // it has none
    CHECK_FALSE(moves(world, [&] { world.placeBefore(entity, Entity()); }));
    ith::Commands empty(world);
    CHECK_FALSE(moves(world, [&] { world.apply(empty); }));
}

TEST_CASE("structureGeneration: the hierarchy's changes move it") {
    World world;
    const Entity parent = world.spawn(ith::LocalTransform{});
    const Entity other = world.spawn(ith::LocalTransform{});
    const Entity child = world.spawn(ith::LocalTransform{});
    const Entity sibling = world.spawn(ith::LocalTransform{}, ChildOf{.parent = parent});

    CHECK(moves(world, [&] { world.add(child, ChildOf{.parent = parent}); })); // attach
    CHECK(moves(world, [&] { world.add(child, ChildOf{.parent = other}); }));  // reparent
    CHECK(moves(world, [&] { world.add(child, ChildOf{.parent = other, .despawnWithParent = true}); }));
    CHECK(moves(world, [&] { world.remove<ChildOf>(child); }));         // detach
    CHECK(moves(world, [&] { world.placeBefore(child, Entity()); }));   // among the roots
    CHECK(moves(world, [&] { world.placeBefore(sibling, Entity()); })); // among children
    CHECK(moves(world, [&] { world.add(parent, ith::Disabled{}); }));
    CHECK(moves(world, [&] { world.remove<ith::Disabled>(parent); }));
}

TEST_CASE("structureGeneration: a hook's placeBefore that runs before its parent has Children changes nothing") {
    // PlacesItself's onInsert records placeBefore(entity, end) when s_placeItself is set. Spawned
    // before a ChildOf, its command runs ahead of the LinkChild that creates the parent's
    // Children list, so placeBefore finds no list, returns false and must not move the counter: a
    // placement that found the list would add one to the reference spawn's count.
    static bool s_placeItself = false;
    static int s_recorded = 0;
    static const bool kSet = ith::setComponentHooks<ith::test::PlacesItself>({
        .onInsert =
            [](ith::HookContext& context) {
                if (s_placeItself) {
                    context.commands().placeBefore(context.entity(), Entity());
                    ++s_recorded;
                }
            },
    });
    REQUIRE(kSet);

    World world;
    const Entity first = world.spawn(Position{});
    const Entity second = world.spawn(Position{});

    // The same spawn without the command: a new entity, then the parent's new Children list.
    s_placeItself = false;
    const std::uint64_t before = world.structureGeneration();
    const Entity reference = world.spawn(ith::test::PlacesItself{}, ChildOf{.parent = first});
    const std::uint64_t withoutPlace = world.structureGeneration() - before;

    s_placeItself = true;
    s_recorded = 0;
    const std::uint64_t start = world.structureGeneration();
    const Entity child = world.spawn(ith::test::PlacesItself{}, ChildOf{.parent = second});
    s_placeItself = false;
    CHECK(s_recorded == 1);
    CHECK(world.structureGeneration() - start == withoutPlace); // the refused placement added nothing

    // Both children still ended up linked to their parents.
    const ith::Children* firstChildren = world.get<const ith::Children>(first);
    const ith::Children* secondChildren = world.get<const ith::Children>(second);
    REQUIRE(firstChildren != nullptr);
    REQUIRE(secondChildren != nullptr);
    CHECK(firstChildren->entities == std::vector<Entity>{reference});
    CHECK(secondChildren->entities == std::vector<Entity>{child});
}

TEST_CASE("structureGeneration: clear moves it, also on an empty world") {
    World world;
    CHECK(moves(world, [&] { world.clear(); }));
    world.spawn(Position{});
    CHECK(moves(world, [&] { world.clear(); }));
}

TEST_CASE("structureGeneration: applying commands moves it") {
    World world;
    const Entity entity = world.spawn(Position{});
    ith::Commands commands(world);

    const std::uint64_t recorded = world.structureGeneration();
    const Entity reserved = commands.spawn(Velocity{});
    CHECK(world.structureGeneration() == recorded); // recording is not a change
    CHECK(moves(world, [&] { world.apply(commands); }));
    CHECK(world.isAlive(reserved));

    commands.add(entity, Health{});
    CHECK(moves(world, [&] { world.apply(commands); }));
    commands.despawn(reserved);
    CHECK(moves(world, [&] { world.apply(commands); }));

    // Commands that change no structure don't move it: an add that replaces a value...
    commands.add(entity, Position{.x = 2.0f});
    CHECK_FALSE(moves(world, [&] { world.apply(commands); }));
    CHECK(world.get<const Position>(entity)->x == 2.0f);

    // ... and commands that all meet stale handles.
    commands.add(reserved, Health{});
    commands.remove<Position>(reserved);
    commands.despawn(reserved);
    commands.placeBefore(reserved, Entity());
    CHECK_FALSE(moves(world, [&] { world.apply(commands); }));
}

TEST_CASE("structureGeneration: applyPatches moves it for structural changes only") {
    World world;
    const Entity entity = world.spawn(Position{});

    // A value written in place.
    CHECK_FALSE(moves(world, [&] {
        REQUIRE(world.applyPatches(std::vector{EntityPatch{.entity = entity, .changes = {settingX(4.0f)}}}).hasValue());
    }));
    CHECK(world.get<const Position>(entity)->x == 4.0f);

    // A refused call changes nothing.
    CHECK_FALSE(moves(world, [&] {
        CHECK_FALSE(world
                        .applyPatches(std::vector{EntityPatch{
                            .entity = entity, .changes = {ComponentChange::makeRemove(ith::typeOf<Velocity>())}}})
                        .hasValue());
    }));

    CHECK(moves(world, [&] {
        std::vector<EntityPatch> add;
        add.push_back(
            EntityPatch{.entity = entity, .changes = {ComponentChange::makeAdd(OwnedValue::of(Velocity{.x = 1.0f}))}});
        REQUIRE(world.applyPatches(add).hasValue());
    }));
    CHECK(moves(world, [&] {
        std::vector<EntityPatch> spawn;
        spawn.push_back(EntityPatch{.op = EntityOp::Spawn,
                                    .entity = Entity::fromParts(1, 1, 0),
                                    .changes = {ComponentChange::makeAdd(OwnedValue::of(Position{}))}});
        REQUIRE(world.applyPatches(spawn).hasValue());
    }));
    CHECK(moves(world, [&] {
        REQUIRE(world.applyPatches(std::vector{EntityPatch{.op = EntityOp::Despawn, .entity = entity}}).hasValue());
    }));
}

TEST_CASE("structureGeneration: copyWorld moves the target's") {
    World source;
    source.spawn(Position{});
    World target;
    CHECK(moves(target, [&] { REQUIRE(ith::copyWorld(source, target, {})); }));

    const World emptySource;
    World emptyTarget;
    CHECK(moves(emptyTarget, [&] { REQUIRE(ith::copyWorld(emptySource, emptyTarget, {})); }));
}

TEST_CASE("structureGeneration: a schedule run moves it only when its commands change structure") {
    World world;
    const Entity entity = world.spawn(Position{}, Velocity{.x = 1.0f});
    bool spawn = false;
    ith::Schedule schedule;
    schedule.add({
        .name = "test::move",
        .access = ith::accessTo<Position, const Velocity>(),
        .run =
            [&spawn](ith::SystemContext& context) {
                context.query<Position, const Velocity>().forEach([](Position& p, const Velocity& v) { p.x += v.x; });
                if (spawn) {
                    static_cast<void>(context.commands().spawn(Position{}));
                }
            },
    });
    schedule.add(ith::transformPropagationSystem());
    REQUIRE(schedule.build().hasValue());

    CHECK_FALSE(moves(world, [&] { schedule.run(world); }));
    CHECK(world.get<const Position>(entity)->x == 1.0f);
    spawn = true;
    CHECK(moves(world, [&] { schedule.run(world); }));
    CHECK(world.entityCount() == 2);
}
