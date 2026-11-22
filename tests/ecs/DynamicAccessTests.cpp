// Tests of access by type record (ADR-0014 slice 2): has, get, add and remove with a component
// type's reflection record, forEachComponent and dynamic queries.

#include "ith/ecs/World.h"
#include "ith/foundation/Hash.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace {

using ith::Access;
using ith::DynamicQuery;
using ith::Entity;
using ith::QueryTerm;
using ith::World;
using ith::test::Counter;
using ith::test::Health;
using ith::test::Name;
using ith::test::Position;
using ith::test::Velocity;

// Counts live values, to check that values made by type record are built and destroyed exactly
// once. Its record is written by hand: types in a .cpp file are not scanned by the generator.
struct Counted {
    ITH_TYPE(Component)

    static inline int s_alive = 0;

    Counted() { ++s_alive; }

    Counted(const Counted& other) : value(other.value) { ++s_alive; }

    Counted(Counted&& other) noexcept : value(other.value) { ++s_alive; }

    ~Counted() { --s_alive; }

    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) = default;

    int value = 7;
};

const ith::TypeInfo& Counted::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo =
        ith::detail::makeStructType<Counted>(ith::fnv1a64("ecs_test::Counted"), "ecs_test::Counted",
                                             ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

const ith::TypeInfo& kHealth = ith::typeOf<Health>();
const ith::TypeInfo& kName = ith::typeOf<Name>();
const ith::TypeInfo& kPosition = ith::typeOf<Position>();
const ith::TypeInfo& kVelocity = ith::typeOf<Velocity>();

std::vector<std::uint64_t> visitDynamic(World& world, std::span<const QueryTerm> terms) {
    std::vector<std::uint64_t> entities;
    world.dynamicQuery(terms).forEach(
        [&](Entity entity, std::span<void* const>) { entities.push_back(entity.bits()); });
    return entities;
}

} // namespace

TEST_CASE("World: has, get, add and remove by type record match the typed functions") {
    World world;
    const Entity entity = world.spawn(Position{.x = 1.0f});

    CHECK(world.has(entity, kPosition));
    CHECK_FALSE(world.has(entity, kHealth));
    CHECK(world.get(entity, kPosition) == world.get<Position>(entity));
    CHECK(world.get(entity, kHealth) == nullptr);

    // add default-constructs the value and moves the entity to the archetype with the new type.
    void* added = world.add(entity, kHealth);
    REQUIRE(added != nullptr);
    CHECK(added == world.get<Health>(entity));
    CHECK(static_cast<Health*>(added)->value == 100);
    CHECK(world.get<Position>(entity)->x == 1.0f);
    static_cast<Health*>(added)->value = 42;
    CHECK(world.get<Health>(entity)->value == 42);

    // Adding a type the entity has replaces the value with a default one.
    CHECK(world.add(entity, kHealth) != nullptr);
    CHECK(world.get<Health>(entity)->value == 100);

    const World& constWorld = world;
    CHECK(constWorld.get(entity, kHealth) == world.get<Health>(entity));

    CHECK(world.remove(entity, kHealth));
    CHECK_FALSE(world.has<Health>(entity));
    CHECK_FALSE(world.remove(entity, kHealth));
    CHECK(world.get<Position>(entity)->x == 1.0f);
    CHECK(world.entityCount() == 1);
}

TEST_CASE("World: values added by type record are built and destroyed exactly once") {
    const ith::TypeInfo& counted = ith::typeOf<Counted>();
    {
        World world;
        std::vector<Entity> entities;
        for (int index = 0; index < 40; ++index) {
            entities.push_back(world.spawn(Counter{.value = index}));
            REQUIRE(world.add(entities.back(), counted) != nullptr);
        }
        CHECK(Counted::s_alive == 40);
        CHECK(world.add(entities[0], counted) != nullptr); // replace
        CHECK(Counted::s_alive == 40);
        CHECK(world.remove(entities[1], counted));
        CHECK(Counted::s_alive == 39);
        CHECK(world.despawn(entities[2]));
        CHECK(Counted::s_alive == 38);
        // Moving between archetypes keeps the value.
        CHECK(world.add(entities[3], kName) != nullptr);
        CHECK(Counted::s_alive == 38);
        CHECK(static_cast<Counted*>(world.get(entities[3], counted))->value == 7);
    }
    CHECK(Counted::s_alive == 0);
}

