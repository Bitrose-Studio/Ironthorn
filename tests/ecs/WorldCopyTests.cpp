// World copy (ADR-0032): the copy equals the source, slot for slot under the target's world id,
// except EditorOnly types when they are left out; the hierarchy, its order and the enabled state
// are kept; reflected entities in components and resources are remapped; ticks start over; no
// hooks run; and a target that isn't fresh, the source itself or a source being iterated are
// refused.

#include "ith/ecs/WorldCopy.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"
#include "EditorEcsComponents.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using ith::ChildOf;
using ith::Children;
using ith::Disabled;
using ith::DisabledInHierarchy;
using ith::Entity;
using ith::StableId;
using ith::Uuid;
using ith::World;
using ith::test::CopyProbe;
using ith::test::EditorLabel;
using ith::test::EditorSelection;
using ith::test::Follow;
using ith::test::Gravity;
using ith::test::Health;
using ith::test::Inventory;
using ith::test::Name;
using ith::test::Patrol;
using ith::test::Position;
using ith::test::Roster;
using ith::test::Waypoint;

// The handle of `entity`'s copy in `target`.
Entity inCopy(Entity entity, const World& target) {
    return Entity::fromParts(entity.index(), entity.generation(), target.id());
}

std::vector<Entity> roots(const World& world) {
    std::vector<Entity> result;
    world.forEachRoot([&](Entity root) { result.push_back(root); });
    return result;
}

std::vector<Entity> childrenOf(const World& world, Entity parent) {
    const Children* children = world.get<const Children>(parent);
    return children != nullptr ? children->entities : std::vector<Entity>{};
}

int g_probeHooks = 0;

void setProbeHooks() {
    static const bool kSet = ith::setComponentHooks<CopyProbe>({
        .onAdd = [](ith::HookContext&) { ++g_probeHooks; },
        .onInsert = [](ith::HookContext&) { ++g_probeHooks; },
        .onReplace = [](ith::HookContext&) { ++g_probeHooks; },
        .onRemove = [](ith::HookContext&) { ++g_probeHooks; },
    });
    CHECK(kSet);
}

} // namespace

TEST_CASE("World copy: every entity and value, in the same slots under the target's id") {
    World source;
    const Entity a = source.spawn(Position{.x = 1.0f}, Name{.text = "a name long enough to live on the heap"});
    const Entity gone = source.spawn(Health{});
    const Entity b = source.spawn(Inventory{.items = {"sword", "shield"}}, Health{.value = 7});
    const Entity empty = source.spawn();
    source.despawn(gone);
    source.insertResource(Gravity{});

    World target;
    REQUIRE(ith::copyWorld(source, target));
    CHECK(target.entityCount() == 3);
    CHECK(target.resourceCount() == 1);
    CHECK(target.isAlive(inCopy(a, target)));
    CHECK(target.isAlive(inCopy(b, target)));
    CHECK(target.isAlive(inCopy(empty, target)));
    REQUIRE(target.get<const Position>(inCopy(a, target)) != nullptr);
    CHECK(target.get<const Position>(inCopy(a, target))->x == 1.0f);
    CHECK(target.get<const Name>(inCopy(a, target))->text == source.get<const Name>(a)->text);
    CHECK(target.get<const Inventory>(inCopy(b, target))->items == std::vector<std::string>{"sword", "shield"});
    CHECK(target.get<const Health>(inCopy(b, target))->value == 7);
    CHECK_FALSE(target.has<Health>(inCopy(a, target)));

    // The copy owns its values: changing one world leaves the other alone.
    target.get<Name>(inCopy(a, target))->text = "changed";
    CHECK(source.get<const Name>(a)->text == "a name long enough to live on the heap");

    // The free slots are the same: both worlds reuse the despawned slot, with the same generation.
    const Entity next = source.spawn();
    const Entity nextCopy = target.spawn();
    CHECK(nextCopy == inCopy(next, target));
}

