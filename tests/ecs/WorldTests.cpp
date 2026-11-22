// Tests of ith::World and ith::Entity: handles and generations, spawning and despawning, archetype
// moves, queries, stale and foreign handles, many and non-trivial component types, alignment.

#include "ith/ecs/World.h"

#include "ith/foundation/Hash.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using ith::Entity;
using ith::World;
using ith::test::Counter;
using ith::test::Health;
using ith::test::Inventory;
using ith::test::Name;
using ith::test::Position;
using ith::test::Velocity;

// Counts live values, to check that the World constructs and destroys each value exactly once.
// Its record is written by hand: types in a .cpp file are not scanned by the generator.
struct Tracked {
    ITH_TYPE(Component)

    static inline int s_alive = 0;

    Tracked() { ++s_alive; }

    explicit Tracked(int initial) : value(initial) { ++s_alive; }

    Tracked(const Tracked& other) : value(other.value) { ++s_alive; }

    Tracked(Tracked&& other) noexcept : value(other.value) {
        other.value = -1;
        ++s_alive;
    }

    ~Tracked() { --s_alive; }

    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;

    int value = 0;
};

const ith::TypeInfo& Tracked::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo =
        ith::detail::makeStructType<Tracked>(ith::fnv1a64("ecs_test::Tracked"), "ecs_test::Tracked",
                                             ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

// Over-aligned; fills its alignment exactly, since MSVC warns about padding for alignas.
struct alignas(64) Aligned {
    ITH_TYPE(Component)

    std::array<float, 16> values{};
};

const ith::TypeInfo& Aligned::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo =
        ith::detail::makeStructType<Aligned>(ith::fnv1a64("ecs_test::Aligned"), "ecs_test::Aligned",
                                             ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

struct alignas(128) Aligned128 {
    ITH_TYPE(Component)

    std::array<float, 32> values{};
};

const ith::TypeInfo& Aligned128::ithTypeInfo() noexcept {
    static constexpr ith::TypeInfo kInfo =
        ith::detail::makeStructType<Aligned128>(ith::fnv1a64("ecs_test::Aligned128"), "ecs_test::Aligned128",
                                                ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    return kInfo;
}

template <typename... Terms>
std::set<std::uint64_t> matching(World& world) {
    std::set<std::uint64_t> entities;
    world.query<const Terms...>().forEach([&](Entity entity, const Terms&...) { entities.insert(entity.bits()); });
    return entities;
}

} // namespace

TEST_CASE("Entity: a handle packs index, generation and world id") {
    const Entity entity = Entity::fromParts(0xDEADBEEFu, 0xABCDEFu, 0x42u);
    CHECK(entity.index() == 0xDEADBEEFu);
    CHECK(entity.generation() == 0xABCDEFu);
    CHECK(entity.worldId() == 0x42u);
    CHECK(Entity::fromBits(entity.bits()) == entity);
    CHECK(entity.bits() == 0x42ABCDEFDEADBEEFull);
    CHECK_FALSE(entity.isNull());
    CHECK(Entity().isNull());
    CHECK(Entity().bits() == 0);
    // The generation keeps its low 24 bits.
    CHECK(Entity::fromParts(1, 0x1000001u, 1).generation() == 1);
    static_assert(sizeof(Entity) == 8);
}

TEST_CASE("World: live worlds have distinct ids from 1 to 255") {
    std::vector<std::unique_ptr<World>> worlds;
    std::set<int> ids;
    for (int index = 0; index < 10; ++index) {
        worlds.push_back(std::make_unique<World>());
        const int id = worlds.back()->id();
        CHECK(id >= 1);
        CHECK(id <= 255);
        ids.insert(id);
    }
    CHECK(ids.size() == 10);
    // An id is not reused right after its world is destroyed.
    const int released = worlds.front()->id();
    worlds.erase(worlds.begin());
    const World next;
    CHECK(next.id() != released);
}

TEST_CASE("World: spawn creates an entity with its components") {
    World world;
    CHECK(world.entityCount() == 0);
    const Entity entity = world.spawn(Position{.x = 1.0f, .y = 2.0f, .z = 3.0f}, Health{.value = 7});
    CHECK(world.isAlive(entity));
    CHECK(entity.worldId() == world.id());
    CHECK(world.entityCount() == 1);
    CHECK(world.has<Position>(entity));
    CHECK(world.has<Health>(entity));
    CHECK_FALSE(world.has<Velocity>(entity));
    REQUIRE(world.get<Position>(entity) != nullptr);
    CHECK(world.get<Position>(entity)->y == 2.0f);
    CHECK(world.get<Health>(entity)->value == 7);
    CHECK(world.get<Velocity>(entity) == nullptr);

    const World& constWorld = world;
    CHECK(constWorld.get<Health>(entity)->value == 7);

    const Entity empty = world.spawn();
    CHECK(world.isAlive(empty));
    CHECK_FALSE(world.has<Position>(empty));
    CHECK(world.entityCount() == 2);
}

TEST_CASE("World: spawn copies lvalues and moves rvalues") {
    World world;
    const Name name{.text = "a name long enough to live on the heap, not in the small buffer"};
    Name moved = name;
    const Entity first = world.spawn(name);
    const Entity second = world.spawn(std::move(moved));
    CHECK(name.text.size() > 20);
    CHECK(world.get<Name>(first)->text == name.text);
    CHECK(world.get<Name>(second)->text == name.text);
}

TEST_CASE("World: despawn makes the handle stale and the slot is reused with a new generation") {
    World world;
    const Entity first = world.spawn(Health{.value = 1});
    CHECK(world.despawn(first));
    CHECK_FALSE(world.isAlive(first));
    CHECK_FALSE(world.has<Health>(first));
    CHECK(world.get<Health>(first) == nullptr);
    CHECK(world.entityCount() == 0);
    CHECK_FALSE(world.despawn(first));

    const Entity second = world.spawn(Health{.value = 2});
    CHECK(second.index() == first.index());
    CHECK(second.generation() == first.generation() + 1);
    CHECK(second != first);
    // The stale handle never reaches the new entity.
    CHECK_FALSE(world.isAlive(first));
    CHECK(world.get<Health>(first) == nullptr);
    CHECK(world.add(first, Counter{}) == nullptr);
    CHECK_FALSE(world.remove<Health>(first));
    CHECK_FALSE(world.despawn(first));
    CHECK(world.get<Health>(second)->value == 2);
}

TEST_CASE("World: the null handle fails every lookup without asserting") {
    const ith::test::AssertCapture capture;
    World world;
    world.spawn(Health{});
    const Entity null;
    CHECK_FALSE(world.isAlive(null));
    CHECK_FALSE(world.has<Health>(null));
    CHECK(world.get<Health>(null) == nullptr);
    CHECK_FALSE(world.despawn(null));
    CHECK(world.add(null, Health{}) == nullptr);
    CHECK(capture.count() == 0);
}

TEST_CASE("World: handles from a handle value that was never issued fail") {
    World world;
    const Entity entity = world.spawn(Health{});
    CHECK_FALSE(world.isAlive(Entity::fromParts(entity.index() + 100, 0, world.id())));
    CHECK_FALSE(world.isAlive(Entity::fromParts(entity.index(), entity.generation() + 1, world.id())));
}

TEST_CASE("World: a handle from another world asserts and fails") {
    World first;
    World second;
    const Entity entity = first.spawn(Health{.value = 3});
    const ith::test::AssertCapture capture;
    CHECK_FALSE(second.isAlive(entity));
    CHECK(second.get<Health>(entity) == nullptr);
    CHECK_FALSE(second.despawn(entity));
    CHECK(first.isAlive(entity));
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 3);
    CHECK(capture.lastMessage() == "ecs: the entity handle belongs to another world");
#endif
}

TEST_CASE("World: clear despawns everything and never resets generations") {
    World world;
    std::vector<Entity> before;
    for (int index = 0; index < 100; ++index) {
        before.push_back(world.spawn(Position{}, Health{.value = index}));
    }
    world.clear();
    CHECK(world.entityCount() == 0);
    for (const Entity entity : before) {
        CHECK_FALSE(world.isAlive(entity));
    }
    CHECK(matching<Position>(world).empty());

    for (int index = 0; index < 100; ++index) {
        const Entity after = world.spawn(Position{}, Health{.value = index});
        CHECK(after.generation() > 0);
        for (const Entity old : before) {
            if (old.index() == after.index()) {
                CHECK(after.generation() > old.generation());
            }
        }
    }
    CHECK(world.entityCount() == 100);
    for (const Entity entity : before) {
        CHECK_FALSE(world.isAlive(entity));
    }
}

TEST_CASE("World: add and remove move the entity between archetypes and keep the other values") {
    World world;
    const Entity entity = world.spawn(Position{.x = 1.0f});
    const Entity other = world.spawn(Position{.x = 2.0f});

    REQUIRE(world.add(entity, Velocity{.x = 5.0f}) != nullptr);
    CHECK(world.has<Velocity>(entity));
    CHECK(world.get<Position>(entity)->x == 1.0f);
    CHECK(world.get<Velocity>(entity)->x == 5.0f);
    // The other entity filled the hole and is still found.
    CHECK(world.get<Position>(other)->x == 2.0f);

    Health* health = world.add(entity, Health{.value = 9});
    REQUIRE(health != nullptr);
    CHECK(health->value == 9);

    // Adding a component the entity has replaces the value in place.
    CHECK(world.add(entity, Health{.value = 11})->value == 11);
    CHECK(world.get<Health>(entity)->value == 11);

    CHECK(world.remove<Velocity>(entity));
    CHECK_FALSE(world.has<Velocity>(entity));
    CHECK_FALSE(world.remove<Velocity>(entity));
    CHECK(world.get<Position>(entity)->x == 1.0f);
    CHECK(world.get<Health>(entity)->value == 11);

    CHECK(world.remove<Position>(entity));
    CHECK(world.remove<Health>(entity));
    CHECK(world.isAlive(entity));
    CHECK_FALSE(world.has<Position>(entity));

    // Taking the same path again uses the cached edges and gives the same result.
    world.add(entity, Position{.x = 4.0f});
    world.add(entity, Velocity{.x = 6.0f});
    CHECK(matching<Position, Velocity>(world) == std::set<std::uint64_t>{entity.bits()});
}

TEST_CASE("World: swap-remove keeps every remaining entity's values") {
    World world;
    std::vector<Entity> entities;
    for (int index = 0; index < 200; ++index) {
        entities.push_back(world.spawn(Health{.value = index}, Name{.text = "entity " + std::to_string(index)}));
    }
    for (int index = 0; index < 200; index += 3) {
        CHECK(world.despawn(entities[static_cast<std::size_t>(index)]));
    }
    for (int index = 1; index < 200; index += 3) {
        CHECK(world.remove<Name>(entities[static_cast<std::size_t>(index)]));
    }
    for (int index = 0; index < 200; ++index) {
        const Entity entity = entities[static_cast<std::size_t>(index)];
        if (index % 3 == 0) {
            CHECK_FALSE(world.isAlive(entity));
            continue;
        }
        REQUIRE(world.get<Health>(entity) != nullptr);
        CHECK(world.get<Health>(entity)->value == index);
        if (index % 3 == 1) {
            CHECK_FALSE(world.has<Name>(entity));
        } else {
            CHECK(world.get<Name>(entity)->text == "entity " + std::to_string(index));
        }
    }
}

TEST_CASE("World: queries visit every matching entity with read and write access") {
    World world;
    const Entity moving = world.spawn(Position{}, Velocity{.x = 1.0f, .y = 2.0f});
    const Entity still = world.spawn(Position{.x = 10.0f});
    const Entity both = world.spawn(Position{}, Velocity{.x = 3.0f}, Health{});
    world.spawn(Velocity{.x = 100.0f});

    world.query<Position, const Velocity>().forEach([](Position& position, const Velocity& velocity) {
        position.x += velocity.x;
        position.y += velocity.y;
    });
    CHECK(world.get<Position>(moving)->x == 1.0f);
    CHECK(world.get<Position>(moving)->y == 2.0f);
    CHECK(world.get<Position>(still)->x == 10.0f);
    CHECK(world.get<Position>(both)->x == 3.0f);

    CHECK(matching<Position>(world) == std::set<std::uint64_t>{moving.bits(), still.bits(), both.bits()});
    CHECK(matching<Position, Velocity>(world) == std::set<std::uint64_t>{moving.bits(), both.bits()});
    CHECK(matching<Health, Position, Velocity>(world) == std::set<std::uint64_t>{both.bits()});
    CHECK(matching<Name>(world).empty());

    // The term order decides the argument order, not the column order.
    float sum = 0.0f;
    world.query<const Velocity, const Position>().forEach(
        [&](const Velocity& velocity, const Position& position) { sum += velocity.x + position.x; });
    CHECK(sum == doctest::Approx(8.0f)); // (1 + 1) for the moving entity, (3 + 3) for the other
}

TEST_CASE("World: a query sees archetypes created after it first ran") {
    World world;
    auto query = world.query<const Health>();
    int visits = 0;
    query.forEach([&](const Health&) { ++visits; });
    CHECK(visits == 0);

    world.spawn(Health{});
    world.spawn(Health{}, Position{});
    visits = 0;
    query.forEach([&](const Health&) { ++visits; });
    CHECK(visits == 2);

    // A new query object of the same type shares the cache.
    world.spawn(Health{}, Name{});
    visits = 0;
    world.query<const Health>().forEach([&](const Health&) { ++visits; });
    CHECK(visits == 3);

    // Emptied archetypes stay matched but are skipped cheaply.
    world.clear();
    visits = 0;
    query.forEach([&](const Health&) { ++visits; });
    CHECK(visits == 0);
}

TEST_CASE("World: queries of the same type are cached per world") {
    World first;
    World second;
    first.spawn(Health{});
    second.spawn(Health{});
    second.spawn(Health{});
    int visits = 0;
    first.query<const Health>().forEach([&](const Health&) { ++visits; });
    CHECK(visits == 1);
    visits = 0;
    second.query<const Health>().forEach([&](const Health&) { ++visits; });
    CHECK(visits == 2);
}

TEST_CASE("World: structural changes while a query iterates are refused") {
    World world;
    const Entity entity = world.spawn(Health{});
    const ith::test::AssertCapture capture;
    world.query<Health>().forEach([&](Entity current, Health& health) {
        health.value = 1;
        CHECK(world.spawn(Health{}).isNull());
        CHECK_FALSE(world.despawn(current));
        CHECK(world.add(current, Position{}) == nullptr);
        CHECK_FALSE(world.remove<Health>(current));
        world.clear();
        // Reads are fine.
        CHECK(world.get<Health>(current)->value == 1);
        CHECK(world.isAlive(current));
    });
    CHECK(world.isAlive(entity));
    CHECK(world.entityCount() == 1);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 5);
    CHECK(capture.lastMessage() ==
          "ecs: structural change while a query iterates or a system runs; record a command instead");
#endif
    // After the iteration, changes work again.
    CHECK(world.add(entity, Position{}) != nullptr);
}

