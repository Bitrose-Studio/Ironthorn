// The P1 exit criterion (ADR-0018 rule 12, ADR-0010 rule 4): every reflected type of the test
// modules, filled with deterministic non-default values through its records alone, survives a
// round trip through the text archive and through the binary archive, compared with the type's
// own equality. Writing the read value again gives the same bytes.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/TextArchive.h"

#include "TestValues.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(test_reflected)
ITH_DECLARE_REFLECTION_MODULE(test_archive)

namespace {

namespace serialization = ith::serialization;
using ith::test::Object;

constexpr serialization::DocumentFormat kFormat{.name = "test.round_trip", .version = 1};

void roundTrip(const ith::TypeInfo& type, std::uint64_t& seed) {
    const std::string name(type.name);
    CAPTURE(name);

    const Object original(type);
    // Transient fields are never saved, so they keep their defaults through a round trip.
    ith::test::fill(type, original.get(), seed, ith::ReflectionFlags::Transient);

    // Text.
    const ith::Expected<std::string, serialization::Error> text =
        serialization::writeText(kFormat, type, original.get());
    REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
    const Object fromText(type);
    std::vector<serialization::Error> warnings;
    const auto readText = serialization::readText(*text, kFormat, type, fromText.get(), {.warnings = &warnings});
    REQUIRE_MESSAGE(readText.hasValue(), readText.error().toString());
    CHECK(warnings.empty());
    CHECK(type.equals(fromText.get(), original.get()));
    const auto textAgain = serialization::writeText(kFormat, type, fromText.get());
    REQUIRE(textAgain.hasValue());
    CHECK(*textAgain == *text);

    // Binary, read into an object that already holds other values.
    const auto blob = serialization::writeBinary(kFormat, type, original.get());
    REQUIRE_MESSAGE(blob.hasValue(), blob.error().toString());
    const Object fromBinary(type);
    std::uint64_t otherSeed = seed * 7;
    ith::test::fill(type, fromBinary.get(), otherSeed, ith::ReflectionFlags::Transient);
    const auto readBinary = serialization::readBinary(*blob, kFormat, type, fromBinary.get());
    REQUIRE_MESSAGE(readBinary.hasValue(), readBinary.error().toString());
    CHECK(type.equals(fromBinary.get(), original.get()));
    const auto blobAgain = serialization::writeBinary(kFormat, type, fromBinary.get());
    REQUIRE(blobAgain.hasValue());
    CHECK(*blobAgain == *blob);
}

} // namespace

TEST_SUITE("serialization.round_trip") {
    TEST_CASE("every reflected type of the test modules round-trips through the text and binary archives") {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_test_reflected(registry);
        ith::generated::registerReflection_test_archive(registry);

        std::size_t structs = 0;
        std::size_t enums = 0;
        std::uint64_t seed = 0;
        for (const ith::TypeInfo* type : registry.types()) {
            structs += type->kind == ith::TypeKind::Struct ? 1 : 0;
            enums += type->kind == ith::TypeKind::Enum ? 1 : 0;
            roundTrip(*type, seed);
        }
        // Leaf types, the math types (structs), 3 enums of test_reflected, 9 structs of
        // test_reflected and 3 of test_archive.
        CHECK(enums == 3);
        CHECK(structs == ith::detail::kMathTypes.size() + 12);
        CHECK(registry.types().size() == ith::detail::kLeafTypes.size() + enums + structs);
    }

    TEST_CASE("default values round-trip too") {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_test_reflected(registry);
        for (const ith::TypeInfo* type : registry.types()) {
            const std::string name(type->name);
            CAPTURE(name);
            const Object original(*type);
            const auto text = serialization::writeText(kFormat, *type, original.get());
            const auto blob = serialization::writeBinary(kFormat, *type, original.get());
            REQUIRE(text.hasValue());
            REQUIRE(blob.hasValue());
            const Object fromText(*type);
            const Object fromBinary(*type);
            REQUIRE(serialization::readText(*text, kFormat, *type, fromText.get()).hasValue());
            REQUIRE(serialization::readBinary(*blob, kFormat, *type, fromBinary.get()).hasValue());
            CHECK(type->equals(fromText.get(), original.get()));
            CHECK(type->equals(fromBinary.get(), original.get()));
        }
    }
}
