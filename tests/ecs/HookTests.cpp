// Component hooks and immutable components (ADR-0014 rules 10 and 11, ADR-0030): when each hook
// runs and in which order, the commands hooks record, structural changes refused inside a hook,
// registration rules, and every path that would write an immutable component in place.

#include "ith/ecs/Hooks.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::Commands;
using ith::Entity;
using ith::HookContext;
using ith::World;
using ith::test::Frozen;
using ith::test::Health;
using ith::test::Hooked;
using ith::test::HookedOther;
using ith::test::HookLog;

// Removals seen by Hooked's onRemove in any world, also one without a HookLog.
int g_hookedRemovals = 0;

// The world StructureProbe's hook tries to change directly.
World* g_probeWorld = nullptr;

// Whether Hooked's last onRemove saw the entity's HookedOther, i.e. the entity was still whole.
bool g_removeSawOther = false;

// When set, Hooked's onAdd asks HookContext::value for another type and stores whether it was
// refused.
bool g_probeValueType = false;
bool g_wrongTypeRefused = false;

template <typename T>
void logEvent(HookContext& context, std::string_view type, std::string_view hook) {
    const T* value = context.value<const T>();
    if (HookLog* log = context.resource<HookLog>()) {
        log->events.push_back(std::string(type) + "." + std::string(hook) + ":" + std::to_string(value->value));
    }
}

void setTestHooks() {
    static const bool kSet = [] {
        CHECK(ith::setComponentHooks<Hooked>({
            .onAdd =
                [](HookContext& context) {
                    if (g_probeValueType) {
                        g_wrongTypeRefused = context.value<const HookedOther>() == nullptr;
                    }
                    logEvent<Hooked>(context, "Hooked", "add");
                },
            .onInsert = [](HookContext& context) { logEvent<Hooked>(context, "Hooked", "insert"); },
            .onReplace = [](HookContext& context) { logEvent<Hooked>(context, "Hooked", "replace"); },
            .onRemove =
                [](HookContext& context) {
                    ++g_hookedRemovals;
                    g_removeSawOther = context.has<HookedOther>(context.entity());
                    logEvent<Hooked>(context, "Hooked", "remove");
                },
        }));
        CHECK(ith::setComponentHooks<HookedOther>({
            .onAdd = [](HookContext& context) { logEvent<HookedOther>(context, "HookedOther", "add"); },
            .onInsert = [](HookContext& context) { logEvent<HookedOther>(context, "HookedOther", "insert"); },
            .onReplace = [](HookContext& context) { logEvent<HookedOther>(context, "HookedOther", "replace"); },
            .onRemove = [](HookContext& context) { logEvent<HookedOther>(context, "HookedOther", "remove"); },
        }));
        CHECK(ith::setComponentHooks<ith::test::Spawner>({
            .onAdd =
                [](HookContext& context) {
                    context.commands().add(context.entity(), Health{.value = 7});
                    context.commands().spawn(Hooked{.value = 99});
                },
        }));
        CHECK(ith::setComponentHooks<ith::test::StructureProbe>({
            .onAdd =
                [](HookContext& context) {
                    CHECK(g_probeWorld->spawn(Health{}).isNull());
                    CHECK_FALSE(g_probeWorld->remove<ith::test::StructureProbe>(context.entity()));
                    CHECK_FALSE(g_probeWorld->despawn(context.entity()));
                },
        }));
        return true;
    }();
    CHECK(kSet);
}

// A world with an empty HookLog.
struct HookWorld {
    HookWorld() {
        setTestHooks();
        world.insertResource(HookLog{});
    }

    std::vector<std::string> takeLog() { return std::exchange(world.resource<HookLog>()->events, {}); }

    World world;
};

using Log = std::vector<std::string>;

} // namespace

