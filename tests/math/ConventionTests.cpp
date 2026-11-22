// The conventions of ADR-0006, pinned: glm's configuration (default values, quaternion order,
// depth range), handedness and axes, matrix layout and multiplication order, quaternion
// composition, the projections and Transform.

#include "ith/math/Math.h"
#include "ith/math/Transform.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <type_traits>

namespace {

constexpr float kEpsilon = 1e-5f;

bool same(const ith::Vec3& a, const ith::Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

bool same(const ith::Vec4& a, const ith::Vec4& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

// Projects a point with a clip-space matrix: x, y and depth after the perspective divide, and w.
struct Projected {
    ith::Vec3 ndc;
    float w = 0.0f;
};

Projected project(const ith::Mat4& m, const ith::Vec3& point) {
    const ith::Vec4 clip = m * ith::Vec4(point, 1.0f);
    return {.ndc = ith::Vec3(clip) / clip.w, .w = clip.w};
}

// Layout and type traits that reflection, the ECS and GPU uploads rely on.
template <typename T, std::size_t Size>
constexpr bool kPlainValue =
    sizeof(T) == Size && alignof(T) == alignof(typename T::value_type) && std::is_trivially_copyable_v<T> &&
    std::is_standard_layout_v<T> && !std::is_trivially_default_constructible_v<T>;

static_assert(kPlainValue<ith::Vec2, 8>);
static_assert(kPlainValue<ith::Vec3, 12>);
static_assert(kPlainValue<ith::Vec4, 16>);
static_assert(kPlainValue<ith::IVec3, 12>);
static_assert(kPlainValue<ith::UVec4, 16>);
static_assert(kPlainValue<ith::Quat, 16>);
static_assert(kPlainValue<ith::Mat3, 36>);
static_assert(kPlainValue<ith::Mat4, 64>);
static_assert(kPlainValue<ith::DVec3, 24>);
static_assert(kPlainValue<ith::DMat4, 128>);
static_assert(offsetof(ith::Vec4, x) == 0 && offsetof(ith::Vec4, y) == 4 && offsetof(ith::Vec4, z) == 8 &&
              offsetof(ith::Vec4, w) == 12);
static_assert(offsetof(ith::Quat, x) == 0 && offsetof(ith::Quat, y) == 4 && offsetof(ith::Quat, z) == 8 &&
              offsetof(ith::Quat, w) == 12);
static_assert(std::is_trivially_copyable_v<ith::Transform> && sizeof(ith::Transform) == 40);

} // namespace

TEST_SUITE("math.conventions") {
    TEST_CASE("default construction gives zero vectors and identity matrices and quaternions") {
        CHECK(ith::Vec3() == ith::Vec3(0.0f));
        CHECK(ith::Vec4() == ith::Vec4(0.0f));
        CHECK(ith::IVec2() == ith::IVec2(0));
        CHECK(ith::UVec3() == ith::UVec3(0u));
        CHECK(ith::DVec3() == ith::DVec3(0.0));
        CHECK(ith::Mat3() == ith::Mat3(1.0f));
        CHECK(ith::Mat4() == ith::Mat4(1.0f));
        CHECK(ith::DMat4() == ith::DMat4(1.0));
        const ith::Quat q;
        CHECK(q.x == 0.0f);
        CHECK(q.y == 0.0f);
        CHECK(q.z == 0.0f);
        CHECK(q.w == 1.0f);
        const ith::Transform transform;
        CHECK(toMatrix(transform) == ith::Mat4(1.0f));
    }

    TEST_CASE("the quaternion constructor takes x, y, z, w, its storage order") {
        const ith::Quat q(1.0f, 2.0f, 3.0f, 4.0f);
        CHECK(q.x == 1.0f);
        CHECK(q.y == 2.0f);
        CHECK(q.z == 3.0f);
        CHECK(q.w == 4.0f);
        const float* storage = glm::value_ptr(q);
        CHECK(storage[0] == 1.0f);
        CHECK(storage[3] == 4.0f);
        CHECK(q[0] == 1.0f);
        CHECK(q[3] == 4.0f);
    }

    TEST_CASE("space is right-handed with +X right, +Y up and forward -Z") {
        CHECK(ith::kWorldRight == ith::Vec3(1.0f, 0.0f, 0.0f));
        CHECK(ith::kWorldUp == ith::Vec3(0.0f, 1.0f, 0.0f));
        CHECK(ith::kWorldForward == ith::Vec3(0.0f, 0.0f, -1.0f));
        // Right-handed: X x Y = Z, and right x up = backwards.
        CHECK(glm::cross(ith::Vec3(1, 0, 0), ith::Vec3(0, 1, 0)) == ith::Vec3(0, 0, 1));
        CHECK(glm::cross(ith::kWorldRight, ith::kWorldUp) == -ith::kWorldForward);
    }

    TEST_CASE("positive angles rotate counter-clockwise about the axis, in radians") {
        CHECK(glm::radians(180.0f) == doctest::Approx(ith::kPi));
        // Seen from +Y looking down, +X turns towards -Z.
        const ith::Quat yaw = glm::angleAxis(ith::kHalfPi, ith::kWorldUp);
        CHECK(same(yaw * ith::Vec3(1, 0, 0), ith::Vec3(0, 0, -1)));
        CHECK(same(yaw * ith::kWorldForward, ith::Vec3(-1, 0, 0)));
        CHECK(same(glm::angleAxis(ith::kHalfPi, ith::Vec3(0, 0, 1)) * ith::Vec3(1, 0, 0), ith::Vec3(0, 1, 0)));
        CHECK(same(glm::angleAxis(ith::kHalfPi, ith::Vec3(1, 0, 0)) * ith::Vec3(0, 1, 0), ith::Vec3(0, 0, 1)));
        // The matrix form agrees.
        CHECK(same(ith::Vec3(glm::rotate(ith::Mat4(1.0f), ith::kHalfPi, ith::kWorldUp) * ith::Vec4(1, 0, 0, 0)),
                   ith::Vec3(0, 0, -1)));
    }

    TEST_CASE("quaternion products apply the right operand first") {
        const ith::Quat yaw = glm::angleAxis(ith::kHalfPi, ith::kWorldUp);
        const ith::Quat roll = glm::angleAxis(ith::kHalfPi, ith::Vec3(0, 0, 1));
        const ith::Vec3 v(1, 0, 0);
        CHECK(same((yaw * roll) * v, yaw * (roll * v)));
        // The two orders differ, so the test tells them apart.
        CHECK_FALSE(same((yaw * roll) * v, (roll * yaw) * v));
        CHECK(same((yaw * roll) * v, ith::Vec3(0, 1, 0)));
    }

    TEST_CASE("matrices are column-major with column vectors") {
        const ith::Mat4 translation = glm::translate(ith::Mat4(1.0f), ith::Vec3(1, 2, 3));
        // m[c] is a column; the translation is the last column, and the last four floats in memory.
        CHECK(translation[3] == ith::Vec4(1, 2, 3, 1));
        CHECK(translation[3][0] == 1.0f);
        const float* storage = glm::value_ptr(translation);
        CHECK(storage[12] == 1.0f);
        CHECK(storage[13] == 2.0f);
        CHECK(storage[14] == 3.0f);
        // v' = M * v moves points, not directions.
        CHECK(translation * ith::Vec4(0, 0, 0, 1) == ith::Vec4(1, 2, 3, 1));
        CHECK(translation * ith::Vec4(1, 0, 0, 0) == ith::Vec4(1, 0, 0, 0));
    }

    TEST_CASE("a product of matrices applies the right operand first: world = parent * local") {
        const ith::Mat4 parent = glm::translate(ith::Mat4(1.0f), ith::Vec3(10, 0, 0));
        const ith::Mat4 local = glm::rotate(ith::Mat4(1.0f), ith::kHalfPi, ith::kWorldUp);
        const ith::Vec4 point(1, 0, 0, 1);
        // Rotate first (to -Z), then translate.
        CHECK(same((parent * local) * point, ith::Vec4(10, 0, -1, 1)));
        CHECK(same((parent * local) * point, parent * (local * point)));
        CHECK_FALSE(same((parent * local) * point, (local * parent) * point));
    }

    TEST_CASE("lookAt is right-handed: the camera looks down its -Z") {
        // A camera at the origin looking down -Z with +Y up is the identity view.
        CHECK(glm::lookAt(ith::Vec3(0.0f), ith::kWorldForward, ith::kWorldUp) == ith::Mat4(1.0f));
        // A camera at +Z looking at the origin sees the origin 5 units in front, along its -Z.
        const ith::Mat4 view = glm::lookAt(ith::Vec3(0, 0, 5), ith::Vec3(0.0f), ith::kWorldUp);
        CHECK(same(view * ith::Vec4(0, 0, 0, 1), ith::Vec4(0, 0, -5, 1)));
        CHECK(same(view * ith::Vec4(1, 1, 0, 1), ith::Vec4(1, 1, -5, 1)));
    }

    TEST_CASE("conventional projections map depth to [0, 1], near to 0, with +Y up") {
        const ith::Mat4 perspective = glm::perspective(ith::kHalfPi, 1.0f, 1.0f, 100.0f);
        CHECK(project(perspective, ith::Vec3(0, 0, -1)).ndc.z == doctest::Approx(0.0f));
        CHECK(project(perspective, ith::Vec3(0, 0, -100)).ndc.z == doctest::Approx(1.0f));
        CHECK(project(perspective, ith::Vec3(0, 1, -2)).ndc.y > 0.0f);

        const ith::Mat4 ortho = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 11.0f);
        CHECK(project(ortho, ith::Vec3(0, 0, -1)).ndc.z == doctest::Approx(0.0f));
        CHECK(project(ortho, ith::Vec3(0, 0, -6)).ndc.z == doctest::Approx(0.5f));
        CHECK(project(ortho, ith::Vec3(0, 0, -11)).ndc.z == doctest::Approx(1.0f));
        CHECK(project(ortho, ith::Vec3(1, 1, -2)).ndc.x == doctest::Approx(1.0f));
        CHECK(project(ortho, ith::Vec3(1, 1, -2)).ndc.y == doctest::Approx(1.0f));
    }

    TEST_CASE("the reversed infinite projection maps the near plane to 1 and infinity towards 0") {
        const float nearPlane = 0.1f;
        const ith::Mat4 projection = ith::perspectiveReversedInfinite(ith::kHalfPi, 2.0f, nearPlane);
        // Depth is near / distance, and clip w is the distance.
        const Projected atNear = project(projection, ith::Vec3(0, 0, -nearPlane));
        CHECK(atNear.ndc.z == doctest::Approx(1.0f));
        CHECK(atNear.w == doctest::Approx(nearPlane));
        CHECK(project(projection, ith::Vec3(0, 0, -0.2f)).ndc.z == doctest::Approx(0.5f));
        CHECK(project(projection, ith::Vec3(0, 0, -1000.0f)).ndc.z == doctest::Approx(1e-4f));
        const float distant = project(projection, ith::Vec3(0, 0, -1e30f)).ndc.z;
        CHECK(distant > 0.0f);
        CHECK(distant < 1e-29f);
        // Nearer is greater, so the depth test is greater-or-equal.
        CHECK(project(projection, ith::Vec3(0, 0, -5)).ndc.z > project(projection, ith::Vec3(0, 0, -6)).ndc.z);
        // A 90-degree vertical field of view: the top edge at distance d is d up; x is divided by
        // the aspect ratio. +Y is up and +X right in clip space.
        CHECK(same(project(projection, ith::Vec3(0, 3, -3)).ndc, ith::Vec3(0, 1, nearPlane / 3.0f)));
        CHECK(same(project(projection, ith::Vec3(6, 0, -3)).ndc, ith::Vec3(1, 0, nearPlane / 3.0f)));
        CHECK(same(project(projection, ith::Vec3(-6, -3, -3)).ndc, ith::Vec3(-1, -1, nearPlane / 3.0f)));
    }

    TEST_CASE("a view-projection of the reversed infinite projection and lookAt") {
        const ith::Mat4 viewProjection = ith::perspectiveReversedInfinite(ith::kHalfPi, 1.0f, 1.0f) *
                                         glm::lookAt(ith::Vec3(0, 0, 5), ith::Vec3(0.0f), ith::kWorldUp);
        const Projected origin = project(viewProjection, ith::Vec3(0.0f));
        CHECK(same(origin.ndc, ith::Vec3(0, 0, 0.2f)));
        CHECK(origin.w == doctest::Approx(5.0f));
        CHECK(project(viewProjection, ith::Vec3(0, 1, 0)).ndc.y > 0.0f);
        CHECK(project(viewProjection, ith::Vec3(1, 0, 0)).ndc.x > 0.0f);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("the reversed infinite projection asserts its preconditions") {
        ith::test::AssertCapture capture;
        static_cast<void>(ith::perspectiveReversedInfinite(0.0f, 1.0f, 0.1f));
        CHECK(capture.count() == 1);
        static_cast<void>(ith::perspectiveReversedInfinite(1.0f, 0.0f, 0.1f));
        CHECK(capture.count() == 2);
        static_cast<void>(ith::perspectiveReversedInfinite(1.0f, 1.0f, 0.0f));
        CHECK(capture.count() == 3);
    }
#endif

    TEST_CASE("a transform scales, then rotates, then translates") {
        const ith::Transform transform{.translation = {0.0f, 1.0f, 0.0f},
                                       .rotation = glm::angleAxis(ith::kHalfPi, ith::kWorldUp),
                                       .scale = {2.0f, 1.0f, 1.0f}};
        CHECK(same(transformPoint(transform, ith::Vec3(1, 0, 0)), ith::Vec3(0, 1, -2)));
        CHECK(same(transformVector(transform, ith::Vec3(1, 0, 0)), ith::Vec3(0, 0, -2)));
        const ith::Mat4 m = toMatrix(transform);
        for (const ith::Vec3 point : {ith::Vec3(1, 0, 0), ith::Vec3(0.5f, -2, 3), ith::Vec3(-1, 4, 0.25f)}) {
            CHECK(same(ith::Vec3(m * ith::Vec4(point, 1.0f)), transformPoint(transform, point)));
            CHECK(same(ith::Vec3(m * ith::Vec4(point, 0.0f)), transformVector(transform, point)));
        }
        const ith::Mat4 composed = glm::translate(ith::Mat4(1.0f), transform.translation) *
                                   glm::mat4_cast(transform.rotation) * glm::scale(ith::Mat4(1.0f), transform.scale);
        for (int column = 0; column < 4; ++column) {
            CHECK(same(m[column], composed[column]));
        }
    }
}
