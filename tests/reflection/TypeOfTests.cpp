// Tests of the records typeOf<T>() returns: leaves, generated structs and enums, containers,
// attributes and the Shipping text stripping.

#include "ith/reflection/TypeOf.h"

#include "ith/foundation/Hash.h"

#include "AssertCapture.h"
#include "Loadout.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

using ith::ReflectionFlags;
using ith::TypeKind;
using ith::TypeRole;

// A type derived from a reflected type is not reflected: its marker is the base's (ADR-0019).
struct DerivedHealth : ith::test::Health {};

enum class Unannotated : std::uint8_t {
    A,
};

static_assert(ith::detail::ReflectionAccess::kIsReflectedStruct<ith::test::Health>);
static_assert(ith::detail::ReflectionAccess::kIsReflectedStruct<ith::test::Inventory>);
static_assert(!ith::detail::ReflectionAccess::kIsReflectedStruct<DerivedHealth>);
static_assert(!ith::detail::ReflectionAccess::kIsReflectedStruct<int>);
static_assert(ith::detail::IsReflectedEnum<ith::test::DamageKind>);
static_assert(!ith::detail::IsReflectedEnum<Unannotated>);

} // namespace

TEST_SUITE("reflection.type_of") {
    TEST_CASE("leaf types have hand-written records") {
        const ith::TypeInfo& int32 = ith::typeOf<std::int32_t>();
        CHECK(int32.name == "int32");
        CHECK(int32.id == ith::fnv1a64("int32"));
        CHECK(int32.kind == TypeKind::SignedInteger);
        CHECK(int32.size == 4);
        CHECK(ith::typeOf<std::uint16_t>().kind == TypeKind::UnsignedInteger);
        CHECK(ith::typeOf<double>().kind == TypeKind::Float);
        CHECK(ith::typeOf<double>().size == 8);
        CHECK(ith::typeOf<bool>().kind == TypeKind::Bool);
        CHECK(ith::typeOf<std::string>().name == "std::string");
        CHECK(ith::typeOf<ith::Uuid>().name == "ith::Uuid");
        CHECK(ith::typeOf<float>().id == ith::fnv1a64("float"));
    }

    TEST_CASE("records say whether a type is trivially copyable") {
        CHECK(ith::typeOf<float>().triviallyCopyable);
        CHECK(ith::typeOf<ith::Uuid>().triviallyCopyable);
        CHECK_FALSE(ith::typeOf<std::string>().triviallyCopyable);
        CHECK(ith::typeOf<ith::test::Health>().triviallyCopyable);
        CHECK(ith::typeOf<ith::test::DamageKind>().triviallyCopyable);
        CHECK_FALSE(ith::typeOf<ith::test::AllLeaves>().triviallyCopyable);
        CHECK(ith::typeOf<std::array<float, 3>>().triviallyCopyable);
        CHECK_FALSE(ith::typeOf<std::vector<float>>().triviallyCopyable);
    }

    TEST_CASE("leaf value functions construct, copy, move, compare and destroy") {
        const ith::TypeInfo& type = ith::typeOf<std::string>();
        alignas(std::string) std::array<std::byte, sizeof(std::string)> a{};
        alignas(std::string) std::array<std::byte, sizeof(std::string)> b{};
        type.construct(a.data());
        *reinterpret_cast<std::string*>(a.data()) = "a long enough string to live on the heap";
        type.copy(b.data(), a.data());
        CHECK(type.equals(a.data(), b.data()));
        type.destroy(b.data());
        type.move(b.data(), a.data());
        CHECK(*reinterpret_cast<std::string*>(b.data()) == "a long enough string to live on the heap");
        CHECK_FALSE(type.equals(a.data(), b.data()));
        type.destroy(a.data());
        type.destroy(b.data());
    }

    TEST_CASE("a generated struct record matches the compiler's layout") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::Health>();
        CHECK(type.name == "ith::test::Health");
        CHECK(type.id == ith::fnv1a64("ith::test::Health"));
        CHECK(type.kind == TypeKind::Struct);
        CHECK(type.role == TypeRole::Component);
        CHECK(type.size == sizeof(ith::test::Health));
        CHECK(type.alignment == alignof(ith::test::Health));
        REQUIRE(type.fields.size() == 2); // cachedRatio is not reflected

        const ith::FieldInfo& current = type.fields[0];
        CHECK(current.name == "current");
        CHECK(current.id == ith::fnv1a64("current"));
        CHECK(current.offset == offsetof(ith::test::Health, current));
        CHECK(current.type().id == ith::typeOf<float>().id);
        CHECK(current.flags == (ReflectionFlags::Replicated | ReflectionFlags::SaveGame));
        CHECK(current.attributes.min == 0.0);
        CHECK(current.attributes.max == 10000.0);
        CHECK(current.attributes.unit == "hp");
        CHECK(type.fields[1].offset == offsetof(ith::test::Health, maximum));
        CHECK(type.fields[1].flags == ReflectionFlags::None);
        CHECK(type.fields[1].attributes.min == 1.0);
    }

    TEST_CASE("every role is recorded") {
        CHECK(ith::typeOf<ith::test::Vector3>().role == TypeRole::Struct);
        CHECK(ith::typeOf<ith::test::Containers>().role == TypeRole::Resource);
        CHECK(ith::typeOf<ith::test::Hit>().role == TypeRole::Event);
        CHECK(ith::typeOf<ith::test::Inventory>().role == TypeRole::Asset);
        CHECK(ith::typeOf<ith::test::Marker>().role == TypeRole::Component);
        CHECK(ith::typeOf<ith::test::DamageKind>().role == TypeRole::None);
    }

    TEST_CASE("types in the global namespace and in nested namespaces") {
        CHECK(ith::typeOf<GlobalCounter>().name == "GlobalCounter");
        CHECK(ith::typeOf<ith::test::gear::Loadout>().name == "ith::test::gear::Loadout");
        CHECK(ith::typeOf<ith::test::gear::Loadout>().fields[1].type().element().id ==
              ith::typeOf<ith::test::Hit>().id);
    }

    TEST_CASE("private fields of a class are reflected") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::Inventory>();
        REQUIRE(type.fields.size() == 2);
        CHECK(type.fields[0].name == "m_capacity");
        CHECK(type.fields[0].flags == ReflectionFlags::ReadOnly);
        CHECK(type.fields[1].flags == ReflectionFlags::Transient);

        ith::test::Inventory inventory;
        const auto* capacity = reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::byte*>(&inventory) +
                                                                     type.fields[0].offset);
        CHECK(*capacity == inventory.capacity());
    }

    TEST_CASE("a struct without reflected fields") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::Marker>();
        CHECK(type.fields.empty());
        const ith::test::Marker a;
        const ith::test::Marker b;
        CHECK(type.equals(&a, &b));
    }

    TEST_CASE("equality compares reflected fields only") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::Health>();
        ith::test::Health a;
        ith::test::Health b;
        b.cachedRatio = 0.25f;
        CHECK(type.equals(&a, &b));
        b.maximum = 50.0f;
        CHECK_FALSE(type.equals(&a, &b));
    }

    TEST_CASE("enum records list their enumerators and underlying type") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::DamageKind>();
        CHECK(type.kind == TypeKind::Enum);
        CHECK(type.name == "ith::test::DamageKind");
        CHECK(type.id == ith::fnv1a64("ith::test::DamageKind"));
        CHECK(type.size == 1);
        CHECK(type.element().id == ith::typeOf<std::uint8_t>().id);
        REQUIRE(type.enumerators.size() == 3);
        CHECK(type.enumerators[0].name == "Physical");
        CHECK(type.enumerators[2].name == "Poison");
        CHECK(type.enumerators[2].value == 7);
        REQUIRE(type.findEnumerator("Fire") != nullptr);
        CHECK(type.findEnumerator("Fire")->value == 1);
        CHECK(type.findEnumerator("Ice") == nullptr);

        CHECK(hasFlags(ith::typeOf<ith::test::Abilities>().flags, ReflectionFlags::Flags));
        CHECK(ith::typeOf<ith::test::Abilities>().enumerators[3].value == 4);
        CHECK(ith::typeOf<ith::test::Altitude>().enumerators[0].value == -300);
    }

    TEST_CASE("enum values are read and written through the record") {
        ith::test::Altitude altitude = ith::test::Altitude::Sea;
        const ith::TypeInfo& type = ith::typeOf<ith::test::Altitude>();
        ith::writeEnumValue(type, &altitude, -300);
        CHECK(altitude == ith::test::Altitude::Deep);
        CHECK(ith::readEnumValue(type, &altitude) == -300);

        ith::test::Abilities abilities = ith::test::Abilities::None;
        ith::writeEnumValue(ith::typeOf<ith::test::Abilities>(), &abilities, 5);
        CHECK(static_cast<std::uint32_t>(abilities) == 5);
        CHECK(ith::readEnumValue(ith::typeOf<ith::test::Abilities>(), &abilities) == 5);
    }

    TEST_CASE("field lookup by name, id and FormerName") {
        const ith::TypeInfo& type = ith::typeOf<ith::test::Health>();
        CHECK(type.findField("current") == &type.fields[0]);
        CHECK(type.findField(ith::fnv1a64("current")) == &type.fields[0]);
        CHECK(type.findField("maxHealth") == &type.fields[1]);
        CHECK(type.findField(ith::fnv1a64("maxHealth")) == &type.fields[1]);
        CHECK(type.findField("cachedRatio") == nullptr);
        CHECK(type.findField(std::uint64_t{0}) == nullptr);
    }

    TEST_CASE("containers describe their elements") {
        const ith::TypeInfo& vector = ith::typeOf<std::vector<ith::test::Vector3>>();
        CHECK(vector.kind == TypeKind::DynamicArray);
        CHECK(vector.name == "std::vector");
        CHECK(vector.element().id == ith::typeOf<ith::test::Vector3>().id);
        CHECK(vector.id != ith::typeOf<std::vector<float>>().id);
        CHECK(ith::typeOf<std::array<std::int32_t, 3>>().id != ith::typeOf<std::array<std::int32_t, 4>>().id);

        std::vector<ith::test::Vector3> points;
        vector.container->resize(&points, 2);
        CHECK(vector.container->size(&points) == 2);
        static_cast<ith::test::Vector3*>(vector.container->at(&points, 1))->y = 3.0f;
        CHECK(points[1].y == 3.0f);

        const ith::TypeInfo& optional = ith::typeOf<std::optional<double>>();
        CHECK(optional.kind == TypeKind::Optional);
        std::optional<double> maybe;
        CHECK(optional.container->size(&maybe) == 0);
        optional.container->resize(&maybe, 1);
        REQUIRE(maybe.has_value());
        *static_cast<double*>(optional.container->at(&maybe, 0)) = 2.5;
        CHECK(*maybe == 2.5);
        optional.container->resize(&maybe, 0);
        CHECK_FALSE(maybe.has_value());

        const ith::TypeInfo& fixed = ith::typeOf<std::array<std::int32_t, 3>>();
        CHECK(fixed.kind == TypeKind::FixedArray);
        CHECK(fixed.container->resize == nullptr);
        const std::array<std::int32_t, 3> values = {1, 2, 3};
        CHECK(fixed.container->size(&values) == 3);
        CHECK(*static_cast<const std::int32_t*>(fixed.container->constAt(&values, 2)) == 3);
    }

    TEST_CASE("container equality compares sizes and elements") {
        const ith::TypeInfo& type = ith::typeOf<std::vector<std::string>>();
        const std::vector<std::string> a = {"x", "y"};
        const std::vector<std::string> b = {"x", "y"};
        const std::vector<std::string> shorter = {"x"};
        const std::vector<std::string> different = {"x", "z"};
        CHECK(type.equals(&a, &b));
        CHECK_FALSE(type.equals(&a, &shorter));
        CHECK_FALSE(type.equals(&a, &different));
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("container access out of range asserts and gives null") {
        const ith::TypeInfo& vector = ith::typeOf<std::vector<std::int32_t>>();
        std::vector<std::int32_t> values = {1};
        ith::test::AssertCapture capture;
        CHECK(vector.container->at(&values, 0) == values.data());
        CHECK(capture.count() == 0);
        CHECK(vector.container->constAt(&values, 1) == nullptr);
        CHECK(capture.count() == 1);

        std::optional<float> empty;
        CHECK(ith::typeOf<std::optional<float>>().container->at(&empty, 0) == nullptr);
        CHECK(capture.count() == 2);
    }
#endif

    TEST_CASE("human-facing text is kept in development builds and stripped in Shipping") {
        const ith::TypeInfo& health = ith::typeOf<ith::test::Health>();
        const ith::TypeInfo& inventory = ith::typeOf<ith::test::Inventory>();
        // Kept in every configuration: names, Min, Max, Unit and FormerName.
        CHECK(health.fields[0].attributes.unit == "hp");
        CHECK(health.fields[1].attributes.formerName == "maxHealth");
        CHECK(ith::typeOf<ith::test::Hit>().attributes.formerName == "ith::test::Damage");
#ifdef ITH_SHIPPING
        CHECK(health.attributes.category.empty());
        CHECK(health.attributes.tooltip.empty());
        CHECK(health.fields[0].attributes.tooltip.empty());
        CHECK(ith::typeOf<ith::test::Vector3>().attributes.displayName.empty());
        CHECK(inventory.attributes.meta.empty());
        CHECK(inventory.fields[0].attributes.meta.empty());
#else
        CHECK(health.attributes.category == "Combat");
        CHECK(health.attributes.tooltip == "Hit points of an entity");
        CHECK(health.fields[0].attributes.tooltip == "Hit points");
        CHECK(ith::typeOf<ith::test::Vector3>().attributes.displayName == "Vector 3");
        CHECK(ith::typeOf<ith::test::DamageKind>().attributes.tooltip == "How damage is dealt");
        REQUIRE(inventory.attributes.meta.size() == 2);
        CHECK(inventory.attributes.meta[0].key == "Icon");
        CHECK(inventory.attributes.meta[0].value == "chest");
        CHECK(inventory.attributes.meta[1].key == "Order");
        REQUIRE(inventory.fields[0].attributes.meta.size() == 1);
        CHECK(inventory.fields[0].attributes.meta[0].value == "Spin");
#endif
    }

    TEST_CASE("signed attribute values") {
        const ith::FieldInfo& amount = ith::typeOf<ith::test::Hit>().fields[5];
        CHECK(amount.name == "amount");
        CHECK(amount.attributes.min == -1.5);
        CHECK(amount.attributes.max == 2500.0);
    }
}