TEST_CASE("Hooks: spawn runs onAdd for each component, then onInsert, in the order given") {
    HookWorld test;
    test.world.spawn(HookedOther{.value = 2}, Hooked{.value = 1});
    CHECK(test.takeLog() == Log{"HookedOther.add:2", "Hooked.add:1", "HookedOther.insert:2", "Hooked.insert:1"});

    Commands commands(test.world);
    commands.spawn(Hooked{.value = 3}, Health{}, HookedOther{.value = 4});
    test.world.apply(commands);
    CHECK(test.takeLog() == Log{"Hooked.add:3", "HookedOther.add:4", "Hooked.insert:3", "HookedOther.insert:4"});

    // Components without hooks run nothing.
    test.world.spawn(Health{});
    CHECK(test.takeLog().empty());
}

TEST_CASE("Hooks: add, replace and remove") {
    HookWorld test;
    const Entity entity = test.world.spawn(Health{});

    SUBCASE("typed") {
        REQUIRE(test.world.add(entity, Hooked{.value = 1}) != nullptr);
        CHECK(test.takeLog() == Log{"Hooked.add:1", "Hooked.insert:1"});
        REQUIRE(test.world.add(entity, Hooked{.value = 2}) != nullptr);
        CHECK(test.takeLog() == Log{"Hooked.replace:1", "Hooked.insert:2"});
        CHECK(test.world.remove<Hooked>(entity));
        CHECK(test.takeLog() == Log{"Hooked.replace:2", "Hooked.remove:2"});
        CHECK_FALSE(test.world.remove<Hooked>(entity));
        CHECK(test.takeLog().empty());
    }
    SUBCASE("by record") {
        const ith::TypeInfo& type = ith::typeOf<Hooked>();
        REQUIRE(test.world.add(entity, type) != nullptr); // default-constructed
        CHECK(test.takeLog() == Log{"Hooked.add:0", "Hooked.insert:0"});
        Hooked value{.value = 5};
        REQUIRE(test.world.add(entity, type, &value) != nullptr); // hooks see the given value
        CHECK(test.takeLog() == Log{"Hooked.replace:0", "Hooked.insert:5"});
        CHECK(test.world.remove(entity, type));
        CHECK(test.takeLog() == Log{"Hooked.replace:5", "Hooked.remove:5"});
    }
    SUBCASE("through commands") {
        Commands commands(test.world);
        commands.add(entity, Hooked{.value = 1});
        commands.add(entity, Hooked{.value = 2});
        commands.remove<Hooked>(entity);
        test.world.apply(commands);
        CHECK(test.takeLog() == Log{"Hooked.add:1", "Hooked.insert:1", "Hooked.replace:1", "Hooked.insert:2",
                                    "Hooked.replace:2", "Hooked.remove:2"});
    }
    // A stale handle runs nothing.
    CHECK(test.world.despawn(entity));
    CHECK(test.world.add(entity, Hooked{.value = 9}) == nullptr);
    CHECK(test.takeLog().empty());
}

TEST_CASE("Hooks: despawn and clear run onReplace for every component, then onRemove, sorted by type id") {
    HookWorld test;
    const bool hookedFirst = ith::typeOf<Hooked>().id < ith::typeOf<HookedOther>().id;
    const Log expected =
        hookedFirst ? Log{"Hooked.replace:1", "HookedOther.replace:2", "Hooked.remove:1", "HookedOther.remove:2"}
                    : Log{"HookedOther.replace:2", "Hooked.replace:1", "HookedOther.remove:2", "Hooked.remove:1"};
    const Entity entity = test.world.spawn(Hooked{.value = 1}, Health{}, HookedOther{.value = 2});
    test.takeLog();

    SUBCASE("despawn") {
        CHECK(test.world.despawn(entity));
        CHECK(test.takeLog() == expected);
    }
    SUBCASE("despawn through commands") {
        Commands commands(test.world);
        commands.despawn(entity);
        test.world.apply(commands);
        CHECK(test.takeLog() == expected);
    }
    SUBCASE("clear") {
        test.world.clear();
        CHECK(test.takeLog() == expected);
        CHECK(test.world.entityCount() == 0);
    }
}

