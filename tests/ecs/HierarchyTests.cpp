// The hierarchy, enabled state and transform propagation (ADR-0014 rules 12 to 14, ADR-0031):
// ChildOf and the ordered Children and roots, placeBefore, keeping the world placement on
// reparent and detach, the despawn policy and despawnRecursive, invalid parents, Disabled and
// DisabledInHierarchy with the queries that skip them, and propagation as a function and a system.

#include "ith/ecs/Hierarchy.h"

#include "ith/ecs/Hooks.h"
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::ChildOf;
using ith::Children;
using ith::Commands;
using ith::Disabled;
using ith::DisabledInHierarchy;
using ith::Entity;
using ith::LocalTransform;
using ith::Mat4;
using ith::Transform;
using ith::Vec3;
using ith::World;
using ith::WorldTransform;
using ith::test::DespawnLog;
using ith::test::DespawnProbe;
using ith::test::Health;

std::vector<Entity> childrenOf(const World& world, Entity parent) {
    const Children* children = world.get<const Children>(parent);
    return children != nullptr ? children->entities : std::vector<Entity>{};
}

std::vector<Entity> roots(const World& world) {
    std::vector<Entity> result;
    world.forEachRoot([&](Entity root) { result.push_back(root); });
    return result;
}

bool sameMatrix(const Mat4& a, const Mat4& b) {
    for (int column = 0; column < 4; ++column) {
        if (!glm::all(glm::epsilonEqual(a[column], b[column], 1e-4f))) {
            return false;
        }
    }
    return true;
}

Mat4 worldOf(World& world, Entity entity) {
    ith::propagateTransforms(world);
    return world.get<const WorldTransform>(entity)->matrix;
}

// A placed entity: LocalTransform and WorldTransform.
LocalTransform at(Vec3 translation, float turn = 0.0f, Vec3 scale = Vec3(1.0f)) {
    return {.value = {
                .translation = translation, .rotation = glm::angleAxis(turn, Vec3(0.0f, 1.0f, 0.0f)), .scale = scale}};
}

void setProbeHooks() {
    static const bool kSet = ith::setComponentHooks<DespawnProbe>({
        .onRemove =
            [](ith::HookContext& context) {
                if (auto* log = context.resource<DespawnLog>()) {
                    log->ids.push_back(context.value<const DespawnProbe>()->id);
                    const ChildOf* link = context.get<const ChildOf>(context.entity());
                    log->parentAlive.push_back(link != nullptr && context.isAlive(link->parent) ? 1 : 0);
                }
            },
    });
    CHECK(kSet);
}

} // namespace

TEST_CASE("Hierarchy: children are kept in order on their parent, roots in order on the world") {
    World world;
    const Entity parent = world.spawn(LocalTransform{});
    const Entity first = world.spawn(LocalTransform{}, ChildOf{.parent = parent});
    const Entity second = world.spawn(ChildOf{.parent = parent}); // no transform
    const Entity other = world.spawn();
    const Entity third = world.spawn(LocalTransform{});
    REQUIRE(world.add(third, ChildOf{.parent = parent}) != nullptr);

    CHECK(childrenOf(world, parent) == std::vector{first, second, third});
    CHECK(roots(world) == std::vector{parent, other});
    CHECK(world.get<const ChildOf>(second)->parent == parent);

    // placeBefore reorders siblings, and roots.
    CHECK(world.placeBefore(third, first));
    CHECK(childrenOf(world, parent) == std::vector{third, first, second});
    CHECK(world.placeBefore(third, Entity()));
    CHECK(childrenOf(world, parent) == std::vector{first, second, third});
    CHECK(world.placeBefore(other, parent));
    CHECK(roots(world) == std::vector{other, parent});

    // Detaching appends to the roots; reparenting appends to the new parent's children.
    CHECK(world.remove<ChildOf>(first));
    CHECK(roots(world) == std::vector{other, parent, first});
    CHECK(childrenOf(world, parent) == std::vector{second, third});
    REQUIRE(world.add(second, ChildOf{.parent = other}) != nullptr);
    CHECK(childrenOf(world, parent) == std::vector{third});
    CHECK(childrenOf(world, other) == std::vector{second});

    // Replacing ChildOf with the same parent keeps the place.
    REQUIRE(world.add(first, ChildOf{.parent = parent}) != nullptr);
    CHECK(world.placeBefore(first, third));
    REQUIRE(world.add(first, ChildOf{.parent = parent, .despawnWithParent = true}) != nullptr);
    CHECK(childrenOf(world, parent) == std::vector{first, third});

    // Through commands, too.
    Commands commands(world);
    commands.placeBefore(third, first);
    const Entity spawned = commands.spawn(ChildOf{.parent = parent});
    world.apply(commands);
    CHECK(childrenOf(world, parent) == std::vector{third, first, spawned});
}

