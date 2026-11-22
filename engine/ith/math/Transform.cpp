#include "ith/math/Transform.h"

#include "ith/math/Math.h"

#include <cmath>

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

namespace {

// Below this squared length a vector counts as zero.
constexpr float kZeroLength2 = 1e-20f;

bool isZero(const Vec3& vector) noexcept {
    return glm::dot(vector, vector) <= kZeroLength2;
}

// A unit vector orthogonal to the unit vector `axis`.
Vec3 anyOrthogonal(const Vec3& axis) noexcept {
    const Vec3 other = std::abs(axis.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(axis, other));
}

} // namespace

Transform decompose(const Mat4& matrix) noexcept {
    const Vec3 x(matrix[0]);
    const Vec3 y(matrix[1]);
    const Vec3 z(matrix[2]);

    // An orthonormal, right-handed basis from the columns: x first, then the part of y orthogonal
    // to it, then their cross product. A zero column takes a direction that keeps the other
    // columns: a zero x lies along y cross z, a zero y along z cross x.
    Vec3 xAxis(1.0f, 0.0f, 0.0f);
    if (!isZero(x)) {
        xAxis = glm::normalize(x);
    } else if (!isZero(glm::cross(y, z))) {
        xAxis = glm::normalize(glm::cross(y, z));
    } else if (!isZero(y)) {
        xAxis = anyOrthogonal(glm::normalize(y));
    } else if (!isZero(z)) {
        xAxis = anyOrthogonal(glm::normalize(z));
    }
    const Vec3 yRest = y - (glm::dot(y, xAxis) * xAxis);
    const Vec3 zRest = z - (glm::dot(z, xAxis) * xAxis);
    Vec3 yAxis = anyOrthogonal(xAxis);
    if (!isZero(yRest)) {
        yAxis = glm::normalize(yRest);
    } else if (!isZero(zRest)) {
        yAxis = glm::normalize(glm::cross(zRest, xAxis));
    }
    const Vec3 zAxis = glm::cross(xAxis, yAxis);

    Transform transform;
    transform.translation = Vec3(matrix[3]);
    transform.rotation = glm::normalize(glm::quat_cast(Mat3(xAxis, yAxis, zAxis)));
    // The scales are the diagonal of the triangular rest; z's sign carries a mirroring.
    transform.scale = Vec3(glm::dot(x, xAxis), glm::dot(y, yAxis), glm::dot(z, zAxis));
    return transform;
}

} // namespace ith
