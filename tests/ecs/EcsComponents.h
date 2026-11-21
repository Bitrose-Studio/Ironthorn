#pragma once

// Component types for the ECS tests and benchmark. The build generates their records into the
// test library ith_test_ecs_components (module name "test_ecs").

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

} // namespace ith::test
