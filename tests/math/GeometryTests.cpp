// Bounds, planes, frustums and ray tests against known points (ADR-0006 rule 5).

#include "ith/math/Geometry.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <optional>

namespace {

const ith::Aabb kUnitBox{.min = ith::Vec3(-1.0f), .max = ith::Vec3(1.0f)};
const ith::Aabb kEmptyBox{.min = ith::Vec3(1.0f), .max = ith::Vec3(-1.0f)};

bool same(const ith::Vec3& a, const ith::Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, 1e-5f));
}

// A camera at +Z 10 looking at the origin with a 90-degree field of view, reversed infinite depth.
ith::Mat4 cameraClip() {
    return ith::perspectiveReversedInfinite(ith::kHalfPi, 1.0f, 1.0f) *
           glm::lookAt(ith::Vec3(0, 0, 10), ith::Vec3(0.0f), ith::kWorldUp);
}

} // namespace

TEST_SUITE("math.geometry") {
    TEST_CASE("a box has a center and half extents and contains its surface") {
        const ith::Aabb box{.min = {0, 2, -4}, .max = {2, 4, 0}};
        CHECK(box.center() == ith::Vec3(1, 3, -2));
        CHECK(box.halfExtents() == ith::Vec3(1, 1, 2));
        CHECK(contains(box, ith::Vec3(1, 3, -2)));
        CHECK(contains(box, ith::Vec3(2, 4, 0)));
        CHECK_FALSE(contains(box, ith::Vec3(2.01f, 3, -2)));
        CHECK_FALSE(contains(kEmptyBox, ith::Vec3(0.0f)));
    }

    TEST_CASE("a plane through a point has a unit normal and signs distances") {
        const ith::Plane plane = ith::makePlane(ith::Vec3(0, 2, 0), ith::Vec3(0, 3, 0));
        CHECK(plane.normal == ith::Vec3(0, 1, 0));
        CHECK(plane.distance == doctest::Approx(-3.0f));
        CHECK(plane.signedDistance(ith::Vec3(5, 5, 5)) == doctest::Approx(2.0f));
        CHECK(plane.signedDistance(ith::Vec3(0, 1, 0)) == doctest::Approx(-2.0f));
    }

    TEST_CASE("a transformed box encloses its moved corners") {
        const ith::Mat4 m = glm::translate(ith::Mat4(1.0f), ith::Vec3(5, 0, 0)) *
                            glm::rotate(ith::Mat4(1.0f), 0.25f * ith::kPi, ith::kWorldUp) *
                            glm::scale(ith::Mat4(1.0f), ith::Vec3(2, 1, 1));
        const ith::Aabb box{.min = {0, 0, 0}, .max = {1, 2, 3}};
        const ith::Aabb moved = transformBounds(box, m);
        ith::Vec3 lower(1e30f);
        ith::Vec3 upper(-1e30f);
        for (int corner = 0; corner < 8; ++corner) {
            const ith::Vec3 point((corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
                                  (corner & 4) != 0 ? box.max.z : box.min.z);
            const ith::Vec3 world(m * ith::Vec4(point, 1.0f));
            lower = glm::min(lower, world);
            upper = glm::max(upper, world);
        }
        CHECK(same(moved.min, lower));
        CHECK(same(moved.max, upper));
        CHECK(transformBounds(kEmptyBox, m) == kEmptyBox);
    }

    TEST_CASE("frustum planes face inwards and an infinite projection has no far plane") {
        const ith::Frustum frustum = ith::makeFrustum(cameraClip());
        for (const ith::Plane& plane : frustum.planes) {
            CHECK(plane.signedDistance(ith::Vec3(0.0f)) > 0.0f); // the origin is in view
        }
        // The near plane is 1 in front of the camera at z = 10.
        CHECK(frustum.planes[5].signedDistance(ith::Vec3(0, 0, 9)) == doctest::Approx(0.0f));
        CHECK(frustum.planes[5].signedDistance(ith::Vec3(0, 0, 9.5f)) < 0.0f);
        // The depth-0 plane is at infinity: zero normal, positive distance.
        CHECK(frustum.planes[4].normal == ith::Vec3(0.0f));
        CHECK(frustum.planes[4].distance > 0.0f);
        CHECK(frustum.planes[4].signedDistance(ith::Vec3(0, 0, -1e30f)) > 0.0f);
    }

    TEST_CASE("frustum tests keep what is in view and reject what is behind one plane") {
        const ith::Frustum frustum = ith::makeFrustum(cameraClip());
        CHECK(intersects(frustum, kUnitBox));
        CHECK(intersects(frustum, ith::Sphere{.center = {0, 0, -1000}, .radius = 1}));
        // Far away in front: an infinite projection culls nothing by distance.
        CHECK(intersects(frustum, ith::Aabb{.min = {-1, -1, -1e6f}, .max = {1, 1, -1e6f + 2}}));
        // Behind the camera, beside the view, and between the camera and the near plane.
        CHECK_FALSE(intersects(frustum, ith::Aabb{.min = {-1, -1, 11}, .max = {1, 1, 12}}));
        CHECK_FALSE(intersects(frustum, ith::Aabb{.min = {30, -1, -1}, .max = {32, 1, 1}}));
        CHECK_FALSE(intersects(frustum, ith::Sphere{.center = {0, 0, 9.5f}, .radius = 0.25f}));
        CHECK_FALSE(intersects(frustum, ith::Sphere{.center = {0, 30, 0}, .radius = 1}));
        // Touching a plane counts as inside.
        CHECK(intersects(frustum, ith::Sphere{.center = {0, 0, 9.5f}, .radius = 0.5f}));
        CHECK_FALSE(intersects(frustum, kEmptyBox));
    }

    TEST_CASE("a frustum from a conventional projection has a far plane") {
        const ith::Frustum frustum = ith::makeFrustum(glm::perspective(ith::kHalfPi, 1.0f, 1.0f, 100.0f));
        CHECK(intersects(frustum, ith::Sphere{.center = {0, 0, -50}, .radius = 1}));
        CHECK_FALSE(intersects(frustum, ith::Sphere{.center = {0, 0, -150}, .radius = 1}));
        CHECK(frustum.planes[4].signedDistance(ith::Vec3(0, 0, -1)) == doctest::Approx(0.0f));
        // The far plane is the difference of two nearly equal rows, so it carries float rounding.
        CHECK(frustum.planes[5].signedDistance(ith::Vec3(0, 0, -100)) == doctest::Approx(0.0f).epsilon(1e-3));
    }

    TEST_CASE("a ray hits a box at its nearest entry, or at 0 from inside") {
        const ith::Ray ray{.origin = {0, 0, 10}, .direction = {0, 0, -2}};
        const std::optional<float> hit = intersect(ray, kUnitBox);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(4.5f)); // in units of the direction's length
        CHECK(same(ray.at(*hit), ith::Vec3(0, 0, 1)));
        CHECK(intersect(ith::Ray{.origin = {0.5f, 0, 0}, .direction = {1, 0, 0}}, kUnitBox) == 0.0f);
        // Pointing away, passing beside, and parallel outside a slab.
        CHECK_FALSE(intersect(ith::Ray{.origin = {0, 0, 10}, .direction = {0, 0, 1}}, kUnitBox).has_value());
        CHECK_FALSE(intersect(ith::Ray{.origin = {0, 3, 10}, .direction = {0, 0, -1}}, kUnitBox).has_value());
        CHECK_FALSE(intersect(ith::Ray{.origin = {2, 0, 10}, .direction = {0, 0, -1}}, kUnitBox).has_value());
        // A diagonal ray through a corner region.
        const std::optional<float> diagonal =
            intersect(ith::Ray{.origin = {-3, -3, 0}, .direction = {1, 1, 0}}, kUnitBox);
        REQUIRE(diagonal.has_value());
        CHECK(*diagonal == doctest::Approx(2.0f));
        CHECK_FALSE(intersect(ray, kEmptyBox).has_value());
    }

    TEST_CASE("a ray hits a plane in front of it only") {
        const ith::Plane ground = ith::makePlane(ith::kWorldUp, ith::Vec3(0.0f));
        const std::optional<float> hit = intersect(ith::Ray{.origin = {1, 4, 2}, .direction = {0, -2, 0}}, ground);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(2.0f));
        CHECK_FALSE(intersect(ith::Ray{.origin = {1, 4, 2}, .direction = {0, 1, 0}}, ground).has_value());
        CHECK_FALSE(intersect(ith::Ray{.origin = {1, 4, 2}, .direction = {1, 0, 0}}, ground).has_value());
        // From below, the plane is hit too.
        CHECK(intersect(ith::Ray{.origin = {0, -1, 0}, .direction = {0, 1, 0}}, ground) == 1.0f);
    }

    TEST_CASE("a ray hits a sphere at its nearest point, or at 0 from inside") {
        const ith::Sphere sphere{.center = {0, 0, -5}, .radius = 2};
        const std::optional<float> hit = intersect(ith::Ray{.origin = {0, 0, 0}, .direction = {0, 0, -1}}, sphere);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(3.0f));
        CHECK(intersect(ith::Ray{.origin = {0, 0, -5}, .direction = {1, 0, 0}}, sphere) == 0.0f);
        CHECK_FALSE(intersect(ith::Ray{.origin = {0, 0, 0}, .direction = {0, 0, 1}}, sphere).has_value());
        CHECK_FALSE(intersect(ith::Ray{.origin = {0, 3, 0}, .direction = {0, 0, -1}}, sphere).has_value());
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("a ray without a direction asserts and hits nothing") {
        ith::test::AssertCapture capture;
        const ith::Ray ray{.origin = {0, 0, 0}, .direction = {0, 0, 0}};
        CHECK_FALSE(intersect(ray, kUnitBox).has_value());
        CHECK_FALSE(intersect(ray, ith::makePlane(ith::kWorldUp, ith::Vec3(0.0f))).has_value());
        CHECK_FALSE(intersect(ray, ith::Sphere{.center = {0, 0, 0}, .radius = 1}).has_value());
        CHECK(capture.count() == 3);
    }
#endif
}