TEST_CASE("World: nested read queries are allowed") {
    World world;
    world.spawn(Health{.value = 1});
    world.spawn(Health{.value = 2});
    int pairs = 0;
    world.query<const Health>().forEach(
        [&](const Health&) { world.query<const Health>().forEach([&](const Health&) { ++pairs; }); });
    CHECK(pairs == 4);
}

TEST_CASE("World: an entity can have many components, added and removed in any order") {
    World world;
    const Entity entity = world.spawn(Position{.x = 1.0f}, Velocity{.x = 2.0f}, Health{.value = 3},
                                      Name{.text = "many"}, Inventory{.items = {"a", "b"}}, Counter{.value = 4});
    world.add(entity, ith::test::Tag0{});
    world.add(entity, ith::test::Tag3{});
    world.add(entity, ith::test::Tag1{});
    world.add(entity, ith::test::Tag7{});
    world.add(entity, ith::test::Tag5{});
    world.add(entity, ith::test::Tag2{});
    world.add(entity, ith::test::Tag6{});
    world.add(entity, ith::test::Tag4{});
    CHECK(world.has<ith::test::Tag0>(entity));
    CHECK(world.has<ith::test::Tag7>(entity));

    CHECK(world.remove<Health>(entity));
    CHECK(world.remove<ith::test::Tag3>(entity));
    CHECK(world.remove<Position>(entity));
    CHECK(world.remove<ith::test::Tag0>(entity));

    CHECK(world.get<Velocity>(entity)->x == 2.0f);
    CHECK(world.get<Name>(entity)->text == "many");
    CHECK(world.get<Inventory>(entity)->items == std::vector<std::string>{"a", "b"});
    CHECK(world.get<Counter>(entity)->value == 4);
    CHECK_FALSE(world.has<Health>(entity));
    CHECK_FALSE(world.has<ith::test::Tag3>(entity));
    CHECK(world.has<ith::test::Tag4>(entity));

    const Entity sameSet = world.spawn(Velocity{}, Name{}, Inventory{}, Counter{}, ith::test::Tag1{}, ith::test::Tag2{},
                                       ith::test::Tag4{}, ith::test::Tag5{}, ith::test::Tag6{}, ith::test::Tag7{});
    CHECK(matching<Velocity, Name, Inventory, Counter, ith::test::Tag1, ith::test::Tag7>(world) ==
          std::set<std::uint64_t>{entity.bits(), sameSet.bits()});
}

