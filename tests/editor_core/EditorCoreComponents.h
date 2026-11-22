#pragma once

// Component types for the editor_core tests. The build generates their records into the test
// library ith_test_editor_core_components (module name "test_editor_core").

#include "ith/foundation/Entity.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>
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

} // namespace ith::test::editor
