#pragma once

// Placing primitive shapes (ADR-0042 rule 19), without any UI: the shapes the editor offers, by
// name, and the arguments of ith.entity.create that add one in front of a viewport camera.
//
//   const ith::EditorCamera& camera = session.viewports().find(ith::kDefaultViewport)->camera;
//   session.execute("ith.entity.create", ith::addShapeArgs(ith::Shape::Cube, camera));
//
// Every function here may be called from any thread.

#include "ith/editor_core/EditorCamera.h"
#include "ith/serialization/Value.h"
#include "ith/shapes/ShapeRenderer.h"

#include <array>
#include <string_view>

namespace ith {

// A shape and the name menus and labels show for it, which is also its enumerator's name.
struct ShapeChoice {
    Shape shape = Shape::Cube;
    std::string_view name;
};

// Every shape, in enumerator order, so a Shape's value indexes it.
inline constexpr std::array<ShapeChoice, 4> kShapeChoices{{{.shape = Shape::Cube, .name = "Cube"},
                                                           {.shape = Shape::Sphere, .name = "Sphere"},
                                                           {.shape = Shape::Plane, .name = "Plane"},
                                                           {.shape = Shape::Cylinder, .name = "Cylinder"}}};

// How far in front of the camera addShapeArgs places a shape, in metres.
inline constexpr float kAddShapeDistance = 2.0f;

// The arguments of ith.entity.create that add `shape`: labelled with its name, with a ShapeRenderer
// of the default colour, kAddShapeDistance in front of `camera` along its view direction, and
// lifted or lowered so the bottom of its bounds rests on y = 0. Precondition (asserted): `shape` is
// one of the enumerators; otherwise a cube is added.
[[nodiscard]] serialization::Value addShapeArgs(Shape shape, const EditorCamera& camera);

} // namespace ith
