// Add Shape (ADR-0042 rule 19), headless: the shapes the editor offers and the arguments of
// ith.entity.create that place one in front of a camera, resting on the ground; and those arguments
// run through a session.

#include "ith/editor_core/AddShape.h"

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/math/Types.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Value.h"
#include "ith/shapes/ShapeRenderer.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)

using namespace ith;

namespace {

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        generated::registerReflection_shapes(result);
        return result;
    }();
    return kTypes;
}

} // namespace

TEST_SUITE("editor_core.add_shape") {
    TEST_CASE("the shape choices list every shape in enumerator order, by name") {
        for (std::size_t index = 0; index < kShapeChoices.size(); ++index) {
            CHECK(static_cast<std::size_t>(kShapeChoices[index].shape) == index);
        }
        CHECK(kShapeChoices[static_cast<std::size_t>(Shape::Sphere)].name == "Sphere");
    }

    TEST_CASE("Add Shape places a shape 2 m in front of the camera, on the ground") {
        EditorCamera camera;
        camera.position = {0.0f, 3.0f, 0.0f};
        camera.yaw = 0.0f;   // along -Z
        camera.pitch = 0.0f; // level
        const serialization::Value args = addShapeArgs(Shape::Cylinder, camera);
        CHECK(args.find("label")->asString() == "Cylinder");
        const serialization::Value* components = args.find("components");
        REQUIRE(components != nullptr);
        const serialization::Value* translation =
            components->find("ith::LocalTransform")->find("value")->find("translation");
        CHECK(translation->find("x")->toNumber<double>().value_or(-100.0) == doctest::Approx(0.0));
        CHECK(translation->find("y")->toNumber<double>().value_or(-100.0) == doctest::Approx(0.5));
        CHECK(translation->find("z")->toNumber<double>().value_or(-100.0) == doctest::Approx(-2.0));
        CHECK(components->find("ith::ShapeRenderer")->find("shape")->asString() == "Cylinder");
        // The plane has no height: it lies on the ground.
        const serialization::Value plane = addShapeArgs(Shape::Plane, camera);
        CHECK(plane.find("components")
                  ->find("ith::LocalTransform")
                  ->find("value")
                  ->find("translation")
                  ->find("y")
                  ->toNumber<double>()
                  .value_or(-100.0) == doctest::Approx(0.0));
    }

    TEST_CASE("the arguments create a selected, labelled shape through ith.entity.create") {
        EditorSession session(types());
        const EditorCamera camera;
        const Expected<serialization::Value> created =
            session.execute("ith.entity.create", addShapeArgs(Shape::Sphere, camera));
        REQUIRE(created.hasValue());
        const Expected<serialization::Value> entity = session.execute("ith.query.entity", *created);
        REQUIRE(entity.hasValue());
        CHECK(entity->find("components")->find("ith::ShapeRenderer")->find("shape")->asString() == "Sphere");
        CHECK(session.document().selection().entities().size() == 1);
    }
}
