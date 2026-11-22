// A randomized model test of ith::World (ADR-0014 rule 19): random spawns, despawns, adds,
// removes, query writes, resource changes and clears, checked after every step against a simple
// reference model (a map from handle to optional components). Adds, removes and query writes go
// through the typed functions or through type records at random, so both paths are exercised
// against the same model. An immutable component with hooks (Tracked) is spawned, replaced,
// removed, despawned and cleared too; its hooks count their calls into a resource, which is checked
// against the counts the model expects (ADR-0030). Queries with With and Without filters are checked
// too. The model also tracks whether each Health was added, or added or changed, since an observing
// system last ran; random schedule runs, whose writer system writes Health and spawns through its
// commands, check the observer's Added and Changed queries, typed and dynamic, against it (ADR-0014
// rule 9). A second model test runs random hierarchy operations (reparent, detach, despawn with the
// link policy, despawnRecursive, placeBefore, Disabled, LocalTransform written, added and removed)
// and checks the hierarchy invariants after every step, with transforms propagated by the system
// or by a direct call, each of which writes only the subtrees that changed. The seeds are fixed, so
// a failure reproduces.

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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ith::Entity;
using ith::World;
using ith::test::GameClock;
using ith::test::Health;
using ith::test::HookCounts;
using ith::test::Name;
using ith::test::Position;
using ith::test::Tag0;
using ith::test::Tracked;

// Tracked's hooks count their calls into the world's HookCounts.
void setTrackedHooks() {
    static const bool kSet = ith::setComponentHooks<Tracked>({
        .onAdd = [](ith::HookContext& context) { ++context.resource<HookCounts>()->adds; },
        .onInsert =
            [](ith::HookContext& context) {
                HookCounts& counts = *context.resource<HookCounts>();
                ++counts.inserts;
                counts.insertedSum += context.value<const Tracked>()->value;
            },
        .onReplace = [](ith::HookContext& context) { ++context.resource<HookCounts>()->replaces; },
        .onRemove =
            [](ith::HookContext& context) {
                HookCounts& counts = *context.resource<HookCounts>();
                ++counts.removes;
                counts.removedSum += context.value<const Tracked>()->value;
            },
    });
    REQUIRE(kSet);
}

struct ModelEntity {
    std::optional<std::int32_t> health;
    std::optional<std::string> name;
    std::optional<float> position;
    bool tag = false;
    std::optional<std::int64_t> tracked;
    // Whether Health was added, or added or changed, since the observing system last ran.
    bool healthAdded = false;
    bool healthChanged = false;
};

class ModelTest {
public:
    explicit ModelTest(std::uint32_t seed) : m_random(seed) {
        setTrackedHooks();
        m_world.insertResource(HookCounts{});
        buildSchedule();
    }

    void run(int steps) {
        for (int step = 0; step < steps; ++step) {
            this->step();
            checkCounts();
            if (step % 64 == 0) {
                checkEverything();
            }
        }
        checkEverything();
    }

private:
    std::uint32_t pick(std::uint32_t count) { return static_cast<std::uint32_t>(m_random() % count); }

    // Any handle ever issued, so stale handles are used as often as live ones.
    Entity anyHandle() {
        if (m_issued.empty()) {
            return {};
        }
        return m_issued[pick(static_cast<std::uint32_t>(m_issued.size()))];
    }

    std::string nextName() { return "entity name long enough for the heap " + std::to_string(m_names++); }

    void spawned(Entity entity, const ModelEntity& components) {
        REQUIRE_FALSE(entity.isNull());
        REQUIRE(m_model.find(entity.bits()) == m_model.end());
        ModelEntity& model = m_model[entity.bits()] = components;
        model.healthAdded = model.health.has_value();
        model.healthChanged = model.health.has_value();
        m_issued.push_back(entity);
    }

