#include "ith/editor_core/AddShape.h"

#include "ith/foundation/Assert.h"
#include "ith/math/Types.h"
#include "ith/shapes/ShapeMesh.h"

#include <cstddef>
#include <string>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

static_assert(static_cast<std::size_t>(Shape::Cylinder) + 1 == kShapeChoices.size(),
              "kShapeChoices lists every shape in enumerator order");

Value vec3Value(const Vec3& value) {
    Value object = Value::object();
    object.set("x", Value::real(value.x));
    object.set("y", Value::real(value.y));
    object.set("z", Value::real(value.z));
    return object;
}

} // namespace

Value addShapeArgs(Shape shape, const EditorCamera& camera) {
    const auto index = static_cast<std::size_t>(shape);
    ITH_ASSERT_MSG(index < kShapeChoices.size(), "a shape is one of the Shape enumerators");
    const ShapeChoice choice = index < kShapeChoices.size() ? kShapeChoices[index] : kShapeChoices.front();

    // The view direction is the view matrix's third row, negated (it looks along -Z).
    const Mat4 view = viewMatrix(camera);
    const Vec3 forward = -Vec3(view[0][2], view[1][2], view[2][2]);
    Vec3 position = camera.position + forward * kAddShapeDistance;
    position.y = -shapeBounds(choice.shape).min.y;

    Value translation = Value::object();
    translation.set("translation", vec3Value(position));
    Value transform = Value::object();
    transform.set("value", std::move(translation));
    Value renderer = Value::object();
    renderer.set("shape", Value::string(std::string(choice.name)));
    renderer.set("color", vec3Value(ShapeRenderer{}.color));
    Value components = Value::object();
    components.set("ith::LocalTransform", std::move(transform));
    components.set("ith::ShapeRenderer", std::move(renderer));
    Value args = Value::object();
    args.set("label", Value::string(std::string(choice.name)));
    args.set("components", std::move(components));
    return args;
}

} // namespace ith
