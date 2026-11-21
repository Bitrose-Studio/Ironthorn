#pragma once

// Reflected types for the reflection tests: every role, every supported field type, attributes,
// private fields and a renamed type. The build generates their records into the test library
// ith_test_reflected (module name "test_reflected").

#include "ith/foundation/Uuid.h"
#include "ith/reflection/Annotations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith::test {

enum class DamageKind : std::uint8_t {
    Physical,
    Fire,
    Poison = 7,
};
ITH_ENUM(DamageKind, Tooltip = "How damage is dealt")

enum class Abilities : std::uint32_t {
    None = 0,
    Jump = 1u << 0,
    Swim = 1u << 1,
    Climb = 1u << 2,
};
ITH_ENUM(Abilities, Flags, DisplayName = "Movement abilities")

enum class Altitude : std::int16_t {
    Deep = -300,
    Sea = 0,
    Peak = 300,
};
ITH_ENUM(Altitude)

struct Vector3 {
    ITH_TYPE(Struct, DisplayName = "Vector 3")
    ITH_FIELD()
    float x = 0.0f;
    ITH_FIELD()
    float y = 0.0f;
    ITH_FIELD()
    float z = 0.0f;
};

struct Health {
    ITH_TYPE(Component, Category = "Combat", Tooltip = "Hit points of an entity")
    ITH_FIELD(Min = 0, Max = 10000, Unit = "hp", Tooltip = "Hit points", Replicated, SaveGame)
    float current = 100.0f;
    ITH_FIELD(Min = 1, Max = 10000, FormerName = "maxHealth")
    float maximum = 100.0f;
    float cachedRatio = 1.0f; // not reflected
};

struct AllLeaves {
    ITH_TYPE(Struct)
    ITH_FIELD()
    bool flag = false;
    ITH_FIELD()
    std::int8_t i8 = 0;
    ITH_FIELD()
    std::int16_t i16 = 0;
    ITH_FIELD()
    std::int32_t i32 = 0;
    ITH_FIELD()
    std::int64_t i64 = 0;
    ITH_FIELD()
    std::uint8_t u8 = 0;
    ITH_FIELD()
    std::uint16_t u16 = 0;
    ITH_FIELD()
    std::uint32_t u32 = 0;
    ITH_FIELD()
    std::uint64_t u64 = 0;
    ITH_FIELD()
    float f32 = 0.0f;
    ITH_FIELD()
    double f64 = 0.0;
    ITH_FIELD()
    std::string text;
    ITH_FIELD()
    Uuid id;
};

struct Containers {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::array<std::int32_t, 3> fixed{};
    ITH_FIELD()
    std::vector<std::string> names;
    ITH_FIELD()
    std::vector<Vector3> points;
    ITH_FIELD()
    std::optional<double> maybe;
    ITH_FIELD()
    std::optional<Vector3> maybePoint;
    ITH_FIELD()
    std::vector<std::vector<std::int32_t>> nested;
    ITH_FIELD()
    std::vector<DamageKind> kinds;
};

struct Hit {
    ITH_TYPE(Event, FormerName = "ith::test::Damage")
    ITH_FIELD()
    DamageKind kind = DamageKind::Physical;
    ITH_FIELD()
    Abilities abilities = Abilities::None;
    ITH_FIELD()
    Altitude altitude = Altitude::Sea;
    ITH_FIELD()
    Vector3 where;
    ITH_FIELD()
    Health target;
    ITH_FIELD(Min = -1.5, Max = +2.5e3)
    double amount = 0.0;
};

// A class whose reflected fields are private.
class Inventory {
    ITH_TYPE(Asset, Meta(Icon = "chest", Order = "3"))
public:
    [[nodiscard]] std::int32_t capacity() const { return m_capacity; }

    [[nodiscard]] const std::vector<Uuid>& items() const { return m_items; }

private:
    ITH_FIELD(ReadOnly, Meta(Widget = "Spin"))
    std::int32_t m_capacity = 10;
    ITH_FIELD(Transient)
    std::vector<Uuid> m_items;
};

// A component without reflected fields: a tag.
struct Marker {
    ITH_TYPE(Component)
};

} // namespace ith::test
