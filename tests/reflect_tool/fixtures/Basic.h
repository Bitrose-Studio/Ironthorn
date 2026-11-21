#pragma once

// A fixture for the generator's golden test (golden/Basic.reflect.cpp.golden). It is only read
// by the scanner, never compiled. It mixes reflected declarations with C++ the scanner must
// step over: comments and strings that mention ITH_TYPE, templates, functions, #if blocks
// without annotations, unreflected types and enums.

#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <string>
#include <vector>

#define FIXTURE_MACRO(x) ITH_TYPE(x) // a macro definition is not an annotation

/* ITH_TYPE(Component) in a block comment is not an annotation */

namespace game {

constexpr const char* kText = "ITH_FIELD() in a string";
constexpr const char* kRaw = R"tag(ITH_TYPE(Struct) "quoted" )tag";

template <typename T>
struct Box {
    T value{};
};

enum class Unreflected {
    A,
    B
};

#if defined(FIXTURE_OPTION)
inline int optional() {
    return 1;
}
#endif

enum class DamageKind : std::uint8_t {
    Physical,
    Fire = 4,
    Poison = Fire + 1,
};
ITH_ENUM(DamageKind, Tooltip = "How damage is dealt")

struct Health {
    ITH_TYPE(Component, Category = "Combat", FormerName = "game::Hp")
    ITH_FIELD(Min = 0, Max = 10000, Tooltip = "Hit points", Replicated, SaveGame)
    float current = 100.0f;
    ITH_FIELD(Min = 1, Max = 10000, Unit = "hp", FormerName = "maxHp")
    float maximum = 100.0f;
    float cachedRatio = 1.0f; // not reflected

    float ratio() const { return maximum > 0.0f ? current / maximum : 0.0f; }
};

namespace detail::inner {

class Tagged final {
    ITH_TYPE(Asset, Meta(Icon = "tag", Order = "2"))
public:
    int publicNumber() const { return m_number; }

private:
    ITH_FIELD(ReadOnly, DisplayName = "Number", Meta(Widget = "Spin"))
    int m_number = 0;
    ITH_FIELD(Transient)
    std::vector<std::string> m_names{"a", "b"};
};

} // namespace detail::inner

struct Empty {
    ITH_TYPE(Event)
};

} // namespace game

struct Global {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::vector<game::DamageKind> kinds = {game::DamageKind::Fire, game::DamageKind::Poison};
    ITH_FIELD(Min = -5)
    game::Box<int> boxed;
};

enum class GlobalFlags : std::uint32_t {
    None = 0,
    A = 1u << 0,
    B = 1u << 1,
};
ITH_ENUM(GlobalFlags, Flags)