TEST_CASE("Hooks: despawn runs them while the entity is whole") {
    HookWorld test;
    const Entity entity = test.world.spawn(Hooked{.value = 1}, HookedOther{.value = 2});
    g_removeSawOther = false;
    CHECK(test.world.despawn(entity));
    CHECK(g_removeSawOther);
    CHECK_FALSE(test.world.isAlive(entity));
}

TEST_CASE("Hooks: commands from hooks apply right after the change, depth-first") {
    HookWorld test;

    SUBCASE("a direct call") {
        const Entity entity = test.world.spawn(ith::test::Spawner{});
        // The Health and the spawned entity exist when spawn returns; the spawned entity's hooks ran.
        REQUIRE(test.world.get<Health>(entity) != nullptr);
        CHECK(test.world.get<Health>(entity)->value == 7);
        CHECK(test.world.entityCount() == 2);
        CHECK(test.takeLog() == Log{"Hooked.add:99", "Hooked.insert:99"});
    }
    SUBCASE("each command's hook commands apply before the next command") {
        const Entity first = test.world.spawn();
        const Entity second = test.world.spawn();
        Commands commands(test.world);
        commands.add(first, ith::test::Spawner{});
        commands.add(second, Hooked{.value = 1});
        test.world.apply(commands);
        CHECK(test.takeLog() == Log{"Hooked.add:99", "Hooked.insert:99", "Hooked.add:1", "Hooked.insert:1"});
        CHECK(test.world.has<Health>(first));
        CHECK(test.world.entityCount() == 3);
    }
    SUBCASE("at a schedule's sync point") {
        ith::Schedule schedule;
        schedule.add({.name = "spawn",
                      .run = [](ith::SystemContext& context) { context.commands().spawn(ith::test::Spawner{}); }});
        REQUIRE(schedule.build());
        schedule.run(test.world);
        CHECK(test.world.entityCount() == 2);
        CHECK(test.takeLog() == Log{"Hooked.add:99", "Hooked.insert:99"});
    }
}

TEST_CASE("Hooks: structural changes inside a hook are refused") {
    HookWorld test;
    g_probeWorld = &test.world;
    const ith::test::AssertCapture capture;
    const Entity entity = test.world.spawn(ith::test::StructureProbe{});
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 3);
#endif
    CHECK(test.world.isAlive(entity));
    CHECK(test.world.has<ith::test::StructureProbe>(entity));
    CHECK(test.world.entityCount() == 1);
    g_probeWorld = nullptr;
}

TEST_CASE("Hooks: the World's destructor runs no hooks") {
    setTestHooks();
    const int before = g_hookedRemovals;
    {
        World world;
        world.spawn(Hooked{.value = 1});
    }
    CHECK(g_hookedRemovals == before);
    {
        World world;
        world.spawn(Hooked{.value = 1});
        world.clear();
    }
    CHECK(g_hookedRemovals == before + 1);
}

TEST_CASE("Hooks: they are set once per type, before a World stores it, and hold in every world") {
    setTestHooks();
    {
        const ith::test::AssertCapture capture;
        CHECK_FALSE(ith::setComponentHooks<Hooked>({}));                                  // set twice
        CHECK_FALSE(ith::setComponentHooks(ith::typeOf<HookLog>(), {}));                  // not a component
        CHECK_FALSE(ith::setComponentHooks(ith::typeOf<ith::test::NotAComponent>(), {})); // not a component
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 3);
#endif
    }
    {
        World world;
        world.spawn(ith::test::LateHooked{});
        const ith::test::AssertCapture capture;
        CHECK_FALSE(ith::setComponentHooks<ith::test::LateHooked>({}));
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 1);
#endif
    }
    // Another world runs the same hooks without registering anything.
    HookWorld first;
    HookWorld second;
    first.world.spawn(Hooked{.value = 1});
    second.world.spawn(Hooked{.value = 2});
    CHECK(first.takeLog() == Log{"Hooked.add:1", "Hooked.insert:1"});
    CHECK(second.takeLog() == Log{"Hooked.add:2", "Hooked.insert:2"});
}

