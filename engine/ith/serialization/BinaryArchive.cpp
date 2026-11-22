#include "ith/serialization/BinaryArchive.h"

#include "ith/foundation/Hash.h"
#include "ith/foundation/Uuid.h"
#include "ith/serialization/ArchiveSupport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace ith::serialization {
namespace {

// Values are copied with memcpy, which is little-endian on every platform the engine targets.
static_assert(std::endian::native == std::endian::little, "the binary archive assumes a little-endian host");

using detail::makeError;

constexpr std::array<std::byte, 4> kMagic = {std::byte{'I'}, std::byte{'T'}, std::byte{'H'}, std::byte{'B'}};
constexpr std::size_t kFixedHeaderSize = 26; // up to and including the name length
constexpr std::size_t kPayloadAlignment = 16;

constexpr std::size_t paddedHeaderSize(std::size_t nameLength) noexcept {
    const std::size_t size = kFixedHeaderSize + nameLength;
    return (size + kPayloadAlignment - 1) / kPayloadAlignment * kPayloadAlignment;
}

// Computes schemaHash. Results are kept per type id for the length of one call, so a type reached
// along many paths is hashed once.
class SchemaHasher {
public:
    std::uint64_t of(const TypeInfo& type) {
        // A struct reached again through a vector of itself is identified by its kind and id alone.
        if (std::ranges::find(m_structs, type.id) != m_structs.end()) {
            return hashCombine(hashCombine(kFnv1a64Basis, static_cast<std::uint64_t>(type.kind)), type.id);
        }
        if (const auto found = m_done.find(type.id); found != m_done.end()) {
            return found->second;
        }
        const std::uint64_t hash = compute(type);
        m_done.emplace(type.id, hash);
        return hash;
    }

private:
    // Hashes what the encoding depends on: encoded widths, never C++ object sizes, which differ
    // between standard libraries and configurations (std::string is 24 to 40 bytes).
    std::uint64_t compute(const TypeInfo& type) {
        std::uint64_t hash = hashCombine(hashCombine(kFnv1a64Basis, static_cast<std::uint64_t>(type.kind)), type.id);
        switch (type.kind) {
            case TypeKind::Bool:
            case TypeKind::SignedInteger:
            case TypeKind::UnsignedInteger:
            case TypeKind::Float:
                return hashCombine(hash, type.size);
            case TypeKind::String:
                return hash; // a uint32 length and the bytes
            case TypeKind::Uuid:
            case TypeKind::Entity: // written as the Uuid its mapping gives
                return hashCombine(hash, 2 * sizeof(std::uint64_t));
            case TypeKind::Enum:
                hash = hashCombine(hash, type.size);
                for (const EnumeratorInfo& enumerator : type.enumerators) {
                    hash = hashCombine(hashCombine(hash, fnv1a64(enumerator.name)),
                                       static_cast<std::uint64_t>(enumerator.value));
                }
                return hash;
            case TypeKind::Struct:
                m_structs.push_back(type.id);
                for (const FieldInfo& field : type.fields) {
                    if (detail::isSaved(field, detail::kNotInBinary)) {
                        hash = hashCombine(hashCombine(hash, field.id), of(field.type()));
                    }
                }
                m_structs.pop_back();
                return hash;
            case TypeKind::FixedArray:
            case TypeKind::DynamicArray:
            case TypeKind::Optional:
                return hashCombine(hash, of(type.element()));
        }
        return hash;
    }

    std::vector<std::uint64_t> m_structs; // the structs being hashed, outermost first
    std::unordered_map<std::uint64_t, std::uint64_t> m_done;
};

constexpr std::size_t saturatingAdd(std::size_t a, std::size_t b) noexcept {
    return a > std::numeric_limits<std::size_t>::max() - b ? std::numeric_limits<std::size_t>::max() : a + b;
}

constexpr std::size_t saturatingMultiply(std::size_t a, std::size_t b) noexcept {
    return b != 0 && a > std::numeric_limits<std::size_t>::max() / b ? std::numeric_limits<std::size_t>::max() : a * b;
}

// The fewest payload bytes a value of `type` takes. Cached per type id, because a fixed
// array's length is known only from a value of it.
class MinimumSizes {
public:
    std::size_t of(const TypeInfo& type) {
        if (const auto found = m_sizes.find(type.id); found != m_sizes.end()) {
            return found->second;
        }
        const std::size_t size = compute(type);
        m_sizes.emplace(type.id, size);
        return size;
    }

private:
    std::size_t compute(const TypeInfo& type) {
        switch (type.kind) {
            case TypeKind::Bool:
            case TypeKind::SignedInteger:
            case TypeKind::UnsignedInteger:
            case TypeKind::Float:
            case TypeKind::Enum:
                return type.size;
            case TypeKind::String:
            case TypeKind::DynamicArray:
                return sizeof(std::uint32_t);
            case TypeKind::Uuid:
            case TypeKind::Entity:
                return 2 * sizeof(std::uint64_t);
            case TypeKind::Optional:
                return 1;
            case TypeKind::Struct: {
                std::size_t size = 0;
                for (const FieldInfo& field : type.fields) {
                    if (detail::isSaved(field, detail::kNotInBinary)) {
                        size = saturatingAdd(size, of(field.type()));
                    }
                }
                return size;
            }
            case TypeKind::FixedArray: {
                const detail::TempValue array(type);
                return saturatingMultiply(type.container->size(array.get()), of(type.element()));
            }
        }
        return 0;
    }