    // A schedule of two systems: a writer that adds 1 to the Health of entities with Tag0 and may
    // spawn an entity through its commands, then an observer that records the entities whose
    // Health was added or changed since its last run, through typed and dynamic queries.
    void buildSchedule() {
        m_schedule.add({
            .name = "model::writer",
            .access = ith::accessTo<Health>(),
            .run =
                [this](ith::SystemContext& context) {
                    context.query<Health, ith::With<Tag0>>().forEach([](Health& health) { ++health.value; });
                    if (m_spawnFromSystem) {
                        m_systemSpawned = context.commands().spawn(Health{.value = 7});
                    }
                },
        });
        m_schedule.add({
            .name = "model::observer",
            .access = ith::accessTo<const Health>(),
            .after = {"model::writer"},
            .run =
                [this](ith::SystemContext& context) {
                    m_seen = {};
                    context.query<ith::Added<Health>>().forEach(
                        [&](Entity entity) { m_seen.added.push_back(entity.bits()); });
                    context.query<const Health, ith::Changed<Health>>().forEach(
                        [&](Entity entity, const Health&) { m_seen.changed.push_back(entity.bits()); });
                    const std::array health{ith::QueryTerm{.type = &ith::typeOf<Health>()}};
                    const std::array added{
                        ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Added}};
                    const std::array changed{
                        ith::QueryFilter{.type = &ith::typeOf<Health>(), .kind = ith::FilterKind::Changed}};
                    context.dynamicQuery(health, added).forEach([&](Entity entity, std::span<void* const>) {
                        m_seen.dynamicAdded.push_back(entity.bits());
                    });
                    context.dynamicQuery({}, changed).forEach([&](Entity entity, std::span<void* const>) {
                        m_seen.dynamicChanged.push_back(entity.bits());
                    });
                },
        });
        REQUIRE(m_schedule.build());
    }

    // Runs the schedule and checks what the observer saw against the model.
    void runSchedule() {
        m_spawnFromSystem = pick(3) == 0;
        for (auto& [bits, entity] : m_model) {
            if (entity.health && entity.tag) {
                ++*entity.health;
                entity.healthChanged = true;
            }
        }
        std::set<std::uint64_t> expectedAdded;
        std::set<std::uint64_t> expectedChanged;
        for (auto& [bits, entity] : m_model) {
            if (entity.healthAdded) {
                expectedAdded.insert(bits);
            }
            if (entity.healthChanged) {
                expectedChanged.insert(bits);
            }
            entity.healthAdded = false;
            entity.healthChanged = false;
        }
        m_schedule.run(m_world);
        // Each query visits an entity at most once, so the sets have the visits' sizes.
        const auto asSet = [](const std::vector<std::uint64_t>& bits) {
            return std::set<std::uint64_t>(bits.begin(), bits.end());
        };
        CHECK(m_seen.added.size() == expectedAdded.size());
        CHECK(asSet(m_seen.added) == expectedAdded);
        CHECK(m_seen.changed.size() == expectedChanged.size());
        CHECK(asSet(m_seen.changed) == expectedChanged);
        CHECK(asSet(m_seen.dynamicAdded) == expectedAdded);
        CHECK(asSet(m_seen.dynamicChanged) == expectedChanged);
        if (m_spawnFromSystem) {
            spawned(m_systemSpawned, {.health = 7}); // applied at the stage's sync point
        }
    }

    // Marks the model's Health of an entity as given a value: added when it had none.
    static void healthGiven(ModelEntity& entity, std::int32_t value) {
        entity.healthAdded = entity.healthAdded || !entity.health.has_value();
        entity.healthChanged = true;
        entity.health = value;
    }

    void step() {
        const std::uint32_t operation = pick(110);
        if (operation >= 100) {
            runSchedule();
            return;
        }
        if (operation < 10) {
            spawned(m_world.spawn(), {});
        } else if (operation < 25) {
            const auto health = static_cast<std::int32_t>(pick(1000));
            std::string name = nextName();
            spawned(m_world.spawn(Health{.value = health}, Name{.text = name}), {.health = health, .name = name});
        } else if (operation < 35) {
            const auto health = static_cast<std::int32_t>(pick(1000));
            const auto position = static_cast<float>(pick(1000));
            if (pick(2) == 0) {
                spawned(m_world.spawn(Position{.x = position}, Tag0{}, Health{.value = health}),
                        {.health = health, .position = position, .tag = true});
            } else {
                const std::int64_t tracked = pick(1000);
                spawned(m_world.spawn(Position{.x = position}, Tracked{.value = tracked}, Health{.value = health}),
                        {.health = health, .position = position, .tracked = tracked});
                expectInsert(tracked, true);
            }
        } else if (operation < 50) {
            const Entity entity = anyHandle();
            const auto found = m_model.find(entity.bits());
            const bool alive = found != m_model.end();
            if (alive) {
                expectRemoval(found->second);
                m_model.erase(found);
            }
            CHECK(m_world.despawn(entity) == alive);
        } else if (operation < 75) {
            add(anyHandle());
        } else if (operation < 95) {
            remove(anyHandle());
        } else if (operation < 99) {
            if (pick(2) == 0) {
                m_world.query<Health, const Position>().forEach([](Health& health, const Position& position) {
                    health.value += static_cast<std::int32_t>(position.x);
                });
            } else {
                const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Health>(), .access = ith::Access::Write},
                                       ith::QueryTerm{.type = &ith::typeOf<Position>()}};
                m_world.dynamicQuery(terms).forEach([](Entity, std::span<void* const> values) {
                    static_cast<Health*>(values[0])->value +=
                        static_cast<std::int32_t>(static_cast<const Position*>(values[1])->x);
                });
            }
            for (auto& [bits, entity] : m_model) {
                if (entity.health && entity.position) {
                    *entity.health += static_cast<std::int32_t>(*entity.position);
                    entity.healthChanged = true; // a written term marks every value it visits
                }
            }
        } else if (pick(4) == 0) {
            for (const auto& [bits, entity] : m_model) {
                expectRemoval(entity);
            }
            m_world.clear(); // keeps the resources
            m_model.clear();
        } else {
            changeResource();
        }
    }

    // Inserts, replaces or removes the GameClock resource, by type or by record.
    void changeResource() {
        const std::uint32_t operation = pick(4);
        if (operation == 0) {
            const std::uint64_t frame = pick(1000);
            REQUIRE(m_world.insertResource(GameClock{.frame = frame}) != nullptr);
            m_clock = frame;
        } else if (operation == 1) {
            auto* clock = static_cast<GameClock*>(m_world.insertResource(ith::typeOf<GameClock>()));
            REQUIRE(clock != nullptr);
            m_clock = clock->frame;
        } else {
            const bool removed =
                operation == 2 ? m_world.removeResource<GameClock>() : m_world.removeResource(ith::typeOf<GameClock>());
            CHECK(removed == m_clock.has_value());
            m_clock.reset();
        }
    }

    // Tracked's hooks for a value given to a live entity: onAdd or onReplace, then onInsert.
    void expectInsert(std::int64_t value, bool added) {
        ++(added ? m_counts.adds : m_counts.replaces);
        ++m_counts.inserts;
        m_counts.insertedSum += value;
    }

    // Tracked's hooks when a live entity loses its Tracked, if it has one.
    void expectRemoval(const ModelEntity& entity) {
        if (entity.tracked) {
            ++m_counts.replaces;
            ++m_counts.removes;
            m_counts.removedSum += *entity.tracked;
        }
    }

    // Adds a value through World::add<T>, or through the type's record, either moved from a value or
    // default-constructed and then written through get. An immutable type is never written in place.
    template <typename T>
    bool addValue(Entity entity, T value) {
        const std::uint32_t path = pick(3);
        if (path == 0) {
            return m_world.add(entity, std::move(value)) != nullptr;
        }
        if (path == 1 || hasFlags(ith::typeOf<T>().flags, ith::ReflectionFlags::Immutable)) {
            return m_world.add(entity, ith::typeOf<T>(), &value) != nullptr;
        }
        T* stored = m_world.add(entity, ith::typeOf<T>()) != nullptr ? m_world.get<T>(entity) : nullptr;
        if (stored != nullptr) {
            *stored = std::move(value);
        }
        return stored != nullptr;
    }

    // Removes a value through World::remove<T> or through the type's record.
    template <typename T>
    bool removeValue(Entity entity) {
        return pick(2) == 0 ? m_world.remove<T>(entity) : m_world.remove(entity, ith::typeOf<T>());
    }

    void add(Entity entity) {
        const auto found = m_model.find(entity.bits());
        ModelEntity* model = found != m_model.end() ? &found->second : nullptr;
        const std::uint32_t component = pick(5);
        bool added = false;
        if (component == 4) {
            const std::int64_t value = pick(1000);
            added = addValue(entity, Tracked{.value = value});
            if (model != nullptr) {
                expectInsert(value, !model->tracked.has_value());
                model->tracked = value;
            }
        } else if (component == 0) {
            const auto value = static_cast<std::int32_t>(pick(1000));
            added = addValue(entity, Health{.value = value});
            if (model != nullptr) {
                healthGiven(*model, value);
            }
        } else if (component == 1) {
            std::string name = nextName();
            added = addValue(entity, Name{.text = name});
            if (model != nullptr) {
                model->name = name;
            }
        } else if (component == 2) {
            const auto value = static_cast<float>(pick(1000));
            added = addValue(entity, Position{.x = value});
            if (model != nullptr) {
                model->position = value;
            }
        } else {
            added = addValue(entity, Tag0{});
            if (model != nullptr) {
                model->tag = true;
            }
        }
        CHECK(added == (model != nullptr));
    }

    void remove(Entity entity) {
        const auto found = m_model.find(entity.bits());
        ModelEntity* model = found != m_model.end() ? &found->second : nullptr;
        const std::uint32_t component = pick(5);
        bool had = false;
        bool removed = false;
        if (component == 4) {
            had = model != nullptr && model->tracked.has_value();
            removed = removeValue<Tracked>(entity);
            if (model != nullptr) {
                expectRemoval(*model);
                model->tracked.reset();
            }
        } else if (component == 0) {
            had = model != nullptr && model->health.has_value();
            removed = removeValue<Health>(entity);
            if (model != nullptr) {
                model->health.reset();
                model->healthAdded = false;
                model->healthChanged = false;
            }
        } else if (component == 1) {
            had = model != nullptr && model->name.has_value();
            removed = removeValue<Name>(entity);
            if (model != nullptr) {
                model->name.reset();
            }
        } else if (component == 2) {
            had = model != nullptr && model->position.has_value();
            removed = removeValue<Position>(entity);
            if (model != nullptr) {
                model->position.reset();
            }
        } else {
            had = model != nullptr && model->tag;
            removed = removeValue<Tag0>(entity);
            if (model != nullptr) {
                model->tag = false;
            }
        }
        CHECK(removed == had);
    }

    void checkCounts() { REQUIRE(m_world.entityCount() == m_model.size()); }

    void checkEverything() {
        for (const Entity entity : m_issued) {
            const auto found = m_model.find(entity.bits());
            const bool alive = found != m_model.end();
            REQUIRE(m_world.isAlive(entity) == alive);
            if (!alive) {
                CHECK(m_world.get<Health>(entity) == nullptr);
                continue;
            }
            const ModelEntity& model = found->second;
            // Read-only access throughout, which marks nothing changed.
            const World& world = m_world;
            const Health* health = world.get<Health>(entity);
            REQUIRE((health != nullptr) == model.health.has_value());
            if (health != nullptr) {
                CHECK(health->value == *model.health);
            }
            const Name* name = m_world.get<Name>(entity);
            REQUIRE((name != nullptr) == model.name.has_value());
            if (name != nullptr) {
                CHECK(name->text == *model.name);
            }
            const Position* position = m_world.get<Position>(entity);
            REQUIRE((position != nullptr) == model.position.has_value());
            if (position != nullptr) {
                CHECK(position->x == *model.position);
            }
            CHECK(m_world.has<Tag0>(entity) == model.tag);
            const Tracked* tracked = m_world.get<const Tracked>(entity);
            REQUIRE((tracked != nullptr) == model.tracked.has_value());
            if (tracked != nullptr) {
                CHECK(tracked->value == *model.tracked);
            }

            // Access by record finds the same values, and forEachComponent visits each once; the
            // const form visits immutable components too.
            CHECK(world.get(entity, ith::typeOf<Health>()) == health);
            CHECK(world.get(entity, ith::typeOf<Tracked>()) == tracked);
            CHECK(m_world.has(entity, ith::typeOf<Tag0>()) == model.tag);
            const std::size_t components =
                static_cast<std::size_t>(model.health.has_value()) + static_cast<std::size_t>(model.name.has_value()) +
                static_cast<std::size_t>(model.position.has_value()) + static_cast<std::size_t>(model.tag) +
                static_cast<std::size_t>(model.tracked.has_value());
            std::size_t visits = 0;
            world.forEachComponent(entity, [&](const ith::TypeInfo& type, const void* value) {
                ++visits;
                CHECK(value == world.get(entity, type));
            });
            CHECK(visits == components);
        }

        // Tracked's hooks ran exactly as often as the model expects, with the expected values.
        const HookCounts* counts = m_world.resource<const HookCounts>();
        REQUIRE(counts != nullptr);
        CHECK(counts->adds == m_counts.adds);
        CHECK(counts->inserts == m_counts.inserts);
        CHECK(counts->replaces == m_counts.replaces);
        CHECK(counts->removes == m_counts.removes);
        CHECK(counts->insertedSum == m_counts.insertedSum);
        CHECK(counts->removedSum == m_counts.removedSum);

        // Resources are untouched by entity operations and clear().
        const GameClock* clock = m_world.resource<GameClock>();
        REQUIRE((clock != nullptr) == m_clock.has_value());
        if (clock != nullptr) {
            CHECK(clock->frame == *m_clock);
        }

        // A dynamic query visits exactly the entities the typed query does, in the same order;
        // without terms it visits every live entity.
        std::vector<std::uint64_t> typedOrder;
        m_world.query<const Health, const Name>().forEach(
            [&](Entity entity, const Health&, const Name&) { typedOrder.push_back(entity.bits()); });
        std::vector<std::uint64_t> dynamicOrder;
        const std::array terms{ith::QueryTerm{.type = &ith::typeOf<Health>()},
                               ith::QueryTerm{.type = &ith::typeOf<Name>()}};
        m_world.dynamicQuery(terms).forEach(
            [&](Entity entity, std::span<void* const>) { dynamicOrder.push_back(entity.bits()); });
        CHECK(dynamicOrder == typedOrder);
        std::size_t everyEntity = 0;
        m_world.dynamicQuery({}).forEach([&](Entity, std::span<void* const>) { ++everyEntity; });
        CHECK(everyEntity == m_model.size());

        // Each query visits exactly the entities the model says match, each once.
        std::multiset<std::uint64_t> visited;
        m_world.query<const Health, const Name>().forEach(
            [&](Entity entity, const Health&, const Name&) { visited.insert(entity.bits()); });
        std::multiset<std::uint64_t> expected;
        for (const auto& [bits, entity] : m_model) {
            if (entity.health && entity.name) {
                expected.insert(bits);
            }
        }
        CHECK(visited == expected);

        visited.clear();
        m_world.query<const Tag0>().forEach([&](Entity entity, const Tag0&) { visited.insert(entity.bits()); });
        expected.clear();
        for (const auto& [bits, entity] : m_model) {
            if (entity.tag) {
                expected.insert(bits);
            }
        }
        CHECK(visited == expected);

        // Filters: Health with a Name and without Tag0, typed and dynamic, in the same order.
        std::vector<std::uint64_t> filtered;
        m_world.query<const Health, ith::With<Name>, ith::Without<Tag0>>().forEach(
            [&](Entity entity, const Health&) { filtered.push_back(entity.bits()); });
        expected.clear();
        for (const auto& [bits, entity] : m_model) {
            if (entity.health && entity.name && !entity.tag) {
                expected.insert(bits);
            }
        }
        CHECK(std::multiset<std::uint64_t>(filtered.begin(), filtered.end()) == expected);
        const std::array healthTerm{ith::QueryTerm{.type = &ith::typeOf<Health>()}};
        const std::array filters{ith::QueryFilter{.type = &ith::typeOf<Name>()},
                                 ith::QueryFilter{.type = &ith::typeOf<Tag0>(), .kind = ith::FilterKind::Without}};
        std::vector<std::uint64_t> dynamicFiltered;
        m_world.dynamicQuery(healthTerm, filters).forEach([&](Entity entity, std::span<void* const>) {
            dynamicFiltered.push_back(entity.bits());
        });
        CHECK(dynamicFiltered == filtered);
    }

    std::mt19937 m_random;
    World m_world;
    std::map<std::uint64_t, ModelEntity> m_model;
    std::vector<Entity> m_issued;
    std::optional<std::uint64_t> m_clock; // the GameClock resource's frame, when it exists
    HookCounts m_counts;                  // the counts Tracked's hooks should have reached
    int m_names = 0;

    // The schedule of the writer and the observer, and what the observer saw in its last run.
    struct Seen {
        std::vector<std::uint64_t> added;
        std::vector<std::uint64_t> changed;
        std::vector<std::uint64_t> dynamicAdded;
        std::vector<std::uint64_t> dynamicChanged;
    };

    ith::Schedule m_schedule;
    Seen m_seen;
    bool m_spawnFromSystem = false;
    Entity m_systemSpawned;
};

} // namespace

