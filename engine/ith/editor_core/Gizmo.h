#pragma once

// The translate gizmo's math (ADR-0042 rules 23, 24 and 26), pure and without any UI or GPU: the
// pick ray through a pixel of a viewport image, the handles' screen-space layout and hit test, the
// closest point on an axis or a plane to a ray, the drag's world-space delta and snapping.
//
//   const ith::GizmoFrame frame = ith::gizmoFrame(world.worldMatrixOf(primary), ith::GizmoSpace::World);
//   const std::optional<ith::GizmoLayout> layout =
//       ith::layoutGizmo(camera, imageSize, frame, ith::kGizmoArrowLength * displayScale);
//   const ith::GizmoHandle handle =
//       ith::hitGizmo(*layout, pressPixel, ith::kGizmoGrabTolerance * displayScale);
//   std::optional<ith::GizmoGrab> grab = ith::grabGizmo(handle, frame, ith::pickRay(camera, imageSize, pressPixel));
//   // every frame of the drag:
//   if (const std::optional<ith::Vec3> delta = ith::dragGizmo(*grab, ith::pickRay(camera, imageSize, pixel))) {
//       moved = ith::snapGizmo(*delta, frame, 0.1f); // or *delta without snapping
//   }
//
// Pixels are image pixels: window pixels from the viewport image's top-left corner, x right and y
// down, over an image `imageSize` pixels large. The camera's projection is the viewport's
// (ADR-0046): its horizontal fovX across the image's width and verticalFov(fovX, aspect) up it, so a
// pixel's ray and a point's pixel agree with what the viewport draws.
//
// The gizmo has three axis handles (X, Y, Z: the frame's axes) and three plane handles (XY, YZ, XZ:
// squares between two axes). Its size on screen is constant: an arrow is `arrowPixels` long when
// its axis is seen side-on. An axis seen nearly end-on, or a plane seen nearly edge-on, fades out and
// can't be grabbed (opacity 0), so a drag never starts where moving the mouse would move the entity
// by unbounded amounts. During a drag, a ray nearly parallel to the dragged axis or plane, or one
// whose closest point lies behind the camera, gives no position (the drag keeps its last one), and
// the drag's reach is clamped to kGizmoMaxReach times the gizmo's distance from the camera: nothing
// explodes. All functions are pure and thread-safe.

#include "ith/editor_core/EditorCamera.h"
#include "ith/math/Geometry.h"
#include "ith/math/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ith {

// What the gizmo's axes follow (ADR-0042 rule 26).
enum class GizmoSpace : std::uint8_t {
    World, // the world axes
    Local, // the primary selected entity's rotation
};

// A handle of the translate gizmo; None: no handle.
enum class GizmoHandle : std::uint8_t {
    None,
    X,
    Y,
    Z,
    XY,
    YZ,
    XZ,
};

// An arrow's length on screen, in logical points (times the display scale in pixels), when its axis
// is seen side-on.
inline constexpr float kGizmoArrowLength = 100.0f;

// Where a plane square starts along both of its axes, and its side, as fractions of the arrow length.
inline constexpr float kGizmoPlaneOffset = 0.2f;
inline constexpr float kGizmoPlaneSize = 0.25f;

// How far from an arrow a press still grabs it, in logical points (times the display scale in pixels).
inline constexpr float kGizmoGrabTolerance = 6.0f;

// How squarely a handle must face the camera to be drawn and grabbed: for an axis the sine of its
// angle to the view direction, for a plane the cosine of its normal's. At kGizmoHiddenFacing or less
// the handle is hidden (opacity 0); from there it fades in, opaque from kGizmoOpaqueFacing.
inline constexpr float kGizmoHiddenFacing = 0.1f;
inline constexpr float kGizmoOpaqueFacing = 0.2f;

// A drag moves at most this many times the gizmo's distance from the camera from where it began.
inline constexpr float kGizmoMaxReach = 50.0f;

// Where the gizmo is and which way its axes point: an origin and three orthonormal axes (X, Y, Z),
// the world's by default.
struct GizmoFrame {
    Vec3 origin{0.0f};
    std::array<Vec3, 3> axes{kWorldRight, kWorldUp, Vec3{0.0f, 0.0f, 1.0f}};
};

// The frame of an entity whose world matrix is `world`: its translation, and the world's axes
// (GizmoSpace::World) or the matrix's rotation (GizmoSpace::Local: its columns normalized and made
// orthogonal, X first, so scale and shear are ignored). A matrix that collapses a direction (a zero
// scale) gives the world's axes.
[[nodiscard]] GizmoFrame gizmoFrame(const Mat4& world, GizmoSpace space) noexcept;

// Whether an image `imageSize` pixels large has area: both sides finite and positive. Every function
// here, and the viewport consumer, use this one check.
[[nodiscard]] bool hasImageArea(Vec2 imageSize) noexcept;

// The ray from the camera through `pixel` of an image `imageSize` pixels large: it starts at the
// camera's position and its direction is unit length. Pixels outside the image give rays outside the
// view. Precondition (asserted): the image has area; otherwise the camera's view direction.
[[nodiscard]] Ray pickRay(const EditorCamera& camera, Vec2 imageSize, Vec2 pixel) noexcept;

