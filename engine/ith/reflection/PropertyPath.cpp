#include "ith/reflection/PropertyPath.h"

#include "ith/foundation/Assert.h"
#include "ith/reflection/TypeRegistry.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ith {
namespace {

constexpr bool isNameStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool isNameChar(char c) noexcept {
    return isNameStart(c) || (c >= '0' && c <= '9');
}

constexpr bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

Unexpected<Error> pathError(const char* code, std::string_view text, const std::string& message) {
    return makeError(code, "path '" + std::string(text) + "': " + message);
}

Unexpected<Error> syntaxError(std::string_view text, std::size_t offset, const std::string& message) {
    return pathError("reflection.path.syntax", text, message + " (at byte " + std::to_string(offset) + ")");
}

// Reads "[digits]" at `position`, which is at the "[", and moves `position` past the "]".
Expected<std::size_t> readIndex(std::string_view text, std::size_t& position) {
    const std::size_t begin = ++position;
    while (position < text.size() && isDigit(text[position])) {
        ++position;
    }
    if (position == begin) {
        return syntaxError(text, begin, "expected an index");
    }
    if (text[begin] == '0' && position - begin > 1) {
        return syntaxError(text, begin, "an index has no leading zeros");
    }
    if (position == text.size() || text[position] != ']') {
        return syntaxError(text, position, "expected ']'");
    }
    std::size_t index = 0;
    if (std::from_chars(text.data() + begin, text.data() + position, index).ec != std::errc()) {
        return syntaxError(text, begin, "the index is too large");
    }
    ++position;
    return index;
}

// Follows the steps of `path` from `root`, with constant or mutable access.
template <typename Pointer>
Expected<Pointer> walk(const PropertyPath& path, Pointer root) {
    using Byte = std::conditional_t<std::is_const_v<std::remove_pointer_t<Pointer>>, const std::byte, std::byte>;
    Pointer value = root;
    const TypeInfo* type = &path.rootType();
    for (const PathStep& step : path.steps()) {
        switch (step.kind) {
            case PathStep::Kind::Field: {
                const FieldInfo& field = type->fields[step.index];
                value = static_cast<Byte*>(value) + field.offset;
                type = &field.type();
                continue;
            }
            case PathStep::Kind::Index:
            case PathStep::Kind::Unwrap:
                break;
        }
        const std::size_t size = type->container->size(value);
        const std::size_t index = step.kind == PathStep::Kind::Index ? step.index : 0;
        if (index >= size) {
            if (step.kind == PathStep::Kind::Unwrap) {
                return pathError("reflection.path.empty_optional", path.toString(), "steps through an empty optional");
            }
            return pathError("reflection.path.index_out_of_range", path.toString(),
                             "index " + std::to_string(index) + " is beyond the " + std::to_string(size) +
                                 " elements of the array");
        }
        if constexpr (std::is_const_v<std::remove_pointer_t<Pointer>>) {
            value = type->container->constAt(value, index);
        } else {
            value = type->container->at(value, index);
        }
        type = &type->element();
    }
    return value;
}

} // namespace

PropertyPath::PropertyPath(const TypeInfo& root) noexcept : m_root(&root), m_leaf(&root) {}

Expected<PropertyPath> PropertyPath::parse(const TypeInfo& root, std::string_view text) {
    PropertyPath path(root);
    if (Expected<void> parsed = path.parseSteps(text, 0); !parsed) {
        return makeUnexpected(std::move(parsed).error());
    }
    return path;
}

Expected<PropertyPath> PropertyPath::parseQualified(const TypeRegistry& registry, std::string_view text) {
    const std::size_t end = text.find_first_of(".[");
    const std::string_view name = text.substr(0, end);
    if (name.empty()) {
        return syntaxError(text, 0, "expected a type name");
    }
    const TypeInfo* type = registry.find(name);
    if (type == nullptr) {
        return pathError("reflection.path.unknown_type", text, "no type is named '" + std::string(name) + "'");
    }
    PropertyPath path(*type);
    if (end == std::string_view::npos) {
        return path;
    }
    std::size_t start = end;
    if (text[end] == '.') {
        start = end + 1;
        if (start == text.size() || !isNameStart(text[start])) {
            return syntaxError(text, start, "expected a field name");
        }
    }
    if (Expected<void> parsed = path.parseSteps(text, start); !parsed) {
        return makeUnexpected(std::move(parsed).error());
    }
    return path;
}