TEST_CASE("World: random operations match a reference model") {
    for (const std::uint32_t seed : {1u, 2u, 3u, 42u, 2026u}) {
        CAPTURE(seed);
        ModelTest test(seed);
        test.run(3000);
    }
}

namespace {

using ith::ChildOf;
using ith::Children;
using ith::Disabled;
using ith::DisabledInHierarchy;
using ith::LocalTransform;
using ith::Mat4;
using ith::Vec3;
using ith::WorldTransform;

// One entity of the hierarchy model.
struct ModelNode {
    Entity parent; // null for a root
    bool despawnWithParent = false;
    std::vector<Entity> children; // in order
    bool disabled = false;
};

// Random hierarchy operations, directly and through commands, against a model of the links, the
// order of children and roots, and the Disabled tags. After every step it checks that parent and
// child links agree both ways, that the root list is exactly the entities without ChildOf, in the
// model's order, that DisabledInHierarchy matches Disabled and the ancestors, and that propagation
// gives every WorldTransform the product of the local transforms above it. Reparenting and
// detaching an entity with a LocalTransform must keep its world matrix.
class HierarchyModelTest {
public:
    explicit HierarchyModelTest(std::uint32_t seed) : m_random(seed) {
        m_propagation.add(ith::transformPropagationSystem());
        REQUIRE(m_propagation.build());
    }

