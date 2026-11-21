// Round-trip of every reflected type (ADR-0018 rule 12).
//
// Walks the registry, fills each struct with deterministic non-default values through its
// records alone, copies it field by field into a fresh object and compares with the type's
// equality; then checks copy, move and destroy. This exercises every generated record and value
// function under the sanitizer jobs.
//
// Pending on the serialization slice (ADR-0010): the full exit criterion writes and reads each
// filled value through the text and binary archives instead of the in-memory field-wise copy.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeRegistry.h"

#include "Loadout.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <ostream>
#include <string>

ITH_DECLARE_REFLECTION_MODULE(test_reflected)

namespace {

// A value of a reflected type in its own aligned memory, destroyed with the type's records.
class Object {
public:
    explicit Object(const ith::TypeInfo& type)
        : m_type(type), m_memory(::operator new(type.size, std::align_val_t(type.alignment))) {
        m_type.construct(m_memory);
    }

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    ~Object() {
        m_type.destroy(m_memory);
        ::operator delete(m_memory, std::align_val_t(m_type.alignment));
    }

    void* get() const { return m_memory; }

private:
    const ith::TypeInfo& m_type;
    void* m_memory;
};

std::byte* at(void* object, const ith::FieldInfo& field) {
    return static_cast<std::byte*>(object) + field.offset;
}

const std::byte* at(const void* object, const ith::FieldInfo& field) {
    return static_cast<const std::byte*>(object) + field.offset;
}

// Changes every reflected value in `object`: afterwards it differs from what it was, so filling a
// default-constructed value gives non-default values. Deterministic for a given `seed`.
void fill(const ith::TypeInfo& type, void* object, std::uint64_t& seed) {
    ++seed;
    const std::uint64_t step = 1 + seed % 100; // never zero, and below the range of every integer type
    switch (type.kind) {
        case ith::TypeKind::Bool:
            *static_cast<bool*>(object) = !*static_cast<bool*>(object);
            break;
        case ith::TypeKind::SignedInteger:
        case ith::TypeKind::UnsignedInteger: {
            // Little-endian: the low bytes of a 64-bit integer hold the value of any narrower one.
            std::uint64_t value = 0;
            std::memcpy(&value, object, type.size);
            value += step;
            std::memcpy(object, &value, type.size);
            break;
        }
        case ith::TypeKind::Float:
            if (type.size == sizeof(float)) {
                *static_cast<float*>(object) += static_cast<float>(step) + 0.5f;
            } else {
                *static_cast<double*>(object) += static_cast<double>(step) + 0.25;
            }
            break;
        case ith::TypeKind::String:
            *static_cast<std::string*>(object) = "value " + std::to_string(seed);
            break;
        case ith::TypeKind::Uuid:
            *static_cast<ith::Uuid*>(object) = ith::Uuid(seed, ~seed);
            break;
        case ith::TypeKind::Enum: {
            REQUIRE_FALSE(type.enumerators.empty());
            const std::int64_t current = ith::readEnumValue(type, object);
            const std::int64_t last = type.enumerators.back().value;
            ith::writeEnumValue(type, object, last != current ? last : type.enumerators.front().value);
            break;
        }
        case ith::TypeKind::Struct:
            for (const ith::FieldInfo& field : type.fields) {
                fill(field.type(), at(object, field), seed);
            }
            break;
        case ith::TypeKind::FixedArray:
        case ith::TypeKind::DynamicArray:
        case ith::TypeKind::Optional:
            if (type.container->resize != nullptr) {
                type.container->resize(object, type.kind == ith::TypeKind::Optional ? 1 : 2);
            }
            for (std::size_t index = 0; index < type.container->size(object); ++index) {
                fill(type.element(), type.container->at(object, index), seed);
            }
            break;
    }
}

// Copies the reflected fields of `source` into `destination`, one field at a time.
void copyFields(const ith::TypeInfo& type, void* destination, const void* source) {
    for (const ith::FieldInfo& field : type.fields) {
        const ith::TypeInfo& fieldType = field.type();
        fieldType.destroy(at(destination, field));
        fieldType.copy(at(destination, field), at(source, field));
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
                fill(type->fields.front().type(), at(fieldCopy.get(), type->fields.front()), seed);
                CHECK_FALSE(type->equals(fieldCopy.get(), original.get()));
            }
        }
        CHECK(structs == 9);
    }
}
