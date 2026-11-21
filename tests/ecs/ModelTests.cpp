// A randomized model test of ith::World (ADR-0014 rule 19): random spawns, despawns, adds,
// removes, query writes and clears, checked after every step against a simple reference model
// (a map from handle to optional components). The seeds are fixed, so a failure reproduces.

#include "ith/ecs/World.h"

#include "EcsComponents.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

using ith::Entity;
using ith::World;
using ith::test::Health;
using ith::test::Name;
using ith::test::Position;
using ith::test::Tag0;

struct ModelEntity {
    std::optional<std::int32_t> health;
    std::optional<std::string> name;
    std::optional<float> position;
    bool tag = false;
};

class ModelTest {
public:
    explicit ModelTest(std::uint32_t seed) : m_random(seed) {}

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
            spawned(m_world.spawn(Position{.x = position}, Tag0{}, Health{.value = health}),
                    {.health = health, .position = position, .tag = true});
        } else if (operation < 50) {
            const Entity entity = anyHandle();
            const bool alive = m_model.erase(entity.bits()) == 1;
            CHECK(m_world.despawn(entity) == alive);
        } else if (operation < 75) {
            add(anyHandle());
        } else if (operation < 95) {
            remove(anyHandle());
        } else if (operation < 99) {
            m_world.query<Health, const Position>().forEach([](Health& health, const Position& position) {
                health.value += static_cast<std::int32_t>(position.x);
            });
            for (auto& [bits, entity] : m_model) {
                if (entity.health && entity.position) {
                    *entity.health += static_cast<std::int32_t>(*entity.position);
                }
            }
        } else if (pick(4) == 0) {
            m_world.clear();
            m_model.clear();
        }
    }

    void add(Entity entity) {
        const auto found = m_model.find(entity.bits());
        ModelEntity* model = found != m_model.end() ? &found->second : nullptr;
        const std::uint32_t component = pick(4);
        bool added = false;
        if (component == 0) {
            const auto value = static_cast<std::int32_t>(pick(1000));
            added = m_world.add(entity, Health{.value = value}) != nullptr;
            if (model != nullptr) {
                model->health = value;
            }
        } else if (component == 1) {
            std::string name = nextName();
            added = m_world.add(entity, Name{.text = name}) != nullptr;
            if (model != nullptr) {
                model->name = name;
            }
        } else if (component == 2) {
            const auto value = static_cast<float>(pick(1000));
            added = m_world.add(entity, Position{.x = value}) != nullptr;
            if (model != nullptr) {
                model->position = value;
            }
        } else {
            added = m_world.add(entity, Tag0{}) != nullptr;
            if (model != nullptr) {
                model->tag = true;
            }
        }
        CHECK(added == (model != nullptr));
    }

    void remove(Entity entity) {
        const auto found = m_model.find(entity.bits());
        ModelEntity* model = found != m_model.end() ? &found->second : nullptr;
        const std::uint32_t component = pick(4);
        bool had = false;
        bool removed = false;
        if (component == 0) {
            had = model != nullptr && model->health.has_value();
            removed = m_world.remove<Health>(entity);
            if (model != nullptr) {
                model->health.reset();
            }
        } else if (component == 1) {
            had = model != nullptr && model->name.has_value();
            removed = m_world.remove<Name>(entity);
            if (model != nullptr) {
                model->name.reset();
            }
        } else if (component == 2) {
            had = model != nullptr && model->position.has_value();
            removed = m_world.remove<Position>(entity);
            if (model != nullptr) {
                model->position.reset();
            }
        } else {
            had = model != nullptr && model->tag;
            removed = m_world.remove<Tag0>(entity);
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
        }

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
    }

    std::mt19937 m_random;
    World m_world;
    std::map<std::uint64_t, ModelEntity> m_model;
    std::vector<Entity> m_issued;
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
