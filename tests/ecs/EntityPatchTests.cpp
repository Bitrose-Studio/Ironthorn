// World::applyPatches (ADR-0033): checks before any change, mutable components written in place
// without hooks, immutable ones and adds replaced through add with hooks, removes, inverses that
// undo, the hierarchy and stable ids, and all-or-nothing failure across entities.

#include "ith/ecs/EntityPatch.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::ChildOf;
using ith::Children;
using ith::ComponentChange;
using ith::Entity;
using ith::EntityPatch;
using ith::LocalTransform;
using ith::Mat4;
using ith::OwnedValue;
using ith::Patch;
using ith::PropertyPath;
using ith::StableId;
using ith::Uuid;
using ith::Vec3;
using ith::World;
using ith::WorldTransform;
using ith::test::Health;
using ith::test::Inventory;
using ith::test::PatchCounted;
using ith::test::PatchFrozen;
using ith::test::Position;
using ith::test::Velocity;

// How many times one failed precondition asserts: none when asserts are compiled out.
#ifdef ITH_ENABLE_ASSERTS
constexpr int kAsserts = 1;
#else
constexpr int kAsserts = 0;
#endif

// The hooks of PatchCounted and PatchFrozen, counted.
struct HookCalls {
    int inserts = 0;
    int replaces = 0;
    int removes = 0;
};

HookCalls g_calls;

void setPatchHooks() {
    static const bool kSet = [] {
        const ith::ComponentHooks counting{
            .onInsert = [](ith::HookContext&) { ++g_calls.inserts; },
            .onReplace = [](ith::HookContext&) { ++g_calls.replaces; },
            .onRemove = [](ith::HookContext&) { ++g_calls.removes; },
        };
        return ith::setComponentHooks<PatchCounted>(counting) && ith::setComponentHooks<PatchFrozen>(counting);
    }();
    CHECK(kSet);
    g_calls = {};
}

template <typename Message>
std::string messageOf(const Message& result) {
    return result.hasValue() ? std::string() : result.error().message;
}

template <typename T>
PropertyPath path(std::string_view text) {
    ith::Expected<PropertyPath> parsed = PropertyPath::parse(ith::typeOf<T>(), text);
    REQUIRE_MESSAGE(parsed.hasValue(), messageOf(parsed));
    return *parsed;
}

// A change that sets one value of a T.
template <typename T, typename Value>
ComponentChange setting(std::string_view text, Value value) {
    Patch patch(ith::typeOf<T>());
    patch.set(path<T>(text), OwnedValue::of(std::move(value)));
    return ComponentChange::makePatch(std::move(patch));
}

EntityPatch patchOf(Entity entity, std::vector<ComponentChange> changes) {
    return EntityPatch{.entity = entity, .changes = std::move(changes)};
}

std::vector<EntityPatch> apply(World& world, const std::vector<EntityPatch>& patches) {
    ith::Expected<std::vector<EntityPatch>> undo = world.applyPatches(patches);
    REQUIRE_MESSAGE(undo.hasValue(), messageOf(undo));
    return std::move(*undo);
}

void checkFails(World& world, const std::vector<EntityPatch>& patches, std::string_view code, std::string_view part) {
    const ith::Expected<std::vector<EntityPatch>> result = world.applyPatches(patches);
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == code);
    CAPTURE(result.error().message);
    CHECK(result.error().message.find(part) != std::string::npos);
}

std::vector<Entity> childrenOf(const World& world, Entity parent) {
    const Children* children = world.get<const Children>(parent);
    return children != nullptr ? children->entities : std::vector<Entity>();
}

LocalTransform at(Vec3 translation) {
    return {.value = {.translation = translation}};
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

// A schedule whose system records the entities whose Health changed since its last run.
struct ChangeWatch {
    std::vector<Entity> changed;
    ith::Schedule schedule;

    ChangeWatch() {
        schedule.add({
            .name = "test::watch_health",
            .stage = ith::Stage::PostUpdate,
            .access = ith::accessTo<const Health>(),
            .run =
                [this](ith::SystemContext& context) {
                    changed.clear();
                    context.query<const Health, ith::Changed<Health>>().forEach(
                        [&](Entity entity, const Health&) { changed.push_back(entity); });
                },
        });
        REQUIRE(schedule.build());
    }
};

} // namespace

