#include "ith/editor_core/Gizmo.h"

#include "ith/foundation/Assert.h"
#include "ith/math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace ith {

namespace {

// A ray within this sine of parallel to an axis, or of lying in a plane, grabs no point: about half a
// degree. Smaller than kGizmoHiddenFacing, so a grabbable handle always grabs at its press.
constexpr float kParallelSine = 0.01f;

// A Gram-Schmidt step shorter than this collapses a direction.
constexpr float kDegenerateLength = 1e-12f;

// The camera's axes in world space.
struct Basis {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

Basis basisOf(const EditorCamera& camera) noexcept {
    const Mat4 view = viewMatrix(camera);
    // The rows of the view matrix's rotation (glm is column-major).
    const Vec3 right{view[0][0], view[1][0], view[2][0]};
    const Vec3 up{view[0][1], view[1][1], view[2][1]};
    const Vec3 back{view[0][2], view[1][2], view[2][2]};
    return {.right = right, .up = up, .forward = -back};
}

// The tangents of half the horizontal and vertical angles of the view (ADR-0046).
Vec2 halfTangents(const EditorCamera& camera, Vec2 imageSize) noexcept {
    const float tanX = std::tan(0.5f * camera.fovX);
    return {tanX, tanX * imageSize.y / imageSize.x};
}

std::optional<Vec2> project(const EditorCamera& camera, const Basis& basis, Vec2 tangents, Vec2 imageSize,
                            const Vec3& point) noexcept {
    const Vec3 offset = point - camera.position;
    const float depth = glm::dot(offset, basis.forward);
    if (!(depth >= kViewportNearPlane)) {
        return std::nullopt;
    }
    const float x = glm::dot(offset, basis.right) / (depth * tangents.x);
    const float y = glm::dot(offset, basis.up) / (depth * tangents.y);
    return Vec2{(x + 1.0f) * 0.5f * imageSize.x, (1.0f - y) * 0.5f * imageSize.y};
}

// 0 at kGizmoHiddenFacing or less, 1 from kGizmoOpaqueFacing, linear between.
float opacityOf(float facing) noexcept {
    return std::clamp((facing - kGizmoHiddenFacing) / (kGizmoOpaqueFacing - kGizmoHiddenFacing), 0.0f, 1.0f);
}

float distanceToSegment(Vec2 point, Vec2 a, Vec2 b) noexcept {
    const Vec2 segment = b - a;
    const float length2 = glm::dot(segment, segment);
    const float t = length2 > 0.0f ? std::clamp(glm::dot(point - a, segment) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::length(point - (a + segment * t));
}

float cross(Vec2 a, Vec2 b) noexcept {
    return a.x * b.y - a.y * b.x;
}

// Whether the convex quad `corners` (in order, either winding) with area contains `point`.
bool insideQuad(const std::array<Vec2, 4>& corners, Vec2 point) noexcept {
    const float area = cross(corners[2] - corners[0], corners[3] - corners[1]);
    if (std::abs(area) < 1.0f) {
        return false; // edge-on: no area to press
    }
    for (std::size_t edge = 0; edge < corners.size(); ++edge) {
        const Vec2 from = corners[edge];
        const Vec2 to = corners[(edge + 1) % corners.size()];
        if (cross(to - from, point - from) * area < 0.0f) {
            return false;
        }
    }
    return true;
}

// The index of the axis normal to plane `plane`.
std::size_t normalOf(std::size_t plane) noexcept {
    const std::array<std::size_t, 2> axes = gizmoPlaneAxes(plane);
    return 3 - axes[0] - axes[1];
}

} // namespace

bool hasImageArea(Vec2 imageSize) noexcept {
    return imageSize.x > 0.0f && imageSize.y > 0.0f && std::isfinite(imageSize.x) && std::isfinite(imageSize.y);
}

GizmoFrame gizmoFrame(const Mat4& world, GizmoSpace space) noexcept {
    GizmoFrame frame;
    frame.origin = Vec3(world[3]);
    if (space == GizmoSpace::World) {
        return frame;
    }
    // Gram-Schmidt on the matrix's columns: rotation only, scale and shear dropped.
    std::array<Vec3, 3> axes{Vec3(world[0]), Vec3(world[1]), Vec3(world[2])};
    for (std::size_t index = 0; index < axes.size(); ++index) {
        for (std::size_t before = 0; before < index; ++before) {
            axes[index] -= glm::dot(axes[index], axes[before]) * axes[before];
        }
        const float length = glm::length(axes[index]);
        if (!(length > kDegenerateLength) || !std::isfinite(length)) {
            return frame; // a collapsed direction: the world's axes
        }
        axes[index] /= length;
    }
    frame.axes = axes;
    return frame;
}

Ray pickRay(const EditorCamera& camera, Vec2 imageSize, Vec2 pixel) noexcept {
    const Basis basis = basisOf(camera);
    const bool area = hasImageArea(imageSize);
    ITH_ASSERT_MSG(area, "a pick ray needs an image with area");
    if (!area) {
        return {.origin = camera.position, .direction = basis.forward};
    }
    const Vec2 tangents = halfTangents(camera, imageSize);
    const float x = 2.0f * pixel.x / imageSize.x - 1.0f;
    const float y = 1.0f - 2.0f * pixel.y / imageSize.y;
    const Vec3 direction = basis.forward + basis.right * (x * tangents.x) + basis.up * (y * tangents.y);
    return {.origin = camera.position, .direction = glm::normalize(direction)};
}

std::optional<Vec2> projectToImage(const EditorCamera& camera, Vec2 imageSize, const Vec3& point) noexcept {
    if (!hasImageArea(imageSize)) {
        return std::nullopt;
    }
    return project(camera, basisOf(camera), halfTangents(camera, imageSize), imageSize, point);
}

std::optional<float> closestOnAxis(const Ray& ray, const Vec3& origin, const Vec3& axis) noexcept {
    // The closest points of the lines ray.origin + s * d and origin + t * axis.
    const Vec3 offset = ray.origin - origin;
    const float cosine = glm::dot(ray.direction, axis);
    const float sine2 = 1.0f - cosine * cosine;
    if (!(sine2 >= kParallelSine * kParallelSine)) {
        return std::nullopt;
    }
    const float alongRay = glm::dot(ray.direction, offset);
    const float alongAxis = glm::dot(axis, offset);
    const float s = (cosine * alongAxis - alongRay) / sine2;
    if (!(s > 0.0f)) {
        return std::nullopt;
    }
    return (alongAxis - cosine * alongRay) / sine2;
}

std::optional<Vec3> closestOnPlane(const Ray& ray, const Vec3& origin, const Vec3& normal) noexcept {
    const float facing = glm::dot(ray.direction, normal);
    if (!(std::abs(facing) >= kParallelSine)) {
        return std::nullopt;
    }
    const float s = glm::dot(origin - ray.origin, normal) / facing;
    if (!(s > 0.0f)) {
        return std::nullopt;
    }
    return ray.at(s);
}

std::array<std::size_t, 2> gizmoPlaneAxes(std::size_t plane) noexcept {
    ITH_ASSERT_MSG(plane < 3, "the gizmo has three planes");
    switch (plane) {
        case 1:
            return {1, 2};
        case 2:
            return {0, 2};
        default:
            return {0, 1};
    }
}

GizmoHandle gizmoAxisHandle(std::size_t axis) noexcept {
    constexpr std::array<GizmoHandle, 3> kAxes{GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z};
    ITH_ASSERT_MSG(axis < kAxes.size(), "the gizmo has three axes");
    return axis < kAxes.size() ? kAxes[axis] : GizmoHandle::None;
}

GizmoHandle gizmoPlaneHandle(std::size_t plane) noexcept {
    constexpr std::array<GizmoHandle, 3> kPlanes{GizmoHandle::XY, GizmoHandle::YZ, GizmoHandle::XZ};
    ITH_ASSERT_MSG(plane < kPlanes.size(), "the gizmo has three planes");
    return plane < kPlanes.size() ? kPlanes[plane] : GizmoHandle::None;
}

std::optional<std::size_t> gizmoAxisOf(GizmoHandle handle) noexcept {
    switch (handle) {
        case GizmoHandle::X:
            return 0;
        case GizmoHandle::Y:
            return 1;
        case GizmoHandle::Z:
            return 2;
        default:
            return std::nullopt;
    }
}

std::optional<std::size_t> gizmoPlaneOf(GizmoHandle handle) noexcept {
    switch (handle) {
        case GizmoHandle::XY:
            return 0;
        case GizmoHandle::YZ:
            return 1;
        case GizmoHandle::XZ:
            return 2;
        default:
            return std::nullopt;
    }
}

std::optional<GizmoLayout> layoutGizmo(const EditorCamera& camera, Vec2 imageSize, const GizmoFrame& frame,
                                       float arrowPixels) noexcept {
    const bool validLength = std::isfinite(arrowPixels) && arrowPixels > 0.0f;
    ITH_ASSERT_MSG(validLength, "the gizmo's arrow length must be finite and positive");
    if (!validLength || !hasImageArea(imageSize)) {
        return std::nullopt;
    }
    const Basis basis = basisOf(camera);
    const Vec2 tangents = halfTangents(camera, imageSize);
    const std::optional<Vec2> center = project(camera, basis, tangents, imageSize, frame.origin);
    if (!center.has_value()) {
        return std::nullopt;
    }
    // The world length that shows arrowPixels long at the origin's depth: pixels per metre there are
    // the focal length (half the width over tanX) over the depth.
    const Vec3 toOrigin = frame.origin - camera.position;
    const float depth = glm::dot(toOrigin, basis.forward);
    const float length = arrowPixels * depth * tangents.x / (0.5f * imageSize.x);
    const Vec3 view = glm::normalize(toOrigin);

    GizmoLayout layout;
    layout.center = *center;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::optional<Vec2> tip =
            project(camera, basis, tangents, imageSize, frame.origin + frame.axes[axis] * length);
        layout.axisEnds[axis] = tip.value_or(*center);
        layout.axisOpacity[axis] = tip.has_value() ? opacityOf(glm::length(glm::cross(frame.axes[axis], view))) : 0.0f;
    }
    const float inner = kGizmoPlaneOffset * length;
    const float outer = (kGizmoPlaneOffset + kGizmoPlaneSize) * length;
    for (std::size_t plane = 0; plane < 3; ++plane) {
        const std::array<std::size_t, 2> axes = gizmoPlaneAxes(plane);
        const Vec3 a = frame.axes[axes[0]];
        const Vec3 b = frame.axes[axes[1]];
        const std::array<Vec3, 4> corners{frame.origin + a * inner + b * inner, frame.origin + a * outer + b * inner,
                                          frame.origin + a * outer + b * outer, frame.origin + a * inner + b * outer};
        bool visible = true;
        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            const std::optional<Vec2> pixel = project(camera, basis, tangents, imageSize, corners[corner]);
            visible = visible && pixel.has_value();
            layout.planes[plane][corner] = pixel.value_or(*center);
        }
        const float facing = std::abs(glm::dot(frame.axes[normalOf(plane)], view));
        layout.planeOpacity[plane] = visible ? opacityOf(facing) : 0.0f;
    }
    return layout;
}

