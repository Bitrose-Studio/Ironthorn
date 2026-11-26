#pragma once

// The geometry of the primitive shapes (ADR-0042 rules 19, 27 and 28): each shape's local bounds
// and its unit mesh.
//
//   const ith::ShapeMesh mesh = ith::shapeMesh(renderer.shape);
//   upload(mesh.vertices); // positions and normals
//   upload(mesh.indices);  // a triangle list
//   const ith::Aabb world = ith::transformBounds(ith::shapeBounds(renderer.shape), worldMatrix);
//
// The unit shapes, in local space (metres, right-handed, +Y up, ADR-0006):
// - Cube: 1 m edges, centred on the origin.
// - Sphere: 0.5 m radius, centred on the origin; 16 segments around Y and 12 rings.
// - Plane: a 1 m square on y = 0, centred on the origin, facing +Y. One-sided: from below, its
//   triangles are back faces. Its bounds have zero height (min.y == max.y == 0), which is a valid,
//   non-empty box.
// - Cylinder: 0.5 m radius and 1 m tall along Y, centred on the origin, with closed caps; 16
//   segments around Y.
//
// Meshes are triangle lists with 16-bit indices. Front faces are counter-clockwise when seen from
// outside the shape (from +Y for the plane); normals are unit length and point outwards. Flat
// faces (the cube's sides, the plane, the cylinder's caps) have their own vertices so their
// normals are flat; the sphere and the cylinder's side are smooth.
//
// Thread safety: every function here may be called from any thread. Each shape's mesh is built once,
// on the first shapeMesh call for that shape, and never changes after.

#include "ith/math/Geometry.h"
#include "ith/math/Types.h"
#include "ith/shapes/ShapeRenderer.h"

#include <cstdint>
#include <span>

namespace ith {

// One vertex of a shape's mesh. 24 bytes, no padding.
struct ShapeVertex {
    Vec3 position;
    Vec3 normal;
};

// A shape's mesh: views of static data that stay valid until static destruction.
struct ShapeMesh {
    std::span<const ShapeVertex> vertices;
    // A triangle list: three indices into `vertices` per triangle.
    std::span<const std::uint16_t> indices;
};

// The unit mesh of `shape` (see the header comment). Precondition (asserted): `shape` is one of
// the enumerators; otherwise the mesh is empty.
[[nodiscard]] ShapeMesh shapeMesh(Shape shape) noexcept;

// The local bounds of `shape`: the box from (-0.5, -0.5, -0.5) to (0.5, 0.5, 0.5), except the
// plane's, from (-0.5, 0, -0.5) to (0.5, 0, 0.5). Every vertex of shapeMesh(shape) lies inside.
// Precondition (asserted): `shape` is one of the enumerators; otherwise the box is empty (min >
// max).
[[nodiscard]] Aabb shapeBounds(Shape shape) noexcept;

} // namespace ith
