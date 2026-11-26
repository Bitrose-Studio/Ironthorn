// The primitive shapes (ADR-0042 rule 19): each shape's bounds, its mesh (valid indices, unit
// outward normals, counter-clockwise winding seen from outside, every vertex inside the bounds),
// ShapeRenderer's reflection record, a level save and load round trip of a ShapeRenderer,
// and an unknown shape name kept unresolved.

#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/level/UnresolvedComponents.h"
#include "ith/math/Geometry.h"
#include "ith/math/Math.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Error.h"
#include "ith/shapes/ShapeMesh.h"
#include "ith/shapes/ShapeRenderer.h"

#include "AssertCapture.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(shapes)

namespace {

using ith::Aabb;
using ith::Shape;
using ith::ShapeMesh;
using ith::ShapeRenderer;
using ith::Vec3;

constexpr std::array kShapes{Shape::Cube, Shape::Sphere, Shape::Plane, Shape::Cylinder};
constexpr float kEpsilon = 1e-5f;

std::string_view nameOf(Shape shape) {
    switch (shape) {
        case Shape::Cube:
            return "Cube";
        case Shape::Sphere:
            return "Sphere";
        case Shape::Plane:
            return "Plane";
        case Shape::Cylinder:
            return "Cylinder";
    }
    return "?";
}

bool inside(const Aabb& box, const Vec3& point) {
    return point.x >= box.min.x - kEpsilon && point.x <= box.max.x + kEpsilon && point.y >= box.min.y - kEpsilon &&
           point.y <= box.max.y + kEpsilon && point.z >= box.min.z - kEpsilon && point.z <= box.max.z + kEpsilon;
}

const ith::TypeRegistry& registry() {
    static const ith::TypeRegistry kRegistry = [] {
        ith::TypeRegistry types;
        ith::generated::registerReflection_ecs(types);
        ith::generated::registerReflection_shapes(types);
        return types;
    }();
    return kRegistry;
}

} // namespace

