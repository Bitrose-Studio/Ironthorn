#include "ith/math/Transform.h"

#include "ith/math/Math.h"

namespace ith {

Mat4 toMatrix(const Transform& transform) noexcept {
    Mat4 m = glm::mat4_cast(transform.rotation);
    m[0] *= transform.scale.x;
    m[1] *= transform.scale.y;
    m[2] *= transform.scale.z;
    m[3] = Vec4(transform.translation, 1.0f);
    return m;
}

Vec3 transformPoint(const Transform& transform, const Vec3& point) noexcept {
    return transform.translation + transform.rotation * (transform.scale * point);
}

Vec3 transformVector(const Transform& transform, const Vec3& vector) noexcept {
    return transform.rotation * (transform.scale * vector);
}

} // namespace ith
