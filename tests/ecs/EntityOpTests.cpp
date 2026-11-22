// World::applyPatches with whole-entity operations (ADR-0037 rule 8): Spawns under placeholders,
// Despawns refused while children remain and undone by a Spawn in the old place, placements among
// siblings, all-or-nothing calls that mix them, and inverses that restore the side effects of hooks
// bit for bit, so a reparent undone and redone gives the same values each time.

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::ChildOf;
using ith::Children;
using ith::ComponentChange;
using ith::Entity;
using ith::EntityOp;
using ith::EntityPatch;
using ith::LocalTransform;
using ith::OwnedValue;
using ith::StableId;
using ith::Uuid;
using ith::Vec3;
using ith::World;
using ith::test::Follow;
using ith::test::Health;
using ith::test::Name;

template <typename Message>
std::string messageOf(const Message& result) {
    return result.hasValue() ? std::string() : result.error().message;
}

std::vector<EntityPatch> applyAll(World& world, const std::vector<EntityPatch>& patches) {
    ith::Expected<std::vector<EntityPatch>> undo = world.applyPatches(patches);
    REQUIRE_MESSAGE(undo.hasValue(), messageOf(undo));
    return std::move(*undo);
}

void checkFails(World& world, const std::vector<EntityPatch>& patches, std::string_view code) {
    const ith::Expected<std::vector<EntityPatch>> result = world.applyPatches(patches);
    REQUIRE_FALSE(result.hasValue());
    CAPTURE(result.error().message);
    CHECK(result.error().code == code);
}

Uuid idOf(std::uint64_t n) {
    return Uuid(0xE0, n);
}

Entity placeholder(std::uint32_t n) {
    return Entity::fromParts(n, 1, 0);
}

template <typename T>
ComponentChange adding(T value) {
    return ComponentChange::makeAdd(OwnedValue::of(std::move(value)));
}

template <typename T>
ComponentChange removing() {
    return ComponentChange::makeRemove(ith::typeOf<T>());
}

EntityPatch spawnOf(Entity slot, std::vector<ComponentChange> changes) {
    return EntityPatch{.op = EntityOp::Spawn, .entity = slot, .changes = std::move(changes)};
}

EntityPatch despawnOf(Entity entity) {
    return EntityPatch{.op = EntityOp::Despawn, .entity = entity};
}

EntityPatch modifyOf(Entity entity, std::vector<ComponentChange> changes) {
    return EntityPatch{.entity = entity, .changes = std::move(changes)};
}

// One entity of a snapshot: its stable id and its saved components, by type, as copies.
struct EntityState {
    Uuid id;
    Uuid parent;
    std::vector<OwnedValue> components;
};

// The world in Outliner order (roots, then each subtree depth-first), with every component except
// the Transient ones and the stable id; parents and Entity fields are compared by stable id.
struct Snapshot {
    std::vector<EntityState> entities;
};

Uuid stableIdOf(const World& world, Entity entity) {
    const StableId* id = world.get<const StableId>(entity);
    return id != nullptr ? id->id : Uuid();
}

void describe(const World& world, Entity entity, Snapshot& snapshot) {
    EntityState state{.id = stableIdOf(world, entity)};
    if (const ChildOf* link = world.get<const ChildOf>(entity)) {
        state.parent = stableIdOf(world, link->parent);
    }
    world.forEachComponent(entity, [&](const ith::TypeInfo& type, const void* value) {
        if (hasFlags(type.flags, ith::ReflectionFlags::Transient) || type.id == ith::typeOf<StableId>().id ||
            type.id == ith::typeOf<ChildOf>().id) {
            return;
        }
        state.components.push_back(OwnedValue::copyOf(type, value));
    });
    std::ranges::sort(state.components, {}, [](const OwnedValue& value) { return value.type()->id; });
    snapshot.entities.push_back(std::move(state));
    if (const Children* children = world.get<const Children>(entity)) {
        for (const Entity child : children->entities) {
            describe(world, child, snapshot);
        }
    }
}

Snapshot snapshotOf(const World& world) {
    Snapshot snapshot;
    world.forEachRoot([&](Entity root) { describe(world, root, snapshot); });
    return snapshot;
}

// True when two snapshots hold the same entities in the same order with equal values. Values with
// Entity fields (Follow) are compared through the stable id they reach.
bool same(const World& world, const Snapshot& a, const Snapshot& b) {
    if (a.entities.size() != b.entities.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.entities.size(); ++i) {
        const EntityState& left = a.entities[i];
        const EntityState& right = b.entities[i];
        if (left.id != right.id || left.parent != right.parent || left.components.size() != right.components.size()) {
            return false;
        }
        for (std::size_t c = 0; c < left.components.size(); ++c) {
            const OwnedValue& x = left.components[c];
            const OwnedValue& y = right.components[c];
            if (x.type()->id != y.type()->id) {
                return false;
            }
            if (const Follow* fx = x.as<Follow>()) {
                if (stableIdOf(world, fx->target) != stableIdOf(world, y.as<Follow>()->target)) {
                    return false;
                }
                continue;
            }
            if (!x.type()->equals(x.get(), y.get())) {
                return false;
            }
        }
    }
    return true;
}

