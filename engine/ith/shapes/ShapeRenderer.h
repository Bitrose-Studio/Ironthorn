#pragma once

// ith::ShapeRenderer: draws an entity as a primitive shape in the editor's preview (ADR-0042
// rule 19).
//
//   world.spawn(ith::LocalTransform{}, ith::WorldTransform{},
//               ith::ShapeRenderer{.shape = ith::Shape::Sphere, .color = {0.9f, 0.2f, 0.1f}});
//
// A runtime component: saved into levels like any component (ADR-0034), copied into Play In
// Editor worlds and loadable by every target. The shape is placed by the entity's WorldTransform;
// its geometry and local bounds come from shapeMesh and shapeBounds (ShapeMesh.h). The preview is
// unlit and has no materials; mesh components replace this component in P3.

#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>

namespace ith {

// The primitive shapes, each a unit shape centred on the origin (sizes in ShapeMesh.h).
enum class Shape : std::uint8_t {
    Cube,
    Sphere,
    Plane,
    Cylinder,
};
ITH_ENUM(Shape)

// Draws its entity as `shape`, in `color`, placed by the entity's WorldTransform.
struct ShapeRenderer {
    ITH_TYPE(Component)
    // Which shape to draw.
    ITH_FIELD()
    Shape shape = Shape::Cube;
    // The shape's colour, linear RGB (ADR-0006 rule 8), each channel usually in [0, 1].
    ITH_FIELD()
    Vec3 color{0.8f, 0.8f, 0.8f};
};

} // namespace ith