    void run(int steps) {
        for (int step = 0; step < steps; ++step) {
            CAPTURE(step);
            this->step();
            check();
        }
    }

private:
    std::uint32_t pick(std::uint32_t count) { return static_cast<std::uint32_t>(m_random() % count); }

    // A live entity, or the null handle when there is none.
    Entity anyLive() {
        if (m_nodes.empty()) {
            return {};
        }
        auto found = m_nodes.begin();
        std::advance(found, pick(static_cast<std::uint32_t>(m_nodes.size())));
        return Entity::fromBits(found->first);
    }

    ModelNode& node(Entity entity) { return m_nodes.at(entity.bits()); }

    // The order list `entity` is in: its parent's children, or the roots.
    std::vector<Entity>& siblings(Entity entity) {
        const Entity parent = node(entity).parent;
        return parent.isNull() ? m_roots : node(parent).children;
    }

    static void erase(std::vector<Entity>& list, Entity entity) {
        const auto found = std::ranges::find_if(list, [entity](Entity entry) { return entry == entity; });
        REQUIRE(found != list.end());
        list.erase(found);
    }

    bool isDescendantOrSelf(Entity candidate, Entity of) {
        for (Entity current = candidate; !current.isNull(); current = node(current).parent) {
            if (current == of) {
                return true;
            }
        }
        return false;
    }