TEST_SUITE("ecs.entity_patch") {
    TEST_CASE("a mutable component is written in place: changed tick stamped, no hooks") {
        setPatchHooks();
        World world;
        ChangeWatch watch;
        const Entity entity = world.spawn(Health{.value = 10}, PatchCounted{.value = 1});
        const Entity other = world.spawn(Health{});
        watch.schedule.run(world);
        watch.schedule.run(world);
        CHECK(watch.changed.empty());
        const HookCalls afterSpawn = g_calls;

        const std::vector<EntityPatch> undo =
            apply(world, {patchOf(entity, {setting<Health>("value", std::int32_t{20}),
                                           setting<PatchCounted>("value", std::int32_t{3})})});
        CHECK(world.get<const Health>(entity)->value == 20);
        CHECK(world.get<const PatchCounted>(entity)->value == 3);
        CHECK(g_calls.inserts == afterSpawn.inserts);
        CHECK(g_calls.replaces == afterSpawn.replaces);
        watch.schedule.run(world);
        CHECK(watch.changed == std::vector<Entity>{entity});

        REQUIRE(undo.size() == 1);
        CHECK(undo[0].entity == entity);
        REQUIRE(undo[0].changes.size() == 2);
        CHECK(undo[0].changes[0].type->id == ith::typeOf<PatchCounted>().id);
        const std::vector<EntityPatch> redo = apply(world, undo);
        CHECK(world.get<const Health>(entity)->value == 10);
        CHECK(world.get<const PatchCounted>(entity)->value == 1);
        CHECK(world.get<const Health>(other)->value == 100);
        CHECK(g_calls.replaces == afterSpawn.replaces);

        apply(world, redo);
        CHECK(world.get<const Health>(entity)->value == 20);
    }

    TEST_CASE("an immutable component is replaced through add, with its hooks") {
        setPatchHooks();
        World world;
        const Entity entity = world.spawn(PatchFrozen{.value = 1});
        const HookCalls afterSpawn = g_calls;
        const std::vector<EntityPatch> undo =
            apply(world, {patchOf(entity, {setting<PatchFrozen>("value", std::int32_t{2})})});
        CHECK(world.get<const PatchFrozen>(entity)->value == 2);
        CHECK(g_calls.replaces == afterSpawn.replaces + 1);
        CHECK(g_calls.inserts == afterSpawn.inserts + 1);
        apply(world, undo);
        CHECK(world.get<const PatchFrozen>(entity)->value == 1);
        CHECK(g_calls.replaces == afterSpawn.replaces + 2);
    }

    TEST_CASE("a StableId patch keeps the lookup") {
        World world;
        const Uuid first(1, 1);
        const Uuid second(2, 2);
        const Entity entity = world.spawn(StableId{.id = first});
        const std::vector<EntityPatch> undo = apply(world, {patchOf(entity, {setting<StableId>("id", second)})});
        CHECK(world.findByStableId(second) == entity);
        CHECK(world.findByStableId(first).isNull());
        apply(world, undo);
        CHECK(world.findByStableId(first) == entity);
        CHECK(world.findByStableId(second).isNull());
    }

    TEST_CASE("adds and removes, and the inverse that undoes them") {
        setPatchHooks();
        World world;
        const Entity entity = world.spawn(Position{.x = 1.0f}, Health{.value = 7}, PatchCounted{.value = 4});
        const HookCalls afterSpawn = g_calls;
        const std::vector<EntityPatch> undo =
            apply(world, {patchOf(entity, {ComponentChange::makeAdd(OwnedValue::of(Velocity{.x = 2.0f})),
                                           ComponentChange::makeAdd(OwnedValue::of(Position{.x = 5.0f})),
                                           ComponentChange::makeRemove(ith::typeOf<Health>()),
                                           ComponentChange::makeAdd(OwnedValue::of(PatchCounted{.value = 8}))})});
        CHECK(world.get<const Velocity>(entity)->x == 2.0f);
        CHECK(world.get<const Position>(entity)->x == 5.0f);
        CHECK_FALSE(world.has<Health>(entity));
        CHECK(world.get<const PatchCounted>(entity)->value == 8);
        CHECK(g_calls.replaces == afterSpawn.replaces + 1);

        REQUIRE(undo.size() == 1);
        REQUIRE(undo[0].changes.size() == 4);
        CHECK(undo[0].changes[0].kind == ComponentChange::Kind::Add);
        CHECK(undo[0].changes[3].kind == ComponentChange::Kind::Remove);
        apply(world, undo);
        CHECK_FALSE(world.has<Velocity>(entity));
        CHECK(world.get<const Position>(entity)->x == 1.0f);
        CHECK(world.get<const Health>(entity)->value == 7);
        CHECK(world.get<const PatchCounted>(entity)->value == 4);
    }

    TEST_CASE("each change sees the changes before it") {
        World world;
        const Entity entity = world.spawn(Position{});
        Patch insert(ith::typeOf<Inventory>());
        insert.insert(path<Inventory>("items[0]"), OwnedValue::of(std::string("rope")));
        insert.insert(path<Inventory>("items[1]"), OwnedValue::of(std::string("lamp")));
        const std::vector<EntityPatch> undo =
            apply(world, {patchOf(entity, {ComponentChange::makeAdd(OwnedValue::of(Inventory{})),
                                           ComponentChange::makePatch(std::move(insert)),
                                           setting<Inventory>("items[0]", std::string("map")),
                                           ComponentChange::makeRemove(ith::typeOf<Position>()),
                                           ComponentChange::makeAdd(OwnedValue::of(Position{.y = 3.0f}))})});
        CHECK(world.get<const Inventory>(entity)->items == std::vector<std::string>{"map", "lamp"});
        CHECK(world.get<const Position>(entity)->y == 3.0f);
        apply(world, undo);
        CHECK_FALSE(world.has<Inventory>(entity));
        CHECK(world.get<const Position>(entity)->y == 0.0f);
    }

    TEST_CASE("replacing an immutable ChildOf reparents, and undo restores the hierarchy") {
        World world;
        const Entity first = world.spawn(at(Vec3(10.0f, 0.0f, 0.0f)), WorldTransform{});
        const Entity second = world.spawn(at(Vec3(0.0f, 5.0f, 0.0f)), WorldTransform{});
        const Entity sibling = world.spawn(at(Vec3(1.0f, 0.0f, 0.0f)), WorldTransform{}, ChildOf{.parent = first});
        const Entity child = world.spawn(at(Vec3(2.0f, 0.0f, 0.0f)), WorldTransform{}, ChildOf{.parent = first});
        const Mat4 placement = worldOf(world, child);
        CHECK(childrenOf(world, first) == std::vector<Entity>{sibling, child});

        const std::vector<EntityPatch> undo = apply(world, {patchOf(child, {setting<ChildOf>("parent", second)})});
        CHECK(world.get<const ChildOf>(child)->parent == second);
        CHECK(childrenOf(world, first) == std::vector<Entity>{sibling});
        CHECK(childrenOf(world, second) == std::vector<Entity>{child});
        CHECK(sameMatrix(worldOf(world, child), placement)); // reparenting keeps the world placement

        apply(world, undo);
        CHECK(world.get<const ChildOf>(child)->parent == first);
        CHECK(childrenOf(world, first) == std::vector<Entity>{sibling, child});
        CHECK(childrenOf(world, second).empty());
        CHECK(sameMatrix(worldOf(world, child), placement));
        CHECK(glm::all(glm::epsilonEqual(world.get<const LocalTransform>(child)->value.translation,
                                         Vec3(2.0f, 0.0f, 0.0f), 1e-4f)));
    }

    TEST_CASE("values add would refuse are refused before anything changes") {
        World world;
        const Entity root = world.spawn(Position{});
        const Entity other = world.spawn(Position{});
        const Entity child = world.spawn(ChildOf{.parent = root});
        const std::string_view refused = "ecs.patch.refused_value";

        checkFails(world, {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = root}))})}, refused,
                   "itself or one of its descendants");
        checkFails(world, {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = child}))})},
                   refused, "itself or one of its descendants");
        checkFails(world, {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(ChildOf{}))})}, refused,
                   "live entity of the same world");
        // A cycle made by two changes, each fine on its own.
        checkFails(world,
                   {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = other}))}),
                    patchOf(other, {ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = child}))})},
                   refused, "entity patch 1, change 0: ");
        CHECK_FALSE(world.has<ChildOf>(root));
        CHECK_FALSE(world.has<ChildOf>(other));

        const Uuid a(1, 1);
        const Uuid b(2, 2);
        const Entity named = world.spawn(StableId{.id = a});
        checkFails(world, {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(StableId{}))})}, refused, "nil");
        checkFails(world, {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(StableId{.id = a}))})}, refused,
                   "already has StableId");
        checkFails(world,
                   {patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(StableId{.id = b}))}),
                    patchOf(other, {ComponentChange::makeAdd(OwnedValue::of(StableId{.id = b}))})},
                   refused, "entity patch 1");
        CHECK_FALSE(world.has<StableId>(root));

        // Moving an id: remove it from its owner first, in the same step.
        apply(world, {patchOf(named, {ComponentChange::makeRemove(ith::typeOf<StableId>())}),
                      patchOf(root, {ComponentChange::makeAdd(OwnedValue::of(StableId{.id = a}))})});
        CHECK(world.findByStableId(a) == root);
    }

    TEST_CASE("errors name the entity patch, the change and the type") {
        World world;
        const Entity entity = world.spawn(Position{}, Inventory{.items = {"a"}});
        const Entity parent = world.spawn(Position{});
        world.spawn(ChildOf{.parent = parent});
        const Entity gone = world.spawn(Position{});
        world.despawn(gone);

        checkFails(world, {patchOf(gone, {})}, "ecs.patch.stale_entity", "entity patch 0");
        checkFails(world, {patchOf(Entity(), {})}, "ecs.patch.stale_entity", "not alive");
        checkFails(world, {patchOf(entity, {ComponentChange{}})}, "ecs.patch.not_a_component", "has no type");
        checkFails(world, {patchOf(entity, {ComponentChange::makeAdd(OwnedValue::of(ith::test::HookLog{}))})},
                   "ecs.patch.not_a_component", "ith::test::HookLog is not a component type");
        checkFails(world, {patchOf(parent, {ComponentChange::makeRemove(ith::typeOf<Children>())})},
                   "ecs.patch.world_owned", "ith::Children is kept by the World");
        checkFails(world, {patchOf(entity, {setting<Velocity>("x", 1.0f)})}, "ecs.patch.missing_component",
                   "no ith::test::Velocity to patch");
        checkFails(world, {patchOf(entity, {ComponentChange::makeRemove(ith::typeOf<Velocity>())})},
                   "ecs.patch.missing_component", "to remove");

        ComponentChange mismatched = setting<Velocity>("x", 1.0f);
        mismatched.type = &ith::typeOf<Position>();
        checkFails(world, {patchOf(entity, {std::move(mismatched)})}, "reflection.patch.wrong_type",
                   "not a patch of ith::test::Position");
        ComponentChange wrongValue = ComponentChange::makeAdd(OwnedValue::of(Velocity{}));
        wrongValue.type = &ith::typeOf<Position>();
        checkFails(world, {patchOf(entity, {std::move(wrongValue)})}, "reflection.patch.wrong_type",
                   "the value is not a ith::test::Position");

        Patch beyond(ith::typeOf<Inventory>());
        beyond.erase(path<Inventory>("items[3]"));
        checkFails(world, {patchOf(parent, {}), patchOf(entity, {ComponentChange::makePatch(std::move(beyond))})},
                   "reflection.path.index_out_of_range", "entity patch 1, change 0: entry 0: ");
    }

    TEST_CASE("a failure in a later entity patch changes nothing, also for the earlier ones") {
        setPatchHooks();
        World world;
        const Entity first = world.spawn(Health{.value = 1}, PatchFrozen{.value = 1});
        const Entity second = world.spawn(Health{.value = 2});
        const HookCalls before = g_calls;
        checkFails(world,
                   {patchOf(first, {setting<Health>("value", std::int32_t{10}),
                                    setting<PatchFrozen>("value", std::int32_t{10}),
                                    ComponentChange::makeAdd(OwnedValue::of(Position{}))}),
                    patchOf(second, {setting<Health>("value", std::int32_t{20}),
                                     ComponentChange::makeRemove(ith::typeOf<Velocity>())})},
                   "ecs.patch.missing_component", "entity patch 1, change 1: ");
        CHECK(world.get<const Health>(first)->value == 1);
        CHECK(world.get<const PatchFrozen>(first)->value == 1);
        CHECK_FALSE(world.has<Position>(first));
        CHECK(world.get<const Health>(second)->value == 2);
        CHECK(g_calls.replaces == before.replaces);
        CHECK(g_calls.inserts == before.inserts);
    }

    TEST_CASE("several entities in one step, undone in reverse order") {
        World world;
        const Entity first = world.spawn(Health{.value = 1});
        const Entity second = world.spawn(Health{.value = 2});
        const std::vector<EntityPatch> undo =
            apply(world, {patchOf(first, {setting<Health>("value", std::int32_t{10})}),
                          patchOf(second, {setting<Health>("value", std::int32_t{20})}),
                          patchOf(first, {setting<Health>("value", std::int32_t{11})})});
        CHECK(world.get<const Health>(first)->value == 11);
        CHECK(world.get<const Health>(second)->value == 20);
        REQUIRE(undo.size() == 3);
        CHECK(undo[0].entity == first);
        CHECK(undo[1].entity == second);
        apply(world, undo);
        CHECK(world.get<const Health>(first)->value == 1);
        CHECK(world.get<const Health>(second)->value == 2);
        CHECK(apply(world, {}).empty());
    }

    TEST_CASE("refused while a query iterates") {
        World world;
        const Entity entity = world.spawn(Health{});
        ith::test::AssertCapture capture;
        world.query<const Health>().forEach([&](Entity, const Health&) {
            const ith::Expected<std::vector<EntityPatch>> result =
                world.applyPatches(std::vector{patchOf(entity, {setting<Health>("value", std::int32_t{5})})});
            REQUIRE_FALSE(result.hasValue());
            CHECK(result.error().code == "ecs.patch.iterating");
        });
        CHECK(capture.count() == kAsserts);
        CHECK(world.get<const Health>(entity)->value == 100);
    }
}

