#pragma once

// ith::EditorLabel: the name the Outliner shows for an entity (ADR-0037 rule 14).
//
//   world.add(door, ith::EditorLabel{.name = "Front door"});
//
// Editor-only data (ADR-0017 rule 8): saved in the entity's "editor" section of a level, left out
// of Play In Editor copies and of cooked levels. An entity without one shows its stable id.

#include "ith/reflection/Annotations.h"

#include <string>

namespace ith {

struct EditorLabel {
    ITH_TYPE(Component, EditorOnly)
    // Any UTF-8 text; may be empty.
    ITH_FIELD()
    std::string name;
};

} // namespace ith
