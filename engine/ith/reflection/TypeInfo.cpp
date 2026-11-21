#include "ith/reflection/TypeInfo.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"

#include <cstddef>
#include <cstring>

namespace ith {
namespace {

template <typename Integer>
std::int64_t loadInteger(const void* object) noexcept {
    Integer value{};
    std::memcpy(&value, object, sizeof(value));
    return static_cast<std::int64_t>(value);
}

template <typename Integer>
void storeInteger(void* object, std::int64_t value) noexcept {
    const auto truncated = static_cast<Integer>(value);
    std::memcpy(object, &truncated, sizeof(truncated));
}

} // namespace

const FieldInfo* TypeInfo::findField(std::string_view fieldName) const noexcept {
    for (const FieldInfo& field : fields) {
        if (field.name == fieldName) {
            return &field;
        }
    }
    for (const FieldInfo& field : fields) {
        if (!field.attributes.formerName.empty() && field.attributes.formerName == fieldName) {
            return &field;
        }
    }
    return nullptr;
}

const FieldInfo* TypeInfo::findField(std::uint64_t fieldId) const noexcept {
    for (const FieldInfo& field : fields) {
        if (field.id == fieldId) {
            return &field;
        }
    }
    for (const FieldInfo& field : fields) {
        if (!field.attributes.formerName.empty() && fnv1a64(field.attributes.formerName) == fieldId) {
            return &field;
        }
    }
    return nullptr;
}

const EnumeratorInfo* TypeInfo::findEnumerator(std::string_view enumeratorName) const noexcept {
    for (const EnumeratorInfo& enumerator : enumerators) {
        if (enumerator.name == enumeratorName) {
            return &enumerator;
        }
    }
    return nullptr;
}

std::int64_t readEnumValue(const TypeInfo& enumType, const void* object) noexcept {
    ITH_ASSERT_MSG(enumType.kind == TypeKind::Enum && enumType.element != nullptr, "readEnumValue: not an enum");
    const bool isSigned = enumType.element().kind == TypeKind::SignedInteger;
    switch (enumType.size) {
        case 1:
            return isSigned ? loadInteger<std::int8_t>(object) : loadInteger<std::uint8_t>(object);
        case 2:
            return isSigned ? loadInteger<std::int16_t>(object) : loadInteger<std::uint16_t>(object);
        case 4:
            return isSigned ? loadInteger<std::int32_t>(object) : loadInteger<std::uint32_t>(object);
        default:
            return isSigned ? loadInteger<std::int64_t>(object) : loadInteger<std::uint64_t>(object);
    }
}

void writeEnumValue(const TypeInfo& enumType, void* object, std::int64_t value) noexcept {
    ITH_ASSERT_MSG(enumType.kind == TypeKind::Enum && enumType.element != nullptr, "writeEnumValue: not an enum");
    switch (enumType.size) {
        case 1:
            storeInteger<std::uint8_t>(object, value);
            break;
        case 2:
            storeInteger<std::uint16_t>(object, value);
            break;
        case 4:
            storeInteger<std::uint32_t>(object, value);
            break;
        default:
            storeInteger<std::uint64_t>(object, value);
            break;
    }
}

namespace detail {

bool equalFields(const TypeInfo& type, const void* a, const void* b) {
    const auto* bytesA = static_cast<const std::byte*>(a);
    const auto* bytesB = static_cast<const std::byte*>(b);
    for (const FieldInfo& field : type.fields) {
        if (!field.type().equals(bytesA + field.offset, bytesB + field.offset)) {
            return false;
        }
    }
    return true;
}

} // namespace detail
} // namespace ith