GizmoHandle hitGizmo(const GizmoLayout& layout, Vec2 pixel, float tolerance) noexcept {
    // Every arrow starts at the centre: there no axis is nearer than another, and the centre handle
    // is follow-up work, so a press there grabs nothing and a click on the pivot still picks.
    if (glm::length(pixel - layout.center) <= tolerance) {
        return GizmoHandle::None;
    }
    GizmoHandle best = GizmoHandle::None;
    float bestDistance = 0.0f;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!(layout.axisOpacity[axis] > 0.0f)) {
            continue;
        }
        const float distance = distanceToSegment(pixel, layout.center, layout.axisEnds[axis]);
        if (distance <= tolerance && (best == GizmoHandle::None || distance < bestDistance)) {
            best = gizmoAxisHandle(axis);
            bestDistance = distance;
        }
    }
    if (best != GizmoHandle::None) {
        return best;
    }
    for (std::size_t plane = 0; plane < 3; ++plane) {
        if (layout.planeOpacity[plane] > 0.0f && insideQuad(layout.planes[plane], pixel)) {
            return gizmoPlaneHandle(plane);
        }
    }
    return GizmoHandle::None;
}

std::optional<GizmoGrab> grabGizmo(GizmoHandle handle, const GizmoFrame& frame, const Ray& ray) noexcept {
    GizmoGrab grab{.handle = handle,
                   .frame = frame,
                   .start = frame.origin,
                   .reach = kGizmoMaxReach * glm::length(frame.origin - ray.origin)};
    if (const std::optional<std::size_t> axis = gizmoAxisOf(handle)) {
        const std::optional<float> t = closestOnAxis(ray, frame.origin, frame.axes[*axis]);
        if (!t.has_value()) {
            return std::nullopt;
        }
        grab.start = frame.origin + frame.axes[*axis] * *t;
        return grab;
    }
    if (const std::optional<std::size_t> plane = gizmoPlaneOf(handle)) {
        const std::optional<Vec3> point = closestOnPlane(ray, frame.origin, frame.axes[normalOf(*plane)]);
        if (!point.has_value()) {
            return std::nullopt;
        }
        grab.start = *point;
        return grab;
    }
    return std::nullopt;
}

