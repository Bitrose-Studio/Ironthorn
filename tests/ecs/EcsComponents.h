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

// An immutable component (ADR-0030): it changes only by being replaced.
struct StableId {
    ITH_TYPE(Component, Immutable)
    ITH_FIELD()
    std::uint64_t value = 0;
};

// Hooks are set once per process before a World stores the type, so the hook tests have their own
// types, which no other test uses. HookTests.cpp sets their hooks.

// Its hooks log "Hooked.<hook>:<value>" into the HookLog resource.
struct Hooked {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int32_t value = 0;
};

// Its hooks log "HookedOther.<hook>:<value>" into the HookLog resource.
struct HookedOther {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int32_t value = 0;
};

// Its onAdd hook records commands: give the entity a Health and spawn an entity with Hooked{99}.
struct Spawner {
    ITH_TYPE(Component)
};

// Its onAdd hook tries to change the world's structure directly, which is refused.
struct StructureProbe {
    ITH_TYPE(Component)
};

// Stored by a World before any hooks are set, to check that setting them later is refused.
struct LateHooked {
    ITH_TYPE(Component)
};

// The events the hooks of the hook tests saw, in order.
struct HookLog {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::vector<std::string> events;
};

// An immutable component with hooks that count into HookCounts, for the model test.
struct Tracked {
    ITH_TYPE(Component, Immutable)
    ITH_FIELD()
    std::int64_t value = 0;
};

struct HookCounts {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::int64_t adds = 0;
    ITH_FIELD()
    std::int64_t inserts = 0;
    ITH_FIELD()
    std::int64_t replaces = 0;
    ITH_FIELD()
    std::int64_t removes = 0;
    ITH_FIELD()
    std::int64_t insertedSum = 0; // the values onInsert saw
    ITH_FIELD()
    std::int64_t removedSum = 0; // the values onRemove saw
};

} // namespace ith::test
