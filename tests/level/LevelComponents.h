#pragma once

// Component types for the level tests and the loadLevel fuzz target. The build generates their
// records into the test library ith_test_level_components (module name "test_level").

#include "ith/foundation/Entity.h"
#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith::test::level {

enum class Shape : std::uint8_t {
    Point,
    Spot,
    Area,
};
ITH_ENUM(Shape)

// Plain data of several kinds: floats, a math type, a string, an enum, an optional and a vector.
struct Light {
    ITH_TYPE(Component)
    ITH_FIELD()
    float intensity = 1.0f;
    ITH_FIELD()
    ith::Vec3 color{1.0f, 1.0f, 1.0f};
    ITH_FIELD()
    std::string label;
    ITH_FIELD()
    Shape shape = Shape::Point;
    ITH_FIELD()
    std::optional<float> range;
    ITH_FIELD()
    std::vector<float> flicker;
};

// One leg of a route: an entity reference inside a nested struct.
struct Leg {
    ITH_TYPE(Struct)
    ITH_FIELD()
    Entity to;
    ITH_FIELD()
    float wait = 0.0f;
};

// Entity references: a field, a vector and a vector of nested structs.
struct Target {
    ITH_TYPE(Component)
    ITH_FIELD()
    Entity entity;
    ITH_FIELD()
    std::vector<Entity> others;
    ITH_FIELD()
    std::vector<Leg> route;
};

// A component without data.
struct Marker {
    ITH_TYPE(Component)
};

// Its hooks are set by the hook test (LevelTests.cpp), which records what each run saw.
struct Probe {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int32_t value = 0;
};

// An immutable component: loading must add it through World::add.
struct Sealed {
    ITH_TYPE(Component, Immutable)
    ITH_FIELD()
    std::uint64_t value = 0;
};

// Renamed from "ith::test::level::OldName": files that use the old name still read.
struct Renamed {
    ITH_TYPE(Component, FormerName = "ith::test::level::OldName")
    ITH_FIELD()
    std::int32_t value = 0;
};

// Not a component: reflected with the Struct role.
struct NotAComponent {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::int32_t value = 0;
};

// A Transient component: never saved.
struct Scratch {
    ITH_TYPE(Component, Transient)
    ITH_FIELD()
    std::int32_t value = 0;
};

// A world resource: levels don't save resources, and loading keeps them.
struct LevelSettings {
    ITH_TYPE(Resource)
    ITH_FIELD()
    float gravity = -9.81f;
};

} // namespace ith::test::level