TEST_CASE("World: a non-trivial value added by type record survives archetype moves") {
    World world;
    const Entity entity = world.spawn(Counter{});
    auto* name = static_cast<Name*>(world.add(entity, kName));
    REQUIRE(name != nullptr);
    CHECK(name->text.empty());
    name->text = "a name long enough to live on the heap, set through a type record";
    for (int index = 0; index < 20; ++index) {
        world.spawn(Counter{}, Name{.text = std::to_string(index)}); // grows the archetype's columns
    }
    CHECK(world.add(entity, kHealth) != nullptr);
    CHECK(world.remove(entity, ith::typeOf<Counter>()));
    CHECK(world.get<Name>(entity)->text == "a name long enough to live on the heap, set through a type record");
}

TEST_CASE("World: access by type record with stale, null and foreign handles") {
    World world;
    const Entity entity = world.spawn(Health{});
    REQUIRE(world.despawn(entity));
    CHECK_FALSE(world.has(entity, kHealth));
    CHECK(world.get(entity, kHealth) == nullptr);
    CHECK(world.add(entity, kHealth) == nullptr);
    CHECK_FALSE(world.remove(entity, kHealth));
    CHECK_FALSE(world.has(Entity{}, kHealth));
    CHECK(world.add(Entity{}, kHealth) == nullptr);

    World other;
    const Entity foreign = other.spawn(Health{});
    const ith::test::AssertCapture capture;
    CHECK(world.add(foreign, kHealth) == nullptr);
    CHECK(world.get(foreign, kHealth) == nullptr);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 2);
#endif
    CHECK(world.entityCount() == 0);
}

TEST_CASE("World: a record without the Component role asserts and is never stored") {
    World world;
    const Entity entity = world.spawn(Health{});
    const ith::test::AssertCapture capture;
    CHECK(world.add(entity, ith::typeOf<ith::test::NotAComponent>()) == nullptr);
    CHECK(world.add(entity, ith::typeOf<ith::test::GameClock>()) == nullptr);
    CHECK_FALSE(world.has(entity, ith::typeOf<ith::test::NotAComponent>()));
    CHECK(world.get(entity, ith::typeOf<ith::test::GameClock>()) == nullptr);
    CHECK_FALSE(world.remove(entity, ith::typeOf<std::int32_t>()));
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 5);
#endif
    int components = 0;
    world.forEachComponent(entity, [&](const ith::TypeInfo&, void*) { ++components; });
    CHECK(components == 1);
}

TEST_CASE("World: another record with the same id reaches the same component") {
    static constexpr ith::TypeInfo kSecondRecord =
        ith::detail::makeStructType<Health>(ith::fnv1a64("ith::test::Health"), "ith::test::Health",
                                            ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    World world;
    const Entity entity = world.spawn(Health{.value = 3});
    CHECK(world.has(entity, kSecondRecord));
    CHECK(world.get(entity, kSecondRecord) == world.get<Health>(entity));
}

TEST_CASE("World: forEachComponent visits each component of an entity once") {
    World world;
    const Entity entity = world.spawn(Position{.x = 2.0f}, Name{.text = "ship"}, Health{.value = 5});
    world.spawn(Velocity{});

    std::set<std::uint64_t> types;
    int visits = 0;
    world.forEachComponent(entity, [&](const ith::TypeInfo& type, void* value) {
        ++visits;
        types.insert(type.id);
        CHECK(value == world.get(entity, type));
    });
    CHECK(visits == 3);
    CHECK(types == std::set<std::uint64_t>{kPosition.id, kName.id, kHealth.id});

    // Values can be written through the pointers.
    world.forEachComponent(entity, [&](const ith::TypeInfo& type, void* value) {
        if (type.id == kHealth.id) {
            static_cast<Health*>(value)->value = 50;
        }
    });
    CHECK(world.get<Health>(entity)->value == 50);

    // Nothing for a stale handle or an entity without components.
    const Entity empty = world.spawn();
    visits = 0;
    world.forEachComponent(empty, [&](const ith::TypeInfo&, void*) { ++visits; });
    REQUIRE(world.despawn(entity));
    world.forEachComponent(entity, [&](const ith::TypeInfo&, void*) { ++visits; });
    CHECK(visits == 0);
}

TEST_CASE("World: structural changes inside forEachComponent are refused") {
    World world;
    const Entity entity = world.spawn(Health{});
    const ith::test::AssertCapture capture;
    world.forEachComponent(entity, [&](const ith::TypeInfo&, void*) {
        CHECK(world.add(entity, kPosition) == nullptr);
        CHECK_FALSE(world.despawn(entity));
    });
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 2);
#endif
    CHECK(world.isAlive(entity));
    CHECK_FALSE(world.has<Position>(entity));
}

