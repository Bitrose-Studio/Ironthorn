#pragma once

// Component and resource types for the ECS tests and benchmark. The build generates their
// records into the test library ith_test_ecs_components (module name "test_ecs").

#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ith::test {

struct Position {
    ITH_TYPE(Component)
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;
};

struct Velocity {
    ITH_TYPE(Component)
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;
};

struct Health {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int32_t value = 100;
};

// Owns heap memory, so moves and destruction must go through the type's record.
struct Name {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::string text;
};

struct Inventory {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::vector<std::string> items;
};

struct Counter {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int64_t value = 0;
};

// Tags: components without data, used to spread entities over many archetypes.
struct Tag0 {
    ITH_TYPE(Component)
};

struct Tag1 {
    ITH_TYPE(Component)
};

struct Tag2 {
    ITH_TYPE(Component)
};

struct Tag3 {
    ITH_TYPE(Component)
};

struct Tag4 {
    ITH_TYPE(Component)
};

struct Tag5 {
    ITH_TYPE(Component)
};

struct Tag6 {
    ITH_TYPE(Component)
};

struct Tag7 {
    ITH_TYPE(Component)
};

// Not a component: reflected with the Struct role.
struct NotAComponent {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t value = 0;
};

// Resources: typed singletons of a world.
struct GameClock {
    ITH_TYPE(Resource)
    ITH_FIELD()
    double seconds = 0.0;
    ITH_FIELD()
    std::uint64_t frame = 0;
};

// A resource with a math field.
struct Gravity {
    ITH_TYPE(Resource)
    ITH_FIELD()
    ith::Vec3 acceleration{0.0f, 0.0f, -9.81f};
};

// Owns heap memory, so replacing and destroying it must go through the type's record.
struct MatchRules {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::string mode = "default rules, long enough to live on the heap";
    ITH_FIELD()
    std::vector<std::string> maps;
};

} // namespace ith::test
