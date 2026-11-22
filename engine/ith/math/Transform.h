#pragma once

// ith::Transform: a translation, a rotation and a scale, applied to a point in the order scale,
// rotation, translation (ADR-0006 rule 5).
//
//   const ith::Transform transform{.translation = {0.0f, 1.0f, 0.0f},
//                                  .rotation = glm::angleAxis(ith::kHalfPi, ith::kWorldUp)};
//   const ith::Vec3 p = transformPoint(transform, ith::Vec3(1.0f, 0.0f, 0.0f)); // (0, 1, -1)
//   const ith::Mat4 world = parentWorld * toMatrix(transform);
//
// A plain value, reflected like the other math types (ith/reflection/TypeOf.h). The ECS component
// that places entities belongs to the ECS, which may hold one of these. Hierarchies compose
// matrices (world = parent * local): the product of two transforms with non-uniform scale is not
// a transform in general.

#include "ith/math/Types.h"

namespace ith {

struct Transform {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    // A unit quaternion. The default is the identity.
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    friend bool operator==(const Transform&, const Transform&) = default;
};

// The matrix translation * rotation * scale.
[[nodiscard]] Mat4 toMatrix(const Transform& transform) noexcept;

// A point moved by the transform: scaled, rotated, then translated.
[[nodiscard]] Vec3 transformPoint(const Transform& transform, const Vec3& point) noexcept;

// A direction moved by the transform: scaled and rotated, not translated.
[[nodiscard]] Vec3 transformVector(const Transform& transform, const Vec3& vector) noexcept;

} // namespace ith