TEST_CASE("World: values are constructed and destroyed exactly once") {
    REQUIRE(Tracked::s_alive == 0);
    {
        World world;
        std::vector<Entity> entities;
        for (int index = 0; index < 100; ++index) {
            entities.push_back(world.spawn(Tracked(index)));
        }
        CHECK(Tracked::s_alive == 100);
        for (int index = 0; index < 100; index += 2) {
            world.add(entities[static_cast<std::size_t>(index)], Health{});
        }
        CHECK(Tracked::s_alive == 100);
        world.add(entities[1], Tracked(1000));
        CHECK(Tracked::s_alive == 100);
        CHECK(world.get<Tracked>(entities[1])->value == 1000);
        for (int index = 0; index < 100; index += 4) {
            world.remove<Tracked>(entities[static_cast<std::size_t>(index)]);
        }
        CHECK(Tracked::s_alive == 75);
        for (int index = 1; index < 100; index += 4) {
            world.despawn(entities[static_cast<std::size_t>(index)]);
        }
        CHECK(Tracked::s_alive == 50);
        for (int index = 2; index < 100; index += 4) {
            CHECK(world.get<Tracked>(entities[static_cast<std::size_t>(index)])->value == index);
        }
        world.clear();
        CHECK(Tracked::s_alive == 0);
        for (int index = 0; index < 10; ++index) {
            world.spawn(Tracked(index), Name{.text = "left for the destructor"});
        }
        CHECK(Tracked::s_alive == 10);
    }
    CHECK(Tracked::s_alive == 0);
}