TEST_CASE("Hierarchy: placeBefore refuses a sibling that isn't one") {
    World world;
    const Entity parent = world.spawn();
    const Entity child = world.spawn(ChildOf{.parent = parent});
    const Entity root = world.spawn();
    const ith::test::AssertCapture capture;
    CHECK_FALSE(world.placeBefore(child, root));  // another parent
    CHECK_FALSE(world.placeBefore(child, child)); // itself
    CHECK_FALSE(world.placeBefore(root, child));
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 3);
#endif
    const Entity gone = world.spawn();
    world.despawn(gone);
    CHECK_FALSE(world.placeBefore(gone, root)); // a stale entity: nothing, no assert
    CHECK_FALSE(world.placeBefore(root, gone)); // a stale sibling, as a command would meet it
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == 3);
#endif
    CHECK(roots(world) == std::vector{parent, root});
}

TEST_CASE("Hierarchy: reparenting and detaching keep the world placement") {
    World world;
    const Entity parent = world.spawn(at(Vec3(10.0f, 0.0f, 0.0f), ith::kHalfPi, Vec3(2.0f)), WorldTransform{});
    const Entity other = world.spawn(at(Vec3(0.0f, 5.0f, 0.0f), 0.3f), WorldTransform{});
    const Entity entity = world.spawn(at(Vec3(1.0f, 2.0f, 3.0f), 0.5f), WorldTransform{});
    const Mat4 placed = worldOf(world, entity);

    REQUIRE(world.add(entity, ChildOf{.parent = parent}) != nullptr);
    CHECK(sameMatrix(worldOf(world, entity), placed));
    // The local transform is now relative to the parent: scaled by a half, turned back.
    CHECK(world.get<const LocalTransform>(entity)->value.scale.x == doctest::Approx(0.5f));

    REQUIRE(world.add(entity, ChildOf{.parent = other}) != nullptr);
    CHECK(sameMatrix(worldOf(world, entity), placed));

    CHECK(world.remove<ChildOf>(entity));
    CHECK(sameMatrix(worldOf(world, entity), placed));
    CHECK(sameMatrix(ith::toMatrix(world.get<const LocalTransform>(entity)->value), placed));

    // Moving the parent moves the child.
    REQUIRE(world.add(entity, ChildOf{.parent = parent}) != nullptr);
    world.get<LocalTransform>(parent)->value.translation.y = 4.0f;
    CHECK(worldOf(world, entity)[3].y == doctest::Approx(placed[3].y + 4.0f));
}

TEST_CASE("Hierarchy: a spawned child keeps its local transform, and adding it after the link keeps it") {
    World world;
    const Entity parent = world.spawn(at(Vec3(10.0f, 0.0f, 0.0f)), WorldTransform{});
    const Entity child = world.spawn(at(Vec3(1.0f, 0.0f, 0.0f)), WorldTransform{}, ChildOf{.parent = parent});
    CHECK(worldOf(world, child)[3].x == doctest::Approx(11.0f));

    // Keeping the local transform on reparent: set it after the ChildOf, in the same buffer.
    const Entity root = world.spawn(at(Vec3(3.0f, 0.0f, 0.0f)), WorldTransform{});
    Commands commands(world);
    commands.add(root, ChildOf{.parent = parent});
    commands.add(root, at(Vec3(3.0f, 0.0f, 0.0f)));
    world.apply(commands);
    CHECK(worldOf(world, root)[3].x == doctest::Approx(13.0f));
}

TEST_CASE("Hierarchy: despawning a parent despawns its parts first and detaches the other children in place") {
    setProbeHooks();
    World world;
    world.insertResource(DespawnLog{});
    const Entity parent = world.spawn(at(Vec3(10.0f, 0.0f, 0.0f)), DespawnProbe{.id = 1});
    const Entity part = world.spawn(DespawnProbe{.id = 2}, ChildOf{.parent = parent, .despawnWithParent = true});
    const Entity partOfPart = world.spawn(DespawnProbe{.id = 3}, ChildOf{.parent = part, .despawnWithParent = true});
    const Entity looseBelowPart = world.spawn(at(Vec3(0.0f, 1.0f, 0.0f)), WorldTransform{}, ChildOf{.parent = part});
    const Entity loose = world.spawn(at(Vec3(1.0f, 0.0f, 0.0f)), WorldTransform{}, ChildOf{.parent = parent});
    const Mat4 looseWorld = worldOf(world, loose);
    const Mat4 belowWorld = worldOf(world, looseBelowPart);

    CHECK(world.despawn(parent));
    CHECK_FALSE(world.isAlive(parent));
    CHECK_FALSE(world.isAlive(part));
    CHECK_FALSE(world.isAlive(partOfPart));
    // Children before parents, each while its parent was still alive.
    const DespawnLog& log = *world.resource<const DespawnLog>();
    CHECK(log.ids == std::vector<std::int32_t>{3, 2, 1});
    CHECK(log.parentAlive == std::vector<std::int32_t>{1, 1, 0});
    // The others are roots where they were.
    CHECK(world.isAlive(loose));
    CHECK(world.isAlive(looseBelowPart));
    CHECK_FALSE(world.has<ChildOf>(loose));
    CHECK_FALSE(world.has<ChildOf>(looseBelowPart));
    CHECK(sameMatrix(worldOf(world, loose), looseWorld));
    CHECK(sameMatrix(worldOf(world, looseBelowPart), belowWorld));
    const std::vector<Entity> rootList = roots(world);
    REQUIRE(rootList.size() == 2);
    CHECK(std::set{rootList[0].bits(), rootList[1].bits()} == std::set{loose.bits(), looseBelowPart.bits()});
}