// A transform whose reparenting goes through inexact math: rotation, non-uniform scale and an
// offset that don't decompose back to the same bits.
LocalTransform awkward(float seed) {
    return {.value = {.translation = Vec3(seed * 1.37f, -2.11f, 0.3f * seed),
                      .rotation = glm::angleAxis(0.7f + seed, glm::normalize(Vec3(0.3f, 1.0f, 0.2f))),
                      .scale = Vec3(1.3f, 0.7f, 2.1f)}};
}

std::size_t changeCount(const std::vector<EntityPatch>& patches) {
    std::size_t count = 0;
    for (const EntityPatch& patch : patches) {
        count += patch.changes.size();
    }
    return count;
}

} // namespace

TEST_SUITE("ecs.entity_patch.entity_ops") {
    TEST_CASE("a reparent is undone and redone bit for bit, and its inverse doesn't grow") {
        World world;
        const Entity a = world.spawn(StableId{.id = idOf(1)}, awkward(1.0f));
        const Entity b = world.spawn(StableId{.id = idOf(2)}, awkward(2.0f));
        const Entity first = world.spawn(StableId{.id = idOf(3)}, ChildOf{.parent = a}, awkward(3.0f));
        const Entity child = world.spawn(StableId{.id = idOf(4)}, ChildOf{.parent = a}, awkward(4.0f));
        const Entity last = world.spawn(StableId{.id = idOf(5)}, ChildOf{.parent = a}, awkward(5.0f));
        static_cast<void>(b);
        const Snapshot before = snapshotOf(world);

        std::vector<EntityPatch> undo = applyAll(world, {modifyOf(child, {adding(ChildOf{.parent = b})})});
        const Snapshot after = snapshotOf(world);
        CHECK_FALSE(same(world, before, after));
        const std::size_t size = changeCount(undo);

        for (int cycle = 0; cycle < 4; ++cycle) {
            std::vector<EntityPatch> redo = applyAll(world, undo);
            CHECK(same(world, snapshotOf(world), before));
            CHECK(world.get<const Children>(a)->entities == std::vector<Entity>{first, child, last});
            undo = applyAll(world, redo);
            CHECK(same(world, snapshotOf(world), after));
            CHECK(changeCount(undo) == size);
        }
    }

    TEST_CASE("a detach is undone into the old place among the siblings") {
        World world;
        const Entity parent = world.spawn(StableId{.id = idOf(1)}, awkward(1.0f));
        const Entity first = world.spawn(StableId{.id = idOf(2)}, ChildOf{.parent = parent}, awkward(2.0f));
        const Entity middle = world.spawn(StableId{.id = idOf(3)}, ChildOf{.parent = parent}, awkward(3.0f));
        const Entity last = world.spawn(StableId{.id = idOf(4)}, ChildOf{.parent = parent}, awkward(4.0f));
        const Snapshot before = snapshotOf(world);

        const std::vector<EntityPatch> undo = applyAll(world, {modifyOf(middle, {removing<ChildOf>()})});
        CHECK(world.get<const Children>(parent)->entities == std::vector<Entity>{first, last});
        const Snapshot after = snapshotOf(world);
        const std::vector<EntityPatch> redo = applyAll(world, undo);
        CHECK(world.get<const Children>(parent)->entities == std::vector<Entity>{first, middle, last});
        CHECK(same(world, snapshotOf(world), before));
        applyAll(world, redo);
        CHECK(same(world, snapshotOf(world), after));
    }

    TEST_CASE("placeholders name the call's new entities in entities, values and placements") {
        World world;
        const Entity existing = world.spawn(StableId{.id = idOf(9)});
        const Snapshot empty = snapshotOf(world);

        const std::vector<EntityPatch> undo = applyAll(
            world, {spawnOf(placeholder(0), {adding(StableId{.id = idOf(1)}), adding(Name{.text = "door"})}),
                    // ChildOf given last: the World adds it first anyway.
                    spawnOf(placeholder(1), {adding(Follow{.target = placeholder(0)}), adding(StableId{.id = idOf(2)}),
                                             adding(ChildOf{.parent = placeholder(0)})}),
                    modifyOf(placeholder(1), {adding(Health{.value = 3})}),
                    EntityPatch{.entity = existing, .placeBefore = placeholder(0)}});
        const Entity door = world.findByStableId(idOf(1));
        const Entity hinge = world.findByStableId(idOf(2));
        REQUIRE(world.isAlive(door));
        REQUIRE(world.isAlive(hinge));
        CHECK(world.get<const ChildOf>(hinge)->parent == door);
        CHECK(world.get<const Follow>(hinge)->target == door);
        CHECK(world.get<const Health>(hinge)->value == 3);
        CHECK(world.get<const Children>(door)->entities == std::vector<Entity>{hinge});
        std::vector<Entity> roots;
        world.forEachRoot([&](Entity root) { roots.push_back(root); });
        CHECK(roots == std::vector<Entity>{existing, door});

        // The inverse despawns them, children first, and puts the existing entity back.
        REQUIRE(undo.size() == 4);
        CHECK(undo[0].op == EntityOp::Modify); // the placement of `existing`
        CHECK(undo[2].op == EntityOp::Despawn);
        CHECK(undo[3].op == EntityOp::Despawn);
        const std::vector<EntityPatch> redo = applyAll(world, undo);
        CHECK(world.entityCount() == 1);
        CHECK(same(world, snapshotOf(world), empty));

        // The redo spawns them again under placeholders, with new handles and the same values.
        applyAll(world, redo);
        const Entity door2 = world.findByStableId(idOf(1));
        const Entity hinge2 = world.findByStableId(idOf(2));
        REQUIRE(world.isAlive(hinge2));
        CHECK(world.get<const ChildOf>(hinge2)->parent == door2);
        CHECK(world.get<const Follow>(hinge2)->target == door2);
        CHECK(world.get<const Name>(door2)->text == "door");
    }

    TEST_CASE("a Despawn is refused while the entity has children, counting the call's own changes") {
        World world;
        const Entity parent = world.spawn(StableId{.id = idOf(1)});
        const Entity child = world.spawn(StableId{.id = idOf(2)}, ChildOf{.parent = parent});
        checkFails(world, {despawnOf(parent)}, "ecs.patch.has_children");
        // A child spawned earlier in the call counts too.
        const Entity loner = world.spawn(StableId{.id = idOf(3)});
        checkFails(world, {spawnOf(placeholder(0), {adding(ChildOf{.parent = loner})}), despawnOf(loner)},
                   "ecs.patch.has_children");
        // Detached first in the same call, the parent goes.
        applyAll(world, {modifyOf(child, {removing<ChildOf>()}), despawnOf(parent)});
        CHECK_FALSE(world.isAlive(parent));
        CHECK(world.get<const ChildOf>(child) == nullptr);
    }

    TEST_CASE("a Despawn is undone by a Spawn of every component, in the old place, under the same id") {
        World world;
        const Entity first = world.spawn(StableId{.id = idOf(1)});
        const Entity middle = world.spawn(StableId{.id = idOf(2)}, Name{.text = "keep"}, Health{.value = 7},
                                          ith::Disabled{}, awkward(1.0f));
        const Entity last = world.spawn(StableId{.id = idOf(3)}, Follow{.target = middle});
        const Snapshot before = snapshotOf(world);

        const std::vector<EntityPatch> undo = applyAll(world, {despawnOf(middle)});
        CHECK_FALSE(world.isAlive(middle));
        CHECK(world.findByStableId(idOf(2)).isNull());
        REQUIRE(undo.size() == 1);
        CHECK(undo[0].op == EntityOp::Spawn);
        CHECK(undo[0].entity.worldId() == 0);
        CHECK(undo[0].placeBefore == last);

        applyAll(world, undo);
        const Entity again = world.findByStableId(idOf(2));
        REQUIRE(world.isAlive(again));
        CHECK(again != middle);
        std::vector<Entity> roots;
        world.forEachRoot([&](Entity root) { roots.push_back(root); });
        CHECK(roots == std::vector<Entity>{first, again, last});
        CHECK(world.has<ith::DisabledInHierarchy>(again)); // rebuilt by the hooks
        // A reference another entity held stays stale: the inverse doesn't touch other entities.
        CHECK_FALSE(world.isAlive(world.get<const Follow>(last)->target));
        CHECK(snapshotOf(world).entities.size() == before.entities.size());
    }

    TEST_CASE("spawn, reparent and delete in one call: all of it or none of it") {
        World world;
        const Entity keep = world.spawn(StableId{.id = idOf(1)}, awkward(1.0f));
        const Entity moved = world.spawn(StableId{.id = idOf(2)}, awkward(2.0f));
        const Entity doomed = world.spawn(StableId{.id = idOf(3)}, ChildOf{.parent = keep}, awkward(3.0f));
        const Entity orphan = world.spawn(StableId{.id = idOf(4)}, ChildOf{.parent = doomed}, awkward(4.0f));
        const Snapshot before = snapshotOf(world);
        const std::size_t count = world.entityCount();

        const auto call = [&](bool failLast) {
            std::vector<EntityPatch> patches{
                spawnOf(placeholder(7),
                        {adding(StableId{.id = idOf(5)}), adding(ChildOf{.parent = keep}), adding(awkward(5.0f))}),
                modifyOf(moved, {adding(ChildOf{.parent = placeholder(7)})}),
                modifyOf(orphan, {removing<ChildOf>()}),
                despawnOf(doomed),
            };
            if (failLast) {
                patches.push_back(despawnOf(keep)); // keep has children: refused
            }
            return patches;
        };

        checkFails(world, call(true), "ecs.patch.has_children");
        CHECK(world.entityCount() == count);
        CHECK(same(world, snapshotOf(world), before));
        CHECK(world.findByStableId(idOf(5)).isNull());

        const std::vector<EntityPatch> undo = applyAll(world, call(false));
        const Entity added = world.findByStableId(idOf(5));
        REQUIRE(world.isAlive(added));
        CHECK(world.get<const ChildOf>(moved)->parent == added);
        CHECK(world.get<const ChildOf>(orphan) == nullptr);
        CHECK_FALSE(world.isAlive(doomed));
        const Snapshot after = snapshotOf(world);

        const std::vector<EntityPatch> redo = applyAll(world, undo);
        CHECK(same(world, snapshotOf(world), before));
        CHECK(world.entityCount() == count);
        const Entity doomedAgain = world.findByStableId(idOf(3));
        CHECK(world.get<const ChildOf>(world.findByStableId(idOf(4)))->parent == doomedAgain);

        applyAll(world, redo);
        CHECK(same(world, snapshotOf(world), after));

        // The failed call gave its reserved handle back: spawning still works.
        CHECK(world.isAlive(world.spawn(Health{})));
    }

    TEST_CASE("malformed entity operations are refused before anything changes") {
        World world;
        const Entity parent = world.spawn(StableId{.id = idOf(1)});
        const Entity other = world.spawn(StableId{.id = idOf(2)});
        const Entity child = world.spawn(StableId{.id = idOf(3)}, ChildOf{.parent = parent});
        const Snapshot before = snapshotOf(world);

        checkFails(world, {spawnOf(parent, {})}, "ecs.patch.invalid_spawn");   // not a placeholder
        checkFails(world, {spawnOf(Entity(), {})}, "ecs.patch.invalid_spawn"); // the null handle
        checkFails(world, {spawnOf(placeholder(0), {}), spawnOf(placeholder(0), {})}, "ecs.patch.invalid_spawn");
        checkFails(world, {spawnOf(placeholder(0), {removing<Health>()})}, "ecs.patch.invalid_spawn");
        checkFails(world, {spawnOf(placeholder(0), {adding(StableId{.id = idOf(1)})})}, "ecs.patch.refused_value");
        checkFails(world, {EntityPatch{.op = EntityOp::Despawn, .entity = other, .changes = {adding(Health{})}}},
                   "ecs.patch.invalid_despawn");
        checkFails(world, {despawnOf(other), modifyOf(other, {adding(Health{})})}, "ecs.patch.stale_entity");
        checkFails(world, {modifyOf(placeholder(3), {adding(Health{})})}, "ecs.patch.stale_entity");
        checkFails(world, {EntityPatch{.entity = other, .placeBefore = child}}, "ecs.patch.invalid_place");
        checkFails(world, {EntityPatch{.entity = other, .placeBefore = other}}, "ecs.patch.invalid_place");
        CHECK(same(world, snapshotOf(world), before));
        CHECK(world.entityCount() == 3);
    }

    TEST_CASE("a placement is undone into the old place") {
        World world;
        const Entity a = world.spawn(StableId{.id = idOf(1)});
        const Entity b = world.spawn(StableId{.id = idOf(2)});
        const Entity c = world.spawn(StableId{.id = idOf(3)});
        const auto roots = [&] {
            std::vector<Entity> result;
            world.forEachRoot([&](Entity root) { result.push_back(root); });
            return result;
        };
        const std::vector<EntityPatch> undo = applyAll(world, {EntityPatch{.entity = c, .placeBefore = a}});
        CHECK(roots() == std::vector<Entity>{c, a, b});
        const std::vector<EntityPatch> redo = applyAll(world, undo);
        CHECK(roots() == std::vector<Entity>{a, b, c});
        applyAll(world, redo);
        CHECK(roots() == std::vector<Entity>{c, a, b});
    }
}
