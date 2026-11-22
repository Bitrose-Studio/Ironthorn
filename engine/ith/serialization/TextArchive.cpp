#include "ith/serialization/TextArchive.h"

#include "ith/foundation/Uuid.h"
#include "ith/serialization/ArchiveSupport.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ith::serialization {
namespace {

using detail::makeError;

const char* kindName(Value::Kind kind) noexcept {
    switch (kind) {
        case Value::Kind::Null:
            return "null";
        case Value::Kind::Bool:
            return "a boolean";
        case Value::Kind::Number:
            return "a number";
        case Value::Kind::String:
            return "a string";
        case Value::Kind::Array:
            return "an array";
        case Value::Kind::Object:
            return "an object";
    }
    return "a value";
}

template <typename T>
T load(const void* object) noexcept {
    T value{};
    std::memcpy(&value, object, sizeof(T));
    return value;
}

template <typename T>
void store(void* object, T value) noexcept {
    std::memcpy(object, &value, sizeof(T));
}

template <typename Float>
std::string shortestText(Float value) {
    std::array<char, 64> buffer{};
    const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())};
}

// The enumerators whose values, ORed, give `value`; false when some bits are left over.
bool flagNames(const TypeInfo& type, std::int64_t value, std::vector<Value>& names) {
    auto remaining = static_cast<std::uint64_t>(value);
    for (const EnumeratorInfo& enumerator : type.enumerators) {
        const auto bits = static_cast<std::uint64_t>(enumerator.value);
        if (bits != 0 && (remaining & bits) == bits) {
            names.push_back(Value::string(std::string(enumerator.name)));
            remaining &= ~bits;
        }
    }
    return remaining == 0;
}

// Turns a reflected value into a Value tree.
class TextWriter {
public:
    explicit TextWriter(const EntityMapping* entities) noexcept : m_entities(entities) {}

    bool write(const TypeInfo& type, const void* object, Value& out, std::size_t depth) {
        switch (type.kind) {
            case TypeKind::Bool:
                out = Value::boolean(load<bool>(object));
                return true;
            case TypeKind::SignedInteger:
                out = Value::integer(loadSigned(type, object));
                return true;
            case TypeKind::UnsignedInteger:
                out = Value::unsignedInteger(loadUnsigned(type, object));
                return true;
            case TypeKind::Float:
                return writeFloat(type, object, out);
            case TypeKind::String:
                out = Value::string(*static_cast<const std::string*>(object));
                return true;
            case TypeKind::Uuid:
                out = Value::string(static_cast<const Uuid*>(object)->toString());
                return true;
            case TypeKind::Entity:
                if (!detail::canWriteEntities(m_entities)) {
                    return fail("serialization.no_entity_mapping", detail::kNoEntityMappingMessage);
                }
                out = Value::string(detail::entityToId(*m_entities, *static_cast<const Entity*>(object)).toString());
                return true;
            case TypeKind::Enum:
                return writeEnum(type, object, out);
            case TypeKind::Struct:
                return writeStruct(type, object, out, depth);
            case TypeKind::FixedArray:
            case TypeKind::DynamicArray:
            case TypeKind::Optional:
                return writeContainer(type, object, out, depth);
        }
        return fail("serialization.unsupported_type", "unknown kind of type");
    }

    Error takeError() { return std::move(m_error); }

private:
    bool fail(std::string code, std::string message) {
        m_error = makeError(std::move(code), std::move(message));
        m_error.path = m_path.path();
        return false;
    }

    static std::int64_t loadSigned(const TypeInfo& type, const void* object) noexcept {
        switch (type.size) {
            case 1:
                return load<std::int8_t>(object);
            case 2:
                return load<std::int16_t>(object);
            case 4:
                return load<std::int32_t>(object);
            default:
                return load<std::int64_t>(object);
        }
    }

    static std::uint64_t loadUnsigned(const TypeInfo& type, const void* object) noexcept {
        switch (type.size) {
            case 1:
                return load<std::uint8_t>(object);
            case 2:
                return load<std::uint16_t>(object);
            case 4:
                return load<std::uint32_t>(object);
            default:
                return load<std::uint64_t>(object);
        }
    }

