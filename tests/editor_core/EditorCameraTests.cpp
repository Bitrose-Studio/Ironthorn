// The editor viewport camera (ADR-0042, ADR-0046): its defaults, the vertical angle derived from
// the horizontal one, fly speed table and axes, mouse look, pitch limits and yaw wrapping, pivot
// tracking, speed-step and dolly wheel gestures, orbit and pan, world-bounds focus and its
// tightness, matrices and asserted preconditions. Input is injected directly; no UI or platform is
// involved.

#include "ith/editor_core/EditorCamera.h"

#include "ith/math/Geometry.h"
#include "ith/math/Math.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {

constexpr float kEpsilon = 1e-5f;

bool same(const ith::Vec3& a, const ith::Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

bool same(const ith::Vec4& a, const ith::Vec4& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

bool same(const ith::Mat4& a, const ith::Mat4& b) {
    for (int column = 0; column < 4; ++column) {
        if (!same(a[column], b[column])) {
            return false;
        }
    }
    return true;
}

ith::Quat rotation(const ith::EditorCamera& camera) {
    return glm::angleAxis(camera.yaw, ith::kWorldUp) * glm::angleAxis(camera.pitch, ith::kWorldRight);
}

ith::Vec3 forward(const ith::EditorCamera& camera) {
    return rotation(camera) * ith::kWorldForward;
}

struct Projected {
    ith::Vec3 ndc;
    float w = 0.0f;
};

Projected project(const ith::Mat4& m, const ith::Vec3& point) {
    const ith::Vec4 clip = m * ith::Vec4(point, 1.0f);
    return {.ndc = ith::Vec3(clip) / clip.w, .w = clip.w};
}

ith::Vec3 corner(const ith::Aabb& bounds, int index) {
    return {(index & 1) != 0 ? bounds.max.x : bounds.min.x, (index & 2) != 0 ? bounds.max.y : bounds.min.y,
            (index & 4) != 0 ? bounds.max.z : bounds.min.z};
}

} // namespace

TEST_SUITE("editor_core.camera") {
    TEST_CASE("the default camera looks at the origin from back-right above") {
        const ith::EditorCamera camera;
        CHECK(same(camera.position, ith::Vec3(3.6742346f, 3.0f, 3.6742346f)));
        CHECK(camera.yaw == doctest::Approx(0.25f * ith::kPi));
        CHECK(camera.pitch == doctest::Approx(-ith::kPi / 6.0f));
        CHECK(camera.fovX == doctest::Approx(ith::kHalfPi));
        CHECK(camera.speedStep == 4);
        CHECK(same(camera.pivot, ith::Vec3(0.0f)));
        CHECK(glm::length(camera.position) == doctest::Approx(6.0f).epsilon(kEpsilon));
        CHECK(same(forward(camera), glm::normalize(-camera.position)));
    }

    TEST_CASE("RMB fly moves one second at every speed step along the camera forward") {
        constexpr std::array<float, 8> speeds{0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};
        for (std::size_t i = 0; i < speeds.size(); ++i) {
            CAPTURE(i);
            ith::EditorCamera camera{
                .position = {1.0f, 2.0f, 3.0f}, .yaw = 0.35f, .pitch = -0.2f, .speedStep = static_cast<int>(i + 1)};
            const ith::Vec3 start = camera.position;
            const ith::Vec3 expectedForward = forward(camera);
            const float pivotDistance = glm::length(camera.position - camera.pivot);
            ith::CameraInput input;
            input.rmb = true;
            input.forward = true;
            ith::updateCamera(camera, input, 1.0f);
            CHECK(same(camera.position, start + expectedForward * speeds[i]));
            CHECK(same(camera.pivot, camera.position + expectedForward * pivotDistance));
        }
    }

    TEST_CASE("fly supports all six axes, normalizes diagonals and requires RMB") {
        for (std::size_t i = 0; i < 6; ++i) {
            CAPTURE(i);
            ith::EditorCamera camera{.position = {1.0f, 2.0f, 3.0f}, .yaw = 0.35f, .pitch = -0.2f};
            const ith::Vec3 start = camera.position;
            const ith::Vec3 right = rotation(camera) * ith::kWorldRight;
            const std::array<ith::Vec3, 6> directions{forward(camera), -forward(camera), -right,
                                                      right,           ith::kWorldUp,    -ith::kWorldUp};
            ith::CameraInput input;
            input.rmb = true;
            input.forward = i == 0;
            input.back = i == 1;
            input.left = i == 2;
            input.right = i == 3;
            input.up = i == 4;
            input.down = i == 5;
            ith::updateCamera(camera, input, 0.5f);
            CHECK(same(camera.position, start + directions[i] * 2.0f));
        }

        ith::EditorCamera camera{.position = ith::Vec3(0.0f), .yaw = 0.0f, .pitch = 0.0f};
        ith::CameraInput input;
        input.forward = true;
        ith::updateCamera(camera, input, 1.0f);
        CHECK(same(camera.position, ith::Vec3(0.0f)));

        input.rmb = true;
        input.right = true;
        input.up = true;
        ith::updateCamera(camera, input, 1.0f);
        CHECK(same(camera.position, glm::normalize(ith::Vec3(1.0f, 1.0f, -1.0f)) * 4.0f));
    }

    TEST_CASE("RMB mouse look turns and clamps pitch without flipping") {
        ith::EditorCamera camera{
            .position = {1.0f, 2.0f, 3.0f}, .yaw = 0.0f, .pitch = 0.0f, .pivot = {1.0f, 2.0f, -7.0f}};
        const ith::Vec3 position = camera.position;
        ith::CameraInput input{.mouseDelta = {40.0f, -20.0f}};
        input.rmb = true;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.yaw == doctest::Approx(-0.2f));
        CHECK(camera.pitch == doctest::Approx(0.1f));
        CHECK(same(camera.position, position));

        input.mouseDelta = {0.0f, 100000.0f};
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.pitch > -ith::kHalfPi);
        CHECK(camera.pitch < 0.0f);
        CHECK(forward(camera).y > -1.0f);
        CHECK(forward(camera).z < 0.0f);
        CHECK(same(ith::viewMatrix(camera) * ith::Vec4(camera.position + forward(camera), 1.0f),
                   ith::Vec4(0.0f, 0.0f, -1.0f, 1.0f)));

        input.mouseDelta = {0.0f, -100000.0f};
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.pitch < ith::kHalfPi);
        CHECK(camera.pitch > 0.0f);
        CHECK(forward(camera).y < 1.0f);
        CHECK(forward(camera).z < 0.0f);
        CHECK(same(ith::viewMatrix(camera) * ith::Vec4(camera.position + forward(camera), 1.0f),
                   ith::Vec4(0.0f, 0.0f, -1.0f, 1.0f)));
    }

    TEST_CASE("RMB look keeps the pivot aligned for orbit and wheel dolly") {
        ith::EditorCamera camera{.position = {0.0f, 0.0f, 10.0f}, .yaw = 0.0f, .pitch = 0.0f};
        ith::CameraInput look{.mouseDelta = {40.0f, -20.0f}};
        look.rmb = true;
        ith::updateCamera(camera, look, 0.0f);
        const ith::Vec3 position = camera.position;
        const ith::Vec3 pivot = position + forward(camera) * 10.0f;
        CHECK(same(camera.pivot, pivot));

        ith::CameraInput orbit;
        orbit.alt = true;
        orbit.lmb = true;
        ith::updateCamera(camera, orbit, 0.0f);
        CHECK(same(camera.position, position));
        CHECK(same(camera.pivot, pivot));

        const ith::Vec3 dollyForward = forward(camera);
        ith::CameraInput wheel;
        wheel.wheel = 1.0f;
        ith::updateCamera(camera, wheel, 0.0f);
        CHECK(same(camera.position, position + dollyForward));
        CHECK(same(camera.pivot, pivot));
    }

    TEST_CASE("updateCamera wraps yaw into [-pi, pi)") {
        ith::EditorCamera camera{.yaw = ith::kPi - 0.1f, .pitch = 0.0f};
        ith::CameraInput input{.mouseDelta = {-40.0f, 0.0f}};
        input.rmb = true;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.yaw == doctest::Approx(-ith::kPi + 0.1f).epsilon(kEpsilon));
        CHECK(camera.yaw >= -ith::kPi);
        CHECK(camera.yaw < ith::kPi);

        input.mouseDelta = {0.0f, 0.0f};
        camera.yaw = 3.0f * ith::kPi;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.yaw == doctest::Approx(-ith::kPi));
        camera.yaw = -3.0f * ith::kPi;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.yaw == doctest::Approx(-ith::kPi));
    }

    TEST_CASE("the RMB wheel changes speed per notch and clamps at the table ends") {
        ith::EditorCamera camera;
        const ith::Vec3 position = camera.position;
        const ith::Vec3 pivot = camera.pivot;
        ith::CameraInput input;
        input.rmb = true;

        input.wheel = 1.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 5);
        input.wheel = -2.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 3);
        input.wheel = 0.9f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 3);
        input.wheel = -100.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 1);
        input.wheel = -1.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 1);
        input.wheel = 100.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 8);
        input.wheel = 1.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(camera.speedStep == 8);
        CHECK(same(camera.position, position));
        CHECK(same(camera.pivot, pivot));
    }

    TEST_CASE("wheel and Alt RMB dolly around the pivot without changing speed or orientation") {
        ith::EditorCamera camera{.position = {0.0f, 0.0f, 10.0f}, .yaw = 0.0f, .pitch = 0.0f};
        ith::CameraInput input;
        input.wheel = 1.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(same(camera.position, ith::Vec3(0.0f, 0.0f, 9.0f)));
        CHECK(same(camera.pivot, ith::Vec3(0.0f)));
        CHECK(camera.speedStep == 4);

        input.wheel = -1.0f;
        ith::updateCamera(camera, input, 0.0f);
        CHECK(same(camera.position, ith::Vec3(0.0f, 0.0f, 10.0f)));

        input.wheel = 0.0f;
        input.rmb = true;
        input.alt = true;
        input.forward = true;
        input.mouseDelta = {0.0f, -100.0f};
        ith::updateCamera(camera, input, 1.0f);
        CHECK(same(camera.position, ith::Vec3(0.0f, 0.0f, 9.0f)));
        CHECK(camera.yaw == 0.0f);
        CHECK(camera.pitch == 0.0f);
        CHECK(camera.speedStep == 4);

        input.mouseDelta = {0.0f, 100.0f};
        ith::updateCamera(camera, input, 0.0f);
        CHECK(same(camera.position, ith::Vec3(0.0f, 0.0f, 10.0f)));
        CHECK(same(camera.pivot, ith::Vec3(0.0f)));
    }

    TEST_CASE("Alt LMB orbit preserves the pivot distance and points at the pivot") {
        ith::EditorCamera camera{
            .position = {1.0f, 2.0f, 11.0f}, .yaw = 0.0f, .pitch = 0.0f, .pivot = {1.0f, 2.0f, 1.0f}};
        const ith::Vec3 pivot = camera.pivot;
        ith::CameraInput input{.mouseDelta = {40.0f, -20.0f}};
        input.alt = true;
        input.lmb = true;
        ith::updateCamera(camera, input, 0.0f);

        CHECK(camera.yaw == doctest::Approx(-0.2f));
        CHECK(camera.pitch == doctest::Approx(0.1f));
        CHECK(same(camera.pivot, pivot));
        CHECK(glm::length(camera.position - camera.pivot) == doctest::Approx(10.0f).epsilon(kEpsilon));
        CHECK(same(camera.position, pivot - forward(camera) * 10.0f));
        CHECK(same(glm::normalize(camera.pivot - camera.position), forward(camera)));
    }

    TEST_CASE("Alt LMB orbit takes precedence over RMB fly") {
        ith::EditorCamera camera{.position = {0.0f, 0.0f, 10.0f}, .yaw = 0.0f, .pitch = 0.0f};
        ith::CameraInput input{.mouseDelta = {40.0f, -20.0f}};
        input.alt = true;
        input.lmb = true;
        input.rmb = true;
        input.forward = true;
        ith::updateCamera(camera, input, 1.0f);

        CHECK(same(camera.position, -forward(camera) * 10.0f));
        CHECK(same(camera.pivot, ith::Vec3(0.0f)));
        CHECK(camera.yaw == doctest::Approx(-0.2f));
        CHECK(camera.pitch == doctest::Approx(0.1f));
    }

    TEST_CASE("MMB pan moves position and pivot together") {
        ith::EditorCamera camera{
            .position = {1.0f, 2.0f, 11.0f}, .yaw = 0.35f, .pitch = -0.2f, .pivot = {1.0f, 2.0f, 1.0f}};
        const ith::Vec3 oldPosition = camera.position;
        const ith::Vec3 oldPivot = camera.pivot;
        const float yaw = camera.yaw;
        const float pitch = camera.pitch;
        const ith::Vec3 right = rotation(camera) * ith::kWorldRight;
        const ith::Vec3 up = rotation(camera) * ith::kWorldUp;
        const ith::Vec3 expectedDelta = (up * 10.0f - right * 20.0f) * 0.01f;
        ith::CameraInput input{.mouseDelta = {20.0f, 10.0f}};
        input.mmb = true;
        ith::updateCamera(camera, input, 0.0f);

        CHECK(same(camera.position, oldPosition + expectedDelta));
        CHECK(same(camera.pivot, oldPivot + expectedDelta));
        CHECK(same(camera.position - oldPosition, camera.pivot - oldPivot));
        CHECK(glm::length(camera.position - camera.pivot) == doctest::Approx(10.0f).epsilon(kEpsilon));
        CHECK(camera.yaw == yaw);
        CHECK(camera.pitch == pitch);
    }

    TEST_CASE("focus fits transformed world bounds at the computed distance and keeps orientation") {
        const ith::Aabb localBounds{.min = {-1.0f, -0.5f, -2.0f}, .max = {1.0f, 0.5f, 2.0f}};
        const ith::Mat4 world = glm::translate(ith::Mat4(1.0f), ith::Vec3(4.0f, 2.0f, -3.0f)) *
                                glm::rotate(ith::Mat4(1.0f), 0.4f, glm::normalize(ith::Vec3(1.0f, 2.0f, 3.0f))) *
                                glm::scale(ith::Mat4(1.0f), ith::Vec3(2.0f, 0.5f, 1.5f));
        const ith::Aabb worldBounds = ith::transformBounds(localBounds, world);
        for (const float aspect : {0.5f, 1.0f, 2.0f}) {
            CAPTURE(aspect);
            ith::EditorCamera camera{.position = {20.0f, 10.0f, 5.0f}, .yaw = 0.7f, .pitch = -0.3f, .speedStep = 6};
            ith::focus(camera, worldBounds, aspect);
            CHECK(same(camera.pivot, worldBounds.center()));
            CHECK(camera.yaw == doctest::Approx(0.7f));
            CHECK(camera.pitch == doctest::Approx(-0.3f));
            CHECK(camera.speedStep == 6);
            const float horizontalHalfFov = 0.5f * camera.fovX;
            const float verticalHalfFov = std::atan(std::tan(horizontalHalfFov) / aspect);
            const float limitingHalfFov = std::min(verticalHalfFov, horizontalHalfFov);
            const float expectedDistance =
                std::max(1.0f, 1.1f * glm::length(worldBounds.halfExtents()) / std::sin(limitingHalfFov));
            CHECK(glm::length(camera.position - camera.pivot) == doctest::Approx(expectedDistance).epsilon(kEpsilon));

            const ith::Mat4 clipFromWorld = ith::projectionMatrix(camera, aspect, 0.1f) * ith::viewMatrix(camera);
            for (int i = 0; i < 8; ++i) {
                CAPTURE(i);
                const Projected projected = project(clipFromWorld, corner(worldBounds, i));
                CHECK(projected.w >= 0.1f);
                CHECK(std::abs(projected.ndc.x) <= 1.0f);
                CHECK(std::abs(projected.ndc.y) <= 1.0f);
                CHECK(projected.ndc.z >= 0.0f);
                CHECK(projected.ndc.z <= 1.0f);
            }
            const Projected centre = project(clipFromWorld, worldBounds.center());
            CHECK(centre.ndc.x == doctest::Approx(0.0f).epsilon(kEpsilon));
            CHECK(centre.ndc.y == doctest::Approx(0.0f).epsilon(kEpsilon));
        }

        ith::EditorCamera pointCamera{.yaw = 0.0f, .pitch = 0.0f};
        const ith::Aabb point{.min = {1.0f, 2.0f, 3.0f}, .max = {1.0f, 2.0f, 3.0f}};
        ith::focus(pointCamera, point, 1.0f);
        CHECK(same(pointCamera.pivot, point.center()));
        CHECK(same(pointCamera.position, ith::Vec3(1.0f, 2.0f, 4.0f)));
    }

    TEST_CASE("the vertical angle follows from the horizontal one and the image aspect") {
        // ADR-0046's table: 90 degrees across 1280x460, 640x400, a square and 640x960.
        struct Case {
            float aspect;
            float degrees;
        };

        for (const Case& c : {Case{1280.0f / 460.0f, 39.53f}, Case{640.0f / 400.0f, 64.01f}, Case{1.0f, 90.0f},
                              Case{640.0f / 960.0f, 112.62f}}) {
            CAPTURE(c.aspect);
            const float fovY = ith::verticalFov(ith::kHalfPi, c.aspect);
            CHECK(glm::degrees(fovY) == doctest::Approx(c.degrees).epsilon(1e-3));
            CHECK(std::tan(0.5f * fovY) * c.aspect == doctest::Approx(1.0f).epsilon(kEpsilon));
        }
        for (const float fovX : {0.01f, 1.0f, 3.1f}) {
            for (const float aspect : {0.25f, 1.0f, 4.0f}) {
                CAPTURE(fovX);
                CAPTURE(aspect);
                const float fovY = ith::verticalFov(fovX, aspect);
                CHECK(fovY > 0.0f);
                CHECK(fovY < ith::kPi);
                CHECK((aspect > 1.0f ? fovY < fovX : aspect < 1.0f ? fovY > fovX : fovY == doctest::Approx(fovX)));
            }
        }

        // The projection shows fovX across the image's width and the derived angle up its height.
        for (const float aspect : {0.5f, 1.0f, 2.5f}) {
            CAPTURE(aspect);
            const ith::EditorCamera camera{.fovX = 1.2f};
            const ith::Mat4 projection = ith::projectionMatrix(camera, aspect, 0.1f);
            const float halfX = std::tan(0.5f * camera.fovX);
            const float halfY = std::tan(0.5f * ith::verticalFov(camera.fovX, aspect));
            CHECK(project(projection, ith::Vec3(halfX * 3.0f, 0.0f, -3.0f)).ndc.x == doctest::Approx(1.0f));
            CHECK(project(projection, ith::Vec3(0.0f, halfY * 3.0f, -3.0f)).ndc.y == doctest::Approx(1.0f));
        }
    }

    TEST_CASE("view and projection follow the coordinate and reversed depth conventions") {
        ith::EditorCamera identity{.position = ith::Vec3(0.0f), .yaw = 0.0f, .pitch = 0.0f};
        CHECK(same(ith::viewMatrix(identity), ith::Mat4(1.0f)));

        ith::EditorCamera camera{.position = {2.0f, 3.0f, 4.0f}, .yaw = 0.4f, .pitch = -0.2f};
        const ith::Mat4 view = ith::viewMatrix(camera);
        CHECK(
            same(view * ith::Vec4(camera.position + forward(camera) * 5.0f, 1.0f), ith::Vec4(0.0f, 0.0f, -5.0f, 1.0f)));

        constexpr float nearPlane = 0.1f;
        const ith::Mat4 projection = ith::projectionMatrix(camera, 2.0f, nearPlane);
        const Projected atNear = project(projection, ith::Vec3(0.0f, 0.0f, -nearPlane));
        CHECK(atNear.ndc.z == doctest::Approx(1.0f));
        CHECK(atNear.w == doctest::Approx(nearPlane));
        const Projected inFront = project(projection, ith::Vec3(0.0f, 0.0f, -5.0f));
        CHECK(inFront.ndc.z == doctest::Approx(nearPlane / 5.0f));
        CHECK(inFront.ndc.z > 0.0f);
        CHECK(inFront.w == doctest::Approx(5.0f));
        const Projected farther = project(projection, ith::Vec3(0.0f, 0.0f, -100.0f));
        CHECK(farther.ndc.z > 0.0f);
        CHECK(farther.ndc.z < inFront.ndc.z);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("camera functions assert their preconditions and leave invalid calls safe") {
        ith::test::AssertCapture capture;
        ith::EditorCamera camera;
        const ith::Vec3 position = camera.position;
        const ith::Vec3 pivot = camera.pivot;
        const ith::CameraInput input;

        ith::updateCamera(camera, input, -1.0f);
        CHECK(capture.count() == 1);
        CHECK(same(camera.position, position));

        const ith::Aabb empty{.min = ith::Vec3(1.0f), .max = ith::Vec3(-1.0f)};
        ith::focus(camera, empty, 1.0f);
        CHECK(capture.count() == 2);
        ith::focus(camera, ith::Aabb{}, 0.0f);
        CHECK(capture.count() == 3);
        camera.fovX = 0.0f;
        ith::focus(camera, ith::Aabb{}, 1.0f);
        CHECK(capture.count() == 4);
        camera.fovX = ith::kPi;
        ith::focus(camera, ith::Aabb{}, 1.0f);
        CHECK(capture.count() == 5);
        CHECK(same(camera.position, position));
        CHECK(same(camera.pivot, pivot));

        // The derived vertical angle asserts its input, and then the projection does too.
        camera.fovX = ith::kHalfPi;
        static_cast<void>(ith::projectionMatrix(camera, 0.0f, 0.1f));
        CHECK(capture.count() == 7);
        static_cast<void>(ith::projectionMatrix(camera, 1.0f, 0.0f));
        CHECK(capture.count() == 8);
        camera.fovX = 0.0f;
        static_cast<void>(ith::projectionMatrix(camera, 1.0f, 0.1f));
        CHECK(capture.count() == 10);

        CHECK(ith::verticalFov(ith::kHalfPi, 0.0f) == ith::kHalfPi);
        CHECK(ith::verticalFov(ith::kHalfPi, std::nanf("")) == ith::kHalfPi);
        CHECK(ith::verticalFov(-1.0f, 1.0f) == -1.0f);
        CHECK(ith::verticalFov(ith::kPi, 1.0f) == ith::kPi);
        CHECK(capture.count() == 14);
    }
#endif
}