TEST_CASE("World: a dynamic query visits the entities and values a typed query does, in the same order") {
    World world;
    for (int index = 0; index < 50; ++index) {
        const Entity entity = world.spawn(Position{.x = static_cast<float>(index)});
        if (index % 2 == 0) {
            world.add(entity, Velocity{.x = 1.0f});
        }
        if (index % 3 == 0) {
            world.add(entity, Health{.value = index});
        }
    }

    const std::array terms{QueryTerm{.type = &kPosition, .access = Access::Write},
                           QueryTerm{.type = &kVelocity, .access = Access::Read}};
    DynamicQuery query = world.dynamicQuery(terms);
    CHECK(query.terms().size() == 2);
    CHECK(query.terms()[1].access == Access::Read);

    std::vector<std::uint64_t> typed;
    world.query<Position, const Velocity>().forEach(
        [&](Entity entity, Position&, const Velocity&) { typed.push_back(entity.bits()); });
    std::vector<std::uint64_t> dynamic;
    query.forEach([&](Entity entity, std::span<void* const> values) {
        dynamic.push_back(entity.bits());
        REQUIRE(values.size() == 2);
        CHECK(values[0] == world.get<Position>(entity));
        CHECK(values[1] == world.get<Velocity>(entity));
        static_cast<Position*>(values[0])->x += static_cast<const Velocity*>(values[1])->x;
    });
    CHECK(dynamic.size() == 25);
    CHECK(dynamic == typed);
    world.query<const Position, const Velocity>().forEach(
        [](const Position& position, const Velocity&) { CHECK(static_cast<int>(position.x) % 2 == 1); });
}

TEST_CASE("World: a dynamic query without terms visits every entity") {
    World world;
    std::set<std::uint64_t> expected;
    expected.insert(world.spawn().bits());
    expected.insert(world.spawn(Health{}).bits());
    expected.insert(world.spawn(Position{}, Name{}).bits());
    std::set<std::uint64_t> visited;
    int calls = 0;
    world.dynamicQuery({}).forEach([&](Entity entity, std::span<void* const> values) {
        ++calls;
        CHECK(values.empty());
        visited.insert(entity.bits());
    });
    CHECK(calls == 3);
    CHECK(visited == expected);
}

TEST_CASE("World: a kept dynamic query sees archetypes created after it first ran") {
    World world;
    const std::array terms{QueryTerm{.type = &kHealth}};
    DynamicQuery query = world.dynamicQuery(terms);
    world.spawn(Health{});
    std::vector<std::uint64_t> first;
    query.forEach([&](Entity entity, std::span<void* const>) { first.push_back(entity.bits()); });
    CHECK(first.size() == 1);

    const Entity later = world.spawn(Health{}, Name{.text = "later"});
    world.spawn(Name{});
    std::set<std::uint64_t> second;
    query.forEach([&](Entity entity, std::span<void* const>) { second.insert(entity.bits()); });
    CHECK(second.size() == 2);
    CHECK(second.contains(later.bits()));

    // The query stays valid across clear().
    world.clear();
    int visits = 0;
    query.forEach([&](Entity, std::span<void* const>) { ++visits; });
    CHECK(visits == 0);
}

TEST_CASE("World: invalid dynamic query terms assert and the query visits nothing") {
    World world;
    world.spawn(Health{}, Position{});
    const ith::TypeInfo& notAComponent = ith::typeOf<ith::test::NotAComponent>();
    const std::array repeated{QueryTerm{.type = &kHealth, .access = Access::Write},
                              QueryTerm{.type = &kHealth, .access = Access::Read}};
    const std::array nullType{QueryTerm{.type = &kHealth}, QueryTerm{}};
    const std::array wrongRole{QueryTerm{.type = &kPosition}, QueryTerm{.type = &notAComponent}};

    const ith::test::AssertCapture capture;
    CHECK(visitDynamic(world, repeated).empty());
    CHECK(visitDynamic(world, nullType).empty());
    CHECK(visitDynamic(world, wrongRole).empty());
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 3);
#endif
}

TEST_CASE("World: structural changes while a dynamic query iterates are refused") {
    World world;
    const Entity entity = world.spawn(Health{});
    const std::array terms{QueryTerm{.type = &kHealth}};
    const ith::test::AssertCapture capture;
    world.dynamicQuery(terms).forEach([&](Entity visited, std::span<void* const>) {
        CHECK(world.remove(visited, kHealth) == false);
        CHECK(world.spawn(Health{}).isNull());
        // Reading and writing values is allowed.
        CHECK(world.get(visited, kHealth) != nullptr);
    });
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 2);
#endif
    CHECK(world.has<Health>(entity));
    CHECK(world.entityCount() == 1);
}