TEST_CASE("World copy: the hierarchy, child and root order and the enabled state are kept") {
    World source;
    const Entity car = source.spawn(ith::LocalTransform{});
    const Entity house = source.spawn();
    const Entity wheel1 = source.spawn(ChildOf{.parent = car, .despawnWithParent = true});
    const Entity wheel2 = source.spawn(ChildOf{.parent = car});
    const Entity bolt = source.spawn(ChildOf{.parent = wheel2});
    REQUIRE(source.placeBefore(wheel2, wheel1));
    REQUIRE(source.placeBefore(house, car));
    source.add(car, Disabled{});

    World target;
    REQUIRE(ith::copyWorld(source, target));
    const Entity carCopy = inCopy(car, target);
    const Entity wheel1Copy = inCopy(wheel1, target);
    const Entity wheel2Copy = inCopy(wheel2, target);
    const Entity boltCopy = inCopy(bolt, target);
    CHECK(roots(target) == std::vector{inCopy(house, target), carCopy});
    CHECK(childrenOf(target, carCopy) == std::vector{wheel2Copy, wheel1Copy});
    CHECK(childrenOf(target, wheel2Copy) == std::vector{boltCopy});
    CHECK(target.get<const ChildOf>(boltCopy)->parent == wheel2Copy);
    CHECK(target.get<const ChildOf>(wheel1Copy)->despawnWithParent);
    CHECK(target.has<Disabled>(carCopy));
    CHECK(target.has<DisabledInHierarchy>(carCopy));
    CHECK(target.has<DisabledInHierarchy>(boltCopy));
    CHECK_FALSE(target.has<DisabledInHierarchy>(inCopy(house, target)));

    // The copy's hooks work on what was copied: enabling and despawning follow the hierarchy.
    target.remove<Disabled>(carCopy);
    CHECK_FALSE(target.has<DisabledInHierarchy>(boltCopy));
    target.despawn(carCopy);
    CHECK_FALSE(target.isAlive(wheel1Copy)); // a part of the car
    CHECK(target.isAlive(wheel2Copy));       // detached, a root now
    CHECK(roots(target) == std::vector{inCopy(house, target), wheel2Copy});
    // The source is unchanged.
    CHECK(source.isAlive(wheel1));
    CHECK(childrenOf(source, car) == std::vector{wheel2, wheel1});
}

TEST_CASE("World copy: reflected entities in components and resources are remapped") {
    World other;
    const Entity elsewhere = other.spawn();

    World source;
    const Entity leader = source.spawn();
    const Entity gone = source.spawn();
    source.despawn(gone);
    const Entity follower = source.spawn(Follow{.target = leader, .unreflected = leader});
    const Entity stale = source.spawn(Follow{.target = gone});
    const Entity foreign = source.spawn(Follow{.target = elsewhere});
    const Entity patrol = source.spawn(
        Patrol{.route = {Waypoint{.at = leader, .wait = 2.0f}, std::nullopt, Waypoint{.at = follower}, Waypoint{}}});
    source.insertResource(Roster{.members = {leader, follower, gone}});

    World target;
    REQUIRE(ith::copyWorld(source, target));
    const Follow* copied = target.get<const Follow>(inCopy(follower, target));
    REQUIRE(copied != nullptr);
    CHECK(copied->target == inCopy(leader, target));
    CHECK(copied->unreflected == leader); // not reflected: copied as it is
    // Handles that aren't alive in the source become null.
    CHECK(target.get<const Follow>(inCopy(stale, target))->target.isNull());
    CHECK(target.get<const Follow>(inCopy(foreign, target))->target.isNull());

    const Patrol* route = target.get<const Patrol>(inCopy(patrol, target));
    REQUIRE(route != nullptr);
    REQUIRE(route->route.size() == 4);
    CHECK(route->route[0]->at == inCopy(leader, target));
    CHECK(route->route[0]->wait == 2.0f);
    CHECK_FALSE(route->route[1].has_value());
    CHECK(route->route[2]->at == inCopy(follower, target));
    CHECK(route->route[3]->at.isNull());

    const Roster* roster = target.resource<const Roster>();
    REQUIRE(roster != nullptr);
    CHECK(roster->members == std::vector{inCopy(leader, target), inCopy(follower, target), Entity()});
    // The source keeps its own handles.
    CHECK(source.resource<const Roster>()->members[0] == leader);
}

