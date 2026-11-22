#include "ith/serialization/ArchiveSupport.h"

#include "ith/foundation/Assert.h"

#include <string>
#include <utility>

namespace ith::serialization::detail {

std::size_t PathBuilder::pushField(std::string_view name) {
    const std::size_t length = m_path.size();
    m_path += '.';
    m_path += printable(name);
    return length;
}

std::size_t PathBuilder::pushIndex(std::size_t index) {
    const std::size_t length = m_path.size();
    m_path += '[';
    m_path += std::to_string(index);
    m_path += ']';
    return length;
}

} // namespace ith::serialization::detail

namespace ith::serialization {

std::string printable(std::string_view text) {
    constexpr std::size_t kMaxBytes = 64;
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string result;
    for (std::size_t index = 0; index < text.size() && index < kMaxBytes; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte >= 0x20 && byte < 0x7f && byte != '\\') {
            result += static_cast<char>(byte);
        } else {
            result += "\\x";
            result += kHex[byte >> 4];
            result += kHex[byte & 0xf];
        }
    }
    if (text.size() > kMaxBytes) {
        result += "...";
    }
    return result;
}

} // namespace ith::serialization

namespace ith::serialization::detail {

bool resizeWithin(const TypeInfo& type, void* object, std::size_t count, AllocationBudget& budget) {
    if (!budget.take(count, type.element().size)) {
        return false;
    }
    type.container->resize(object, 0);
    type.container->resize(object, count);
    return true;
}

bool isSaved(const FieldInfo& field, ReflectionFlags excluded) noexcept {
    return !isExcluded(field.flags, excluded) && !isExcluded(field.type().flags, excluded);
}

bool isValidFormat(const DocumentFormat& format) noexcept {
    const bool valid = !format.name.empty() && format.name.size() <= 255 && format.minimumVersion >= 1 &&
                       format.minimumVersion <= format.version &&
                       format.migrations.size() == format.version - format.minimumVersion;
    ITH_ASSERT_MSG(valid, "serialization: invalid DocumentFormat (see Document.h)");
    return valid;
}

Error makeError(std::string code, std::string message) {
    return Error{.code = std::move(code), .message = std::move(message)};
}

std::string describe(const TypeInfo& type) {
    switch (type.kind) {
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
        case TypeKind::Optional:
            return std::string(type.name) + '<' + describe(type.element()) + '>';
        default:
            return std::string(type.name);
    }
}

} // namespace ith::serialization::detail