TEST_CASE("World: strings survive column growth and archetype moves") {
    World world;
    std::vector<Entity> entities;
    for (int index = 0; index < 1000; ++index) {
        entities.push_back(
            world.spawn(Name{.text = "a string longer than any small-string buffer #" + std::to_string(index)}));
    }
    for (std::size_t index = 0; index < entities.size(); index += 2) {
        world.add(entities[index], Inventory{.items = {"sword", std::to_string(index)}});
    }
    for (std::size_t index = 0; index < entities.size(); index += 4) {
        world.remove<Inventory>(entities[index]);
    }
    for (std::size_t index = 0; index < entities.size(); ++index) {
        CHECK(world.get<Name>(entities[index])->text ==
              "a string longer than any small-string buffer #" + std::to_string(index));
        if (index % 4 == 2) {
            CHECK(world.get<Inventory>(entities[index])->items[1] == std::to_string(index));
        } else {
            CHECK_FALSE(world.has<Inventory>(entities[index]));
        }
    }
}

TEST_CASE("World: components are stored at their alignment") {
    World world;
    std::vector<Entity> entities;
    for (int index = 0; index < 100; ++index) {
        entities.push_back(world.spawn(Health{}, Aligned{}));
    }
    for (std::size_t index = 0; index < entities.size(); index += 3) {
        world.add(entities[index], Position{});
    }
    bool aligned = true;
    world.query<const Aligned>().forEach(
        [&](const Aligned& value) { aligned = aligned && reinterpret_cast<std::uintptr_t>(&value) % 64 == 0; });
    CHECK(aligned);
    for (const Entity entity : entities) {
        CHECK(reinterpret_cast<std::uintptr_t>(world.get<Aligned>(entity)) % 64 == 0);
        CHECK(reinterpret_cast<std::uintptr_t>(world.get<Health>(entity)) % alignof(Health) == 0);
    }
}

