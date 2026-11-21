#pragma once

// A second header of the test module: types that use types of ReflectedTypes.h, in a nested
// namespace and in the global namespace.

#include "ith/reflection/Annotations.h"

#include "ReflectedTypes.h"

#include <cstdint>
#include <vector>

namespace ith::test::gear {

struct Loadout {
    ITH_TYPE(Component, Category = "Gear")
    ITH_FIELD(Tooltip = "Damage of each slot")
    std::vector<DamageKind> slots;
    ITH_FIELD()
    std::vector<Hit> history;
};

} // namespace ith::test::gear

// A reflected type in the global namespace.
struct GlobalCounter {
    ITH_TYPE(Resource)
    ITH_FIELD(Min = 0)
    std::uint64_t value = 0;
};
