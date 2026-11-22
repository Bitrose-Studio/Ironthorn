#pragma once

// Bounds, planes, frustums and rays, with the tests culling and picking need (ADR-0006 rule 5).
//
//   const ith::Frustum frustum = ith::makeFrustum(viewProjection);
//   if (ith::intersects(frustum, ith::transformBounds(meshBounds, world))) { ... draw ... }
//
//   const ith::Ray ray{.origin = eye, .direction = pickDirection};
//   if (const std::optional<float> t = ith::intersect(ray, bounds)) { hit = ray.at(*t); }
//
// All functions are pure and thread-safe. Values are in one space of the caller's choosing
// (world space, usually).

#include "ith/math/Math.h"

#include <array>
#include <optional>

namespace ith {

// An axis-aligned box from `min` to `max`, inclusive. A box with min > max on any axis is empty.
struct Aabb {
    Vec3 min{0.0f, 0.0f, 0.0f};
    Vec3 max{0.0f, 0.0f, 0.0f};

    [[nodiscard]] Vec3 center() const noexcept { return 0.5f * (min + max); }

    [[nodiscard]] Vec3 halfExtents() const noexcept { return 0.5f * (max - min); }

    friend bool operator==(const Aabb&, const Aabb&) = default;
};

// A sphere; `radius` is not negative.
struct Sphere {
    Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;

    friend bool operator==(const Sphere&, const Sphere&) = default;
};

// The plane dot(normal, p) + distance = 0. Points with a positive signed distance are in front
// (on the side `normal` points to). `normal` is unit length when it comes from makePlane or
// makeFrustum.
struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;

    [[nodiscard]] float signedDistance(const Vec3& point) const noexcept { return glm::dot(normal, point) + distance; }

    friend bool operator==(const Plane&, const Plane&) = default;
};

// The plane through `point` facing along `normal`, normalized. Precondition (asserted): normal
// is not zero.
[[nodiscard]] Plane makePlane(const Vec3& normal, const Vec3& point) noexcept;

// The planes of a view volume, normals pointing inwards: left, right, bottom, top, the plane of
// clip depth 0 and the plane of clip depth 1, in that order. A point is inside when no plane has
// it behind.
struct Frustum {
    std::array<Plane, 6> planes;
};

// The frustum of a clip-space matrix (projection * view, or projection * view * world for the
// object's space): the points p whose clip position c = m * (p, 1) has -c.w <= c.x, c.y <= c.w
// and 0 <= c.z <= c.w. Works for reversed and conventional depth. With
// perspectiveReversedInfinite, the depth-0 plane is at infinity: it has a zero normal and a
// positive distance, so it rejects nothing.
[[nodiscard]] Frustum makeFrustum(const Mat4& clipFromSpace) noexcept;

// Whether the box may be inside the frustum: false only when the box lies entirely behind one
// plane. Conservative: some boxes outside the frustum near its edges also give true. An empty
// box gives false.
[[nodiscard]] bool intersects(const Frustum& frustum, const Aabb& box) noexcept;

// Whether the sphere may be inside the frustum: false only when it lies entirely behind one
// plane. Conservative like the box test.
[[nodiscard]] bool intersects(const Frustum& frustum, const Sphere& sphere) noexcept;

// The axis-aligned box around `box` moved by the affine matrix `m` (the last row is 0, 0, 0, 1).
// An empty box stays empty.
[[nodiscard]] Aabb transformBounds(const Aabb& box, const Mat4& m) noexcept;

// Whether `point` is inside the box or on its surface.
[[nodiscard]] bool contains(const Aabb& box, const Vec3& point) noexcept;

// A half-line from `origin` along `direction`; at(t) is origin + t * direction for t >= 0.
// `direction` need not be unit length; the t of a hit is in units of its length.
struct Ray {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, -1.0f};

    [[nodiscard]] Vec3 at(float t) const noexcept { return origin + t * direction; }
};

// The ray tests return the smallest t >= 0 at which the ray meets the shape, or nullopt when it
// doesn't. A ray that starts inside a box or sphere hits at t = 0. Precondition (asserted): the
// ray's direction is not zero; when it is, the result is nullopt.

// The ray against a box (surface and inside). An empty box is never hit.
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Aabb& box) noexcept;

// The ray against a plane. A ray parallel to the plane never hits it, even when it lies in it.
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Plane& plane) noexcept;

// The ray against a sphere (surface and inside).
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Sphere& sphere) noexcept;

} // namespace ith