TEST_CASE("World: component ids come from reflection") {
    const ith::detail::ComponentIndex health = ith::detail::componentIndexOf<Health>();
    CHECK(&ith::detail::componentType(health) == &ith::typeOf<Health>());
    CHECK(ith::detail::componentType(health).id == ith::fnv1a64("ith::test::Health"));
    CHECK(ith::detail::componentType(health).triviallyCopyable);
    CHECK_FALSE(ith::detail::componentType(ith::detail::componentIndexOf<Name>()).triviallyCopyable);
    // Registering the same type id again gives the same index, also through another record
    // with the same name and layout.
    CHECK(ith::detail::registerComponentType(ith::typeOf<Health>()) == health);
    static constexpr ith::TypeInfo kSecondRecord =
        ith::detail::makeStructType<Health>(ith::fnv1a64("ith::test::Health"), "ith::test::Health",
                                            ith::TypeRole::Component, ith::ReflectionFlags::None, {}, {});
    CHECK(ith::detail::registerComponentType(kSecondRecord) == health);
    CHECK(ith::detail::componentIndexOf<Position>() != health);
}

TEST_CASE("World: a type without the Component role asserts and is never stored") {
    const ith::test::AssertCapture capture;
    CHECK(ith::detail::registerComponentType(ith::typeOf<ith::test::NotAComponent>()) == ith::detail::kNoComponent);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
    CHECK(ith::detail::componentIndexOf<ith::test::NotAComponent>() == ith::detail::kNoComponent);

    World world;
    const Entity entity = world.spawn(Health{});
    CHECK(world.spawn(ith::test::NotAComponent{}).isNull());
    CHECK(world.spawn(Health{}, ith::test::NotAComponent{}).isNull());
    CHECK(world.add(entity, ith::test::NotAComponent{}) == nullptr);
    CHECK_FALSE(world.has<ith::test::NotAComponent>(entity));
    CHECK(world.get<ith::test::NotAComponent>(entity) == nullptr);
    CHECK_FALSE(world.remove<ith::test::NotAComponent>(entity));
    int visits = 0;
    world.query<const ith::test::NotAComponent>().forEach([&](const ith::test::NotAComponent&) { ++visits; });
    world.query<const Health, const ith::test::NotAComponent>().forEach(
        [&](const Health&, const ith::test::NotAComponent&) { ++visits; });
    CHECK(visits == 0);
    CHECK(world.entityCount() == 1);
}