// Where `point` shows in an image `imageSize` pixels large, possibly outside it; std::nullopt when it
// is not in front of the camera's near plane (kViewportNearPlane) or the image has no area.
[[nodiscard]] std::optional<Vec2> projectToImage(const EditorCamera& camera, Vec2 imageSize,
                                                 const Vec3& point) noexcept;

// The parameter t of the point origin + t * axis of the axis line that is closest to `ray`, for a
// unit-length `axis` and ray direction; std::nullopt when the ray is within about half a degree of
// parallel to the axis, or when the closest point on the ray's line lies behind its origin.
[[nodiscard]] std::optional<float> closestOnAxis(const Ray& ray, const Vec3& origin, const Vec3& axis) noexcept;

// Where `ray` meets the plane through `origin` with unit-length `normal`; std::nullopt when the ray
// is within about half a degree of parallel to the plane, or meets it behind its origin.
[[nodiscard]] std::optional<Vec3> closestOnPlane(const Ray& ray, const Vec3& origin, const Vec3& normal) noexcept;

// The gizmo's handles on screen, in image pixels. Plane i (0: XY, 1: YZ, 2: XZ) spans gizmoPlaneAxes(i).
struct GizmoLayout {
    // The origin's pixel.
    Vec2 center{0.0f};
    // Each arrow's tip, by axis.
    std::array<Vec2, 3> axisEnds{};
    // 0 (hidden, not grabbable) to 1 (opaque), by axis.
    std::array<float, 3> axisOpacity{};
    // Each plane square's corners in order around it, by plane.
    std::array<std::array<Vec2, 4>, 3> planes{};
    // 0 to 1, by plane.
    std::array<float, 3> planeOpacity{};
};

// The two axes (frame axis indices) plane handle `plane` (0: XY, 1: YZ, 2: XZ) spans; its normal is
// the third. Precondition (asserted): plane < 3; otherwise XY's.
[[nodiscard]] std::array<std::size_t, 2> gizmoPlaneAxes(std::size_t plane) noexcept;

// The gizmo handle of axis `axis` (0 to 2) or plane `plane` (0 to 2), and back: the axis index of an
// axis handle and the plane index of a plane handle, std::nullopt for the other kinds.
[[nodiscard]] GizmoHandle gizmoAxisHandle(std::size_t axis) noexcept;
[[nodiscard]] GizmoHandle gizmoPlaneHandle(std::size_t plane) noexcept;
[[nodiscard]] std::optional<std::size_t> gizmoAxisOf(GizmoHandle handle) noexcept;
[[nodiscard]] std::optional<std::size_t> gizmoPlaneOf(GizmoHandle handle) noexcept;

// The layout of the gizmo at `frame` seen by `camera` in an image `imageSize` pixels large, with
// arrows `arrowPixels` long when seen side-on (kGizmoArrowLength times the display scale). Plane
// squares span kGizmoPlaneOffset to kGizmoPlaneOffset + kGizmoPlaneSize of the arrow along their
// two axes. A handle facing the camera less than kGizmoHiddenFacing, or with a point not in front
// of the near plane, has opacity 0. std::nullopt when the origin is not in front of the near plane
// or the image has no area: no gizmo is shown. Precondition (asserted): `arrowPixels` is finite and
// positive; otherwise std::nullopt.
[[nodiscard]] std::optional<GizmoLayout> layoutGizmo(const EditorCamera& camera, Vec2 imageSize,
                                                     const GizmoFrame& frame, float arrowPixels) noexcept;

// The handle a press at `pixel` grabs: None within `tolerance` pixels of the centre, where every
// arrow starts (the gizmo has no centre handle in P2, so a click on the pivot still picks); else the
// nearest arrow within `tolerance` pixels of its segment (from the centre to its tip); else the
// first plane square (XY, YZ, XZ) that contains the pixel; else None. Handles with opacity 0 are
// never grabbed.
[[nodiscard]] GizmoHandle hitGizmo(const GizmoLayout& layout, Vec2 pixel, float tolerance) noexcept;

// A drag's start: the grabbed handle, the gizmo's frame at the press, the point the press's ray
// grabbed (on the axis line or the plane) and how far the drag may reach from it, in metres.
struct GizmoGrab {
    GizmoHandle handle = GizmoHandle::None;
    GizmoFrame frame;
    Vec3 start{0.0f};
    float reach = 0.0f;
};

// Starts a drag of `handle` of the gizmo at `frame` with the press's ray; std::nullopt for None or
// when the ray grabs no point of the handle's axis or plane (closestOnAxis, closestOnPlane).
[[nodiscard]] std::optional<GizmoGrab> grabGizmo(GizmoHandle handle, const GizmoFrame& frame, const Ray& ray) noexcept;

// The world-space translation dragging `grab` to `ray` gives: along the grabbed axis, or within the
// grabbed plane, at most grab.reach long. std::nullopt when the ray grabs no point (nearly parallel,
// or behind the camera): the caller keeps the last translation.
[[nodiscard]] std::optional<Vec3> dragGizmo(const GizmoGrab& grab, const Ray& ray) noexcept;

// `delta` with each of its components along the frame's axes rounded to a multiple of `step`, so
// snapping happens in gizmo space. Precondition (asserted): `step` is finite and positive; otherwise
// `delta` unchanged.
[[nodiscard]] Vec3 snapGizmo(const Vec3& delta, const GizmoFrame& frame, float step) noexcept;

} // namespace ith