    // Uniform scales only, so that a hierarchy has no shear and keeping the world placement is
    // exact up to rounding.
    LocalTransform randomLocal() {
        const Vec3 axis = glm::normalize(
            Vec3(static_cast<float>(pick(5)) + 0.5f, static_cast<float>(pick(5)), static_cast<float>(pick(5))));
        return {.value = {.translation = Vec3(static_cast<float>(pick(20)) - 10.0f, static_cast<float>(pick(10)),
                                              static_cast<float>(pick(20)) - 10.0f),
                          .rotation = glm::angleAxis(static_cast<float>(pick(628)) / 100.0f, axis),
                          .scale = Vec3(0.5f + (static_cast<float>(pick(4)) * 0.25f))}};
    }

    // The world matrix from the World's local transforms along the model's links.
    Mat4 reference(Entity entity) {
        std::vector<Entity> chain;
        for (Entity current = entity; !current.isNull(); current = node(current).parent) {
            chain.push_back(current);
        }
        Mat4 world(1.0f);
        for (auto current = chain.rbegin(); current != chain.rend(); ++current) {
            if (const LocalTransform* local = m_world.get<const LocalTransform>(*current)) {
                world = world * ith::toMatrix(local->value);
            }
        }
        return world;
    }

    static bool close(const Mat4& a, const Mat4& b) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                const float tolerance = 1e-3f * (1.0f + std::abs(a[column][row]));
                if (std::abs(a[column][row] - b[column][row]) > tolerance) {
                    return false;
                }
            }
        }
        return true;
    }

    void spawnEntity() {
        const bool placed = pick(4) != 0;
        const Entity parent = pick(3) != 0 ? anyLive() : Entity();
        const bool part = !parent.isNull() && pick(3) == 0;
        Entity entity;
        if (parent.isNull()) {
            entity = placed ? m_world.spawn(randomLocal(), WorldTransform{}) : m_world.spawn(WorldTransform{});
        } else if (pick(4) == 0) {
            ith::Commands commands(m_world);
            const ChildOf link{.parent = parent, .despawnWithParent = part};
            entity = placed ? commands.spawn(randomLocal(), WorldTransform{}, link) : commands.spawn(link);
            m_world.apply(commands);
        } else {
            const ChildOf link{.parent = parent, .despawnWithParent = part};
            entity = placed ? m_world.spawn(randomLocal(), WorldTransform{}, link) : m_world.spawn(link);
        }
        REQUIRE(m_world.isAlive(entity));
        m_nodes[entity.bits()] = ModelNode{.parent = parent, .despawnWithParent = part};
        (parent.isNull() ? m_roots : node(parent).children).push_back(entity);
    }

    void reparent() {
        const Entity entity = anyLive();
        const Entity parent = anyLive();
        if (entity.isNull()) {
            return;
        }
        const bool part = pick(3) == 0;
        if (isDescendantOrSelf(parent, entity)) {
            // Refused: the entity itself or one of its descendants.
            const ith::test::AssertCapture capture;
            CHECK(m_world.add(entity, ChildOf{.parent = parent}) == nullptr);
#ifdef ITH_ENABLE_ASSERTS
            CHECK(capture.count() == 1);
#endif
            return;
        }
        const bool placed = m_world.has<LocalTransform>(entity);
        const Mat4 before = reference(entity);
        if (pick(2) == 0) {
            REQUIRE(m_world.add(entity, ChildOf{.parent = parent, .despawnWithParent = part}) != nullptr);
        } else {
            ith::Commands commands(m_world);
            commands.add(entity, ChildOf{.parent = parent, .despawnWithParent = part});
            m_world.apply(commands);
        }
        if (node(entity).parent != parent) { // the same parent keeps the place
            erase(siblings(entity), entity);
            node(entity).parent = parent;
            node(parent).children.push_back(entity);
        }
        node(entity).despawnWithParent = part;
        if (placed) {
            CHECK(close(reference(entity), before));
        }
    }

    void detach() {
        const Entity entity = anyLive();
        if (entity.isNull()) {
            return;
        }
        const bool hadParent = !node(entity).parent.isNull();
        const bool placed = m_world.has<LocalTransform>(entity);
        const Mat4 before = reference(entity);
        CHECK(m_world.remove<ChildOf>(entity) == hadParent);
        if (hadParent) {
            erase(siblings(entity), entity);
            node(entity).parent = {};
            node(entity).despawnWithParent = false;
            m_roots.push_back(entity);
            if (placed) {
                CHECK(close(reference(entity), before));
            }
        }
    }

    // The model of World::despawn and despawnRecursive, with the World's walk: the entities that
    // go, breadth first, then in reverse, each detaching the children left to it.
    void despawnModel(Entity entity, bool everything) {
        std::vector<Entity> order{entity};
        for (std::size_t next = 0; next < order.size(); ++next) {
            for (const Entity child : node(order[next]).children) {
                if (everything || node(child).despawnWithParent) {
                    order.push_back(child);
                }
            }
        }
        for (std::size_t index = order.size(); index-- > 0;) {
            const Entity current = order[index];
            const std::vector<Entity> remaining = node(current).children;
            for (const Entity child : remaining) {
                node(child).parent = {};
                node(child).despawnWithParent = false;
                m_roots.push_back(child);
            }
            erase(siblings(current), current);
            m_nodes.erase(current.bits());
        }
    }

    void despawn() {
        const Entity entity = anyLive();
        if (entity.isNull()) {
            return;
        }
        const bool everything = pick(3) == 0;
        if (pick(2) == 0) {
            CHECK((everything ? m_world.despawnRecursive(entity) : m_world.despawn(entity)));
        } else {
            ith::Commands commands(m_world);
            if (everything) {
                commands.despawnRecursive(entity);
            } else {
                commands.despawn(entity);
            }
            m_world.apply(commands);
        }
        despawnModel(entity, everything);
    }

    void placeBefore() {
        const Entity entity = anyLive();
        if (entity.isNull()) {
            return;
        }
        std::vector<Entity>& list = siblings(entity);
        const Entity sibling = pick(4) == 0 ? Entity() : list[pick(static_cast<std::uint32_t>(list.size()))];
        if (sibling == entity) {
            return;
        }
        if (pick(2) == 0) {
            CHECK(m_world.placeBefore(entity, sibling));
        } else {
            ith::Commands commands(m_world);
            commands.placeBefore(entity, sibling);
            m_world.apply(commands);
        }
        erase(list, entity);
        const auto position = sibling.isNull()
                                  ? list.end()
                                  : std::ranges::find_if(list, [sibling](Entity entry) { return entry == sibling; });
        list.insert(position, entity);
    }

    void toggleDisabled() {
        const Entity entity = anyLive();
        if (entity.isNull()) {
            return;
        }
        ModelNode& model = node(entity);
        if (model.disabled) {
            CHECK(m_world.remove<Disabled>(entity));
        } else {
            CHECK(m_world.add(entity, Disabled{}) != nullptr);
        }
        model.disabled = !model.disabled;
    }

    void step() {
        const std::uint32_t operation = pick(100);
        if (operation < 30 || m_nodes.size() < 4) {
            spawnEntity();
        } else if (operation < 50) {
            reparent();
        } else if (operation < 58) {
            detach();
        } else if (operation < 70) {
            despawn();
        } else if (operation < 80) {
            placeBefore();
        } else if (operation < 92) {
            toggleDisabled();
        } else if (operation < 99) {
            const Entity entity = anyLive();
            if (entity.isNull()) {
                return;
            }
            if (!m_world.has<LocalTransform>(entity)) {
                CHECK(m_world.add(entity, randomLocal()) != nullptr);
            } else if (pick(4) == 0) {
                CHECK(m_world.remove<LocalTransform>(entity)); // it counts as the identity from now on
            } else {
                m_world.get<LocalTransform>(entity)->value = randomLocal().value;
            }
        } else {
            m_world.clear();
            m_nodes.clear();
            m_roots.clear();
        }
    }

    bool disabledInModel(Entity entity) {
        for (Entity current = entity; !current.isNull(); current = node(current).parent) {
            if (node(current).disabled) {
                return true;
            }
        }
        return false;
    }

    void check() {
        REQUIRE(m_world.entityCount() == m_nodes.size());
        // The roots, in order, are exactly the entities without ChildOf.
        std::vector<Entity> roots;
        m_world.forEachRoot([&](Entity root) { roots.push_back(root); });
        CHECK(roots == m_roots);
        // Propagation writes only what changed, through the system or a direct call, each with a
        // window of its own; both must leave every WorldTransform right.
        if (pick(2) == 0) {
            ith::propagateTransforms(m_world);
        } else {
            m_propagation.run(m_world);
        }
        std::size_t placed = 0;
        std::size_t placedEnabled = 0;
        for (const auto& [bits, model] : m_nodes) {
            const Entity entity = Entity::fromBits(bits);
            REQUIRE(m_world.isAlive(entity));
            // Parent and child links agree both ways, in the model's order.
            const ChildOf* link = m_world.get<const ChildOf>(entity);
            REQUIRE((link != nullptr) == !model.parent.isNull());
            if (link != nullptr) {
                CHECK(link->parent == model.parent);
                CHECK(link->despawnWithParent == model.despawnWithParent);
                const Children* siblings = m_world.get<const Children>(link->parent);
                REQUIRE(siblings != nullptr);
                CHECK(std::ranges::count_if(siblings->entities, [entity](Entity entry) { return entry == entity; }) ==
                      1);
            }
            if (const Children* children = m_world.get<const Children>(entity)) {
                CHECK(children->entities == model.children);
                for (const Entity child : children->entities) {
                    REQUIRE(m_world.isAlive(child));
                    CHECK(m_world.get<const ChildOf>(child)->parent == entity);
                }
            } else {
                CHECK(model.children.empty());
            }
            // DisabledInHierarchy matches Disabled and the ancestors.
            const bool disabled = disabledInModel(entity);
            CHECK(m_world.has<Disabled>(entity) == model.disabled);
            CHECK(m_world.has<DisabledInHierarchy>(entity) == disabled);
            // Propagation matches the reference.
            if (const WorldTransform* world = m_world.get<const WorldTransform>(entity)) {
                CHECK(close(world->matrix, reference(entity)));
                ++placed;
                placedEnabled += disabled ? 0 : 1;
            }
        }
        // Queries skip disabled entities unless they ask for them.
        std::size_t visited = 0;
        m_world.query<const WorldTransform>().forEach([&](Entity entity, const WorldTransform&) {
            CHECK_FALSE(disabledInModel(entity));
            ++visited;
        });
        CHECK(visited == placedEnabled);
        std::size_t all = 0;
        m_world.query<const WorldTransform, ith::IncludeDisabled>().forEach([&](const WorldTransform&) { ++all; });
        CHECK(all == placed);
    }

    std::mt19937 m_random;
    World m_world;
    std::map<std::uint64_t, ModelNode> m_nodes;
    std::vector<Entity> m_roots; // in order
    ith::Schedule m_propagation;
};

} // namespace

TEST_CASE("World: random hierarchy operations match a reference model") {
    for (const std::uint32_t seed : {1u, 7u, 42u, 2026u}) {
        CAPTURE(seed);
        HierarchyModelTest test(seed);
        test.run(1500);
    }
}
