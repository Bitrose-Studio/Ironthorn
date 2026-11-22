// Stable ids (ADR-0032): the world's lookup kept by the StableId hooks through spawn, add, replace,
// remove, despawn, clear and commands, and duplicate or nil ids refused before anything is stored.

#include "ith/ecs/StableId.h"

#include "ith/ecs/Commands.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

namespace {

using ith::Commands;
using ith::Entity;
using ith::StableId;
using ith::Uuid;
using ith::World;
using ith::test::Health;

constexpr Uuid kFirst(0x1111, 1);
constexpr Uuid kSecond(0x2222, 2);

} // namespace

TEST_CASE("StableId: findByStableId finds the entity that has the id") {
    World world;
    CHECK(world.findByStableId(kFirst).isNull());
    const Entity first = world.spawn(StableId{.id = kFirst}, Health{});
    const Entity second = world.spawn(StableId{.id = kSecond});
    const Entity none = world.spawn(Health{});
    CHECK(world.findByStableId(kFirst) == first);
    CHECK(world.findByStableId(kSecond) == second);
    CHECK(world.findByStableId(Uuid(9, 9)).isNull());
    CHECK_FALSE(world.has<StableId>(none));
}

TEST_CASE("StableId: the lookup follows add, replace, remove, despawn and clear") {
    World world;
    const Entity entity = world.spawn(Health{});
    REQUIRE(world.add(entity, StableId{.id = kFirst}) != nullptr);
    CHECK(world.findByStableId(kFirst) == entity);

    // Replacing with the same id keeps it; with another id moves the entity to it.
    REQUIRE(world.add(entity, StableId{.id = kFirst}) != nullptr);
    CHECK(world.findByStableId(kFirst) == entity);
    REQUIRE(world.add(entity, StableId{.id = kSecond}) != nullptr);
    CHECK(world.findByStableId(kFirst).isNull());
    CHECK(world.findByStableId(kSecond) == entity);

    CHECK(world.remove<StableId>(entity));
    CHECK(world.findByStableId(kSecond).isNull());

    // An id that is free again can be taken by another entity.
    REQUIRE(world.add(entity, StableId{.id = kSecond}) != nullptr);
    CHECK(world.despawn(entity));
    CHECK(world.findByStableId(kSecond).isNull());
    const Entity other = world.spawn(StableId{.id = kSecond});
    CHECK(world.findByStableId(kSecond) == other);

    world.clear();
    CHECK(world.findByStableId(kSecond).isNull());
}

TEST_CASE("StableId: by type record") {
    World world;
    const Entity entity = world.spawn();
    StableId value{.id = kFirst};
    REQUIRE(world.add(entity, ith::typeOf<StableId>(), &value) != nullptr);
    CHECK(world.findByStableId(kFirst) == entity);
    CHECK(world.remove(entity, ith::typeOf<StableId>()));
    CHECK(world.findByStableId(kFirst).isNull());
}

TEST_CASE("StableId: a duplicate or nil id asserts and is refused before anything is stored") {
    World world;
    const Entity first = world.spawn(StableId{.id = kFirst});
    const Entity second = world.spawn(Health{});
    const ith::test::AssertCapture capture;

    CHECK(world.spawn(StableId{.id = kFirst}, Health{}).isNull());
    CHECK(world.add(second, StableId{.id = kFirst}) == nullptr);
    CHECK(world.spawn(StableId{}).isNull());
    CHECK(world.add(second, StableId{}) == nullptr);
    // A default value by type record has the nil id.
    CHECK(world.add(second, ith::typeOf<StableId>()) == nullptr);
    StableId duplicate{.id = kFirst};
    CHECK(world.add(second, ith::typeOf<StableId>(), &duplicate) == nullptr);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 6);
#endif
    CHECK(world.entityCount() == 2);
    CHECK_FALSE(world.has<StableId>(second));
    CHECK(world.findByStableId(kFirst) == first);

    // A stale handle is refused quietly, as by any add.
    const Entity gone = world.spawn();
    world.despawn(gone);
    CHECK(world.add(gone, StableId{.id = kSecond}) == nullptr);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 6);
#endif
}

TEST_CASE("StableId: commands keep the lookup, and a duplicate spawn command is dropped") {
    World world;
    Commands commands(world);
    const Entity first = commands.spawn(StableId{.id = kFirst});
    const Entity duplicate = commands.spawn(StableId{.id = kFirst}, Health{});
    const ith::test::AssertCapture capture;
    world.apply(commands);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
    CHECK(world.findByStableId(kFirst) == first);
    CHECK_FALSE(world.isAlive(duplicate));
    CHECK(world.entityCount() == 1);

    commands.despawn(first);
    world.apply(commands);
    CHECK(world.findByStableId(kFirst).isNull());
}

TEST_CASE("StableId: it is immutable") {
    World world;
    const Entity entity = world.spawn(StableId{.id = kFirst});
    const ith::test::AssertCapture capture;
    CHECK(world.get<StableId>(entity) == nullptr);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
    REQUIRE(world.get<const StableId>(entity) != nullptr);
    CHECK(world.get<const StableId>(entity)->id == kFirst);
}
