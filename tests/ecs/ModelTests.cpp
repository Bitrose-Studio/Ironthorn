// A randomized model test of ith::World (ADR-0014 rule 19): random spawns, despawns, adds,
// removes, query writes, resource changes and clears, checked after every step against a simple
// reference model (a map from handle to optional components). Adds, removes and query writes go
// through the typed functions or through type records at random, so both paths are exercised
// against the same model. An immutable component with hooks (Tracked) is spawned, replaced,
// removed, despawned and cleared too; its hooks count their calls into a resource, which is checked
// against the counts the model expects (ADR-0030). Queries with With and Without filters are checked
// too. The seeds are fixed, so a failure reproduces.

#include "ith/ecs/Hooks.h"
#include "ith/ecs/World.h"
#include "ith/reflection/TypeOf.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
};

class ModelTest {
public:
    explicit ModelTest(std::uint32_t seed) : m_random(seed) {
        setTrackedHooks();
        m_world.insertResource(HookCounts{});
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
        m_model[entity.bits()] = components;
        m_issued.push_back(entity);
    }

    void step() {
        const std::uint32_t operation = pick(100);
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
                model->health = value;
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
            const Health* health = m_world.get<Health>(entity);
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
            const World& world = m_world;
            CHECK(m_world.get(entity, ith::typeOf<Health>()) == health);
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
};

} // namespace

TEST_CASE("World: random operations match a reference model") {
    for (const std::uint32_t seed : {1u, 2u, 3u, 42u, 2026u}) {
        CAPTURE(seed);
        ModelTest test(seed);
        test.run(3000);
    }
}