std::optional<Vec3> dragGizmo(const GizmoGrab& grab, const Ray& ray) noexcept {
    std::optional<Vec3> point;
    if (const std::optional<std::size_t> axis = gizmoAxisOf(grab.handle)) {
        const Vec3 direction = grab.frame.axes[*axis];
        if (const std::optional<float> t = closestOnAxis(ray, grab.frame.origin, direction)) {
            point = grab.frame.origin + direction * *t;
        }
    } else if (const std::optional<std::size_t> plane = gizmoPlaneOf(grab.handle)) {
        point = closestOnPlane(ray, grab.frame.origin, grab.frame.axes[normalOf(*plane)]);
    }
    if (!point.has_value()) {
        return std::nullopt;
    }
    Vec3 delta = *point - grab.start;
    const float length = glm::length(delta);
    if (length > grab.reach) {
        delta *= grab.reach / length;
    }
    return delta;
}

Vec3 snapGizmo(const Vec3& delta, const GizmoFrame& frame, float step) noexcept {
    const bool valid = std::isfinite(step) && step > 0.0f;
    ITH_ASSERT_MSG(valid, "the snapping step must be finite and positive");
    if (!valid) {
        return delta;
    }
    Vec3 snapped{0.0f};
    for (const Vec3& axis : frame.axes) {
        snapped += axis * (std::round(glm::dot(delta, axis) / step) * step);
    }
    return snapped;
}

} // namespace ith