    std::unordered_map<std::uint64_t, std::size_t> m_sizes;
};

bool isValidEnumValue(const TypeInfo& type, std::int64_t value) noexcept {
    if (hasFlags(type.flags, ReflectionFlags::Flags)) {
        std::uint64_t known = 0;
        for (const EnumeratorInfo& enumerator : type.enumerators) {
            known |= static_cast<std::uint64_t>(enumerator.value);
        }
        return (static_cast<std::uint64_t>(value) & ~known) == 0;
    }
    return std::ranges::any_of(type.enumerators,
                               [value](const EnumeratorInfo& enumerator) { return enumerator.value == value; });
}

class BinaryWriter {
public:
    explicit BinaryWriter(const EntityMapping* entities) noexcept : m_entities(entities) {}

    bool write(const TypeInfo& type, const void* object, std::size_t depth) {
        switch (type.kind) {
            case TypeKind::Bool:
                return writeByte(*static_cast<const bool*>(object) ? 1 : 0);
            case TypeKind::SignedInteger:
            case TypeKind::UnsignedInteger:
            case TypeKind::Float:
                append(object, type.size);
                return true;
            case TypeKind::String: {
                const auto& text = *static_cast<const std::string*>(object);
                if (!writeCount(text.size())) {
                    return false;
                }
                append(text.data(), text.size());
                return true;
            }
            case TypeKind::Uuid:
                writeUuid(*static_cast<const Uuid*>(object));
                return true;
            case TypeKind::Entity:
                if (!detail::canWriteEntities(m_entities)) {
                    return fail("serialization.no_entity_mapping", detail::kNoEntityMappingMessage);
                }
                writeUuid(detail::entityToId(*m_entities, *static_cast<const Entity*>(object)));
                return true;
            case TypeKind::Enum:
                if (!isValidEnumValue(type, readEnumValue(type, object))) {
                    return fail("serialization.invalid_enum", std::to_string(readEnumValue(type, object)) +
                                                                  " is not a valid " + std::string(type.name));
                }
                append(object, type.size);
                return true;
            case TypeKind::Struct:
                return writeStruct(type, object, depth);
            case TypeKind::FixedArray:
            case TypeKind::DynamicArray:
            case TypeKind::Optional:
                return writeContainer(type, object, depth);
        }
        return fail("serialization.unsupported_type", "unknown kind of type");
    }

    std::vector<std::byte>& bytes() noexcept { return m_bytes; }

    Error takeError() { return std::move(m_error); }

private:
    bool fail(std::string code, std::string message) {
        m_error = makeError(std::move(code), std::move(message));
        m_error.path = m_path.path();
        return false;
    }

    void append(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::byte*>(data);
        m_bytes.insert(m_bytes.end(), bytes, bytes + size);
    }

    bool writeByte(std::uint8_t value) {
        append(&value, 1);
        return true;
    }

    void writeUuid(const Uuid& id) {
        const std::uint64_t high = id.high();
        const std::uint64_t low = id.low();
        append(&high, sizeof(high));
        append(&low, sizeof(low));
    }

    bool writeCount(std::size_t count) {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return fail("serialization.too_large", std::to_string(count) + " elements are more than a blob can hold");
        }
        const auto value = static_cast<std::uint32_t>(count);
        append(&value, sizeof(value));
        return true;
    }

    bool enter(std::size_t depth) {
        return depth + 1 <= kMaxDepth ||
               fail("serialization.too_deep", "the value nests deeper than " + std::to_string(kMaxDepth));
    }