TEST_CASE("World: spawn copies its arguments before storage grows") {
    // Archetypes grow from 16 rows; the 17th spawn reallocates the columns the arguments refer to.
    World world;
    std::vector<Entity> entities;
    for (int index = 0; index < 16; ++index) {
        entities.push_back(
            world.spawn(Name{.text = "a value long enough to live on the heap #" + std::to_string(index)},
                        Counter{.value = index}));
    }
    const Entity source = entities[3];
    const Entity copy = world.spawn(*world.get<Name>(source), *world.get<Counter>(source));
    REQUIRE(world.isAlive(copy));
    CHECK(world.get<Name>(copy)->text == "a value long enough to live on the heap #3");
    CHECK(world.get<Counter>(copy)->value == 3);
    CHECK(world.get<Name>(source)->text == "a value long enough to live on the heap #3");

    // add takes its value by copy too, so a value of another entity can be passed.
    const Entity target = world.spawn(Counter{});
    REQUIRE(world.add(target, *world.get<Name>(source)) != nullptr);
    CHECK(world.get<Name>(target)->text == world.get<Name>(source)->text);
}

TEST_CASE("World: add replaces a non-trivial value, destroying the old one") {
    REQUIRE(Tracked::s_alive == 0);
    {
        World world;
        const Entity entity = world.spawn(Tracked(1), Name{.text = "keeps its name"});
        CHECK(Tracked::s_alive == 1);
        Tracked* replaced = world.add(entity, Tracked(2));
        REQUIRE(replaced != nullptr);
        CHECK(replaced->value == 2);
        CHECK(Tracked::s_alive == 1);
        CHECK(world.add(entity, Name{.text = "a new name"})->text == "a new name");
        CHECK(world.get<Tracked>(entity)->value == 2);
    }
    CHECK(Tracked::s_alive == 0);
}