TEST_CASE("Hierarchy: despawnRecursive removes the whole subtree, children first") {
    setProbeHooks();
    World world;
    world.insertResource(DespawnLog{});
    const Entity keep = world.spawn();
    const Entity top = world.spawn(DespawnProbe{.id = 1});
    const Entity middle = world.spawn(DespawnProbe{.id = 2}, ChildOf{.parent = top});
    world.spawn(DespawnProbe{.id = 3}, ChildOf{.parent = middle});
    world.spawn(DespawnProbe{.id = 4}, ChildOf{.parent = top});
    const Entity sibling = world.spawn(ChildOf{.parent = keep});

    CHECK(world.despawnRecursive(middle));
    CHECK(world.resource<const DespawnLog>()->ids == std::vector<std::int32_t>{3, 2});
    CHECK(childrenOf(world, top).size() == 1);

    Commands commands(world);
    commands.despawnRecursive(top);
    world.apply(commands);
    CHECK(world.entityCount() == 2);
    CHECK(world.isAlive(sibling));
    CHECK(roots(world) == std::vector{keep});
    CHECK_FALSE(world.despawnRecursive(top)); // stale
}

TEST_CASE("Hierarchy: an invalid parent asserts and nothing is stored") {
    World world;
    World foreign;
    const Entity root = world.spawn();
    const Entity child = world.spawn(ChildOf{.parent = root});
    const Entity grandchild = world.spawn(ChildOf{.parent = child});
    const Entity dead = world.spawn();
    world.despawn(dead);
    const Entity stranger = foreign.spawn();

    const ith::test::AssertCapture capture;
    CHECK(world.add(root, ChildOf{.parent = root}) == nullptr);       // itself
    CHECK(world.add(root, ChildOf{.parent = grandchild}) == nullptr); // a descendant
    CHECK(world.add(child, ChildOf{.parent = dead}) == nullptr);      // dead
    CHECK(world.add(child, ChildOf{}) == nullptr);                    // none
    CHECK(world.add(child, ith::typeOf<ChildOf>()) == nullptr);       // by record: no parent
    ChildOf loop{.parent = grandchild};
    CHECK(world.add(root, ith::typeOf<ChildOf>(), &loop) == nullptr);
    CHECK(world.spawn(ChildOf{.parent = dead}).isNull());
    int asserts = 7;
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == asserts);
#endif
    CHECK(world.add(child, ChildOf{.parent = stranger}) == nullptr); // another world
    asserts += 1;
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == asserts);
#endif

    Commands commands(world);
    commands.add(root, ChildOf{.parent = child});
    const Entity refused = commands.spawn(Health{}, ChildOf{.parent = dead});
    world.apply(commands);
    asserts += 2;
#ifdef ITH_ENABLE_ASSERTS
    CHECK(capture.count() == asserts);
#endif
    CHECK_FALSE(world.isAlive(refused));
    CHECK_FALSE(world.has<ChildOf>(root));
    CHECK(world.get<const ChildOf>(child)->parent == root);
    CHECK(world.entityCount() == 3);
    CHECK(roots(world) == std::vector{root});
}

