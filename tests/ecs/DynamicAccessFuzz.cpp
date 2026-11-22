// libFuzzer target for the ECS paths that take type records (ADR-0014 rule 19: the dynamic paths
// are fuzzed): spawn, despawn, add, remove, has and get by record, forEachComponent, dynamic
// queries over any set of terms, resources by record, and clear. The input is read as a sequence
// of operations, checked against a reference model of which types each entity has and of each
// Counter's value. Built only in Clang sanitizer builds; CTest runs a short smoke run, and longer
// runs are started by hand:
//
//   ith_ecs_dynamic_fuzz -max_total_time=600 <corpus directory>

#include "ith/ecs/World.h"
#include "ith/reflection/TypeOf.h"

#include "EcsComponents.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace {

using ith::Entity;

// The component types operations choose from. Counter's value is modelled.
const std::array<const ith::TypeInfo*, 8> kTypes = {
    &ith::typeOf<ith::test::Counter>(),  &ith::typeOf<ith::test::Health>(),    &ith::typeOf<ith::test::Name>(),
    &ith::typeOf<ith::test::Position>(), &ith::typeOf<ith::test::Inventory>(), &ith::typeOf<ith::test::Tag0>(),
    &ith::typeOf<ith::test::Tag1>(),     &ith::typeOf<ith::test::Tag2>(),
};
constexpr std::size_t kCounter = 0;

struct ModelEntity {
    std::uint32_t types = 0; // bit i: has kTypes[i]
    std::int64_t counter = 0;
};

void check(bool condition) {
    if (!condition) {
        __builtin_trap();
    }
}

class Input {
public:
    explicit Input(std::span<const std::uint8_t> data) : m_data(data) {}

    [[nodiscard]] bool empty() const { return m_position == m_data.size(); }

    std::uint8_t next() { return empty() ? 0 : m_data[m_position++]; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_position = 0;
};

class Fuzz {
public:
    void run(Input& input) {
        while (!input.empty()) {
            step(input);
        }
        checkAll();
    }

private:
    Entity anyHandle(Input& input) {
        if (m_issued.empty()) {
            return {};
        }
        const auto low = static_cast<std::size_t>(input.next());
        const auto high = static_cast<std::size_t>(input.next());
        return m_issued[((high << 8) | low) % m_issued.size()];
    }

    ModelEntity* model(Entity entity) {
        const auto found = m_model.find(entity.bits());
        return found != m_model.end() ? &found->second : nullptr;
    }

    void step(Input& input) {
        const std::uint8_t operation = input.next() % 10;
        const std::size_t type = input.next() % kTypes.size();
        const std::uint32_t bit = 1u << type;
        if (operation == 0) {
            const Entity entity = m_world.spawn();
            check(!entity.isNull());
            m_model[entity.bits()] = {};
            m_issued.push_back(entity);
        } else if (operation == 1) {
            const Entity entity = anyHandle(input);
            check(m_world.despawn(entity) == (m_model.erase(entity.bits()) == 1));
        } else if (operation == 2) {
            const Entity entity = anyHandle(input);
            ModelEntity* entry = model(entity);
            const void* added = m_world.add(entity, *kTypes[type]);
            void* value = added != nullptr ? m_world.get(entity, *kTypes[type]) : nullptr;
            check((value != nullptr) == (entry != nullptr));
            if (entry != nullptr) {
                entry->types |= bit;
                if (type == kCounter) {
                    const std::int64_t counter = input.next();
                    static_cast<ith::test::Counter*>(value)->value = counter;
                    entry->counter = counter;
                }
            }
        } else if (operation == 3) {
            const Entity entity = anyHandle(input);
            ModelEntity* entry = model(entity);
            const bool had = entry != nullptr && (entry->types & bit) != 0;
            check(m_world.remove(entity, *kTypes[type]) == had);
            if (entry != nullptr) {
                entry->types &= ~bit;
            }
        } else if (operation == 4) {
            const Entity entity = anyHandle(input);
            const ModelEntity* entry = model(entity);
            const bool has = entry != nullptr && (entry->types & bit) != 0;
            check(m_world.has(entity, *kTypes[type]) == has);
            check((m_world.get(entity, *kTypes[type]) != nullptr) == has);
        } else if (operation == 5) {
            const std::uint8_t mask = input.next();
            query(mask, input.next() % 2 == 0);
        } else if (operation == 6) {
            checkEntity(anyHandle(input));
        } else if (operation == 7) {
            const ith::TypeInfo& clock = ith::typeOf<ith::test::GameClock>();
            if (input.next() % 2 == 0) {
                check(m_world.insertResource(clock) != nullptr);
                m_hasClock = true;
            } else {
                check(m_world.removeResource(clock) == m_hasClock);
                m_hasClock = false;
            }
            check((m_world.resource(clock) != nullptr) == m_hasClock);
        } else if (operation == 8 && input.next() % 8 == 0) {
            m_world.clear();
            m_model.clear();
        } else {
            checkAll();
        }
        check(m_world.entityCount() == m_model.size());
    }

    // Runs a dynamic query over the types in `mask`; with `write`, it increments every Counter.
    void query(std::uint8_t mask, bool write) {
        std::vector<ith::QueryTerm> terms;
        bool countsCounter = false;
        for (std::size_t type = 0; type < kTypes.size(); ++type) {
            if ((mask & (1u << type)) != 0) {
                terms.push_back({.type = kTypes[type], .access = write ? ith::Access::Write : ith::Access::Read});
                countsCounter = countsCounter || type == kCounter;
            }
        }
        std::size_t visits = 0;
        m_world.dynamicQuery(terms).forEach([&](Entity entity, std::span<void* const> values) {
            ++visits;
            const ModelEntity* entry = model(entity);
            check(entry != nullptr && (entry->types & mask) == mask && values.size() == terms.size());
            for (std::size_t term = 0; term < terms.size(); ++term) {
                check(values[term] == m_world.get(entity, *terms[term].type));
            }
            if (write && countsCounter) {
                ++static_cast<ith::test::Counter*>(m_world.get(entity, *kTypes[kCounter]))->value;
            }
        });
        std::size_t expected = 0;
        for (auto& [bits, entry] : m_model) {
            if ((entry.types & mask) == mask) {
                ++expected;
                if (write && countsCounter) {
                    ++entry.counter;
                }
            }
        }
        check(visits == expected);
    }

    void checkEntity(Entity entity) {
        const ModelEntity* entry = model(entity);
        check(m_world.isAlive(entity) == (entry != nullptr));
        std::size_t visits = 0;
        m_world.forEachComponent(entity, [&](const ith::TypeInfo& type, void* value) {
            ++visits;
            check(value == m_world.get(entity, type));
        });
        if (entry == nullptr) {
            check(visits == 0);
            return;
        }
        check(visits == static_cast<std::size_t>(std::popcount(entry->types)));
        for (std::size_t type = 0; type < kTypes.size(); ++type) {
            check(m_world.has(entity, *kTypes[type]) == ((entry->types & (1u << type)) != 0));
        }
        if ((entry->types & (1u << kCounter)) != 0) {
            check(static_cast<const ith::test::Counter*>(m_world.get(entity, *kTypes[kCounter]))->value ==
                  entry->counter);
        }
    }

    void checkAll() {
        for (const Entity entity : m_issued) {
            checkEntity(entity);
        }
    }

    ith::World m_world;
    std::map<std::uint64_t, ModelEntity> m_model;
    std::vector<Entity> m_issued;
    bool m_hasClock = false;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Input input(std::span<const std::uint8_t>(data, size));
    Fuzz fuzz;
    fuzz.run(input);
    return 0;
}