    bool writeFloat(const TypeInfo& type, const void* object, Value& out) {
        if (type.size == sizeof(float)) {
            const auto value = load<float>(object);
            if (!std::isfinite(value)) {
                return fail("serialization.non_finite", "JSON can't hold an infinite or NaN float");
            }
            out = Value::number(shortestText(value));
        } else {
            const auto value = load<double>(object);
            if (!std::isfinite(value)) {
                return fail("serialization.non_finite", "JSON can't hold an infinite or NaN double");
            }
            out = Value::number(shortestText(value));
        }
        return true;
    }

    bool writeEnum(const TypeInfo& type, const void* object, Value& out) {
        const std::int64_t value = readEnumValue(type, object);
        if (hasFlags(type.flags, ReflectionFlags::Flags)) {
            std::vector<Value> names;
            if (!flagNames(type, value, names)) {
                return fail("serialization.invalid_enum", std::to_string(value) + " has bits that no enumerator of " +
                                                              std::string(type.name) + " has");
            }
            out = Value::array(std::move(names));
            return true;
        }
        for (const EnumeratorInfo& enumerator : type.enumerators) {
            if (enumerator.value == value) {
                out = Value::string(std::string(enumerator.name));
                return true;
            }
        }
        return fail("serialization.invalid_enum",
                    std::to_string(value) + " is not an enumerator of " + std::string(type.name));
    }

    bool enter(std::size_t depth) {
        return depth + 1 <= kMaxDepth ||
               fail("serialization.too_deep", "the value nests deeper than " + std::to_string(kMaxDepth));
    }