TEST_SUITE("shapes") {
    TEST_CASE("each shape's bounds are a unit box centred on the origin, flat for the plane") {
        const Aabb unit{.min = {-0.5f, -0.5f, -0.5f}, .max = {0.5f, 0.5f, 0.5f}};
        CHECK(ith::shapeBounds(Shape::Cube) == unit);
        CHECK(ith::shapeBounds(Shape::Sphere) == unit);
        CHECK(ith::shapeBounds(Shape::Cylinder) == unit);

        const Aabb plane = ith::shapeBounds(Shape::Plane);
        CHECK(plane == Aabb{.min = {-0.5f, 0.0f, -0.5f}, .max = {0.5f, 0.0f, 0.5f}});
        // Zero height is a valid, non-empty box: it has a centre and a bottom and top at y = 0.
        CHECK(plane.min.y == plane.max.y);
        CHECK(plane.center() == Vec3(0.0f));
        CHECK(plane.halfExtents() == Vec3(0.5f, 0.0f, 0.5f));
    }

    TEST_CASE("meshes are small triangle lists with valid indices") {
        for (const Shape shape : kShapes) {
            CAPTURE(nameOf(shape));
            const ShapeMesh mesh = ith::shapeMesh(shape);
            REQUIRE_FALSE(mesh.vertices.empty());
            REQUIRE(mesh.indices.size() % 3 == 0);
            const std::size_t triangles = mesh.indices.size() / 3;
            CHECK(triangles >= 2);
            CHECK(triangles <= 1024);
            for (const std::uint16_t index : mesh.indices) {
                REQUIRE(index < mesh.vertices.size());
            }
        }
        CHECK(ith::shapeMesh(Shape::Cube).indices.size() == 36);
        // 16 segments: two triangles per quad, one per quad touching a pole (12 rings).
        CHECK(ith::shapeMesh(Shape::Sphere).indices.size() == 3 * 16 * 11 * 2);
        // 16 segments: two triangles per side quad and one per cap slice.
        CHECK(ith::shapeMesh(Shape::Cylinder).indices.size() == 3 * 16 * 4);
        CHECK(ith::shapeMesh(Shape::Plane).indices.size() == 6);
        CHECK(ith::shapeMesh(Shape::Plane).vertices.size() == 4);
    }

    TEST_CASE("the same shape gives the same data every call") {
        const ShapeMesh first = ith::shapeMesh(Shape::Sphere);
        const ShapeMesh second = ith::shapeMesh(Shape::Sphere);
        CHECK(first.vertices.data() == second.vertices.data());
        CHECK(first.indices.data() == second.indices.data());
        CHECK(first.vertices.size() == second.vertices.size());
    }

    TEST_CASE("every vertex lies inside the shape's bounds, and the bounds are tight") {
        for (const Shape shape : kShapes) {
            CAPTURE(nameOf(shape));
            const Aabb bounds = ith::shapeBounds(shape);
            const ShapeMesh mesh = ith::shapeMesh(shape);
            Aabb reached{.min = mesh.vertices[0].position, .max = mesh.vertices[0].position};
            for (const ith::ShapeVertex& vertex : mesh.vertices) {
                CHECK(inside(bounds, vertex.position));
                reached.min = glm::min(reached.min, vertex.position);
                reached.max = glm::max(reached.max, vertex.position);
            }
            for (int axis = 0; axis < 3; ++axis) {
                CHECK(std::abs(reached.min[axis] - bounds.min[axis]) < kEpsilon);
                CHECK(std::abs(reached.max[axis] - bounds.max[axis]) < kEpsilon);
            }
        }
    }

    TEST_CASE("normals are unit length and point outwards; the plane's point up") {
        for (const Shape shape : kShapes) {
            CAPTURE(nameOf(shape));
            const Vec3 centre = ith::shapeBounds(shape).center();
            for (const ith::ShapeVertex& vertex : ith::shapeMesh(shape).vertices) {
                CHECK(std::abs(glm::length(vertex.normal) - 1.0f) < kEpsilon);
                if (shape == Shape::Plane) {
                    CHECK(vertex.normal == ith::kWorldUp);
                } else {
                    CHECK(glm::dot(vertex.normal, vertex.position - centre) > 0.0f);
                }
            }
        }
    }

    TEST_CASE("triangles are counter-clockwise seen from outside and not degenerate") {
        for (const Shape shape : kShapes) {
            CAPTURE(nameOf(shape));
            const ShapeMesh mesh = ith::shapeMesh(shape);
            for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
                const ith::ShapeVertex& a = mesh.vertices[mesh.indices[at]];
                const ith::ShapeVertex& b = mesh.vertices[mesh.indices[at + 1]];
                const ith::ShapeVertex& c = mesh.vertices[mesh.indices[at + 2]];
                const Vec3 face = glm::cross(b.position - a.position, c.position - a.position);
                CAPTURE(at);
                // Area above a small fraction of the smallest sphere triangle.
                REQUIRE(glm::length(face) > 1e-4f);
                CHECK(glm::dot(face, a.normal + b.normal + c.normal) > 0.0f);
            }
        }
    }

    TEST_CASE("the cube and the plane have flat faces") {
        for (const Shape shape : {Shape::Cube, Shape::Plane}) {
            CAPTURE(nameOf(shape));
            const ShapeMesh mesh = ith::shapeMesh(shape);
            for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
                const ith::ShapeVertex& a = mesh.vertices[mesh.indices[at]];
                const ith::ShapeVertex& b = mesh.vertices[mesh.indices[at + 1]];
                const ith::ShapeVertex& c = mesh.vertices[mesh.indices[at + 2]];
                const Vec3 face = glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
                CHECK(glm::length(face - a.normal) < kEpsilon);
                CHECK(a.normal == b.normal);
                CHECK(a.normal == c.normal);
            }
        }
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("a value that is not a Shape asserts and gives an empty mesh and empty bounds") {
        ith::test::AssertCapture capture;
        const auto invalid = static_cast<Shape>(200);
        const ShapeMesh mesh = ith::shapeMesh(invalid);
        CHECK(mesh.vertices.empty());
        CHECK(mesh.indices.empty());
        const Aabb bounds = ith::shapeBounds(invalid);
        CHECK(bounds.min.x > bounds.max.x);
        CHECK(capture.count() == 2);
    }
