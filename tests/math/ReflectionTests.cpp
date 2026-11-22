// The math types in reflection, the archives and the ECS: their records, a component with math
// fields saved through the text and binary archives, and the same component stored in a World.

#include "ith/ecs/World.h"
#include "ith/math/Math.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/TextArchive.h"

#include "MathComponents.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <ostream>
#include <string>
#include <string_view>

ITH_DECLARE_REFLECTION_MODULE(test_math)

namespace {

namespace serialization = ith::serialization;

constexpr serialization::DocumentFormat kFormat{.name = "test.math", .version = 1};

// The names of a record's fields, joined with commas.
std::string fieldNames(const ith::TypeInfo& type) {
    std::string names;
    for (const ith::FieldInfo& field : type.fields) {
        names += (names.empty() ? "" : ",") + std::string(field.name);
    }
    return names;
}

ith::test::Placement makePlacement() {
    ith::test::Placement placement;
    placement.local = {.translation = {1.0f, 2.0f, 3.0f},
                       .rotation = glm::angleAxis(0.5f, ith::kWorldUp),
                       .scale = {1.0f, 2.0f, 1.0f}};
    placement.velocity = {0.25f, -1.0f, 4.0f};
    placement.spin = ith::Quat(0.0f, 0.6f, 0.0f, 0.8f);
    placement.world = toMatrix(placement.local);
    placement.cell = {-3, 7};
    placement.precise = {1e9, 0.5, -2.25};
    placement.path = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    return placement;
}

} // namespace

