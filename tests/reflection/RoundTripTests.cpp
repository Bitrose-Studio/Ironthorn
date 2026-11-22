// Round-trip of every reflected type through its records (ADR-0018 rule 12).
//
// Walks the registry, fills each struct with deterministic non-default values through its
// records alone, copies it field by field into a fresh object and compares with the type's
// equality; then checks copy, move and destroy. This exercises every generated record and value
// function under the sanitizer jobs. The round-trip through the text and binary archives
// (ADR-0010) is in tests/serialization/RoundTripTests.cpp.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

#include "Loadout.h"
#include "ReflectedTypes.h"
#include "TestValues.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <cstdint>
#include <ostream>
#include <string>

ITH_DECLARE_REFLECTION_MODULE(test_reflected)

namespace {

using ith::test::fieldAt;
using ith::test::fill;
using ith::test::Object;

// Copies the reflected fields of `source` into `destination`, one field at a time.
void copyFields(const ith::TypeInfo& type, void* destination, const void* source) {
    for (const ith::FieldInfo& field : type.fields) {
        const ith::TypeInfo& fieldType = field.type();
        fieldType.destroy(fieldAt(destination, field));
        fieldType.copy(fieldAt(destination, field), fieldAt(source, field));
    }
}

} // namespace

TEST_SUITE("reflection.round_trip") {
    TEST_CASE("every reflected struct survives a field-wise copy, copy, move and destroy") {
        ith::TypeRegistry registry;
        ith::generated::registerReflection_test_reflected(registry);

        int structs = 0;
        std::uint64_t seed = 0;
        for (const ith::TypeInfo* type : registry.types()) {
            if (type->kind != ith::TypeKind::Struct) {
                continue;
            }
            ++structs;
            const std::string name(type->name);
            CAPTURE(name);

            const Object defaults(*type);
            const Object original(*type);
            fill(*type, original.get(), seed);
            CHECK(type->equals(original.get(), original.get()));
            if (!type->fields.empty()) {
                CHECK_FALSE(type->equals(original.get(), defaults.get()));
            }

            const Object fieldCopy(*type);
            copyFields(*type, fieldCopy.get(), original.get());
            CHECK(type->equals(fieldCopy.get(), original.get()));

            Object copied(*type);
            type->destroy(copied.get());
            type->copy(copied.get(), original.get());
            CHECK(type->equals(copied.get(), original.get()));

            Object moved(*type);
            type->destroy(moved.get());
            type->move(moved.get(), copied.get());
            CHECK(type->equals(moved.get(), original.get()));

            if (!type->fields.empty()) {
                fill(type->fields.front().type(), fieldAt(fieldCopy.get(), type->fields.front()), seed);
                CHECK_FALSE(type->equals(fieldCopy.get(), original.get()));
            }
        }
        // The 9 structs of test_reflected and the math types.
        CHECK(structs == 9 + static_cast<int>(ith::detail::kMathTypes.size()));
    }
}