    bool writeStruct(const TypeInfo& type, const void* object, Value& out, std::size_t depth) {
        if (!enter(depth)) {
            return false;
        }
        out = Value::object();
        for (const FieldInfo& field : type.fields) {
            if (!detail::isSaved(field, detail::kNotInText)) {
                continue;
            }
            const std::size_t mark = m_path.pushField(field.name);
            Value& member = out.members().emplace_back(Value::Member{.key = std::string(field.name)}).value;
            if (!write(field.type(), static_cast<const std::byte*>(object) + field.offset, member, depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    bool writeContainer(const TypeInfo& type, const void* object, Value& out, std::size_t depth) {
        const TypeInfo& element = type.element();
        if (detail::isExcluded(element.flags, detail::kNotInText)) {
            return fail("serialization.not_saved", detail::describe(element) + " is Transient and can't be saved");
        }
        const std::size_t count = type.container->size(object);
        if (type.kind == TypeKind::Optional) {
            if (element.kind == TypeKind::Optional) {
                return fail("serialization.unsupported_type",
                            "an optional of an optional can't be told apart from an empty one in JSON");
            }
            if (count == 0) {
                out = Value();
                return true;
            }
            return write(element, type.container->constAt(object, 0), out, depth);
        }
        if (!enter(depth)) {
            return false;
        }
        out = Value::array();
        out.elements().resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t mark = m_path.pushIndex(index);
            if (!write(element, type.container->constAt(object, index), out.elements()[index], depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    const EntityMapping* m_entities;
    detail::PathBuilder m_path;
    Error m_error;
};

// Reads a Value tree into a reflected value.
class TextReader {
public:
    TextReader(UnknownFields unknownFields, bool collectWarnings, std::size_t allocationBudget,
               const EntityMapping* entities)
        : m_unknownFields(unknownFields),
          m_collectWarnings(collectWarnings),
          m_budget(allocationBudget),
          m_entities(entities) {}

    bool read(const TypeInfo& type, const Value& value, void* object, std::size_t depth) {
        switch (type.kind) {
            case TypeKind::Bool:
                if (!expect(value, Value::Kind::Bool, "a boolean")) {
                    return false;
                }
                store<bool>(object, value.asBool());
                return true;
            case TypeKind::SignedInteger:
            case TypeKind::UnsignedInteger:
            case TypeKind::Float:
                return readNumber(type, value, object);
            case TypeKind::String:
                if (!expect(value, Value::Kind::String, "a string")) {
                    return false;
                }
                if (!m_budget.take(value.asString().size(), 1)) {
                    return tooLarge();
                }
                *static_cast<std::string*>(object) = value.asString();
                return true;
            case TypeKind::Uuid:
                return readUuid(value, object);
            case TypeKind::Entity:
                return readEntity(value, object);
            case TypeKind::Enum:
                return readEnum(type, value, object);
            case TypeKind::Struct:
                return readStruct(type, value, object, depth);
            case TypeKind::FixedArray:
            case TypeKind::DynamicArray:
            case TypeKind::Optional:
                return readContainer(type, value, object, depth);
        }
        return fail("serialization.unsupported_type", "unknown kind of type");
    }

    Error takeError() { return std::move(m_error); }

    // The collected warnings, with one that counts the unknown fields beyond kMaxWarnings.
    std::vector<Error> takeWarnings() {
        if (m_unknownCount > kMaxWarnings) {
            m_warnings.push_back(
                makeError("serialization.unknown_field",
                          std::to_string(m_unknownCount - kMaxWarnings) + " more unknown fields are dropped"));
        }
        return std::move(m_warnings);
    }

private:
    bool fail(std::string code, std::string message) {
        m_error = makeError(std::move(code), std::move(message));
        m_error.path = m_path.path();
        return false;
    }

    bool tooLarge() {
        return fail("serialization.too_large", "the value needs more memory than the allocation budget allows");
    }

    bool expect(const Value& value, Value::Kind kind, const char* what) {
        return value.kind() == kind ||
               fail("serialization.wrong_type", std::string("expected ") + what + ", found " + kindName(value.kind()));
    }

    template <typename T>
    bool parse(const Value& value, void* object, const char* what) {
        const std::string_view text = value.numberText();
        T result{};
        const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec == std::errc::result_out_of_range) {
            return fail("serialization.out_of_range", detail::printable(text) + " is out of the range of " + what);
        }
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
            return fail("serialization.wrong_type",
                        std::string("expected ") + what + ", found " + detail::printable(text));
        }
        store<T>(object, result);
        return true;
    }

    bool readNumber(const TypeInfo& type, const Value& value, void* object) {
        if (!expect(value, Value::Kind::Number, "a number")) {
            return false;
        }
        const bool isSigned = type.kind == TypeKind::SignedInteger;
        if (type.kind == TypeKind::Float) {
            return type.size == sizeof(float) ? parse<float>(value, object, "a float")
                                              : parse<double>(value, object, "a double");
        }
        switch (type.size) {
            case 1:
                return isSigned ? parse<std::int8_t>(value, object, "an int8")
                                : parse<std::uint8_t>(value, object, "a uint8");
            case 2:
                return isSigned ? parse<std::int16_t>(value, object, "an int16")
                                : parse<std::uint16_t>(value, object, "a uint16");
            case 4:
                return isSigned ? parse<std::int32_t>(value, object, "an int32")
                                : parse<std::uint32_t>(value, object, "a uint32");
            default:
                return isSigned ? parse<std::int64_t>(value, object, "an int64")
                                : parse<std::uint64_t>(value, object, "a uint64");
        }
    }

    bool readUuid(const Value& value, void* object) {
        if (!expect(value, Value::Kind::String, "a UUID string")) {
            return false;
        }
        const Expected<Uuid> parsed = Uuid::parse(value.asString());
        if (!parsed) {
            return fail("serialization.invalid_uuid", "'" + detail::printable(value.asString()) + "' is not a UUID");
        }
        *static_cast<Uuid*>(object) = *parsed;
        return true;
    }

    bool readEntity(const Value& value, void* object) {
        if (!detail::canReadEntities(m_entities)) {
            return fail("serialization.no_entity_mapping", detail::kNoEntityMappingMessage);
        }
        Uuid id;
        if (!readUuid(value, &id)) {
            return false;
        }
        *static_cast<Entity*>(object) = detail::idToEntity(*m_entities, id);
        return true;
    }

    bool enumerator(const TypeInfo& type, const Value& name, std::int64_t& out) {
        if (!expect(name, Value::Kind::String, "an enumerator name")) {
            return false;
        }
        const EnumeratorInfo* found = type.findEnumerator(name.asString());
        if (found == nullptr) {
            return fail("serialization.unknown_enumerator", "'" + detail::printable(name.asString()) +
                                                                "' is not an enumerator of " + std::string(type.name));
        }
        out = found->value;
        return true;
    }

    bool readEnum(const TypeInfo& type, const Value& value, void* object) {
        std::int64_t result = 0;
        if (hasFlags(type.flags, ReflectionFlags::Flags)) {
            if (!expect(value, Value::Kind::Array, "an array of enumerator names")) {
                return false;
            }
            std::uint64_t bits = 0;
            for (std::size_t index = 0; index < value.elements().size(); ++index) {
                const std::size_t mark = m_path.pushIndex(index);
                std::int64_t flag = 0;
                if (!enumerator(type, value.elements()[index], flag)) {
                    return false;
                }
                bits |= static_cast<std::uint64_t>(flag);
                m_path.pop(mark);
            }
            result = static_cast<std::int64_t>(bits);
        } else if (!enumerator(type, value, result)) {
            return false;
        }
        writeEnumValue(type, object, result);
        return true;
    }

    bool enter(std::size_t depth) {
        return depth + 1 <= kMaxDepth ||
               fail("serialization.too_deep", "the value nests deeper than " + std::to_string(kMaxDepth));
    }

    bool unknownField(const Value::Member& member) {
        const std::string key = detail::printable(member.key);
        if (m_unknownFields == UnknownFields::Fail) {
            const std::size_t mark = m_path.pushField(member.key);
            fail("serialization.unknown_field", "unknown field '" + key + "'");
            m_path.pop(mark);
            return false;
        }
        ++m_unknownCount;
        if (m_collectWarnings && m_unknownCount <= kMaxWarnings) {
            Error problem = makeError("serialization.unknown_field", "unknown field '" + key + "' is dropped");
            const std::size_t mark = m_path.pushField(member.key);
            problem.path = m_path.path();
            m_path.pop(mark);
            m_warnings.push_back(std::move(problem));
        }
        return true;
    }

    bool readStruct(const TypeInfo& type, const Value& value, void* object, std::size_t depth) {
        if (!enter(depth) || !expect(value, Value::Kind::Object, "an object")) {
            return false;
        }
        std::vector<const std::string*> seen(type.fields.size(), nullptr);
        for (const Value::Member& member : value.members()) {
            const FieldInfo* field = type.findField(std::string_view(member.key));
            if (field == nullptr || !detail::isSaved(*field, detail::kNotInText)) {
                if (!unknownField(member)) {
                    return false;
                }
                continue;
            }
            const std::size_t mark = m_path.pushField(member.key);
            const auto index = static_cast<std::size_t>(field - type.fields.data());
            if (seen[index] != nullptr) {
                return fail("serialization.duplicate_field", "field '" + std::string(field->name) +
                                                                 "' is given twice, also as '" +
                                                                 detail::printable(*seen[index]) + "'");
            }
            seen[index] = &member.key;
            if (!read(field->type(), member.value, static_cast<std::byte*>(object) + field->offset, depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    bool readContainer(const TypeInfo& type, const Value& value, void* object, std::size_t depth) {
        const TypeInfo& element = type.element();
        if (detail::isExcluded(element.flags, detail::kNotInText)) {
            return fail("serialization.not_saved", detail::describe(element) + " is Transient and can't be loaded");
        }
        if (type.kind == TypeKind::Optional) {
            if (element.kind == TypeKind::Optional) {
                return fail("serialization.unsupported_type",
                            "an optional of an optional can't be told apart from an empty one in JSON");
            }
            if (value.kind() == Value::Kind::Null) {
                type.container->resize(object, 0);
                return true;
            }
            if (!detail::resizeWithin(type, object, 1, m_budget)) {
                return tooLarge();
            }
            return read(element, value, type.container->at(object, 0), depth);
        }
        if (!enter(depth) || !expect(value, Value::Kind::Array, "an array")) {
            return false;
        }
        const std::size_t count = value.elements().size();
        if (type.kind == TypeKind::FixedArray) {
            if (count != type.container->size(object)) {
                return fail("serialization.wrong_size", "expected " + std::to_string(type.container->size(object)) +
                                                            " elements, found " + std::to_string(count));
            }
        } else if (!detail::resizeWithin(type, object, count, m_budget)) {
            return tooLarge();
        }
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t mark = m_path.pushIndex(index);
            if (!read(element, value.elements()[index], type.container->at(object, index), depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    // Unknown fields reported one by one; more are counted in one warning.
    static constexpr std::size_t kMaxWarnings = 100;

    detail::PathBuilder m_path;
    UnknownFields m_unknownFields;
    bool m_collectWarnings;
    detail::AllocationBudget m_budget;
    const EntityMapping* m_entities;
    std::size_t m_unknownCount = 0;
    Error m_error;
    std::vector<Error> m_warnings;
};

Unexpected<Error> withSource(Error error, std::string_view source) {
    error.source = std::string(source);
    return makeUnexpected(std::move(error));
}

Unexpected<Error> headerError(std::string_view source, std::string code, std::string message) {
    return withSource(makeError(std::move(code), std::move(message)), source);
}

constexpr const char* kNotADocument = R"(a document is an object with "format", "version" and "data")";

// The keys of a document.
struct Header {
    const Value* name = nullptr;
    const Value* version = nullptr;
    const Value* id = nullptr;
    Value* data = nullptr;
};

[[nodiscard]] bool isHeaderKey(std::string_view key) noexcept {
    return key == "format" || key == "version" || key == "id" || key == "data";
}

// The id of an asset document, or nullopt when `id` is not a UUID other than nil.
std::optional<Uuid> documentId(const Value& id) {
    if (id.kind() != Value::Kind::String) {
        return std::nullopt;
    }
    const Expected<Uuid> parsed = Uuid::parse(id.asString());
    if (!parsed || parsed->isNil()) {
        return std::nullopt;
    }
    return *parsed;
}

// Finds the keys of a document and checks its "id" against options.id (ADR-0024 rule 3). Other
// keys are skipped, or fail with UnknownFields::Fail.
Expected<Header, Error> findHeader(Value& document, const TextReadOptions& options) {
    if (document.kind() != Value::Kind::Object) {
        return headerError(options.source, "serialization.not_a_document", kNotADocument);
    }
    Header header;
    for (Value::Member& member : document.members()) {
        bool duplicate = false;
        if (member.key == "format") {
            duplicate = header.name != nullptr;
            header.name = &member.value;
        } else if (member.key == "version") {
            duplicate = header.version != nullptr;
            header.version = &member.value;
        } else if (member.key == "id") {
            duplicate = header.id != nullptr;
            header.id = &member.value;
        } else if (member.key == "data") {
            duplicate = header.data != nullptr;
            header.data = &member.value;
        } else if (options.unknownFields == UnknownFields::Fail) {
            return headerError(options.source, "serialization.unknown_field",
                               "unknown document key '" + detail::printable(member.key) + "'");
        }
        if (duplicate) {
            return headerError(options.source, "serialization.duplicate_field",
                               "document key '" + detail::printable(member.key) + "' is given twice");
        }
    }
    if (header.name == nullptr || header.version == nullptr || header.data == nullptr ||
        header.name->kind() != Value::Kind::String) {
        return headerError(options.source, "serialization.not_a_document", kNotADocument);
    }
    if (options.id == nullptr && header.id != nullptr) {
        return headerError(options.source, "serialization.not_a_document",
                           R"(the document has an "id", but only asset documents have one)");
    }
    if (options.id != nullptr && header.id == nullptr) {
        return headerError(options.source, "serialization.not_a_document", R"(an asset document needs an "id")");
    }
    if (options.id != nullptr && !documentId(*header.id).has_value()) {
        return headerError(options.source, "serialization.not_a_document",
                           R"(the document's "id" is not a UUID other than nil)");
    }
    return header;
}

// The version of a document of `format`, when this engine reads it.
Expected<std::uint32_t, Error> checkVersion(const Header& header, const DocumentFormat& format,
                                            std::string_view source) {
    if (header.name->asString() != format.name) {
        return headerError(source, "serialization.wrong_format",
                           "the document is a '" + detail::printable(header.name->asString()) + "', expected '" +
                               std::string(format.name) + "'");
    }
    const std::optional<std::uint32_t> version = header.version->toNumber<std::uint32_t>();
    if (!version.has_value()) {
        return headerError(source, "serialization.not_a_document", R"("version" is not a version number)");
    }
    if (*version > format.version) {
        return headerError(source, "serialization.newer_version",
                           "the document has version " + std::to_string(*version) + ", newer than version " +
                               std::to_string(format.version) + " that this engine reads");
    }
    if (*version < format.minimumVersion) {
        return headerError(source, "serialization.version_too_old",
                           "the document has version " + std::to_string(*version) +
                               "; the oldest version this engine reads is " + std::to_string(format.minimumVersion));
    }
    return *version;
}

// Runs the migrations from `version` to the current version on the parsed data. They change `data`
// through function pointers, which misc-const-correctness doesn't follow.
// NOLINTNEXTLINE(misc-const-correctness)
Expected<void, Error> migrate(Value& data, std::uint32_t version, const DocumentFormat& format,
                              std::string_view source) {
    for (std::uint32_t step = version; step < format.version; ++step) {
        Expected<void, Error> migrated = format.migrations[step - format.minimumVersion](data);
        if (!migrated) {
            Error error = std::move(migrated).error();
            error.message = "migration from version " + std::to_string(step) + " to " + std::to_string(step + 1) +
                            " failed: " + error.message + " [" + error.code + "]";
            error.code = "serialization.migration_failed";
            return withSource(std::move(error), source);
        }
    }
    return {};
}

// Parses `text`, checks its envelope against `format` and options.id, and migrates its data in
// place. Returns the whole document; its "data" member holds the current version.
Expected<Value, Error> parseDocument(std::string_view text, const DocumentFormat& format,
                                     const TextReadOptions& options) {
    Expected<Value, Error> document = parseJson(text, JsonReadOptions{.source = options.source,
                                                                      .syntax = options.syntax,
                                                                      .maxSize = options.maxSize,
                                                                      .allocationBudget = options.allocationBudget});
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    const Expected<Header, Error> header = findHeader(*document, options);
    if (!header) {
        return makeUnexpected(header.error());
    }
    const Expected<std::uint32_t, Error> version = checkVersion(*header, format, options.source);
    if (!version) {
        return makeUnexpected(version.error());
    }
    // Migrations edit the parsed tree, which is discarded on failure. They see "data" only, never
    // the envelope.
    if (Expected<void, Error> migrated = migrate(*header->data, *version, format, options.source); !migrated) {
        return makeUnexpected(std::move(migrated).error());
    }
    return document;
}

// Completes a successful read: stores the id (checked by findHeader) and appends the warnings for
// unknown document keys, at most kMaxHeaderWarnings of them.
void finishRead(const Value& document, const TextReadOptions& options) {
    if (options.id != nullptr) {
        *options.id = documentId(*document.find("id")).value_or(Uuid());
    }
    if (options.warnings == nullptr) {
        return;
    }
    constexpr std::size_t kMaxHeaderWarnings = 100;
    std::size_t unknownKeys = 0;
    for (const Value::Member& member : document.members()) {
        if (!isHeaderKey(member.key) && ++unknownKeys <= kMaxHeaderWarnings) {
            options.warnings->push_back(
                Error{.code = "serialization.unknown_field",
                      .message = "unknown document key '" + detail::printable(member.key) + "' is dropped",
                      .source = std::string(options.source)});
        }
    }
    if (unknownKeys > kMaxHeaderWarnings) {
        options.warnings->push_back(Error{.code = "serialization.unknown_field",
                                          .message = std::to_string(unknownKeys - kMaxHeaderWarnings) +
                                                     " more unknown document keys are dropped",
                                          .source = std::string(options.source)});
    }
}

// A document object with "format", "version" and, for an asset document, "id"; the caller adds
// "data". Fails with "serialization.invalid_format" or "serialization.invalid_id".
Expected<Value, Error> makeEnvelope(const DocumentFormat& format, const Uuid* id) {
    if (!detail::isValidFormat(format)) {
        return makeUnexpected(makeError("serialization.invalid_format", "the DocumentFormat is invalid"));
    }
    if (id != nullptr && id->isNil()) {
        return makeUnexpected(makeError("serialization.invalid_id", "an asset document's id can't be nil"));
    }
    Value document = Value::object();
    document.set("format", Value::string(std::string(format.name)));
    document.set("version", Value::unsignedInteger(format.version));
    if (id != nullptr) {
        document.set("id", Value::string(id->toString()));
    }
    return document;
}

} // namespace

Expected<std::string, Error> writeText(const DocumentFormat& format, const TypeInfo& type, const void* object,
                                       const Uuid* id, const EntityMapping* entities) {
    Expected<Value, Error> document = makeEnvelope(format, id);
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    if (detail::isExcluded(type.flags, detail::kNotInText)) {
        return makeUnexpected(
            makeError("serialization.not_saved", std::string(type.name) + " is Transient and can't be saved"));
    }
    TextWriter writer(entities);
    // The document object is the first level of nesting.
    if (!writer.write(type, object, document->set("data", Value()), 1)) {
        return makeUnexpected(writer.takeError());
    }
    return writeJson(*document);
}

Expected<std::string, Error> writeDocument(const DocumentFormat& format, const Value& data, const Uuid* id) {
    Expected<Value, Error> document = makeEnvelope(format, id);
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    document->set("data", data);
    return writeJson(*document);
}

Expected<Value, Error> readDocument(std::string_view text, const DocumentFormat& format,
                                    const TextReadOptions& options) {
    if (!detail::isValidFormat(format)) {
        return headerError(options.source, "serialization.invalid_format", "the DocumentFormat is invalid");
    }
    Expected<Value, Error> document = parseDocument(text, format, options);
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    finishRead(*document, options);
    Value data = std::move(*document->find("data"));
    return data;
}

Expected<void, Error> readText(std::string_view text, const DocumentFormat& format, const TypeInfo& type, void* target,
                               const TextReadOptions& options) {
    if (!detail::isValidFormat(format)) {
        return headerError(options.source, "serialization.invalid_format", "the DocumentFormat is invalid");
    }
    if (detail::isExcluded(type.flags, detail::kNotInText)) {
        return headerError(options.source, "serialization.not_saved",
                           std::string(type.name) + " is Transient and can't be loaded");
    }
    const Expected<Value, Error> document = parseDocument(text, format, options);
    if (!document) {
        return makeUnexpected(document.error());
    }

    OwnedValue result(type);
    TextReader reader(options.unknownFields, options.warnings != nullptr,
                      allocationBudget(text.size(), options.allocationBudget), options.entities);
    if (!reader.read(type, *document->find("data"), result.get(), 1)) {
        return withSource(reader.takeError(), options.source);
    }
    result.moveInto(target);

    finishRead(*document, options);
    if (options.warnings != nullptr) {
        for (Error& warning : reader.takeWarnings()) {
            warning.source = std::string(options.source);
            options.warnings->push_back(std::move(warning));
        }
    }
    return {};
}

Expected<Value, Error> toValue(const TypeInfo& type, const void* object, const EntityMapping* entities) {
    if (detail::isExcluded(type.flags, detail::kNotInText)) {
        return makeUnexpected(
            makeError("serialization.not_saved", std::string(type.name) + " is Transient and can't be saved"));
    }
    Value result;
    TextWriter writer(entities);
    if (!writer.write(type, object, result, 0)) {
        return makeUnexpected(writer.takeError());
    }
    return result;
}

Expected<void, Error> fromValue(const Value& value, const TypeInfo& type, void* target,
                                const ValueReadOptions& options) {
    if (detail::isExcluded(type.flags, detail::kNotInText)) {
        return makeUnexpected(
            makeError("serialization.not_saved", std::string(type.name) + " is Transient and can't be loaded"));
    }
    OwnedValue result(type);
    TextReader reader(options.unknownFields, options.warnings != nullptr, allocationBudget(0, options.allocationBudget),
                      options.entities);
    if (!reader.read(type, value, result.get(), 0)) {
        return makeUnexpected(reader.takeError());
    }
    result.moveInto(target);
    if (options.warnings != nullptr) {
        for (Error& warning : reader.takeWarnings()) {
            options.warnings->push_back(std::move(warning));
        }
    }
    return {};
}

} // namespace ith::serialization