TEST_SUITE("math.reflection") {
    TEST_CASE("math types are structs of their components or columns") {
        const ith::TypeInfo& vec3 = ith::typeOf<ith::Vec3>();
        CHECK(vec3.name == "ith::Vec3");
        CHECK(vec3.id == ith::fnv1a64("ith::Vec3"));
        CHECK(vec3.kind == ith::TypeKind::Struct);
        CHECK(vec3.size == 12);
        CHECK(vec3.triviallyCopyable);
        CHECK(fieldNames(vec3) == "x,y,z");
        CHECK(vec3.fields[2].offset == offsetof(ith::Vec3, z));
        CHECK(vec3.fields[0].type().id == ith::typeOf<float>().id);

        const ith::TypeInfo& quat = ith::typeOf<ith::Quat>();
        CHECK(fieldNames(quat) == "x,y,z,w");
        CHECK(quat.fields[3].offset == offsetof(ith::Quat, w));
        CHECK(fieldNames(ith::typeOf<ith::IVec4>()) == "x,y,z,w");
        CHECK(ith::typeOf<ith::IVec4>().fields[0].type().id == ith::typeOf<std::int32_t>().id);
        CHECK(ith::typeOf<ith::UVec2>().fields[0].type().id == ith::typeOf<std::uint32_t>().id);
        CHECK(ith::typeOf<ith::DVec3>().fields[0].type().id == ith::typeOf<double>().id);

        const ith::TypeInfo& mat4 = ith::typeOf<ith::Mat4>();
        CHECK(fieldNames(mat4) == "c0,c1,c2,c3");
        CHECK(mat4.fields[3].offset == 48);
        CHECK(mat4.fields[3].type().id == ith::typeOf<ith::Vec4>().id);
        CHECK(fieldNames(ith::typeOf<ith::Mat3>()) == "c0,c1,c2");
        CHECK(ith::typeOf<ith::Mat3>().fields[1].type().id == ith::typeOf<ith::Vec3>().id);

        const ith::TypeInfo& transform = ith::typeOf<ith::Transform>();
        CHECK(fieldNames(transform) == "translation,rotation,scale");
        CHECK(transform.fields[1].offset == offsetof(ith::Transform, rotation));
    }

    TEST_CASE("constructing through a record gives the zero or identity default") {
        alignas(ith::Mat4) std::array<std::byte, sizeof(ith::Mat4)> storage{};
        const ith::TypeInfo& mat4 = ith::typeOf<ith::Mat4>();
        mat4.construct(storage.data());
        CHECK(*std::launder(reinterpret_cast<const ith::Mat4*>(storage.data())) == ith::Mat4(1.0f));
        mat4.destroy(storage.data());
    }

    TEST_CASE("a new registry finds the math types by name") {
        const ith::TypeRegistry registry;
        CHECK(registry.find("ith::Vec3") == &ith::typeOf<ith::Vec3>());
        CHECK(registry.find("ith::Quat") == &ith::typeOf<ith::Quat>());
        CHECK(registry.find("ith::Mat4") == &ith::typeOf<ith::Mat4>());
        CHECK(registry.find("ith::Transform") == &ith::typeOf<ith::Transform>());
    }

    TEST_CASE("the generator accepts math fields") {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_test_math(registry);
        const ith::TypeInfo* placement = registry.find("ith::test::Placement");
        REQUIRE(placement != nullptr);
        CHECK(placement->role == ith::TypeRole::Component);
        CHECK(placement->findField("spin")->type().id == ith::typeOf<ith::Quat>().id);
        CHECK(placement->findField("local")->type().id == ith::typeOf<ith::Transform>().id);
    }

    TEST_CASE("vectors are saved as objects of their components") {
        const auto text = serialization::writeText(kFormat, ith::Vec3(1.0f, 2.5f, -3.0f));
        REQUIRE(text.hasValue());
        CHECK(*text == "{\n"
                       "  \"format\": \"test.math\",\n"
                       "  \"version\": 1,\n"
                       "  \"data\": {\n"
                       "    \"x\": 1,\n"
                       "    \"y\": 2.5,\n"
                       "    \"z\": -3\n"
                       "  }\n"
                       "}\n");
        ith::Vec3 read;
        REQUIRE(serialization::readText(*text, kFormat, read).hasValue());
        CHECK(read == ith::Vec3(1.0f, 2.5f, -3.0f));
    }

    TEST_CASE("a component with math fields round-trips through the text and binary archives") {
        const ith::test::Placement original = makePlacement();
        const ith::TypeInfo& type = ith::typeOf<ith::test::Placement>();

        const auto text = serialization::writeText(kFormat, original);
        REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
        ith::test::Placement fromText;
        const auto readText = serialization::readText(*text, kFormat, fromText);
        REQUIRE_MESSAGE(readText.hasValue(), readText.error().toString());
        CHECK(type.equals(&fromText, &original));

        const auto blob = serialization::writeBinary(kFormat, original);
        REQUIRE_MESSAGE(blob.hasValue(), blob.error().toString());
        ith::test::Placement fromBinary;
        const auto readBinary = serialization::readBinary(*blob, kFormat, fromBinary);
        REQUIRE_MESSAGE(readBinary.hasValue(), readBinary.error().toString());
        CHECK(type.equals(&fromBinary, &original));
    }

    TEST_CASE("a component with math fields lives in a World") {
        ith::World world;
        const ith::Entity entity = world.spawn(ith::test::Placement{});
        const ith::Entity moved = world.spawn(makePlacement());
        // The defaults survive storage: identity rotation and matrix, unit scale.
        const ith::test::Placement* placement = world.get<ith::test::Placement>(entity);
        REQUIRE(placement != nullptr);
        CHECK(placement->world == ith::Mat4(1.0f));
        CHECK(placement->spin == ith::Quat(0.0f, 0.0f, 0.0f, 1.0f));
        CHECK(placement->local.scale == ith::Vec3(1.0f));
        // Despawning the first entity moves the second into its row.
        world.despawn(entity);
        const ith::test::Placement* after = world.get<ith::test::Placement>(moved);
        REQUIRE(after != nullptr);
        const ith::test::Placement expected = makePlacement();
        CHECK(ith::typeOf<ith::test::Placement>().equals(after, &expected));
    }
}
