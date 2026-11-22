// Tests of TypeRegistry: registration, lookup by id and name, FormerName aliases, collisions.

#include "ith/reflection/TypeRegistry.h"

#include "ith/foundation/Hash.h"
#include "ith/reflection/TypeOf.h"

#include "Loadout.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <set>
#include <string>

ITH_DECLARE_REFLECTION_MODULE(test_reflected)

namespace {

// The types every new registry holds.
constexpr std::size_t kBuiltIn = ith::detail::kLeafTypes.size() + ith::detail::kMathTypes.size();

// A record that claims the name "ith::test::Health" without being that type.
constexpr ith::TypeInfo kImpostor = ith::detail::makeValueType<std::int32_t>(ith::TypeKind::Struct, "ith::test::Health",
                                                                             &ith::detail::equalValues<std::int32_t>);

// A record whose FormerName is the current name of another type.
constexpr ith::TypeInfo kRenamedOnto = [] {
    ith::TypeInfo info = ith::detail::makeValueType<std::int32_t>(ith::TypeKind::Struct, "ith::test::Other",
                                                                  &ith::detail::equalValues<std::int32_t>);
    info.attributes.formerName = "ith::test::Vector3";
    return info;
}();

ith::TypeRegistry makeRegistry() {
    ith::TypeRegistry registry;
    ith::generated::registerReflection_test_reflected(registry);
    return registry;
}

} // namespace

TEST_SUITE("reflection.registry") {
    TEST_CASE("a new registry holds the leaf and math types") {
        const ith::TypeRegistry registry;
        CHECK(registry.types().size() == kBuiltIn);
        CHECK(registry.find("int32") == &ith::typeOf<std::int32_t>());
        CHECK(registry.find("std::string") == &ith::typeOf<std::string>());
        CHECK(registry.find(ith::fnv1a64("ith::Uuid")) == &ith::typeOf<ith::Uuid>());
    }

    TEST_CASE("a module's registration function adds its types in declaration order") {
        const ith::TypeRegistry registry = makeRegistry();
        CHECK(registry.types().size() == kBuiltIn + 12);
        CHECK(registry.types()[kBuiltIn] == &ith::typeOf<ith::test::DamageKind>());
        CHECK(registry.types().back() == &ith::typeOf<GlobalCounter>());
    }

    TEST_CASE("lookup by id and by name") {
        const ith::TypeRegistry registry = makeRegistry();
        const ith::TypeInfo* health = registry.find("ith::test::Health");
        CHECK(health == &ith::typeOf<ith::test::Health>());
        CHECK(registry.find(ith::fnv1a64("ith::test::Health")) == health);
        CHECK(registry.find("ith::test::gear::Loadout") == &ith::typeOf<ith::test::gear::Loadout>());
        CHECK(registry.find("GlobalCounter") == &ith::typeOf<GlobalCounter>());
        CHECK(registry.find("ith::test::Abilities") == &ith::typeOf<ith::test::Abilities>());
    }

    TEST_CASE("unknown names and ids find nothing") {
        const ith::TypeRegistry registry = makeRegistry();
        CHECK(registry.find("ith::test::Missing") == nullptr);
        CHECK(registry.find("Health") == nullptr); // names are fully qualified
        CHECK(registry.find("") == nullptr);
        CHECK(registry.find(std::uint64_t{12345}) == nullptr);
    }

    TEST_CASE("a FormerName is an alias for the type") {
        const ith::TypeRegistry registry = makeRegistry();
        const ith::TypeInfo* hit = &ith::typeOf<ith::test::Hit>();
        CHECK(registry.find("ith::test::Damage") == hit);
        CHECK(registry.find(ith::fnv1a64("ith::test::Damage")) == hit);
        CHECK(registry.find("ith::test::Hit") == hit);
    }

    TEST_CASE("ids are unique across the registry") {
        const ith::TypeRegistry registry = makeRegistry();
        std::set<std::uint64_t> ids;
        for (const ith::TypeInfo* type : registry.types()) {
            CHECK(ids.insert(type->id).second);
        }
    }

    TEST_CASE("a colliding id is refused with both names") {
        ith::TypeRegistry registry = makeRegistry();
        const ith::Expected<void> added = registry.add(kImpostor);
        REQUIRE_FALSE(added.hasValue());
        CHECK(added.error().code == "reflection.duplicate_id");
        CHECK(added.error().message.find("ith::test::Health") != std::string::npos);
        CHECK(registry.types().size() == kBuiltIn + 12);
    }

    TEST_CASE("a FormerName that collides with another type's id is refused") {
        ith::TypeRegistry registry = makeRegistry();
        const ith::Expected<void> added = registry.add(kRenamedOnto);
        REQUIRE_FALSE(added.hasValue());
        CHECK(added.error().code == "reflection.duplicate_id");
        CHECK(added.error().message.find("ith::test::Other") != std::string::npos);
        CHECK(added.error().message.find("ith::test::Vector3") != std::string::npos);
        CHECK(registry.find("ith::test::Other") == nullptr);
    }

    TEST_CASE("registering the same type twice is refused") {
        ith::TypeRegistry registry;
        REQUIRE(registry.add(ith::typeOf<ith::test::Vector3>()).hasValue());
        const ith::Expected<void> again = registry.add(ith::typeOf<ith::test::Vector3>());
        REQUIRE_FALSE(again.hasValue());
        CHECK(again.error().code == "reflection.duplicate_id");
    }
}