#endif

    TEST_CASE("ShapeRenderer is a reflected component with a shape and a colour") {
        const ith::TypeInfo& type = ith::typeOf<ShapeRenderer>();
        CHECK(type.name == "ith::ShapeRenderer");
        CHECK(type.role == ith::TypeRole::Component);
        REQUIRE(type.fields.size() == 2);
        CHECK(type.fields[0].name == "shape");
        CHECK(type.fields[0].type().id == ith::typeOf<Shape>().id);
        CHECK(type.fields[1].name == "color");
        CHECK(type.fields[1].type().id == ith::typeOf<Vec3>().id);
        CHECK(registry().find("ith::ShapeRenderer") == &type);

        const ith::TypeInfo& shape = ith::typeOf<Shape>();
        CHECK(registry().find("ith::Shape") == &shape);
        REQUIRE(shape.enumerators.size() == 4);
        CHECK(shape.enumerators[0].name == "Cube");
        CHECK(shape.enumerators[1].name == "Sphere");
        CHECK(shape.enumerators[2].name == "Plane");
        CHECK(shape.enumerators[3].name == "Cylinder");

        const ShapeRenderer defaults;
        CHECK(defaults.shape == Shape::Cube);
        CHECK(defaults.color == Vec3(0.8f, 0.8f, 0.8f));
    }

    TEST_CASE("a ShapeRenderer is saved into a level and loads back") {
        constexpr ith::Uuid kLevel(0x1b2c3d4e5f607182, 0x93a4b5c6d7e8f901);
        constexpr ith::Uuid kEntity(0, 7);
        const ShapeRenderer saved{.shape = Shape::Cylinder, .color = {0.25f, 0.5f, 1.0f}};

        ith::World source;
        source.spawn(ith::StableId{.id = kEntity}, saved);
        const auto text = ith::saveLevel(source, kLevel, registry());
        REQUIRE(text.hasValue());
        CHECK(text->find(R"("ith::ShapeRenderer")") != std::string::npos);
        CHECK(text->find(R"("Cylinder")") != std::string::npos);

        ith::World loaded;
        const auto info = ith::loadLevel(*text, loaded, registry(), {.source = "shapes.ithmap"});
        REQUIRE(info.hasValue());
        const ith::Entity entity = loaded.findByStableId(kEntity);
        REQUIRE(loaded.isAlive(entity));
        const ShapeRenderer* renderer = loaded.get<const ShapeRenderer>(entity);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->shape == saved.shape);
        CHECK(renderer->color == saved.color);

        // Saving the loaded world gives the same bytes.
        const auto again = ith::saveLevel(loaded, kLevel, registry());
        REQUIRE(again.hasValue());
        CHECK(*again == *text);
    }

    TEST_CASE("a ShapeRenderer with an unknown shape name is kept unresolved and saved back") {
        constexpr ith::Uuid kLevel(0x1b2c3d4e5f607182, 0x93a4b5c6d7e8f901);
        constexpr ith::Uuid kEntity(0, 7);
        const std::string text =
            R"({"format": "ith.level", "version": 1, "id": ")" + kLevel.toString() +
            R"(", "data": {"entities": [{"id": ")" + kEntity.toString() +
            R"(", "components": {"ith::ShapeRenderer": {"shape": "Torus", "color": {"x": 1, "y": 1, "z": 1}}}}]}})";

        ith::World world;
        std::vector<ith::serialization::Error> warnings;
        const auto info = ith::loadLevel(text, world, registry(), {.source = "shapes.ithmap", .warnings = &warnings});
        REQUIRE(info.hasValue());
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].code == "level.unresolved_component");
        const ith::Entity entity = world.findByStableId(kEntity);
        REQUIRE(world.isAlive(entity));
        CHECK_FALSE(world.has<ShapeRenderer>(entity));
        CHECK(world.has<ith::UnresolvedComponents>(entity));

        const auto saved = ith::saveLevel(world, kLevel, registry());
        REQUIRE(saved.hasValue());
        CHECK(saved->find(R"("Torus")") != std::string::npos);
    }
}
