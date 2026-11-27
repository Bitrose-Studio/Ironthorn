// The translate gizmo's math (ADR-0042 rules 23, 24, 26 and 30), pure: the pick ray and the
// projection agree with the viewport's camera (ADR-0046's horizontal field of view); the closest
// point on an axis or a plane refuses rays nearly parallel or behind the camera; the frame follows
// the world or the entity's rotation without its scale; the layout keeps a constant screen size and
// fades end-on axes and edge-on planes, which can't be grabbed; the hit test prefers the nearest
// arrow, then the plane squares, and leaves the centre alone; drags move along the axis or in the
// plane, clamp their reach, and snap per axis in gizmo space.

#include "ith/editor_core/Gizmo.h"

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/Viewports.h"
#include "ith/math/Geometry.h"
#include "ith/math/Math.h"
#include "ith/math/Transform.h"
#include "ith/math/Types.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>

namespace {

using ith::EditorCamera;
using ith::GizmoFrame;
using ith::GizmoHandle;
using ith::GizmoLayout;
using ith::GizmoSpace;
using ith::Ray;
using ith::Vec2;
using ith::Vec3;

constexpr float kEpsilon = 1e-3f;
const Vec2 kImage{400.0f, 300.0f};

// How many asserts a broken precondition raises: none where asserts are compiled out, and the call
// stays safe either way.
#ifdef ITH_ENABLE_ASSERTS
constexpr int kAssertCount = 1;
#else
constexpr int kAssertCount = 0;
#endif

bool same(const Vec3& a, const Vec3& b, float epsilon = kEpsilon) {
    return glm::all(glm::epsilonEqual(a, b, epsilon));
}

bool same(const Vec2& a, const Vec2& b, float epsilon = kEpsilon) {
    return glm::all(glm::epsilonEqual(a, b, epsilon));
}

// A camera at `position` looking along -Z (yaw 0, pitch 0).
EditorCamera cameraAt(const Vec3& position) {
    EditorCamera camera;
    camera.position = position;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    return camera;
}

Vec2 midpoint(Vec2 a, Vec2 b) {
    return 0.5f * (a + b);
}

Vec2 centreOf(const std::array<Vec2, 4>& corners) {
    return 0.25f * (corners[0] + corners[1] + corners[2] + corners[3]);
}

// The layout of the default camera at the origin, at display scale 1.
GizmoLayout defaultLayout(const GizmoFrame& frame = {}) {
    const std::optional<GizmoLayout> layout = ith::layoutGizmo(EditorCamera{}, kImage, frame, ith::kGizmoArrowLength);
    REQUIRE(layout.has_value());
    return *layout;
}

} // namespace