TEST_CASE("World copy: EditorOnly components and resources are left out on request") {
    World source;
    const Entity labelled = source.spawn(Position{.x = 3.0f}, EditorLabel{.text = "Spawn point"});
    const Entity onlyLabel = source.spawn(EditorLabel{.text = "Note"});
    source.insertResource(EditorSelection{.selected = "Spawn point"});
    source.insertResource(Gravity{});

    World full;
    REQUIRE(ith::copyWorld(source, full));
    CHECK(full.get<const EditorLabel>(inCopy(labelled, full))->text == "Spawn point");
    CHECK(full.resource<const EditorSelection>() != nullptr);

    World play;
    REQUIRE(ith::copyWorld(source, play, {.excludeEditorOnly = true}));
    CHECK(play.entityCount() == 2);
    CHECK_FALSE(play.has<EditorLabel>(inCopy(labelled, play)));
    CHECK(play.get<const Position>(inCopy(labelled, play))->x == 3.0f);
    CHECK(play.isAlive(inCopy(onlyLabel, play))); // the entity stays, without the component
    CHECK(play.resource<const EditorSelection>() == nullptr);
    CHECK(play.resource<const Gravity>() != nullptr);
}

TEST_CASE("World copy: stable ids are kept and found in the copy") {
    constexpr Uuid kDoor(0xd00, 1);
    World source;
    const Entity door = source.spawn(StableId{.id = kDoor}, Health{});
    World target;
    REQUIRE(ith::copyWorld(source, target));
    CHECK(target.findByStableId(kDoor) == inCopy(door, target));
    CHECK(source.findByStableId(kDoor) == door);
    // The copy's lookup is its own: a duplicate is refused there too.
    const ith::test::AssertCapture capture;
    CHECK(target.spawn(StableId{.id = kDoor}).isNull());
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 1);
#endif
}

TEST_CASE("World copy: ticks start over, so every value of the copy is new to its systems") {
    using ith::Schedule;
    using ith::Stage;
    using ith::SystemContext;
    std::set<std::uint64_t> added;
    Schedule schedule;
    schedule.add({
        .name = "test::copy_watch",
        .stage = Stage::PostUpdate,
        .access = ith::accessTo<const Health>(),
        .run =
            [&added](SystemContext& context) {
                added.clear();
                context.query<ith::Added<Health>>().forEach([&](Entity entity) { added.insert(entity.bits()); });
            },
    });
    REQUIRE(schedule.build());

    World source;
    const Entity a = source.spawn(Health{});
    const Entity b = source.spawn(Health{});
    schedule.run(source);
    schedule.run(source);
    CHECK(added.empty()); // the source's second run saw nothing new

    World target;
    REQUIRE(ith::copyWorld(source, target));
    schedule.run(target);
    CHECK(added == std::set{inCopy(a, target).bits(), inCopy(b, target).bits()});
    schedule.run(target);
    CHECK(added.empty());
}

TEST_CASE("World copy: no hooks run") {
    setProbeHooks();
    World source;
    const Entity entity = source.spawn(CopyProbe{.value = 4});
    const int before = g_probeHooks;
    {
        World target;
        REQUIRE(ith::copyWorld(source, target));
        CHECK(g_probeHooks == before);
        CHECK(target.get<const CopyProbe>(inCopy(entity, target))->value == 4);
    } // the World's destructor runs no hooks either
    CHECK(g_probeHooks == before);
}

TEST_CASE("World copy: a target that isn't fresh, the source itself and an iterated source are refused") {
    World source;
    const Entity entity = source.spawn(Health{});
    const ith::test::AssertCapture capture;

    World used;
    used.despawn(used.spawn()); // nothing alive, but a slot was used
    CHECK_FALSE(ith::copyWorld(source, used));
    CHECK(used.entityCount() == 0);

    World withResource;
    withResource.insertResource(Gravity{});
    CHECK_FALSE(ith::copyWorld(source, withResource));
    CHECK(withResource.entityCount() == 0);

    CHECK_FALSE(ith::copyWorld(source, source));

    World target;
    source.forEachRoot([&](Entity) { CHECK_FALSE(ith::copyWorld(source, target)); });
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 4);
#endif
    // Refusals changed nothing: the fresh target still takes a copy.
    REQUIRE(ith::copyWorld(source, target));
    CHECK(target.isAlive(inCopy(entity, target)));
}
