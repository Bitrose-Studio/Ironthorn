#pragma once

// A reflected component with math fields, for the math reflection tests. The build generates its
// record into the test library ith_test_math_reflected (module name "test_math").

#include "ith/math/Transform.h"
#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <vector>

namespace ith::test {

struct Placement {
    ITH_TYPE(Component)
    ITH_FIELD()
    Transform local;
    ITH_FIELD()
    Vec3 velocity;
    ITH_FIELD()
    Quat spin;
    ITH_FIELD()
    Mat4 world;
    ITH_FIELD()
    IVec2 cell;
    ITH_FIELD()
    DVec3 precise;
    ITH_FIELD()
    std::vector<Vec2> path;
};

} // namespace ith::test
