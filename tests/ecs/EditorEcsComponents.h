#pragma once

// EditorOnly types for the world copy tests. Generated into the test library
// ith_test_ecs_components (module name "test_ecs_editor") as an editor module, the only kind that
// may use EditorOnly.

#include "ith/reflection/Annotations.h"

#include <string>

namespace ith::test {

// What the editor keeps about an entity, which a Play In Editor copy leaves out.
struct EditorLabel {
    ITH_TYPE(Component, EditorOnly)
    ITH_FIELD()
    std::string text;
};

// Editor state of a world.
struct EditorSelection {
    ITH_TYPE(Resource, EditorOnly)
    ITH_FIELD()
    std::string selected;
};

} // namespace ith::test
