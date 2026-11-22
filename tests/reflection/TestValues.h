#pragma once

// Test helpers that work on any reflected value through its records alone: an owning, aligned
// object of a TypeInfo, and filling a value with deterministic non-default values. Used by the
// reflection and serialization round-trip tests.

#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeInfo.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace ith::test {

// A value of a reflected type in its own aligned memory, destroyed with the type's records.
class Object {
public:
    explicit Object(const TypeInfo& type)
        : m_type(type), m_memory(::operator new(type.size, std::align_val_t(type.alignment))) {
        m_type.construct(m_memory);
    }

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    ~Object() {
        m_type.destroy(m_memory);
        ::operator delete(m_memory, std::align_val_t(m_type.alignment));
    }

    [[nodiscard]] void* get() const { return m_memory; }

private:
    const TypeInfo& m_type;
    void* m_memory;
};

inline std::byte* fieldAt(void* object, const FieldInfo& field) {
    return static_cast<std::byte*>(object) + field.offset;
}

inline const std::byte* fieldAt(const void* object, const FieldInfo& field) {
    return static_cast<const std::byte*>(object) + field.offset;
}

// Changes every reflected value in `object`, except fields that have any of `skip`: afterwards
// it differs from what it was, so filling a default-constructed value gives non-default values.
// Deterministic for a given `seed`. Containers grow only in the first levels, so a type that
// contains itself through a vector stays finite.
inline void fill(const TypeInfo& type, void* object, std::uint64_t& seed, ReflectionFlags skip = ReflectionFlags::None,
                 int depth = 0) {
    ++seed;
    const std::uint64_t step = 1 + seed % 100; // never zero, and below the range of every integer type
    switch (type.kind) {
        case TypeKind::Bool:
            *static_cast<bool*>(object) = !*static_cast<bool*>(object);
            break;
        case TypeKind::SignedInteger:
        case TypeKind::UnsignedInteger: {
            // Little-endian: the low bytes of a 64-bit integer hold the value of any narrower one.
            std::uint64_t value = 0;
            std::memcpy(&value, object, type.size);
            value += step;
            std::memcpy(object, &value, type.size);
            break;
        }
        case TypeKind::Float:
            if (type.size == sizeof(float)) {
                *static_cast<float*>(object) += static_cast<float>(step) + 0.5f;
            } else {
                *static_cast<double*>(object) += static_cast<double>(step) + 0.25;
            }
            break;
        case TypeKind::String:
            *static_cast<std::string*>(object) = "value " + std::to_string(seed);
            break;
        case TypeKind::Uuid:
            *static_cast<Uuid*>(object) = Uuid(seed, ~seed);
            break;
        case TypeKind::Entity:
            *static_cast<Entity*>(object) = Entity::fromParts(static_cast<std::uint32_t>(seed), 1, 1);
            break;
        case TypeKind::Enum: {
            REQUIRE_FALSE(type.enumerators.empty());
            const std::int64_t current = readEnumValue(type, object);
            const std::int64_t last = type.enumerators.back().value;
            writeEnumValue(type, object, last != current ? last : type.enumerators.front().value);
            break;
        }
        case TypeKind::Struct:
            for (const FieldInfo& field : type.fields) {
                if ((field.flags & skip) == ReflectionFlags::None) {
                    fill(field.type(), fieldAt(object, field), seed, skip, depth + 1);
                }
            }
            break;
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
        case TypeKind::Optional:
            if (type.container->resize != nullptr && depth < 6) {
                type.container->resize(object, type.kind == TypeKind::Optional ? 1 : 2);
            }
            for (std::size_t index = 0; index < type.container->size(object); ++index) {
                fill(type.element(), type.container->at(object, index), seed, skip, depth + 1);
            }
            break;
    }
}

} // namespace ith::test