TEST_SUITE("editor_core.gizmo") {
    TEST_CASE("the pick ray and the projection agree with the viewport's projection matrix") {
        const EditorCamera camera; // the default camera, looking at the origin from above and behind
        const Ray centre = ith::pickRay(camera, kImage, 0.5f * kImage);
        CHECK(same(centre.origin, camera.position));
        CHECK(std::abs(glm::length(centre.direction) - 1.0f) < kEpsilon);
        CHECK(same(centre.direction, glm::normalize(-camera.position)));

        // Points through the image's corners and edges project back to their pixels; the GPU's
        // matrices put them at the same place.
        const ith::Mat4 clipFromWorld =
            ith::projectionMatrix(camera, kImage.x / kImage.y, ith::kViewportNearPlane) * ith::viewMatrix(camera);
        for (const Vec2 pixel : {Vec2{0.0f, 0.0f}, Vec2{400.0f, 300.0f}, Vec2{37.0f, 251.0f}, Vec2{390.0f, 12.0f}}) {
            const Ray ray = ith::pickRay(camera, kImage, pixel);
            const Vec3 point = ray.at(7.0f);
            const std::optional<Vec2> back = ith::projectToImage(camera, kImage, point);
            REQUIRE(back.has_value());
            CHECK(same(*back, pixel, 0.01f));
            const ith::Vec4 clip = clipFromWorld * ith::Vec4(point, 1.0f);
            const Vec2 ndc = Vec2(clip) / clip.w;
            CHECK(same(Vec2{(ndc.x * 0.5f + 0.5f) * kImage.x, (0.5f - ndc.y * 0.5f) * kImage.y}, pixel, 0.01f));
        }
    }

    TEST_CASE("the horizontal field of view spans the image's width at any aspect") {
        EditorCamera camera = cameraAt(Vec3{0.0f});
        // 90 degrees across: the right edge's ray is 45 degrees off forward, in a wide and a tall image.
        for (const Vec2 image : {Vec2{800.0f, 200.0f}, Vec2{200.0f, 600.0f}}) {
            const Ray edge = ith::pickRay(camera, image, Vec2{image.x, 0.5f * image.y});
            CHECK(same(edge.direction, glm::normalize(Vec3{1.0f, 0.0f, -1.0f})));
        }
    }

    TEST_CASE("points behind the near plane, and images without area, project nowhere") {
        const EditorCamera camera = cameraAt(Vec3{0.0f});
        CHECK_FALSE(ith::projectToImage(camera, kImage, Vec3{0.0f, 0.0f, 1.0f}).has_value());
        CHECK_FALSE(ith::projectToImage(camera, kImage, Vec3{0.0f, 0.0f, -0.05f}).has_value());
        CHECK_FALSE(ith::projectToImage(camera, Vec2{0.0f, 300.0f}, Vec3{0.0f, 0.0f, -5.0f}).has_value());
        CHECK(ith::projectToImage(camera, kImage, Vec3{0.0f, 0.0f, -5.0f}).has_value());

        ith::test::AssertCapture asserts;
        const Ray ray = ith::pickRay(camera, Vec2{0.0f}, Vec2{1.0f});
        CHECK(asserts.count() == kAssertCount);
        CHECK(same(ray.direction, Vec3{0.0f, 0.0f, -1.0f}));
    }

    TEST_CASE("the closest point on an axis to a ray; nearly parallel and behind rays give none") {
        // A ray down -Z from (2, 1, 5) passes over the X axis at x = 2.
        const Ray down{.origin = {2.0f, 1.0f, 5.0f}, .direction = {0.0f, 0.0f, -1.0f}};
        const std::optional<float> t = ith::closestOnAxis(down, Vec3{0.0f}, ith::kWorldRight);
        REQUIRE(t.has_value());
        CHECK(std::abs(*t - 2.0f) < kEpsilon);

        // Along the axis, and a hair (0.3 degrees) off it: no point, so a drag can't explode.
        CHECK_FALSE(ith::closestOnAxis({.origin = {0.0f, 1.0f, 0.0f}, .direction = ith::kWorldRight}, Vec3{0.0f},
                                       ith::kWorldRight)
                        .has_value());
        const float hair = 0.3f * ith::kPi / 180.0f;
        CHECK_FALSE(
            ith::closestOnAxis({.origin = {0.0f, 1.0f, 0.0f}, .direction = {std::cos(hair), std::sin(hair), 0.0f}},
                               Vec3{0.0f}, ith::kWorldRight)
                .has_value());
        // Two degrees off is far but finite.
        const float twoDegrees = 2.0f * ith::kPi / 180.0f;
        const std::optional<float> far = ith::closestOnAxis(
            {.origin = {0.0f, 1.0f, 0.0f}, .direction = {std::cos(twoDegrees), -std::sin(twoDegrees), 0.0f}},
            Vec3{0.0f}, ith::kWorldRight);
        REQUIRE(far.has_value());
        CHECK(std::isfinite(*far));
        CHECK(*far > 20.0f);

        // The axis's closest point behind the ray's origin.
        CHECK_FALSE(ith::closestOnAxis({.origin = {2.0f, 1.0f, 5.0f}, .direction = {0.0f, 0.0f, 1.0f}}, Vec3{0.0f},
                                       ith::kWorldRight)
                        .has_value());
    }

    TEST_CASE("where a ray meets a plane; nearly parallel and behind rays give none") {
        const Ray ray{.origin = {1.0f, 4.0f, 2.0f}, .direction = glm::normalize(Vec3{1.0f, -2.0f, 0.0f})};
        const std::optional<Vec3> hit = ith::closestOnPlane(ray, Vec3{0.0f, 1.0f, 0.0f}, ith::kWorldUp);
        REQUIRE(hit.has_value());
        CHECK(same(*hit, Vec3{2.5f, 1.0f, 2.0f}));

        CHECK_FALSE(ith::closestOnPlane({.origin = {0.0f, 4.0f, 0.0f}, .direction = ith::kWorldRight}, Vec3{0.0f},
                                        ith::kWorldUp)
                        .has_value());
        CHECK_FALSE(
            ith::closestOnPlane({.origin = {0.0f, 4.0f, 0.0f}, .direction = ith::kWorldUp}, Vec3{0.0f}, ith::kWorldUp)
                .has_value());
    }

    TEST_CASE("the frame is the world's or the entity's rotation without its scale; a zero scale falls back") {
        const ith::Transform transform{.translation = {1.0f, 2.0f, 3.0f},
                                       .rotation = glm::angleAxis(ith::kHalfPi, ith::kWorldUp),
                                       .scale = {2.0f, 3.0f, 0.5f}};
        const ith::Mat4 world = ith::toMatrix(transform);

        const GizmoFrame worldFrame = ith::gizmoFrame(world, GizmoSpace::World);
        CHECK(same(worldFrame.origin, Vec3{1.0f, 2.0f, 3.0f}));
        CHECK(same(worldFrame.axes[0], ith::kWorldRight));
        CHECK(same(worldFrame.axes[1], ith::kWorldUp));
        CHECK(same(worldFrame.axes[2], Vec3{0.0f, 0.0f, 1.0f}));

        // A quarter turn about +Y: local X points along world -Z, local Z along world +X.
        const GizmoFrame local = ith::gizmoFrame(world, GizmoSpace::Local);
        CHECK(same(local.origin, Vec3{1.0f, 2.0f, 3.0f}));
        CHECK(same(local.axes[0], Vec3{0.0f, 0.0f, -1.0f}));
        CHECK(same(local.axes[1], ith::kWorldUp));
        CHECK(same(local.axes[2], ith::kWorldRight));

        ith::Transform flat = transform;
        flat.scale.y = 0.0f;
        const GizmoFrame collapsed = ith::gizmoFrame(ith::toMatrix(flat), GizmoSpace::Local);
        CHECK(same(collapsed.axes[0], ith::kWorldRight));
        CHECK(same(collapsed.axes[1], ith::kWorldUp));
    }

    TEST_CASE("the arrows keep their length on screen at any distance") {
        for (const float distance : {2.0f, 10.0f, 80.0f}) {
            CAPTURE(distance);
            const EditorCamera camera = cameraAt(Vec3{0.0f, 0.0f, distance});
            for (const float pixels : {100.0f, 200.0f}) {
                const std::optional<GizmoLayout> layout = ith::layoutGizmo(camera, kImage, GizmoFrame{}, pixels);
                REQUIRE(layout.has_value());
                CHECK(same(layout->center, 0.5f * kImage));
                // X and Y are side-on: their arrows are as long as asked.
                CHECK(std::abs(glm::length(layout->axisEnds[0] - layout->center) - pixels) < 0.05f);
                CHECK(std::abs(glm::length(layout->axisEnds[1] - layout->center) - pixels) < 0.05f);
            }
        }
    }

    TEST_CASE("an end-on axis and an edge-on plane are hidden and can't be grabbed; they fade in") {
        // Looking down -Z at the origin: Z is end-on, the XZ and YZ planes are edge-on.
        const EditorCamera camera = cameraAt(Vec3{0.0f, 0.0f, 5.0f});
        const std::optional<GizmoLayout> layout = ith::layoutGizmo(camera, kImage, GizmoFrame{}, 100.0f);
        REQUIRE(layout.has_value());
        CHECK(layout->axisOpacity[0] == 1.0f);
        CHECK(layout->axisOpacity[1] == 1.0f);
        CHECK(layout->axisOpacity[2] == 0.0f);
        CHECK(layout->planeOpacity[0] == 1.0f); // XY faces the camera
        CHECK(layout->planeOpacity[1] == 0.0f); // YZ
        CHECK(layout->planeOpacity[2] == 0.0f); // XZ
        // A press right where Z's arrow would be grabs X or Y or nothing, never Z or an edge-on plane.
        for (const Vec2 offset : {Vec2{2.0f, 0.0f}, Vec2{0.0f, -8.0f}, Vec2{9.0f, 9.0f}}) {
            const GizmoHandle hit = ith::hitGizmo(*layout, layout->center + offset, 6.0f);
            CHECK(hit != GizmoHandle::Z);
            CHECK(hit != GizmoHandle::YZ);
            CHECK(hit != GizmoHandle::XZ);
        }

        // Turned a little, Z fades in: between the hidden and the opaque facing it is translucent.
        const float angle = std::asin(0.5f * (ith::kGizmoHiddenFacing + ith::kGizmoOpaqueFacing));
        EditorCamera turned = cameraAt(Vec3{5.0f * std::sin(angle), 0.0f, 5.0f * std::cos(angle)});
        turned.yaw = angle;
        const std::optional<GizmoLayout> fading = ith::layoutGizmo(turned, kImage, GizmoFrame{}, 100.0f);
        REQUIRE(fading.has_value());
        CHECK(fading->axisOpacity[2] > 0.3f);
        CHECK(fading->axisOpacity[2] < 0.7f);
    }

    TEST_CASE("no gizmo for an origin behind the camera, an image without area or an invalid length") {
        const EditorCamera camera = cameraAt(Vec3{0.0f});
        GizmoFrame behind;
        behind.origin = {0.0f, 0.0f, 3.0f};
        CHECK_FALSE(ith::layoutGizmo(camera, kImage, behind, 100.0f).has_value());
        CHECK_FALSE(ith::layoutGizmo(camera, Vec2{400.0f, 0.0f}, GizmoFrame{}, 100.0f).has_value());

        ith::test::AssertCapture asserts;
        CHECK_FALSE(ith::layoutGizmo(EditorCamera{}, kImage, GizmoFrame{}, 0.0f).has_value());
        CHECK(asserts.count() == kAssertCount);
    }

    TEST_CASE("the hit test takes the nearest arrow, then the plane squares, and nothing at the centre") {
        const GizmoLayout layout = defaultLayout();
        for (std::size_t axis = 0; axis < 3; ++axis) {
            CAPTURE(axis);
            CHECK(layout.axisOpacity[axis] == 1.0f);
            const Vec2 middle = midpoint(layout.center, layout.axisEnds[axis]);
            CHECK(ith::hitGizmo(layout, middle, 6.0f) == ith::gizmoAxisHandle(axis));
            CHECK(ith::hitGizmo(layout, layout.axisEnds[axis], 6.0f) == ith::gizmoAxisHandle(axis));
        }
        for (std::size_t plane = 0; plane < 3; ++plane) {
            CAPTURE(plane);
            CHECK(layout.planeOpacity[plane] > 0.0f);
            CHECK(ith::hitGizmo(layout, centreOf(layout.planes[plane]), 6.0f) == ith::gizmoPlaneHandle(plane));
        }
        // The centre, where every arrow starts, and empty space grab nothing.
        CHECK(ith::hitGizmo(layout, layout.center, 6.0f) == GizmoHandle::None);
        CHECK(ith::hitGizmo(layout, layout.center + Vec2{3.0f, 3.0f}, 6.0f) == GizmoHandle::None);
        CHECK(ith::hitGizmo(layout, Vec2{5.0f, 5.0f}, 6.0f) == GizmoHandle::None);
        // Just past the tolerance off X's tip.
        const Vec2 along = glm::normalize(layout.axisEnds[0] - layout.center);
        CHECK(ith::hitGizmo(layout, layout.axisEnds[0] + along * 7.0f, 6.0f) == GizmoHandle::None);
        CHECK(ith::hitGizmo(layout, layout.axisEnds[0] + along * 5.0f, 6.0f) == GizmoHandle::X);
    }

    TEST_CASE("the handle kinds map to their axes and planes") {
        CHECK(ith::gizmoAxisOf(GizmoHandle::Y) == 1u);
        CHECK_FALSE(ith::gizmoAxisOf(GizmoHandle::XY).has_value());
        CHECK(ith::gizmoPlaneOf(GizmoHandle::XZ) == 2u);
        CHECK_FALSE(ith::gizmoPlaneOf(GizmoHandle::None).has_value());
        CHECK(ith::gizmoPlaneAxes(0) == std::array<std::size_t, 2>{0, 1});
        CHECK(ith::gizmoPlaneAxes(1) == std::array<std::size_t, 2>{1, 2});
        CHECK(ith::gizmoPlaneAxes(2) == std::array<std::size_t, 2>{0, 2});
        ith::test::AssertCapture asserts;
        CHECK(ith::gizmoAxisHandle(3) == GizmoHandle::None);
        CHECK(asserts.count() == kAssertCount);
    }

    TEST_CASE("an axis drag moves along the axis only, a plane drag within the plane") {
        const EditorCamera camera;
        GizmoFrame frame;
        frame.origin = {0.5f, 0.25f, -0.5f};
        const GizmoLayout layout = defaultLayout(frame);
        const Vec2 press = midpoint(layout.center, layout.axisEnds[0]);
        const std::optional<ith::GizmoGrab> grab =
            ith::grabGizmo(GizmoHandle::X, frame, ith::pickRay(camera, kImage, press));
        REQUIRE(grab.has_value());
        CHECK(std::abs(grab->start.y - frame.origin.y) < kEpsilon);
        CHECK(std::abs(grab->start.z - frame.origin.z) < kEpsilon);

        // To where the grabbed point 1.5 m on along X shows, and somewhere off the axis.
        const std::optional<Vec2> target = ith::projectToImage(camera, kImage, grab->start + Vec3{1.5f, 0.0f, 0.0f});
        REQUIRE(target.has_value());
        const std::optional<Vec3> delta = ith::dragGizmo(*grab, ith::pickRay(camera, kImage, *target));
        REQUIRE(delta.has_value());
        CHECK(same(*delta, Vec3{1.5f, 0.0f, 0.0f}));
        const std::optional<Vec3> off =
            ith::dragGizmo(*grab, ith::pickRay(camera, kImage, *target + Vec2{0.0f, 40.0f}));
        REQUIRE(off.has_value());
        CHECK(off->y == 0.0f);
        CHECK(off->z == 0.0f);

        // The XZ plane: the drag stays at the plane's height.
        const std::optional<ith::GizmoGrab> planeGrab =
            ith::grabGizmo(GizmoHandle::XZ, frame, ith::pickRay(camera, kImage, centreOf(layout.planes[2])));
        REQUIRE(planeGrab.has_value());
        const std::optional<Vec2> planeTarget =
            ith::projectToImage(camera, kImage, planeGrab->start + Vec3{-0.7f, 0.0f, 1.2f});
        REQUIRE(planeTarget.has_value());
        const std::optional<Vec3> moved = ith::dragGizmo(*planeGrab, ith::pickRay(camera, kImage, *planeTarget));
        REQUIRE(moved.has_value());
        CHECK(same(*moved, Vec3{-0.7f, 0.0f, 1.2f}));

        // None grabs nothing.
        CHECK_FALSE(ith::grabGizmo(GizmoHandle::None, frame, ith::pickRay(camera, kImage, press)).has_value());
    }

    TEST_CASE("a drag toward the horizon is clamped to its reach; a ray past it keeps the last position") {
        // Over the XZ plane: rays near the horizon meet it far away.
        const Vec3 camera{0.0f, 4.0f, 1.0f};
        const GizmoFrame frame;
        const Ray pressRay{.origin = camera, .direction = glm::normalize(Vec3{0.3f, 0.0f, 0.3f} - camera)};
        const std::optional<ith::GizmoGrab> grab = ith::grabGizmo(GizmoHandle::XZ, frame, pressRay);
        REQUIRE(grab.has_value());
        CHECK(same(grab->start, Vec3{0.3f, 0.0f, 0.3f}));
        CHECK(std::abs(grab->reach - ith::kGizmoMaxReach * glm::length(camera)) < kEpsilon);

        // Just below the horizon (about 0.6 degrees down): 380 m away, beyond the reach of about
        // 206 m, so clamped to it.
        const Ray grazing{.origin = camera, .direction = glm::normalize(Vec3{0.0f, -0.0105f, -1.0f})};
        const std::optional<Vec3> clamped = ith::dragGizmo(*grab, grazing);
        REQUIRE(clamped.has_value());
        CHECK(std::abs(glm::length(*clamped) - grab->reach) < 0.01f);
        CHECK(std::abs(clamped->y) < kEpsilon);
        // Above the horizon the plane is behind: no position.
        const Ray sky{.origin = camera, .direction = glm::normalize(Vec3{0.0f, 0.2f, -1.0f})};
        CHECK_FALSE(ith::dragGizmo(*grab, sky).has_value());
    }

    TEST_CASE("snapping rounds each component along the frame's axes to the step") {
        const GizmoFrame world;
        CHECK(same(ith::snapGizmo(Vec3{0.26f, -0.04f, 0.151f}, world, 0.1f), Vec3{0.3f, 0.0f, 0.2f}));
        CHECK(same(ith::snapGizmo(Vec3{-0.74f, 1.0f, 0.0f}, world, 0.25f), Vec3{-0.75f, 1.0f, 0.0f}));

        // In a frame turned a quarter about +Y, 0.33 m along its X (world -Z) snaps to 0.3 m along it.
        const GizmoFrame local =
            ith::gizmoFrame(ith::Mat4(glm::mat4_cast(glm::angleAxis(ith::kHalfPi, ith::kWorldUp))), GizmoSpace::Local);
        const Vec3 snapped = ith::snapGizmo(local.axes[0] * 0.33f + local.axes[2] * 0.02f, local, 0.1f);
        CHECK(same(snapped, Vec3{0.0f, 0.0f, -0.3f}));

        ith::test::AssertCapture asserts;
        CHECK(same(ith::snapGizmo(Vec3{0.26f}, world, 0.0f), Vec3{0.26f}));
        CHECK(asserts.count() == kAssertCount);
    }
}
