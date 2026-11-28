#pragma once

// Component types for the editor_core tests. The build generates their records into the test
// library ith_test_editor_core_components (module name "test_editor_core").

#include "ith/assets/AssetRef.h"
#include "ith/foundation/Entity.h"
#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith::test::editor {

// Plain values of several kinds, one of them read-only in the editor.
struct Lamp {
    ITH_TYPE(Component)
    ITH_FIELD()
    float intensity = 1.0f;
    ITH_FIELD()
    std::string note;
    ITH_FIELD()
    std::vector<std::int32_t> steps;
    ITH_FIELD(ReadOnly)
    std::int32_t serial = 0;
};

// A reference to another entity, written as its stable id.
struct Aim {
    ITH_TYPE(Component)
    ITH_FIELD()
    Entity target;
};

// How a Gadget runs.
enum class GadgetMode : std::uint8_t {
    Off,
    Low,
    High,
};
ITH_ENUM(GadgetMode)

// A struct inside a component, in an optional and in a list.
struct GadgetPart {
    ITH_TYPE(Struct)
    ITH_FIELD()
    bool on = true;
    ITH_FIELD(Min = 0, Max = 10)
    std::int32_t level = 3;
};

// Every kind of value the Details panel edits, with ranges, a tooltip and categories.
struct Gadget {
    ITH_TYPE(Component, Tooltip = "A test gadget")
    ITH_FIELD(Min = 0, Max = 100, Tooltip = "How strong it is")
    float power = 50.0f;
    ITH_FIELD(Min = -1, Max = 1)
    Vec3 offset{0.0f};
    ITH_FIELD()
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    ITH_FIELD()
    GadgetMode mode = GadgetMode::Low;
    ITH_FIELD()
    GadgetPart part;
    ITH_FIELD()
    std::optional<float> boost;
    ITH_FIELD()
    std::optional<GadgetPart> extra;
    ITH_FIELD()
    std::array<float, 2> pair{};
    ITH_FIELD()
    std::vector<GadgetPart> parts;
    ITH_FIELD(Category = "Links")
    Entity link;
    ITH_FIELD(Category = "Links")
    AssetRef asset;
};

// A world resource a game changes while it plays (Play In Editor: every session starts from the
// editor world's value).
struct Score {
    ITH_TYPE(Resource)
    ITH_FIELD()
    std::int32_t points = 0;
};

} // namespace ith::test::editor
