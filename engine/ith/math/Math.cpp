#include "ith/math/Math.h"

#include "ith/foundation/Assert.h"

#include <cmath>

namespace ith {

Mat4 perspectiveReversedInfinite(float verticalFov, float aspect, float nearPlane) noexcept {
    ITH_ASSERT_MSG(verticalFov > 0.0f && verticalFov < kPi, "the vertical field of view must be in (0, pi)");
    ITH_ASSERT_MSG(aspect > 0.0f, "the aspect ratio must be positive");
    ITH_ASSERT_MSG(nearPlane > 0.0f, "the near plane must be in front of the camera");
    const float f = 1.0f / std::tan(0.5f * verticalFov);
    // Column vectors: clip.x = f / aspect * x, clip.y = f * y, clip.z = near, clip.w = -z.
    Mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;
    m[3][2] = nearPlane;
    return m;
}

} // namespace ith