    bool writeStruct(const TypeInfo& type, const void* object, std::size_t depth) {
        if (!enter(depth)) {
            return false;
        }
        for (const FieldInfo& field : type.fields) {
            if (!detail::isSaved(field, detail::kNotInBinary)) {
                continue;
            }
            const std::size_t mark = m_path.pushField(field.name);
            if (!write(field.type(), static_cast<const std::byte*>(object) + field.offset, depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    bool writeContainer(const TypeInfo& type, const void* object, std::size_t depth) {
        const TypeInfo& element = type.element();
        if (detail::isExcluded(element.flags, detail::kNotInBinary)) {
            return fail("serialization.not_saved",
                        detail::describe(element) + " is Transient or EditorOnly and can't be cooked");
        }
        const std::size_t count = type.container->size(object);
        if (type.kind == TypeKind::Optional) {
            writeByte(count == 0 ? 0 : 1);
            return count == 0 || write(element, type.container->constAt(object, 0), depth);
        }
        if (!enter(depth)) {
            return false;
        }
        if (type.kind == TypeKind::DynamicArray) {
            if (m_sizes.of(element) == 0) {
                return fail("serialization.unsupported_type",
                            "a vector of " + detail::describe(element) + " has elements of zero bytes");
            }
            if (!writeCount(count)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t mark = m_path.pushIndex(index);
            if (!write(element, type.container->constAt(object, index), depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    const EntityMapping* m_entities;
    std::vector<std::byte> m_bytes;
    detail::PathBuilder m_path;
    MinimumSizes m_sizes;
    Error m_error;
};

class BinaryReader {
public:
    BinaryReader(std::span<const std::byte> data, std::size_t allocationBudget, const EntityMapping* entities)
        : m_data(data), m_budget(allocationBudget), m_entities(entities) {}

    bool read(const TypeInfo& type, void* object, std::size_t depth) {
        switch (type.kind) {
            case TypeKind::Bool: {
                std::uint8_t value = 0;
                if (!readZeroOrOne(value, "a bool")) {
                    return false;
                }
                *static_cast<bool*>(object) = value != 0;
                return true;
            }
            case TypeKind::SignedInteger:
            case TypeKind::UnsignedInteger:
            case TypeKind::Float:
                return take(object, type.size);
            case TypeKind::String:
                return readString(object);
            case TypeKind::Uuid:
                return readUuid(*static_cast<Uuid*>(object));
            case TypeKind::Entity: {
                if (!detail::canReadEntities(m_entities)) {
                    return fail("serialization.no_entity_mapping", detail::kNoEntityMappingMessage);
                }
                Uuid id;
                if (!readUuid(id)) {
                    return false;
                }
                *static_cast<Entity*>(object) = detail::idToEntity(*m_entities, id);
                return true;
            }
            case TypeKind::Enum: {
                const std::size_t at = m_position;
                if (!take(object, type.size)) {
                    return false;
                }
                const std::int64_t value = readEnumValue(type, object);
                if (!isValidEnumValue(type, value)) {
                    m_position = at;
                    return fail("serialization.invalid_value",
                                std::to_string(value) + " is not a valid " + std::string(type.name));
                }
                return true;
            }
            case TypeKind::Struct:
                return readStruct(type, object, depth);
            case TypeKind::FixedArray:
            case TypeKind::DynamicArray:
            case TypeKind::Optional:
                return readContainer(type, object, depth);
        }
        return fail("serialization.unsupported_type", "unknown kind of type");
    }

    // Copies the next `size` bytes to `out`, or fails when fewer are left.
    bool take(void* out, std::size_t size) {
        if (size > remaining()) {
            return fail("serialization.truncated",
                        "needs " + std::to_string(size) + " bytes, " + std::to_string(remaining()) + " are left");
        }
        if (size != 0) {
            std::memcpy(out, m_data.data() + m_position, size);
        }
        m_position += size;
        return true;
    }

    bool fail(std::string code, std::string message) {
        m_error = makeError(std::move(code), std::move(message));
        m_error.path = m_path.path();
        m_error.offset = m_position;
        return false;
    }

    // A failure outside the payload, which has no path.
    bool failHeader(std::string code, std::string message) {
        fail(std::move(code), std::move(message));
        m_error.path.clear();
        return false;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_position; }

    [[nodiscard]] std::size_t position() const noexcept { return m_position; }

    Error takeError() { return std::move(m_error); }

private:
    bool readUuid(Uuid& id) {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
        if (!take(&high, sizeof(high)) || !take(&low, sizeof(low))) {
            return false;
        }
        id = Uuid(high, low);
        return true;
    }

    bool readZeroOrOne(std::uint8_t& value, const char* what) {
        const std::size_t at = m_position;
        if (!take(&value, 1)) {
            return false;
        }
        if (value > 1) {
            m_position = at;
            return fail("serialization.invalid_value",
                        std::string(what) + " must be 0 or 1, found " + std::to_string(value));
        }
        return true;
    }

    bool readCount(std::uint32_t& count, std::size_t minimumElementSize) {
        const std::size_t at = m_position;
        if (!take(&count, sizeof(count))) {
            return false;
        }
        if (count > remaining() / minimumElementSize) {
            m_position = at;
            return fail("serialization.truncated", std::to_string(count) + " elements need at least " +
                                                       std::to_string(minimumElementSize) + " bytes each, " +
                                                       std::to_string(remaining()) + " are left");
        }
        return true;
    }

    bool tooLarge() {
        return fail("serialization.too_large", "the value needs more memory than the allocation budget allows");
    }

    bool readString(void* object) {
        std::uint32_t length = 0;
        if (!readCount(length, 1)) {
            return false;
        }
        if (!m_budget.take(length, 1)) {
            return tooLarge();
        }
        auto& text = *static_cast<std::string*>(object);
        text.assign(reinterpret_cast<const char*>(m_data.data() + m_position), length);
        m_position += length;
        return true;
    }

    bool enter(std::size_t depth) {
        return depth + 1 <= kMaxDepth ||
               fail("serialization.too_deep", "the value nests deeper than " + std::to_string(kMaxDepth));
    }

    bool readStruct(const TypeInfo& type, void* object, std::size_t depth) {
        if (!enter(depth)) {
            return false;
        }
        for (const FieldInfo& field : type.fields) {
            if (!detail::isSaved(field, detail::kNotInBinary)) {
                continue;
            }
            const std::size_t mark = m_path.pushField(field.name);
            if (!read(field.type(), static_cast<std::byte*>(object) + field.offset, depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    bool readContainer(const TypeInfo& type, void* object, std::size_t depth) {
        const TypeInfo& element = type.element();
        if (detail::isExcluded(element.flags, detail::kNotInBinary)) {
            return fail("serialization.not_saved",
                        detail::describe(element) + " is Transient or EditorOnly and can't be cooked");
        }
        if (type.kind == TypeKind::Optional) {
            std::uint8_t present = 0;
            if (!readZeroOrOne(present, "the presence byte of an optional")) {
                return false;
            }
            if (!detail::resizeWithin(type, object, present, m_budget)) {
                return tooLarge();
            }
            return present == 0 || read(element, type.container->at(object, 0), depth);
        }
        if (!enter(depth)) {
            return false;
        }
        if (type.kind == TypeKind::DynamicArray) {
            const std::size_t elementSize = m_sizes.of(element);
            if (elementSize == 0) {
                return fail("serialization.unsupported_type",
                            "a vector of " + detail::describe(element) + " has elements of zero bytes");
            }
            std::uint32_t count = 0;
            if (!readCount(count, elementSize)) {
                return false;
            }
            if (!detail::resizeWithin(type, object, count, m_budget)) {
                return tooLarge();
            }
        }
        const std::size_t count = type.container->size(object);
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t mark = m_path.pushIndex(index);
            if (!read(element, type.container->at(object, index), depth + 1)) {
                return false;
            }
            m_path.pop(mark);
        }
        return true;
    }

    std::span<const std::byte> m_data;
    std::size_t m_position = 0;
    detail::AllocationBudget m_budget;
    const EntityMapping* m_entities;
    detail::PathBuilder m_path;
    MinimumSizes m_sizes;
    Error m_error;
};

Unexpected<Error> withSource(Error error, std::string_view source) {
    error.source = std::string(source);
    return makeUnexpected(std::move(error));
}

} // namespace

std::uint64_t schemaHash(const TypeInfo& type) {
    SchemaHasher hasher;
    return hasher.of(type);
}

Expected<std::vector<std::byte>, Error> writeBinary(const DocumentFormat& format, const TypeInfo& type,
                                                    const void* object, const EntityMapping* entities) {
    if (!detail::isValidFormat(format)) {
        return makeUnexpected(makeError("serialization.invalid_format", "the DocumentFormat is invalid"));
    }
    if (detail::isExcluded(type.flags, detail::kNotInBinary)) {
        return makeUnexpected(makeError("serialization.not_saved",
                                        std::string(type.name) + " is Transient or EditorOnly and can't be cooked"));
    }
    BinaryWriter writer(entities);
    std::vector<std::byte>& blob = writer.bytes();
    const std::size_t headerSize = paddedHeaderSize(format.name.size());
    blob.resize(headerSize);
    if (!writer.write(type, object, 0)) {
        return makeUnexpected(writer.takeError());
    }

    const std::uint32_t version = format.version;
    const std::uint64_t hash = schemaHash(type);
    const std::uint64_t payloadSize = blob.size() - headerSize;
    const auto nameLength = static_cast<std::uint16_t>(format.name.size());
    std::memcpy(blob.data(), kMagic.data(), kMagic.size());
    std::memcpy(blob.data() + 4, &version, sizeof(version));
    std::memcpy(blob.data() + 8, &hash, sizeof(hash));
    std::memcpy(blob.data() + 16, &payloadSize, sizeof(payloadSize));
    std::memcpy(blob.data() + 24, &nameLength, sizeof(nameLength));
    std::memcpy(blob.data() + kFixedHeaderSize, format.name.data(), format.name.size());
    return std::move(blob);
}

Expected<void, Error> readBinary(std::span<const std::byte> data, const DocumentFormat& format, const TypeInfo& type,
                                 void* target, const BinaryReadOptions& options) {
    const std::string_view source = options.source;
    BinaryReader reader(data, allocationBudget(data.size(), options.allocationBudget), options.entities);
    if (!detail::isValidFormat(format)) {
        reader.failHeader("serialization.invalid_format", "the DocumentFormat is invalid");
        return withSource(reader.takeError(), source);
    }
    if (detail::isExcluded(type.flags, detail::kNotInBinary)) {
        reader.failHeader("serialization.not_saved",
                          std::string(type.name) + " is Transient or EditorOnly and can't be cooked");
        return withSource(reader.takeError(), source);
    }

    std::array<std::byte, 4> magic{};
    std::uint32_t version = 0;
    std::uint64_t hash = 0;
    std::uint64_t payloadSize = 0;
    std::uint16_t nameLength = 0;
    if (!reader.take(magic.data(), magic.size())) {
        return withSource(reader.takeError(), source);
    }
    if (magic != kMagic) {
        reader.failHeader("serialization.bad_magic", "the data is not an Ironthorn binary blob");
        return withSource(reader.takeError(), source);
    }
    std::string name;
    if (!reader.take(&version, sizeof(version)) || !reader.take(&hash, sizeof(hash)) ||
        !reader.take(&payloadSize, sizeof(payloadSize)) || !reader.take(&nameLength, sizeof(nameLength))) {
        return withSource(reader.takeError(), source);
    }
    if (nameLength == 0 || nameLength > 255 || nameLength > reader.remaining()) {
        reader.failHeader("serialization.truncated", "the format name length " + std::to_string(nameLength) +
                                                         " is invalid or longer than the data");
        return withSource(reader.takeError(), source);
    }
    name.resize(nameLength);
    static_cast<void>(reader.take(name.data(), nameLength));
    if (name != format.name) {
        reader.failHeader("serialization.wrong_format", "the blob is a '" + detail::printable(name) + "', expected '" +
                                                            std::string(format.name) + "'");
        return withSource(reader.takeError(), source);
    }
    if (version != format.version) {
        const bool newer = version > format.version;
        reader.failHeader(newer ? "serialization.newer_version" : "serialization.version_too_old",
                          "the blob has version " + std::to_string(version) + ", this engine reads version " +
                              std::to_string(format.version) + (newer ? "" : "; recook it"));
        return withSource(reader.takeError(), source);
    }
    if (hash != schemaHash(type)) {
        reader.failHeader("serialization.schema_mismatch",
                          "the blob was written with another layout of " + std::string(type.name) + "; recook it");
        return withSource(reader.takeError(), source);
    }
    const std::size_t headerSize = paddedHeaderSize(nameLength);
    while (reader.position() < headerSize) {
        std::byte padding{};
        if (!reader.take(&padding, 1)) {
            return withSource(reader.takeError(), source);
        }
        if (padding != std::byte{0}) {
            reader.failHeader("serialization.invalid_value", "a header padding byte is not zero");
            return withSource(reader.takeError(), source);
        }
    }
    if (payloadSize != reader.remaining()) {
        reader.failHeader(payloadSize > reader.remaining() ? "serialization.truncated" : "serialization.trailing_data",
                          "the header says the payload has " + std::to_string(payloadSize) + " bytes, the data has " +
                              std::to_string(reader.remaining()));
        return withSource(reader.takeError(), source);
    }

    const detail::TempValue result(type);
    if (!reader.read(type, result.get(), 0)) {
        return withSource(reader.takeError(), source);
    }
    if (reader.remaining() != 0) {
        reader.failHeader("serialization.trailing_data",
                          std::to_string(reader.remaining()) + " bytes are left after the value");
        return withSource(reader.takeError(), source);
    }
    result.moveInto(target);
    return {};
}

} // namespace ith::serialization
