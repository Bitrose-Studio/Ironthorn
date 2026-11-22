#pragma once

// Internal helpers shared by the text and binary archives. Not part of the module's API.

#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Error.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ith::serialization::detail {

// Builds paths such as "$.items[3].count" while an archive walks a value.
class PathBuilder {
public:
    // Appends ".name" or "[index]"; returns the previous length to pass to pop().
    std::size_t pushField(std::string_view name);
    std::size_t pushIndex(std::size_t index);

    void pop(std::size_t length) { m_path.resize(length); }

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path = "$";
};

// Counts down the bytes a read may still allocate (Document.h, allocationBudget).
class AllocationBudget {
public:
    explicit AllocationBudget(std::size_t bytes) noexcept : m_left(bytes) {}

    // The bytes left.
    [[nodiscard]] std::size_t left() const noexcept { return m_left; }

    // Takes `count` * `elementSize` bytes; false, taking nothing, when that is more than is left.
    bool take(std::size_t count, std::size_t elementSize) noexcept {
        if (elementSize != 0 && count > m_left / elementSize) {
            return false;
        }
        m_left -= count * elementSize;
        return true;
    }

private:
    std::size_t m_left;
};

// printable (Error.h), which the archives use for names and text from the data in messages.
using serialization::printable;

// Empties the DynamicArray or Optional of `type` at `object`, then gives it `count`
// default-constructed elements, charging `budget` for count * the element size first. False,
// without allocating, when the budget is exceeded. A count above 1 for an optional asserts.
bool resizeWithin(const TypeInfo& type, void* object, std::size_t count, AllocationBudget& budget);

// The flags that keep a field or type out of an archive: Transient always, EditorOnly also in
// cooked binary data.
inline constexpr ReflectionFlags kNotInText = ReflectionFlags::Transient;
inline constexpr ReflectionFlags kNotInBinary = ReflectionFlags::Transient | ReflectionFlags::EditorOnly;

// True when `flags` has any of `excluded`.
constexpr bool isExcluded(ReflectionFlags flags, ReflectionFlags excluded) noexcept {
    return (flags & excluded) != ReflectionFlags::None;
}

// True when a field is written and read: neither the field nor its type has an excluded flag.
bool isSaved(const FieldInfo& field, ReflectionFlags excluded) noexcept;

// Asserts the DocumentFormat contract (Document.h); false when it is broken.
bool isValidFormat(const DocumentFormat& format) noexcept;

// An error with a code and message; the caller adds the source, path and position.
Error makeError(std::string code, std::string message);

// The entity mapping rules (Document.h, EntityMapping): whether `mapping` can write or read
// entities, the message of the error when it can't, and the conversions, which keep the null
// handle and the nil Uuid to themselves without calling the mapping.
inline constexpr const char* kNoEntityMappingMessage =
    "an Entity is a runtime handle; it is saved only through an EntityMapping";

constexpr bool canWriteEntities(const EntityMapping* mapping) noexcept {
    return mapping != nullptr && mapping->toId != nullptr;
}

constexpr bool canReadEntities(const EntityMapping* mapping) noexcept {
    return mapping != nullptr && mapping->toEntity != nullptr;
}

inline Uuid entityToId(const EntityMapping& mapping, Entity entity) {
    return entity.isNull() ? Uuid() : mapping.toId(mapping.context, entity);
}

inline Entity idToEntity(const EntityMapping& mapping, const Uuid& id) {
    return id.isNil() ? Entity() : mapping.toEntity(mapping.context, id);
}

// The name of a type for messages: "float", "ith::test::Health", "std::vector<int32>".
std::string describe(const TypeInfo& type);

} // namespace ith::serialization::detail
