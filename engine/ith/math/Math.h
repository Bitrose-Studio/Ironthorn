#pragma once

// The engine's math functions: glm's core and stable extensions, and the projections glm lacks
// (ADR-0006).
//
//   const ith::Mat4 view = glm::lookAt(eye, target, ith::kWorldUp);
//   const ith::Mat4 projection = ith::perspectiveReversedInfinite(glm::radians(60.0f), aspect, 0.1f);
//   const ith::Mat4 viewProjection = projection * view;
//
// Include this header, never <glm/...>: it brings glm's functions in the engine's configuration
// (glm/glm.hpp and the gtc extensions for quaternions, matrix transforms, constants, epsilon
// comparisons and value_ptr). Experimental gtx headers are not available (ADR-0006 rule 3).
//
// Projections, all for the engine's clip space: x and y in [-1, 1] with +Y up, depth in [0, 1],
// on every backend (ADR-0006 rule 6).
// - Scene cameras use perspectiveReversedInfinite: reversed depth (1 at the near plane, towards 0
//   far away) with no far plane. Pair it with a 32-bit float depth buffer cleared to 0 and a
//   greater-or-equal depth test.
// - Shadow maps and other conventional-depth views use glm::perspective and glm::ortho, which map
//   the near plane to 0 and the far plane to 1 in this configuration.
// - glm::lookAt builds a right-handed view matrix: the camera looks down its -Z.

#include "ith/math/Types.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ith {

// A right-handed perspective projection with reversed depth and an infinite far plane: a point at
// distance d in front of the camera (view-space z = -d) gets depth near / d, which is 1 at the
// near plane and approaches 0 as d grows. Clip w is d. `verticalFov` is the full vertical field of
// view in radians, `aspect` is width / height.
// Preconditions (asserted): 0 < verticalFov < pi, aspect > 0, nearPlane > 0.
[[nodiscard]] Mat4 perspectiveReversedInfinite(float verticalFov, float aspect, float nearPlane) noexcept;

} // namespace ith
