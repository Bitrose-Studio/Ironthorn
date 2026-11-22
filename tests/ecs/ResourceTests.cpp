// Tests of world resources (ADR-0014 rule 16): typed singletons stored apart from entities,
// reached by C++ type or by reflection record.

#include "ith/ecs/World.h"
#include "ith/foundation/Hash.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>

namespace {

using ith::World;
using ith::test::GameClock;
using ith::test::Gravity;
using ith::test::Health;
using ith::test::MatchRules;

// Counts live values. Its record is written by hand: types in a .cpp file are not scanned by the
// generator.
struct CountedResource {
    ITH_TYPE(Resource)

    static inline int s_alive = 0;

    CountedResource() { ++s_alive; }

    explicit CountedResource(int initial) : value(initial) { ++s_alive; }

    CountedResource(const CountedResource& other) : value(other.value) { ++s_alive; }

    CountedResource(CountedResource&& other) noexcept : value(other.value) { ++s_alive; }

    ~CountedResource() { --s_alive; }

    CountedResource& operator=(const CountedResource&) = default;
    CountedResource& operator=(CountedResource&&) = default;

    int value = 0;
};

const ith::TypeInfo& CountedResource::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo = ith::detail::makeStructType<CountedResource>(
        ith::fnv1a64("ecs_test::CountedResource"), "ecs_test::CountedResource", ith::TypeRole::Resource,
        ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

struct alignas(64) AlignedResource {
    ITH_TYPE(Resource)

    std::array<float, 16> values{};
};

const ith::TypeInfo& AlignedResource::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo = ith::detail::makeStructType<AlignedResource>(
        ith::fnv1a64("ecs_test::AlignedResource"), "ecs_test::AlignedResource", ith::TypeRole::Resource,
        ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

} // namespace

TEST_CASE("World: insert, read, replace and remove a resource") {
    World world;
    CHECK(world.resource<GameClock>() == nullptr);
    CHECK(world.resourceCount() == 0);

    GameClock* clock = world.insertResource(GameClock{.seconds = 1.5, .frame = 3});
    REQUIRE(clock != nullptr);
    CHECK(world.resource<GameClock>() == clock);
    CHECK(world.resource<GameClock>()->frame == 3);
    CHECK(world.resourceCount() == 1);
    clock->frame = 4;

    const World& constWorld = world;
    CHECK(constWorld.resource<GameClock>()->frame == 4);
    CHECK(world.resource<const GameClock>()->seconds == 1.5);

    // Replacing keeps the address.
    CHECK(world.insertResource(GameClock{.seconds = 2.0}) == clock);
    CHECK(clock->frame == 0);
    CHECK(clock->seconds == 2.0);
    CHECK(world.resourceCount() == 1);

    CHECK(world.removeResource<GameClock>());
    CHECK(world.resource<GameClock>() == nullptr);
    CHECK_FALSE(world.removeResource<GameClock>());
    CHECK(world.resourceCount() == 0);
}

TEST_CASE("World: a resource can be replaced by a copy of its own value") {
    World world;
    MatchRules* rules = world.insertResource(
        MatchRules{.mode = "capture the flag, long enough for the heap", .maps = {"harbour", "quarry"}});
    REQUIRE(rules != nullptr);
    CHECK(world.insertResource(*rules) == rules);
    CHECK(rules->mode == "capture the flag, long enough for the heap");
    CHECK(rules->maps.size() == 2);

    const MatchRules copy = *rules;
    CHECK(world.insertResource(copy) == rules);
    CHECK(rules->maps[1] == "quarry");
}

TEST_CASE("World: resources by type record") {
    World world;
    const ith::TypeInfo& rulesType = ith::typeOf<MatchRules>();
    void* value = world.insertResource(rulesType);
    REQUIRE(value != nullptr);
    CHECK(value == world.resource<MatchRules>());
    CHECK(static_cast<MatchRules*>(value)->mode == MatchRules{}.mode); // default-constructed
    static_cast<MatchRules*>(value)->maps.emplace_back("harbour");

    CHECK(world.resource(rulesType) == value);
    const World& constWorld = world;
    CHECK(constWorld.resource(rulesType) == value);

    // Inserting again resets the value in place.
    CHECK(world.insertResource(rulesType) == value);
    CHECK(world.resource<MatchRules>()->maps.empty());

    CHECK(world.removeResource(rulesType));
    CHECK(world.resource(rulesType) == nullptr);
    CHECK_FALSE(world.removeResource(rulesType));
}

TEST_CASE("World: resources are built and destroyed exactly once") {
    {
        World world;
        world.insertResource(CountedResource(1));
        CHECK(CountedResource::s_alive == 1);
        world.insertResource(CountedResource(2));
        CHECK(CountedResource::s_alive == 1);
        CHECK(world.resource<CountedResource>()->value == 2);
        world.insertResource(ith::typeOf<CountedResource>());
        CHECK(CountedResource::s_alive == 1);
        CHECK(world.resource<CountedResource>()->value == 0);
        CHECK(world.removeResource<CountedResource>());
        CHECK(CountedResource::s_alive == 0);
        world.insertResource(CountedResource(3));
    }
    // The world destroyed it.
    CHECK(CountedResource::s_alive == 0);
}

TEST_CASE("World: resources are stored at their alignment") {
    World world;
    world.insertResource(GameClock{});
    const AlignedResource* aligned = world.insertResource(AlignedResource{});
    REQUIRE(aligned != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(world.resource<GameClock>()) % alignof(GameClock) == 0);
}

TEST_CASE("World: a resource with a math field keeps its reflected layout") {
    World world;
    REQUIRE(world.insertResource(ith::typeOf<Gravity>()) != nullptr);
    const Gravity* gravity = world.resource<Gravity>();
    REQUIRE(gravity != nullptr);
    CHECK(gravity->acceleration == ith::Vec3(0.0f, 0.0f, -9.81f));
    const ith::TypeInfo& type = ith::typeOf<Gravity>();
    REQUIRE(type.fields.size() == 1);
    CHECK(type.fields[0].type().id == ith::typeOf<ith::Vec3>().id);
}

TEST_CASE("World: a type without the Resource role asserts and is never stored") {
    World world;
    const ith::test::AssertCapture capture;
    CHECK(world.insertResource(Health{}) == nullptr);
    CHECK(world.insertResource(ith::typeOf<ith::test::NotAComponent>()) == nullptr);
    CHECK(world.resource<Health>() == nullptr);
    CHECK(world.resource(ith::typeOf<float>()) == nullptr);
    CHECK_FALSE(world.removeResource<Health>());
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 5);
#endif
    CHECK(world.resourceCount() == 0);
}

TEST_CASE("World: resources outlive clear() and may change while a query iterates") {
    World world;
    world.insertResource(GameClock{.frame = 9});
    world.spawn(Health{.value = 1});
    world.spawn(Health{.value = 2});

    // Resources are not structural: inserting, reading and removing them inside a query works.
    world.query<const Health>().forEach([&](const Health& health) {
        world.resource<GameClock>()->frame += static_cast<std::uint64_t>(health.value);
        world.insertResource(MatchRules{});
    });
    CHECK(world.resource<GameClock>()->frame == 12);
    world.query<const Health>().forEach([&](const Health&) { world.removeResource<MatchRules>(); });
    CHECK(world.resource<MatchRules>() == nullptr);

    world.clear();
    CHECK(world.entityCount() == 0);
    CHECK(world.resource<GameClock>()->frame == 12);
}

TEST_CASE("World: each world has its own resources") {
    World first;
    World second;
    first.insertResource(GameClock{.frame = 1});
    CHECK(second.resource<GameClock>() == nullptr);
    second.insertResource(GameClock{.frame = 2});
    CHECK(first.resource<GameClock>()->frame == 1);
    CHECK(second.resource<GameClock>()->frame == 2);
    CHECK(first.resource<GameClock>() != second.resource<GameClock>());
}
