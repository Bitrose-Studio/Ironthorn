#include "ith/math/Geometry.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ith {
namespace {

// Row `index` of a column-major matrix.
Vec4 row(const Mat4& m, int index) noexcept {
    return {m[0][index], m[1][index], m[2][index], m[3][index]};
}

// The plane dot(xyz, p) + w >= 0, scaled to a unit normal. A plane with a zero normal (the far
// plane of an infinite projection) is kept as it is: its sign alone decides.
Plane planeFrom(const Vec4& coefficients) noexcept {
    const Vec3 normal(coefficients);
    const float length = glm::length(normal);
    if (length <= 0.0f) {
        return {.normal = normal, .distance = coefficients.w};
    }
    return {.normal = normal / length, .distance = coefficients.w / length};
}

bool isEmpty(const Aabb& box) noexcept {
    return box.min.x > box.max.x || box.min.y > box.max.y || box.min.z > box.max.z;
}

// Whether the ray has a direction; asserts that it does.
bool hasDirection(const Ray& ray) noexcept {
    const bool valid = ray.direction != Vec3(0.0f);
    ITH_ASSERT_MSG(valid, "a ray needs a non-zero direction");
    return valid;
}

} // namespace

Plane makePlane(const Vec3& normal, const Vec3& point) noexcept {
    ITH_ASSERT_MSG(normal != Vec3(0.0f), "a plane needs a non-zero normal");
    const Vec3 unit = glm::normalize(normal);
    return {.normal = unit, .distance = -glm::dot(unit, point)};
}

Frustum makeFrustum(const Mat4& clipFromSpace) noexcept {
    const Vec4 x = row(clipFromSpace, 0);
    const Vec4 y = row(clipFromSpace, 1);
    const Vec4 z = row(clipFromSpace, 2);
    const Vec4 w = row(clipFromSpace, 3);
    return {.planes = {planeFrom(w + x), planeFrom(w - x), planeFrom(w + y), planeFrom(w - y), planeFrom(z),
                       planeFrom(w - z)}};
}

bool intersects(const Frustum& frustum, const Aabb& box) noexcept {
    if (isEmpty(box)) {
        return false;
    }
    for (const Plane& plane : frustum.planes) {
        // The corner furthest along the plane's normal.
        const Vec3 corner(plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                          plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                          plane.normal.z >= 0.0f ? box.max.z : box.min.z);
        if (plane.signedDistance(corner) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool intersects(const Frustum& frustum, const Sphere& sphere) noexcept {
    return std::ranges::all_of(
        frustum.planes, [&](const Plane& plane) { return plane.signedDistance(sphere.center) >= -sphere.radius; });
}

Aabb transformBounds(const Aabb& box, const Mat4& m) noexcept {
    if (isEmpty(box)) {
        return box;
    }
    // Each output axis gathers the smaller and larger product of every input axis (Arvo, 1990).
    Vec3 lower(m[3]);
    Vec3 upper(m[3]);
    for (int column = 0; column < 3; ++column) {
        const Vec3 a = Vec3(m[column]) * box.min[column];
        const Vec3 b = Vec3(m[column]) * box.max[column];
        lower += glm::min(a, b);
        upper += glm::max(a, b);
    }
    return {.min = lower, .max = upper};
}

bool contains(const Aabb& box, const Vec3& point) noexcept {
    return glm::all(glm::greaterThanEqual(point, box.min)) && glm::all(glm::lessThanEqual(point, box.max));
}

std::optional<float> intersect(const Ray& ray, const Aabb& box) noexcept {
    if (!hasDirection(ray) || isEmpty(box)) {
        return std::nullopt;
    }
    // Slabs: the ray is inside the box for t in [first, last].
    float first = 0.0f;
    float last = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray.origin[axis];
        const float direction = ray.direction[axis];
        if (direction == 0.0f) {
            if (origin < box.min[axis] || origin > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float t0 = (box.min[axis] - origin) / direction;
        float t1 = (box.max[axis] - origin) / direction;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        first = std::max(first, t0);
        last = std::min(last, t1);
        if (first > last) {
            return std::nullopt;
        }
    }
    return first;
}

std::optional<float> intersect(const Ray& ray, const Plane& plane) noexcept {
    if (!hasDirection(ray)) {
        return std::nullopt;
    }
    const float approach = glm::dot(plane.normal, ray.direction);
    if (approach == 0.0f) {
        return std::nullopt;
    }
    const float t = -plane.signedDistance(ray.origin) / approach;
    if (!(t >= 0.0f)) {
        return std::nullopt;
    }
    return t;
}

std::optional<float> intersect(const Ray& ray, const Sphere& sphere) noexcept {
    if (!hasDirection(ray)) {
        return std::nullopt;
    }
    // |origin + t * direction - center|^2 = radius^2, a quadratic a t^2 + 2 b t + c = 0.
    const Vec3 offset = ray.origin - sphere.center;
    const float a = glm::dot(ray.direction, ray.direction);
    const float b = glm::dot(offset, ray.direction);
    const float c = glm::dot(offset, offset) - sphere.radius * sphere.radius;
    if (c <= 0.0f) {
        return 0.0f; // the origin is inside or on the surface
    }
    const float discriminant = b * b - a * c;
    if (b > 0.0f || discriminant < 0.0f) {
        return std::nullopt; // moving away, or passing by
    }
    return (-b - std::sqrt(discriminant)) / a;
}

} // namespace ith
