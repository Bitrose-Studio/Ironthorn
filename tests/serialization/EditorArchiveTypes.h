#pragma once

// Reflected types with EditorOnly data, for the serialization tests. Generated into the test
// library ith_test_archive_editor (module name "test_archive_editor") as an editor module, the
// only kind that may use EditorOnly.

#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <string>

namespace ith::test::archive {

// A type that exists only in the editor.
struct EditorNote {
    ITH_TYPE(Struct, EditorOnly)
    ITH_FIELD()
    std::string text;
};

// A type with an EditorOnly field and a field of an EditorOnly type.
struct Annotated {
    ITH_TYPE(Component)
    ITH_FIELD()
    std::int32_t value = 0;
    ITH_FIELD(EditorOnly)
    std::string label;
    ITH_FIELD()
    EditorNote note;
};

} // namespace ith::test::archive
