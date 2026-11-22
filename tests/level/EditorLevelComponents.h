#pragma once

// EditorOnly component types for the level tests. Generated into the test library
// ith_test_level_components (module name "test_level_editor") as an editor module, the only kind
// that may use EditorOnly.

#include "ith/reflection/Annotations.h"

#include <string>

namespace ith::test::level {

// What the editor keeps about an entity: saved in the record's "editor" section.
struct EditorNote {
    ITH_TYPE(Component, EditorOnly)
    ITH_FIELD()
    std::string text;
};

// A second one, to check the order of the "editor" section.
struct EditorColor {
    ITH_TYPE(Component, EditorOnly)
    ITH_FIELD()
    float hue = 0.0f;
};

} // namespace ith::test::level