TEST_CASE("applyPatches: 80,000 changes are planned in linear time") {
    // 20,000 entities, each with four changes: an add, a patch of the value just added, a new
    // StableId and a parent. Searching the earlier changes for each one would take minutes; the bound
    // is generous for Debug and sanitizer builds.
    constexpr std::size_t kEntities = 20'000;
    World world;
    const Entity parent = world.spawn(Health{});
    std::vector<EntityPatch> patches;
    patches.reserve(kEntities);
    for (std::size_t index = 0; index < kEntities; ++index) {
        const Entity entity = world.spawn(Health{});
        std::vector<ComponentChange> changes;
        changes.push_back(ComponentChange::makeAdd(OwnedValue::of(Position{.x = 1.0f})));
        changes.push_back(setting<Position>("y", static_cast<float>(index)));
        changes.push_back(ComponentChange::makeAdd(OwnedValue::of(StableId{.id = Uuid(0x5EED, index + 1)})));
        changes.push_back(ComponentChange::makeAdd(OwnedValue::of(ChildOf{.parent = parent})));
        patches.push_back(patchOf(entity, std::move(changes)));
    }

    const auto start = std::chrono::steady_clock::now();
    const ith::Expected<std::vector<EntityPatch>> undo = world.applyPatches(patches);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 60.0);
    REQUIRE(undo.hasValue());
    CHECK(undo->size() == kEntities);
    const Entity last = world.findByStableId(Uuid(0x5EED, kEntities));
    REQUIRE(world.get<const Position>(last) != nullptr);
    CHECK(world.get<const Position>(last)->y == static_cast<float>(kEntities - 1));
    CHECK(world.get<const Children>(parent)->entities.size() == kEntities);

    // A duplicate id among the changes is still found, at the end of a long batch.
    std::vector<EntityPatch> duplicate;
    duplicate.push_back(patchOf(parent, {ComponentChange::makeAdd(OwnedValue::of(StableId{.id = Uuid(0x5EED, 1)}))}));
    const auto refused = world.applyPatches(duplicate);
    REQUIRE_FALSE(refused.hasValue());
    CHECK(refused.error().code == "ecs.patch.refused_value");
}