Expected<void> PropertyPath::parseSteps(std::string_view text, std::size_t position) {
    bool first = true;
    while (position < text.size()) {
        if (text[position] == '[') {
            Expected<std::size_t> index = readIndex(text, position);
            if (!index) {
                return makeUnexpected(std::move(index).error());
            }
            if (Expected<void> added = appendIndex(text, *index); !added) {
                return added;
            }
        } else {
            if (!first) {
                if (text[position] != '.') {
                    return syntaxError(text, position, "expected '.' or '['");
                }
                ++position;
            }
            const std::size_t begin = position;
            if (position == text.size() || !isNameStart(text[position])) {
                return syntaxError(text, begin, "expected a field name");
            }
            while (position < text.size() && isNameChar(text[position])) {
                ++position;
            }
            if (Expected<void> added = appendField(text, text.substr(begin, position - begin)); !added) {
                return added;
            }
        }
        first = false;
    }
    return {};
}

void PropertyPath::unwrapOptionals() {
    while (m_leaf->kind == TypeKind::Optional) {
        pushStep(PathStep{.kind = PathStep::Kind::Unwrap});
    }
}

Expected<void> PropertyPath::appendField(std::string_view text, std::string_view name) {
    unwrapOptionals();
    if (m_leaf->kind != TypeKind::Struct) {
        return pathError("reflection.path.not_a_struct", text,
                         "'" + std::string(name) + "' is a field step on " + std::string(m_leaf->name) +
                             ", which has no fields");
    }
    const FieldInfo* field = m_leaf->findField(name);
    if (field == nullptr) {
        return pathError("reflection.path.unknown_field", text,
                         std::string(m_leaf->name) + " has no field '" + std::string(name) + "'");
    }
    pushStep(PathStep{.kind = PathStep::Kind::Field, .index = static_cast<std::size_t>(field - m_leaf->fields.data())});
    return {};
}

Expected<void> PropertyPath::appendIndex(std::string_view text, std::size_t index) {
    unwrapOptionals();
    if (m_leaf->kind != TypeKind::FixedArray && m_leaf->kind != TypeKind::DynamicArray) {
        return pathError("reflection.path.not_indexable", text,
                         "index " + std::to_string(index) + " on " + std::string(m_leaf->name) +
                             ", which is not an array");
    }
    if (m_leaf->kind == TypeKind::FixedArray && index >= m_leaf->count) {
        return pathError("reflection.path.index_out_of_range", text,
                         "index " + std::to_string(index) + " is beyond the " + std::to_string(m_leaf->count) +
                             " elements of the std::array");
    }
    pushStep(PathStep{.kind = PathStep::Kind::Index, .index = index});
    return {};
}

void PropertyPath::pushStep(const PathStep& step) {
    if (step.kind == PathStep::Kind::Field) {
        const FieldInfo& field = m_leaf->fields[step.index];
        m_leaf = &field.type();
        m_leafField = &field;
        m_chainFlags = m_chainFlags | field.flags;
    } else {
        m_leaf = &m_leaf->element();
    }
    m_steps.push_back(step);
}

PropertyPath PropertyPath::withoutLastStep() const {
    ITH_ASSERT_MSG(!m_steps.empty(), "PropertyPath::withoutLastStep: the path is empty");
    PropertyPath result(*m_root);
    for (std::size_t index = 0; index + 1 < m_steps.size(); ++index) {
        result.pushStep(m_steps[index]);
    }
    return result;
}

std::string PropertyPath::toString() const {
    std::string text;
    const TypeInfo* type = m_root;
    for (const PathStep& step : m_steps) {
        switch (step.kind) {
            case PathStep::Kind::Field: {
                const FieldInfo& field = type->fields[step.index];
                if (!text.empty()) {
                    text += '.';
                }
                text += field.name;
                type = &field.type();
                break;
            }
            case PathStep::Kind::Index:
                text += '[';
                text += std::to_string(step.index);
                text += ']';
                type = &type->element();
                break;
            case PathStep::Kind::Unwrap:
                type = &type->element();
                break;
        }
    }
    return text;
}

std::string PropertyPath::toQualifiedString() const {
    std::string text(m_root->name);
    const std::string steps = toString();
    if (!steps.empty() && steps.front() != '[') {
        text += '.';
    }
    return text + steps;
}

bool PropertyPath::isPrefixOf(const PropertyPath& other) const noexcept {
    if (m_root->id != other.m_root->id || m_steps.size() > other.m_steps.size()) {
        return false;
    }
    for (std::size_t index = 0; index < m_steps.size(); ++index) {
        if (m_steps[index] != other.m_steps[index]) {
            return false;
        }
    }
    return true;
}

bool operator==(const PropertyPath& a, const PropertyPath& b) noexcept {
    return a.m_root->id == b.m_root->id && a.m_steps == b.m_steps;
}

Expected<void*> resolve(const PropertyPath& path, void* root) {
    return walk<void*>(path, root);
}

Expected<const void*> resolve(const PropertyPath& path, const void* root) {
    return walk<const void*>(path, root);
}

} // namespace ith