TEST_CASE("World: removing the last component leaves the entity alive without components") {
    World world;
    const Entity entity = world.spawn(Health{.value = 5});
    const Entity other = world.spawn(Health{.value = 6});
    CHECK(world.remove<Health>(entity));
    CHECK(world.isAlive(entity));
    CHECK_FALSE(world.has<Health>(entity));
    CHECK(matching<Health>(world) == std::set<std::uint64_t>{other.bits()});
    CHECK(world.add(entity, Health{.value = 7})->value == 7);
    CHECK(matching<Health>(world) == std::set<std::uint64_t>{entity.bits(), other.bits()});
    CHECK(world.remove<Health>(entity));
    CHECK(world.despawn(entity));
    CHECK(world.entityCount() == 1);
}

TEST_CASE("World: has and get accept a const component type") {
    World world;
    const Entity entity = world.spawn(Health{.value = 4});
    CHECK(world.has<const Health>(entity));
    const Health* health = world.get<const Health>(entity);
    REQUIRE(health != nullptr);
    CHECK(health->value == 4);
    CHECK(world.remove<const Health>(entity));
}

TEST_CASE("World: over-aligned components keep a 128-byte alignment") {
    World world;
    std::vector<Entity> entities;
    for (int index = 0; index < 40; ++index) {
        entities.push_back(world.spawn(Aligned128{}, Health{.value = index}));
    }
    for (std::size_t index = 0; index < entities.size(); index += 2) {
        world.add(entities[index], Position{});
    }
    for (std::size_t index = 1; index < entities.size(); index += 5) {
        world.despawn(entities[index]);
    }
    world.query<const Aligned128>().forEach(
        [&](const Aligned128& value) { CHECK(reinterpret_cast<std::uintptr_t>(&value) % 128 == 0); });
}

// The 16.7 million despawn/spawn cycles take seconds in Debug and much longer under the
// sanitizers, where the other tests already cover the code paths.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ITH_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ITH_TEST_SANITIZED 1
#endif
#endif
#ifdef ITH_TEST_SANITIZED
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

TEST_CASE("World: a slot whose generation would wrap is retired" * doctest::skip(kSanitized)) {
    World world;
    const Entity first = world.spawn();
    Entity entity = first;
    for (std::uint32_t generation = 0; generation < Entity::kMaxGeneration; ++generation) {
        world.despawn(entity);
        entity = world.spawn();
        if (entity.index() != first.index()) {
            break;
        }
    }
    REQUIRE(entity.index() == first.index());
    CHECK(entity.generation() == Entity::kMaxGeneration);
    world.despawn(entity);
    // The slot is retired: the next entity takes a new slot, and the old handles stay stale.
    const Entity next = world.spawn();
    CHECK(next.index() != first.index());
    CHECK_FALSE(world.isAlive(first));
    CHECK_FALSE(world.isAlive(entity));
}