TEST_CASE("Hooks: HookContext::value checks the type") {
    HookWorld test;
    const ith::test::AssertCapture capture;
    g_probeValueType = true;
    g_wrongTypeRefused = false;
    test.world.spawn(Hooked{.value = 1});
    g_probeValueType = false;
    CHECK(g_wrongTypeRefused);
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
    // The hook's own type is given (logEvent asks for it).
    CHECK(test.takeLog() == Log{"Hooked.add:1", "Hooked.insert:1"});
}

TEST_CASE("Immutable components: reads work, writes in place are refused") {
    World world;
    const Entity entity = world.spawn(Frozen{.value = 7}, Health{});
    const ith::TypeInfo& type = ith::typeOf<Frozen>();
    CHECK(hasFlags(type.flags, ith::ReflectionFlags::Immutable));

    // Reading is allowed on every path.
    REQUIRE(world.get<const Frozen>(entity) != nullptr);
    CHECK(world.get<const Frozen>(entity)->value == 7);
    CHECK(std::as_const(world).get<Frozen>(entity)->value == 7);
    CHECK(std::as_const(world).get(entity, type) == world.get<const Frozen>(entity));
    int reads = 0;
    world.query<const Frozen>().forEach([&](const Frozen& id) { reads += static_cast<int>(id.value); });
    CHECK(reads == 7);
    const std::array readTerm{ith::QueryTerm{.type = &type, .access = ith::Access::Read}};
    int dynamicReads = 0;
    world.dynamicQuery(readTerm).forEach([&](Entity, std::span<void* const>) { ++dynamicReads; });
    CHECK(dynamicReads == 1);
    int visits = 0;
    std::as_const(world).forEachComponent(entity, [&](const ith::TypeInfo&, const void*) { ++visits; });
    CHECK(visits == 2);

    // Writing in place asserts and is refused.
    {
        const ith::test::AssertCapture capture;
        CHECK(world.get<Frozen>(entity) == nullptr);
        CHECK(world.get(entity, type) == nullptr);
        int writes = 0;
        world.query<Frozen, const Health>().forEach([&](Frozen&, const Health&) { ++writes; });
        const std::array writeTerm{ith::QueryTerm{.type = &type, .access = ith::Access::Write}};
        world.dynamicQuery(writeTerm).forEach([&](Entity, std::span<void* const>) { ++writes; });
        CHECK(writes == 0);
        int writableVisits = 0;
        world.forEachComponent(entity, [&](const ith::TypeInfo& visited, void*) {
            CHECK(visited.id != type.id);
            ++writableVisits;
        });
        CHECK(writableVisits == 1); // the Health only
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 5);
#endif
    }

    // Replacing is allowed.
    REQUIRE(world.add(entity, Frozen{.value = 8}) != nullptr);
    CHECK(world.get<const Frozen>(entity)->value == 8);
    Commands commands(world);
    commands.add(entity, Frozen{.value = 9});
    world.apply(commands);
    CHECK(world.get<const Frozen>(entity)->value == 9);
    Frozen loaded{.value = 10};
    REQUIRE(world.add(entity, type, &loaded) != nullptr);
    CHECK(world.get<const Frozen>(entity)->value == 10);
    CHECK(world.remove<Frozen>(entity));
}

TEST_CASE("Immutable components: systems may read them but not declare write access") {
    World world;
    world.spawn(Frozen{.value = 3});

    ith::Schedule writer;
    writer.add({.name = "writer", .access = ith::accessTo<Frozen>(), .run = [](ith::SystemContext&) {}});
    const ith::Expected<void> built = writer.build();
    REQUIRE_FALSE(built);
    CHECK(built.error().code == "ecs.schedule.invalid_access");

    ith::Schedule reader;
    std::uint64_t sum = 0;
    reader.add({.name = "reader", .access = ith::accessTo<const Frozen>(), .run = [&](ith::SystemContext& context) {
                    context.query<const Frozen>().forEach([&](const Frozen& id) { sum += id.value; });
                    context.commands().spawn(Frozen{.value = 4}); // new values come through commands
                }});
    REQUIRE(reader.build());
    reader.run(world);
    CHECK(sum == 3);
    CHECK(world.entityCount() == 2);
}