TEST_CASE("Hierarchy: Disabled turns off the entity and everything below it for queries") {
    World world;
    const Entity top = world.spawn(Health{.value = 1});
    const Entity middle = world.spawn(Health{.value = 2}, ChildOf{.parent = top});
    const Entity bottom = world.spawn(Health{.value = 3}, ChildOf{.parent = middle});
    const Entity apart = world.spawn(Health{.value = 4});

    const auto visible = [&] {
        std::set<std::uint64_t> seen;
        world.query<const Health>().forEach([&](Entity entity, const Health&) { seen.insert(entity.bits()); });
        return seen;
    };
    CHECK(visible().size() == 4);

    REQUIRE(world.add(middle, Disabled{}) != nullptr);
    CHECK_FALSE(world.has<DisabledInHierarchy>(top));
    CHECK(world.has<DisabledInHierarchy>(middle));
    CHECK(world.has<DisabledInHierarchy>(bottom));
    CHECK(visible() == std::set{top.bits(), apart.bits()});

    // Asking for them: IncludeDisabled, or naming Disabled or DisabledInHierarchy.
    int all = 0;
    world.query<const Health, ith::IncludeDisabled>().forEach([&](const Health&) { ++all; });
    CHECK(all == 4);
    int below = 0;
    world.query<const Health, ith::With<DisabledInHierarchy>>().forEach([&](const Health&) { ++below; });
    CHECK(below == 2);
    int own = 0;
    world.query<ith::With<Disabled>>().forEach([&](Entity entity) {
        CHECK(entity == middle);
        ++own;
    });
    CHECK(own == 1);
    const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Health>()}};
    int dynamicEnabled = 0;
    world.dynamicQuery(terms).forEach([&](Entity, std::span<void* const>) { ++dynamicEnabled; });
    CHECK(dynamicEnabled == 2);
    const std::array include{ith::QueryFilter{.kind = ith::FilterKind::IncludeDisabled}};
    int everyEntity = 0;
    world.dynamicQuery({}, include).forEach([&](Entity, std::span<void* const>) { ++everyEntity; });
    CHECK(everyEntity == 4);

    // Disabling above a disabled entity, then enabling the lower one, keeps it off.
    REQUIRE(world.add(top, Disabled{}) != nullptr);
    CHECK(world.remove<Disabled>(middle));
    CHECK(world.has<DisabledInHierarchy>(middle));
    CHECK(world.has<DisabledInHierarchy>(bottom));
    CHECK(world.remove<Disabled>(top));
    CHECK(visible().size() == 4);

    // Moving under a disabled parent, and away from it.
    REQUIRE(world.add(apart, Disabled{}) != nullptr);
    REQUIRE(world.add(middle, ChildOf{.parent = apart}) != nullptr);
    CHECK(world.has<DisabledInHierarchy>(middle));
    CHECK(world.has<DisabledInHierarchy>(bottom));
    CHECK(world.remove<ChildOf>(middle));
    CHECK_FALSE(world.has<DisabledInHierarchy>(middle));
    CHECK_FALSE(world.has<DisabledInHierarchy>(bottom));
    const Entity spawned = world.spawn(ChildOf{.parent = apart});
    CHECK(world.has<DisabledInHierarchy>(spawned));

    // Despawning a disabled parent detaches its children, which are enabled again.
    world.despawn(apart);
    CHECK_FALSE(world.has<DisabledInHierarchy>(spawned));
}

TEST_CASE("Hierarchy: propagation composes world = parent * local, as a function and as a system") {
    World world;
    const Entity root = world.spawn(at(Vec3(1.0f, 0.0f, 0.0f), ith::kHalfPi), WorldTransform{});
    const Entity gap = world.spawn(ChildOf{.parent = root}); // no transform: identity
    const Entity leaf =
        world.spawn(at(Vec3(0.0f, 0.0f, -2.0f), 0.0f, Vec3(3.0f)), WorldTransform{}, ChildOf{.parent = gap});
    const Entity unplaced = world.spawn(WorldTransform{}, ChildOf{.parent = leaf});
    world.add(leaf, Disabled{}); // disabled entities are propagated too

    ith::Schedule schedule;
    schedule.add(ith::transformPropagationSystem());
    REQUIRE(schedule.build());
    const std::vector<std::string_view> order = schedule.order(ith::Stage::PostUpdate);
    REQUIRE(order.size() == 1);
    CHECK(std::string(order[0]) == "ith::propagate_transforms");
    schedule.run(world);

    const Mat4 rootWorld = ith::toMatrix(world.get<const LocalTransform>(root)->value);
    const Mat4 leafWorld = rootWorld * ith::toMatrix(world.get<const LocalTransform>(leaf)->value);
    CHECK(sameMatrix(world.get<const WorldTransform>(root)->matrix, rootWorld));
    CHECK(sameMatrix(world.get<const WorldTransform>(leaf)->matrix, leafWorld));
    CHECK(sameMatrix(world.get<const WorldTransform>(unplaced)->matrix, leafWorld));
    // The leaf at (0, 0, -2) under a quarter turn about +Y lands at (1 - 2, 0, 0).
    CHECK(world.get<const WorldTransform>(leaf)->matrix[3].x == doctest::Approx(-1.0f));
}

TEST_CASE("Hierarchy: clear empties the roots, and entities spawned afterwards are roots again") {
    World world;
    const Entity parent = world.spawn();
    world.spawn(ChildOf{.parent = parent});
    world.add(parent, Disabled{});
    world.clear();
    CHECK(roots(world).empty());
    const Entity fresh = world.spawn();
    CHECK(roots(world) == std::vector{fresh});
}
