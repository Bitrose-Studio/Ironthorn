#include "ith/asset_importers/GltfImporter.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_importers/ImportedModel.h"
#include "ith/foundation/StringUtils.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/Json.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace ith {
namespace {

using serialization::Value;

constexpr std::uint32_t kImporterVersion = 1;

// The binary container (GLB): a 12-byte header, then chunks of a 4-byte length, a 4-byte type and
// the data, every number little-endian.
constexpr std::uint32_t kGlbMagic = 0x46546C67U; // "glTF"
constexpr std::uint32_t kGlbVersion = 2;
constexpr std::uint32_t kChunkJson = 0x4E4F534AU; // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942U;  // "BIN\0"
constexpr std::size_t kGlbHeaderSize = 12;
constexpr std::size_t kChunkHeaderSize = 8;

// Accessor component types.
constexpr std::uint64_t kByte = 5120;
constexpr std::uint64_t kUnsignedByte = 5121;
constexpr std::uint64_t kShort = 5122;
constexpr std::uint64_t kUnsignedShort = 5123;
constexpr std::uint64_t kUnsignedInt = 5125;
constexpr std::uint64_t kFloat = 5126;

constexpr std::uint64_t kModeTriangles = 4;
constexpr std::uint64_t kMinByteStride = 4;
constexpr std::uint64_t kMaxByteStride = 252;
constexpr std::uint64_t kNoNode = std::numeric_limits<std::uint64_t>::max();

// The longest part of a URI quoted in an error message, in bytes.
constexpr std::size_t kMaxQuotedUri = 80;

Unexpected<Error> failure(std::string_view code, std::string_view where, std::string_view message) {
    std::string text = where.empty() ? std::string(message) : std::string(where) + ": " + std::string(message);
    return makeError(std::string(code), std::move(text));
}

Unexpected<Error> malformed(std::string_view where, std::string_view message) {
    return failure("import.malformed", where, message);
}

Unexpected<Error> unsupported(std::string_view where, std::string_view message) {
    return failure("import.unsupported", where, message);
}

Unexpected<Error> invalidUri(std::string_view where, std::string_view message) {
    return failure("import.invalid_uri", where, message);
}

// The error of the first failed result, or null when all of them hold values.
template <typename... Results>
const Error* firstError(const Results&... results) {
    const Error* error = nullptr;
    ((error = (error == nullptr && !results) ? &results.error() : error), ...);
    return error;
}

std::string child(std::string_view where, std::string_view key) {
    return where.empty() ? std::string(key) : std::string(where) + "." + std::string(key);
}

std::string element(std::string_view where, std::uint64_t index) {
    return std::string(where) + "[" + std::to_string(index) + "]";
}

// `uri` in quotes, shortened (at a UTF-8 character boundary) when it is long.
std::string inQuotes(std::string_view uri) {
    if (uri.size() <= kMaxQuotedUri) {
        return "'" + std::string(uri) + "'";
    }
    std::size_t end = kMaxQuotedUri;
    while (end > 0 && (static_cast<unsigned char>(uri[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return "'" + std::string(uri.substr(0, end)) + "...'";
}

std::optional<std::uint64_t> checkedAdd(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        return std::nullopt;
    }
    return a + b;
}

std::optional<std::uint64_t> checkedMultiply(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::nullopt;
    }
    return a * b;
}

// The little-endian unsigned number of `size` (1 to 4) bytes at `offset`.
std::uint32_t loadUnsigned(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

std::uint32_t loadU32(std::span<const std::byte> bytes, std::size_t offset) {
    return loadUnsigned(bytes, offset, 4);
}

// The size in bytes of a component type, or 0 for an unknown one.
std::uint64_t componentSize(std::uint64_t componentType) {
    switch (componentType) {
        case kByte:
        case kUnsignedByte:
            return 1;
        case kShort:
        case kUnsignedShort:
            return 2;
        case kUnsignedInt:
        case kFloat:
            return 4;
        default:
            return 0;
    }
}

// The number of components of an accessor type, or 0 for an unknown one.
std::uint64_t componentCount(std::string_view type) {
    constexpr std::array<std::pair<std::string_view, std::uint64_t>, 7> kTypes = {
        {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT2", 4}, {"MAT3", 9}, {"MAT4", 16}}};
    for (const auto& [name, count] : kTypes) {
        if (name == type) {
            return count;
        }
    }
    return 0;
}

std::string_view typeName(std::uint64_t components) {
    switch (components) {
        case 1:
            return "SCALAR";
        case 2:
            return "VEC2";
        case 3:
            return "VEC3";
        default:
            return "VEC4";
    }
}

// ---- JSON access ---------------------------------------------------------------------------

// The array member `key` of `object`, or an empty one when it is absent.
Expected<std::span<const Value>> arrayMember(const Value& object, std::string_view key, std::string_view where) {
    const Value* value = object.find(key);
    if (value == nullptr) {
        return std::span<const Value>();
    }
    if (value->kind() != Value::Kind::Array) {
        return malformed(child(where, key), "isn't an array");
    }
    return std::span<const Value>(value->elements());
}

// The object member `key` of `object`, or null when it is absent.
Expected<const Value*> objectMember(const Value& object, std::string_view key, std::string_view where) {
    const Value* value = object.find(key);
    if (value != nullptr && value->kind() != Value::Kind::Object) {
        return malformed(child(where, key), "isn't an object");
    }
    return value;
}

// The object member `key` of `object`, which must be there.
Expected<const Value*> requiredObject(const Value& object, std::string_view key, std::string_view where) {
    Expected<const Value*> value = objectMember(object, key, where);
    if (value && *value == nullptr) {
        return malformed(where, "has no " + std::string(key));
    }
    return value;
}

// The non-negative integer member `key` of `object`, or nullopt when it is absent.
Expected<std::optional<std::uint64_t>> integerMember(const Value& object, std::string_view key,
                                                     std::string_view where) {
    const Value* value = object.find(key);
    if (value == nullptr) {
        return std::optional<std::uint64_t>();
    }
    const std::optional<std::uint64_t> number = value->toNumber<std::uint64_t>();
    if (!number) {
        return malformed(child(where, key), "isn't a non-negative integer");
    }
    return number;
}

Expected<std::uint64_t> requiredInteger(const Value& object, std::string_view key, std::string_view where) {
    Expected<std::optional<std::uint64_t>> value = integerMember(object, key, where);
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    if (!value->has_value()) {
        return malformed(where, "has no " + std::string(key));
    }
    return **value;
}

Expected<float> floatMember(const Value& object, std::string_view key, std::string_view where, float fallback) {
    const Value* value = object.find(key);
    if (value == nullptr) {
        return fallback;
    }
    const std::optional<float> number = value->toNumber<float>();
    if (!number || !std::isfinite(*number)) {
        return malformed(child(where, key), "isn't a finite number");
    }
    return *number;
}

Expected<bool> boolMember(const Value& object, std::string_view key, std::string_view where, bool fallback) {
    const Value* value = object.find(key);
    if (value == nullptr) {
        return fallback;
    }
    if (value->kind() != Value::Kind::Bool) {
        return malformed(child(where, key), "isn't a boolean");
    }
    return value->asBool();
}

// The string member `key` of `object`, or nullopt when it is absent.
Expected<std::optional<std::string_view>> stringMember(const Value& object, std::string_view key,
                                                       std::string_view where) {
    const Value* value = object.find(key);
    if (value == nullptr) {
        return std::optional<std::string_view>();
    }
    if (value->kind() != Value::Kind::String) {
        return malformed(child(where, key), "isn't a string");
    }
    return std::optional<std::string_view>(value->asString());
}

// The "name" of `object`: empty when it has none; at most kGltfMaxNameSize bytes.
Expected<std::string_view> nameMember(const Value& object, std::string_view where) {
    Expected<std::optional<std::string_view>> name = stringMember(object, "name", where);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    if (name->value_or("").size() > kGltfMaxNameSize) {
        return failure("import.too_large", child(where, "name"),
                       "is longer than " + std::to_string(kGltfMaxNameSize) + " bytes");
    }
    return name->value_or("");
}

// Reads the array member `key` of `object`, exactly `target.size()` finite numbers, into `target`;
// leaves `target` alone when the member is absent.
Expected<void> floatArrayMember(const Value& object, std::string_view key, std::string_view where,
                                std::span<float> target) {
    if (object.find(key) == nullptr) {
        return {};
    }
    Expected<std::span<const Value>> values = arrayMember(object, key, where);
    if (!values) {
        return makeUnexpected(std::move(values).error());
    }
    if (values->size() != target.size()) {
        return malformed(child(where, key), "doesn't have " + std::to_string(target.size()) + " elements");
    }
    for (std::size_t index = 0; index < target.size(); ++index) {
        const std::optional<float> number = (*values)[index].toNumber<float>();
        if (!number || !std::isfinite(*number)) {
            return malformed(element(child(where, key), index), "isn't a finite number");
        }
        target[index] = *number;
    }
    return {};
}

// Element `index` of the top-level array `arrayName`, which must be an object.
Expected<const Value*> elementAt(std::span<const Value> array, std::uint64_t index, std::string_view arrayName) {
    if (index >= array.size()) {
        return malformed(arrayName, "has no element " + std::to_string(index));
    }
    const Value& value = array[static_cast<std::size_t>(index)];
    if (value.kind() != Value::Kind::Object) {
        return malformed(element(arrayName, index), "isn't an object");
    }
    return &value;
}

// ---- Container and URIs --------------------------------------------------------------------

struct Container {
    std::string_view json;
    // The BIN chunk of a GLB file.
    std::optional<std::span<const std::byte>> bin;
};

Expected<Container> splitGlb(std::span<const std::byte> source) {
    if (source.size() < kGlbHeaderSize) {
        return malformed("", "the GLB header is truncated");
    }
    if (const std::uint32_t version = loadU32(source, 4); version != kGlbVersion) {
        return unsupported("", "the file is GLB version " + std::to_string(version) + "; only version 2 is supported");
    }
    if (const std::uint32_t length = loadU32(source, 8); length != source.size()) {
        return malformed("", "the GLB header gives a length of " + std::to_string(length) +
                                 " bytes, but the file has " + std::to_string(source.size()));
    }
    Container container;
    std::size_t offset = kGlbHeaderSize;
    std::size_t index = 0;
    for (; offset < source.size(); ++index) {
        const std::string chunk = "GLB chunk " + std::to_string(index);
        if (source.size() - offset < kChunkHeaderSize) {
            return malformed(chunk, "its header is truncated");
        }
        const std::uint32_t length = loadU32(source, offset);
        const std::uint32_t type = loadU32(source, offset + 4);
        offset += kChunkHeaderSize;
        if (length > source.size() - offset) {
            return malformed(chunk, "it is longer than the rest of the file");
        }
        if (length % 4 != 0) {
            return malformed(chunk, "its length isn't a multiple of 4");
        }
        const std::span<const std::byte> data = source.subspan(offset, length);
        offset += length;
        if (index == 0) {
            if (type != kChunkJson) {
                return malformed(chunk, "the first chunk must be JSON");
            }
            container.json = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        } else if (type == kChunkJson) {
            return malformed(chunk, "a GLB file has one JSON chunk");
        } else if (type == kChunkBin) {
            if (index != 1) {
                return malformed(chunk, "the BIN chunk must be the second chunk");
            }
            container.bin = data;
        }
        // Chunks of other types are skipped, as the specification asks.
    }
    if (index == 0) {
        return malformed("", "the GLB file has no JSON chunk");
    }
    return container;
}

Expected<Container> splitContainer(std::span<const std::byte> source) {
    if (source.size() >= 4 && loadU32(source, 0) == kGlbMagic) {
        return splitGlb(source);
    }
    return Container{.json = std::string_view(reinterpret_cast<const char*>(source.data()), source.size()),
                     .bin = std::nullopt};
}

bool isDataUri(std::string_view uri) {
    constexpr std::string_view kScheme = "data:";
    return uri.size() >= kScheme.size() && equalsIgnoreCaseAscii(uri.substr(0, kScheme.size()), kScheme);
}

int base64Value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

// The bytes of standard base64 text (RFC 4648), with or without its '=' padding.
Expected<std::vector<std::byte>> decodeBase64(std::string_view text, std::string_view where) {
    std::size_t end = text.size();
    std::size_t padding = 0;
    while (padding < 2 && end > 0 && text[end - 1] == '=') {
        --end;
        ++padding;
    }
    if ((padding > 0 && text.size() % 4 != 0) || end % 4 == 1) {
        return invalidUri(where, "the base64 data has a wrong length");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(end / 4 * 3 + 2);
    std::uint32_t bits = 0;
    std::uint32_t bitCount = 0;
    for (std::size_t index = 0; index < end; ++index) {
        const int value = base64Value(text[index]);
        if (value < 0) {
            return invalidUri(where, "the data isn't valid base64");
        }
        bits = (bits << 6U) | static_cast<std::uint32_t>(value);
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            bytes.push_back(static_cast<std::byte>((bits >> bitCount) & 0xFFU));
            bits &= (1U << bitCount) - 1U;
        }
    }
    // Canonical base64 leaves the unused bits of the last character zero.
    if (bits != 0) {
        return invalidUri(where, "the base64 data has non-zero padding bits");
    }
    return bytes;
}

// The base64 text of a "data:<media type>;base64,<data>" URI.
Expected<std::string_view> dataUriPayload(std::string_view uri, std::string_view where) {
    const std::size_t comma = uri.find(',');
    if (comma == std::string_view::npos || !endsWithIgnoreCaseAscii(uri.substr(0, comma), ";base64")) {
        return invalidUri(where, "only base64 data: URIs are supported");
    }
    return uri.substr(comma + 1);
}

// The file path of a relative URI reference: percent-decoded, and refused unless it names a file
// inside the source file's folder (isValidContentPath: no "..", "." or empty segments, no
// absolute path, drive letter or backslash). Schemes other than "data:", queries and fragments
// are refused too. Paths longer than kGltfMaxNameSize are refused.
Expected<std::string> uriToPath(std::string_view uri, std::string_view where) {
    if (uri.size() > kGltfMaxNameSize) {
        return invalidUri(where, inQuotes(uri) + " is longer than " + std::to_string(kGltfMaxNameSize) + " bytes");
    }
    if (uri.find_first_of(":?#\\") != std::string_view::npos) {
        return invalidUri(where, inQuotes(uri) + " isn't a relative path; only relative paths and data: URIs are read");
    }
    std::string path;
    path.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] != '%') {
            path += uri[index];
            continue;
        }
        const int high = index + 2 < uri.size() ? hexDigitValue(uri[index + 1]) : -1;
        const int low = index + 2 < uri.size() ? hexDigitValue(uri[index + 2]) : -1;
        if (high < 0 || low < 0) {
            return invalidUri(where, inQuotes(uri) + " has a '%' that isn't followed by two hex digits");
        }
        path += static_cast<char>(high * 16 + low);
        index += 2;
    }
    if (!isValidContentPath(path)) {
        return invalidUri(where, inQuotes(uri) + " doesn't name a file inside the source file's folder");
    }
    return path;
}

// ---- The document --------------------------------------------------------------------------

struct Document {
    Value root;
    std::optional<std::span<const std::byte>> bin;
};

Expected<void> checkAsset(const Value& root) {
    Expected<const Value*> asset = requiredObject(root, "asset", "the document");
    if (!asset) {
        return makeUnexpected(std::move(asset).error());
    }
    Expected<std::optional<std::string_view>> version = stringMember(**asset, "version", "asset");
    Expected<std::optional<std::string_view>> minVersion = stringMember(**asset, "minVersion", "asset");
    Expected<std::span<const Value>> required = arrayMember(root, "extensionsRequired", "");
    if (const Error* error = firstError(version, minVersion, required)) {
        return makeUnexpected(*error);
    }
    if (!version->has_value()) {
        return malformed("asset", "has no version");
    }
    if (!(*version)->starts_with("2.")) {
        return unsupported("asset.version", "the file is glTF " + inQuotes(**version) + "; only 2.x is supported");
    }
    if (minVersion->has_value() && **minVersion != "2.0") {
        return unsupported("asset.minVersion", "the file needs glTF " + inQuotes(**minVersion));
    }
    if (!required->empty()) {
        const Value& first = required->front();
        const std::string name = first.kind() == Value::Kind::String ? inQuotes(first.asString()) : "an extension";
        return unsupported("extensionsRequired", "the file requires " + name + ", which isn't supported");
    }
    return {};
}

Expected<Document> parseDocument(std::span<const std::byte> source) {
    Expected<Container> container = splitContainer(source);
    if (!container) {
        return makeUnexpected(std::move(container).error());
    }
    Expected<Value, serialization::Error> root = serialization::parseJson(container->json, {.source = "glTF JSON"});
    if (!root) {
        const bool tooLarge = root.error().code == "serialization.too_large";
        return makeError(tooLarge ? "import.too_large" : "import.malformed", root.error().toString());
    }
    if (root->kind() != Value::Kind::Object) {
        return malformed("", "the document isn't a JSON object");
    }
    if (Expected<void> checked = checkAsset(*root); !checked) {
        return makeUnexpected(std::move(checked).error());
    }
    return Document{.root = std::move(*root), .bin = container->bin};
}

// The decoded bytes that one import may still allocate.
class DecodeBudget {
public:
    explicit DecodeBudget(std::uint64_t limit) : m_left(limit) {}

    // Takes `bytes` from the budget, or fails with "import.too_large".
    Expected<void> take(std::uint64_t bytes, std::string_view where) {
        if (bytes > m_left) {
            return failure("import.too_large", where, "the decoded data would exceed the import's limit");
        }
        m_left -= bytes;
        return {};
    }

    // Gives back bytes taken for data that has been freed.
    void give(std::uint64_t bytes) { m_left += bytes; }

private:
    std::uint64_t m_left;
};

// A buffer view: its bytes inside its buffer, their offset in the buffer and the view's stride.
struct BufferView {
    std::span<const std::byte> bytes;
    std::uint64_t offset = 0;
    std::optional<std::uint64_t> stride;
};

// An accessor, checked, before its data is copied.
struct Accessor {
    const Value* object = nullptr;
    std::string where;
    std::uint64_t componentType = 0;
    std::uint64_t componentSize = 0;
    std::uint64_t count = 0;
    std::uint64_t elementSize = 0;
    bool normalized = false;
    // The dense data from the first element on, with its stride; empty without a buffer view.
    std::span<const std::byte> data;
    std::uint64_t stride = 0;
};

// One part of a sparse accessor: its bytes and the size and type of their components.
struct SparsePart {
    std::span<const std::byte> bytes;
    std::uint64_t componentType = 0;
    std::uint64_t componentSize = 0;
};

class Reader {
public:
    Reader(const Value& root, DecodeBudget& budget) : m_root(root), m_budget(budget) {}

    Expected<void> readArrays();
    Expected<void> resolveBuffers(std::optional<std::span<const std::byte>> bin, std::span<const ImportFile> files);
    Expected<std::vector<std::string>> externalBufferPaths() const;

    [[nodiscard]] std::size_t meshCount() const { return m_meshes.size(); }

    [[nodiscard]] std::size_t materialCount() const { return m_materials.size(); }

    Expected<std::string> materialName(std::size_t index);
    Expected<ImportedMaterial> material(std::size_t index, std::set<std::string>& references);
    Expected<std::vector<std::string>> meshNames();
    Expected<ImportedMesh> mesh(std::size_t index, const std::vector<std::string>& materialKeys);

private:
    Expected<std::span<const std::byte>> bufferData(std::size_t index, const Value& buffer,
                                                    std::optional<std::span<const std::byte>> bin,
                                                    std::span<const ImportFile> files);
    Expected<BufferView> bufferView(std::uint64_t index) const;
    Expected<Accessor> accessor(std::uint64_t index, std::uint64_t components, std::string_view where) const;
    Expected<SparsePart> sparsePart(const Accessor& accessor, const Value& sparse, std::string_view key,
                                    std::uint64_t count) const;
    Expected<void> applySparse(const Accessor& accessor, const Value& sparse, std::span<std::byte> packed) const;
    Expected<std::vector<std::byte>> unpack(const Accessor& accessor);
    Expected<std::vector<float>> decodeFloats(std::uint64_t index, std::uint64_t components, bool allowNormalized,
                                              std::string_view where);
    Expected<std::vector<float>> readFloats(std::uint64_t index, std::uint64_t components, bool allowNormalized,
                                            std::string_view where);
    Expected<std::vector<std::uint32_t>> decodeIndices(std::uint64_t index, std::string_view where);
    Expected<std::vector<std::uint32_t>> readIndices(std::uint64_t index, std::uint64_t vertexCount,
                                                     std::string_view where);
    Expected<void> readVertices(const Value& attributes, std::string_view where, ImportedPrimitive& primitive);
    Expected<ImportedPrimitive> primitive(const Value& object, std::string_view where,
                                          const std::vector<std::string>& materialKeys);
    Expected<std::optional<ImportedTexture>> texture(const Value& object, std::string_view key, std::string_view where,
                                                     float* factor, std::string_view factorKey,
                                                     std::set<std::string>& references);
    Expected<std::string> copyName(const Value& object, std::string_view where);
    Expected<std::vector<std::uint64_t>> nodeParents();
    Expected<std::string> nodePath(std::uint64_t node, const std::vector<std::uint64_t>& parents);

    const Value& m_root;
    DecodeBudget& m_budget;
    std::span<const Value> m_buffersJson;
    std::span<const Value> m_bufferViews;
    std::span<const Value> m_accessors;
    std::span<const Value> m_meshes;
    std::span<const Value> m_materials;
    std::span<const Value> m_textures;
    std::span<const Value> m_images;
    std::span<const Value> m_nodes;
    // The data of each buffer, exactly its byteLength, and the decoded data: URIs it points into.
    std::vector<std::span<const std::byte>> m_buffers;
    std::vector<std::vector<std::byte>> m_decoded;
    // Decoded accessors by (index, components, normalized integers allowed), and index accessors
    // by index: primitives that share an accessor decode it once.
    std::map<std::tuple<std::uint64_t, std::uint64_t, bool>, std::vector<float>> m_floatAccessors;
    std::map<std::uint64_t, std::vector<std::uint32_t>> m_indexAccessors;
    // The name of every node (checked by nodeParents), and the name paths found so far.
    std::vector<std::string_view> m_nodeNames;
    std::vector<std::optional<std::string>> m_nodePaths;
};

Expected<void> Reader::readArrays() {
    const std::array<std::pair<std::string_view, std::span<const Value>*>, 8> arrays = {
        {{"buffers", &m_buffersJson},
         {"bufferViews", &m_bufferViews},
         {"accessors", &m_accessors},
         {"meshes", &m_meshes},
         {"materials", &m_materials},
         {"textures", &m_textures},
         {"images", &m_images},
         {"nodes", &m_nodes}}};
    for (const auto& [key, target] : arrays) {
        Expected<std::span<const Value>> array = arrayMember(m_root, key, "");
        if (!array) {
            return makeUnexpected(std::move(array).error());
        }
        *target = *array;
    }
    return {};
}

Expected<std::vector<std::string>> Reader::externalBufferPaths() const {
    std::vector<std::string> paths;
    for (std::size_t index = 0; index < m_buffersJson.size(); ++index) {
        Expected<const Value*> buffer = elementAt(m_buffersJson, index, "buffers");
        if (!buffer) {
            return makeUnexpected(std::move(buffer).error());
        }
        const std::string where = element("buffers", index);
        Expected<std::optional<std::string_view>> uri = stringMember(**buffer, "uri", where);
        if (!uri) {
            return makeUnexpected(std::move(uri).error());
        }
        if (uri->has_value() && !isDataUri(**uri)) {
            Expected<std::string> path = uriToPath(**uri, child(where, "uri"));
            if (!path) {
                return makeUnexpected(std::move(path).error());
            }
            paths.push_back(std::move(*path));
        }
    }
    return paths;
}

// The bytes of buffer `index`: the GLB's BIN chunk, a decoded data: URI or a file in `files`.
Expected<std::span<const std::byte>> Reader::bufferData(std::size_t index, const Value& buffer,
                                                        std::optional<std::span<const std::byte>> bin,
                                                        std::span<const ImportFile> files) {
    const std::string where = element("buffers", index);
    const std::string uriWhere = child(where, "uri");
    Expected<std::optional<std::string_view>> uri = stringMember(buffer, "uri", where);
    if (!uri) {
        return makeUnexpected(std::move(uri).error());
    }
    if (!uri->has_value()) {
        if (index != 0 || !bin) {
            return malformed(where, "has no uri; only the first buffer of a GLB file with a BIN chunk may omit it");
        }
        return *bin;
    }
    if (isDataUri(**uri)) {
        Expected<std::string_view> text = dataUriPayload(**uri, uriWhere);
        if (!text) {
            return makeUnexpected(std::move(text).error());
        }
        if (Expected<void> taken = m_budget.take(text->size() / 4 * 3 + 2, where); !taken) {
            return makeUnexpected(std::move(taken).error());
        }
        Expected<std::vector<std::byte>> decoded = decodeBase64(*text, uriWhere);
        if (!decoded) {
            return makeUnexpected(std::move(decoded).error());
        }
        // Moving a vector keeps its elements in place, so the span stays valid as m_decoded grows.
        m_decoded.push_back(std::move(*decoded));
        return std::span<const std::byte>(m_decoded.back());
    }
    Expected<std::string> path = uriToPath(**uri, uriWhere);
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    const auto file = std::ranges::find(files, std::string_view(*path), &ImportFile::path);
    if (file == files.end()) {
        return malformed(uriWhere, "the file " + inQuotes(*path) + " wasn't given to the importer");
    }
    return file->bytes;
}

Expected<void> Reader::resolveBuffers(std::optional<std::span<const std::byte>> bin,
                                      std::span<const ImportFile> files) {
    for (std::size_t index = 0; index < m_buffersJson.size(); ++index) {
        Expected<const Value*> buffer = elementAt(m_buffersJson, index, "buffers");
        if (!buffer) {
            return makeUnexpected(std::move(buffer).error());
        }
        const std::string where = element("buffers", index);
        Expected<std::uint64_t> length = requiredInteger(**buffer, "byteLength", where);
        if (!length) {
            return makeUnexpected(std::move(length).error());
        }
        if (*length == 0) {
            return malformed(child(where, "byteLength"), "is 0");
        }
        Expected<std::span<const std::byte>> data = bufferData(index, **buffer, bin, files);
        if (!data) {
            return makeUnexpected(std::move(data).error());
        }
        if (data->size() < *length) {
            return malformed(where, "its byteLength is " + std::to_string(*length) + ", but its data has only " +
                                        std::to_string(data->size()) + " bytes");
        }
        m_buffers.push_back(data->first(static_cast<std::size_t>(*length)));
    }
    return {};
}

Expected<BufferView> Reader::bufferView(std::uint64_t index) const {
    Expected<const Value*> view = elementAt(m_bufferViews, index, "bufferViews");
    if (!view) {
        return makeUnexpected(std::move(view).error());
    }
    const std::string where = element("bufferViews", index);
    Expected<std::uint64_t> buffer = requiredInteger(**view, "buffer", where);
    Expected<std::optional<std::uint64_t>> offset = integerMember(**view, "byteOffset", where);
    Expected<std::uint64_t> length = requiredInteger(**view, "byteLength", where);
    Expected<std::optional<std::uint64_t>> stride = integerMember(**view, "byteStride", where);
    if (const Error* error = firstError(buffer, offset, length, stride)) {
        return makeUnexpected(*error);
    }
    if (*buffer >= m_buffers.size()) {
        return malformed(child(where, "buffer"), "there is no buffer " + std::to_string(*buffer));
    }
    if (*length == 0) {
        return malformed(child(where, "byteLength"), "is 0");
    }
    const std::span<const std::byte> data = m_buffers[static_cast<std::size_t>(*buffer)];
    const std::uint64_t start = offset->value_or(0);
    const std::optional<std::uint64_t> end = checkedAdd(start, *length);
    if (!end || *end > data.size()) {
        return malformed(where, "byteOffset + byteLength is past the end of buffer " + std::to_string(*buffer));
    }
    if (stride->has_value() &&
        (**stride < kMinByteStride || **stride > kMaxByteStride || **stride % kMinByteStride != 0)) {
        return malformed(child(where, "byteStride"), "must be a multiple of 4 from 4 to 252");
    }
    return BufferView{.bytes = data.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(*length)),
                      .offset = start,
                      .stride = *stride};
}

Expected<Accessor> Reader::accessor(std::uint64_t index, std::uint64_t components, std::string_view where) const {
    Expected<const Value*> object = elementAt(m_accessors, index, "accessors");
    if (!object) {
        return makeUnexpected(std::move(object).error());
    }
    Accessor result{.object = *object, .where = element("accessors", index)};
    Expected<std::uint64_t> componentType = requiredInteger(**object, "componentType", result.where);
    Expected<std::uint64_t> count = requiredInteger(**object, "count", result.where);
    Expected<std::optional<std::uint64_t>> offset = integerMember(**object, "byteOffset", result.where);
    Expected<std::optional<std::uint64_t>> view = integerMember(**object, "bufferView", result.where);
    Expected<bool> normalized = boolMember(**object, "normalized", result.where, false);
    Expected<std::optional<std::string_view>> type = stringMember(**object, "type", result.where);
    if (const Error* error = firstError(componentType, count, offset, view, normalized, type)) {
        return makeUnexpected(*error);
    }
    result.componentType = *componentType;
    result.componentSize = componentSize(*componentType);
    if (result.componentSize == 0) {
        return malformed(child(result.where, "componentType"), "isn't a glTF component type");
    }
    if (!type->has_value() || componentCount(**type) == 0) {
        return malformed(child(result.where, "type"), "is missing or not an accessor type");
    }
    if (componentCount(**type) != components) {
        return malformed(where, "accessor " + std::to_string(index) + " is " + inQuotes(**type) + "; " +
                                    std::string(typeName(components)) + " is expected");
    }
    result.normalized = *normalized;
    if (result.normalized && (result.componentType == kFloat || result.componentType == kUnsignedInt)) {
        return malformed(child(result.where, "normalized"), "floats and unsigned ints can't be normalized");
    }
    result.count = *count;
    if (result.count == 0) {
        return malformed(child(result.where, "count"), "is 0");
    }
    result.elementSize = result.componentSize * components;
    const std::uint64_t start = offset->value_or(0);
    if (!view->has_value()) {
        if (start != 0) {
            return malformed(child(result.where, "byteOffset"), "is set without a bufferView");
        }
        return result;
    }
    Expected<BufferView> bytes = bufferView(**view);
    if (!bytes) {
        return makeUnexpected(std::move(bytes).error());
    }
    if ((bytes->offset + start) % result.componentSize != 0) {
        return malformed(child(result.where, "byteOffset"), "its data isn't aligned to its component size");
    }
    result.stride = bytes->stride.value_or(result.elementSize);
    if (result.stride < result.elementSize) {
        return malformed(result.where, "its buffer view's byteStride is smaller than an element");
    }
    // The last element ends at byteOffset + stride * (count - 1) + elementSize.
    const std::optional<std::uint64_t> lastStart = checkedMultiply(result.stride, result.count - 1);
    const std::optional<std::uint64_t> afterStart = lastStart ? checkedAdd(*lastStart, start) : std::nullopt;
    const std::optional<std::uint64_t> end = afterStart ? checkedAdd(*afterStart, result.elementSize) : std::nullopt;
    if (!end || *end > bytes->bytes.size()) {
        return malformed(result.where, "its elements reach past the end of buffer view " + std::to_string(**view));
    }
    result.data = bytes->bytes.subspan(static_cast<std::size_t>(start));
    return result;
}

// The "indices" or "values" part of a sparse accessor with `count` entries, checked.
Expected<SparsePart> Reader::sparsePart(const Accessor& accessor, const Value& sparse, std::string_view key,
                                        std::uint64_t count) const {
    const std::string sparseWhere = child(accessor.where, "sparse");
    const std::string where = child(sparseWhere, key);
    Expected<const Value*> object = requiredObject(sparse, key, sparseWhere);
    if (!object) {
        return makeUnexpected(std::move(object).error());
    }
    Expected<std::uint64_t> viewIndex = requiredInteger(**object, "bufferView", where);
    Expected<std::optional<std::uint64_t>> offset = integerMember(**object, "byteOffset", where);
    if (const Error* error = firstError(viewIndex, offset)) {
        return makeUnexpected(*error);
    }
    SparsePart part{.componentType = accessor.componentType, .componentSize = accessor.componentSize};
    std::uint64_t elementSize = accessor.elementSize;
    if (key == "indices") {
        Expected<std::uint64_t> type = requiredInteger(**object, "componentType", where);
        if (!type) {
            return makeUnexpected(std::move(type).error());
        }
        if (*type != kUnsignedByte && *type != kUnsignedShort && *type != kUnsignedInt) {
            return malformed(child(where, "componentType"), "must be an unsigned byte, short or int");
        }
        part.componentType = *type;
        part.componentSize = componentSize(*type);
        elementSize = part.componentSize;
    }
    Expected<BufferView> view = bufferView(*viewIndex);
    if (!view) {
        return makeUnexpected(std::move(view).error());
    }
    const std::uint64_t start = offset->value_or(0);
    const std::optional<std::uint64_t> size = checkedMultiply(count, elementSize);
    const std::optional<std::uint64_t> end = size ? checkedAdd(start, *size) : std::nullopt;
    if (!end || *end > view->bytes.size()) {
        return malformed(where, "its data reaches past the end of buffer view " + std::to_string(*viewIndex));
    }
    if ((view->offset + start) % part.componentSize != 0) {
        return malformed(where, "its data isn't aligned to its component size");
    }
    part.bytes = view->bytes.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(*size));
    return part;
}

// Writes the sparse elements over `packed`, the accessor's dense elements.
Expected<void> Reader::applySparse(const Accessor& accessor, const Value& sparse, std::span<std::byte> packed) const {
    const std::string where = child(accessor.where, "sparse");
    Expected<std::uint64_t> count = requiredInteger(sparse, "count", where);
    if (!count) {
        return makeUnexpected(std::move(count).error());
    }
    if (*count == 0 || *count > accessor.count) {
        return malformed(child(where, "count"), "must be from 1 to the accessor's count");
    }
    Expected<SparsePart> indices = sparsePart(accessor, sparse, "indices", *count);
    Expected<SparsePart> values = sparsePart(accessor, sparse, "values", *count);
    if (const Error* error = firstError(indices, values)) {
        return makeUnexpected(*error);
    }
    const auto elementSize = static_cast<std::size_t>(accessor.elementSize);
    const auto indexSize = static_cast<std::size_t>(indices->componentSize);
    std::uint64_t previous = 0;
    for (std::size_t index = 0; index < *count; ++index) {
        const std::uint64_t target = loadUnsigned(indices->bytes, index * indexSize, indexSize);
        if (target >= accessor.count || (index > 0 && target <= previous)) {
            return malformed(child(where, "indices"), "the indices must increase and be less than the count");
        }
        previous = target;
        std::memcpy(packed.data() + static_cast<std::size_t>(target) * elementSize,
                    values->bytes.data() + index * elementSize, elementSize);
    }
    return {};
}

// The accessor's elements, tightly packed, with its sparse elements applied.
Expected<std::vector<std::byte>> Reader::unpack(const Accessor& accessor) {
    const std::optional<std::uint64_t> size = checkedMultiply(accessor.count, accessor.elementSize);
    if (!size) {
        return failure("import.too_large", accessor.where, "the accessor is too large");
    }
    if (Expected<void> taken = m_budget.take(*size, accessor.where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    std::vector<std::byte> packed(static_cast<std::size_t>(*size));
    const auto elementSize = static_cast<std::size_t>(accessor.elementSize);
    if (!accessor.data.empty()) {
        const auto stride = static_cast<std::size_t>(accessor.stride);
        for (std::size_t index = 0; index < accessor.count; ++index) {
            std::memcpy(packed.data() + index * elementSize, accessor.data.data() + index * stride, elementSize);
        }
    }
    Expected<const Value*> sparse = objectMember(*accessor.object, "sparse", accessor.where);
    if (!sparse) {
        return makeUnexpected(std::move(sparse).error());
    }
    if (*sparse != nullptr) {
        if (Expected<void> applied = applySparse(accessor, **sparse, packed); !applied) {
            return makeUnexpected(std::move(applied).error());
        }
    }
    return packed;
}

float componentToFloat(std::span<const std::byte> bytes, std::size_t offset, std::uint64_t componentType,
                       bool normalized) {
    switch (componentType) {
        case kFloat:
            return std::bit_cast<float>(loadU32(bytes, offset));
        case kUnsignedByte: {
            const auto value = static_cast<float>(loadUnsigned(bytes, offset, 1));
            return normalized ? value / 255.0F : value;
        }
        case kUnsignedShort: {
            const auto value = static_cast<float>(loadUnsigned(bytes, offset, 2));
            return normalized ? value / 65535.0F : value;
        }
        case kByte: {
            const auto value = static_cast<float>(static_cast<std::int8_t>(loadUnsigned(bytes, offset, 1)));
            return normalized ? std::max(value / 127.0F, -1.0F) : value;
        }
        case kShort: {
            const auto value = static_cast<float>(static_cast<std::int16_t>(loadUnsigned(bytes, offset, 2)));
            return normalized ? std::max(value / 32767.0F, -1.0F) : value;
        }
        default:
            return static_cast<float>(loadU32(bytes, offset));
    }
}

Expected<std::vector<float>> Reader::decodeFloats(std::uint64_t index, std::uint64_t components, bool allowNormalized,
                                                  std::string_view where) {
    Expected<Accessor> view = accessor(index, components, where);
    if (!view) {
        return makeUnexpected(std::move(view).error());
    }
    const bool normalizedInteger =
        view->normalized && (view->componentType == kUnsignedByte || view->componentType == kUnsignedShort);
    if (view->componentType != kFloat && !(allowNormalized && normalizedInteger)) {
        return unsupported(where, allowNormalized ? "must be floats or normalized unsigned bytes or shorts"
                                                  : "must be floats");
    }
    Expected<std::vector<std::byte>> packed = unpack(*view);
    if (!packed) {
        return makeUnexpected(std::move(packed).error());
    }
    const std::uint64_t valueCount = view->count * components;
    if (Expected<void> taken = m_budget.take(valueCount * sizeof(float), where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    std::vector<float> values(static_cast<std::size_t>(valueCount));
    const auto size = static_cast<std::size_t>(view->componentSize);
    for (std::size_t value = 0; value < values.size(); ++value) {
        values[value] = componentToFloat(*packed, value * size, view->componentType, view->normalized);
        if (!std::isfinite(values[value])) {
            return malformed(where, "accessor " + std::to_string(index) + " has a value that isn't finite");
        }
    }
    m_budget.give(packed->size());
    return values;
}

Expected<std::vector<float>> Reader::readFloats(std::uint64_t index, std::uint64_t components, bool allowNormalized,
                                                std::string_view where) {
    const auto key = std::make_tuple(index, components, allowNormalized);
    auto cached = m_floatAccessors.find(key);
    if (cached == m_floatAccessors.end()) {
        Expected<std::vector<float>> decoded = decodeFloats(index, components, allowNormalized, where);
        if (!decoded) {
            return makeUnexpected(std::move(decoded).error());
        }
        cached = m_floatAccessors.emplace(key, std::move(*decoded)).first;
    }
    // Every primitive gets its own copy, which counts against the budget like the decoded data.
    if (Expected<void> taken = m_budget.take(cached->second.size() * sizeof(float), where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    return cached->second;
}

Expected<std::vector<std::uint32_t>> Reader::decodeIndices(std::uint64_t index, std::string_view where) {
    Expected<Accessor> view = accessor(index, 1, where);
    if (!view) {
        return makeUnexpected(std::move(view).error());
    }
    if ((view->componentType != kUnsignedByte && view->componentType != kUnsignedShort &&
         view->componentType != kUnsignedInt) ||
        view->normalized) {
        return malformed(where, "indices must be unsigned bytes, shorts or ints, not normalized");
    }
    if (view->count % 3 != 0) {
        return malformed(where, "the index count of a triangle list must be a multiple of 3");
    }
    Expected<std::vector<std::byte>> packed = unpack(*view);
    if (!packed) {
        return makeUnexpected(std::move(packed).error());
    }
    if (Expected<void> taken = m_budget.take(view->count * sizeof(std::uint32_t), where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    std::vector<std::uint32_t> indices(static_cast<std::size_t>(view->count));
    const auto size = static_cast<std::size_t>(view->componentSize);
    for (std::size_t element = 0; element < indices.size(); ++element) {
        indices[element] = loadUnsigned(*packed, element * size, size);
    }
    m_budget.give(packed->size());
    return indices;
}

Expected<std::vector<std::uint32_t>> Reader::readIndices(std::uint64_t index, std::uint64_t vertexCount,
                                                         std::string_view where) {
    auto cached = m_indexAccessors.find(index);
    if (cached == m_indexAccessors.end()) {
        Expected<std::vector<std::uint32_t>> decoded = decodeIndices(index, where);
        if (!decoded) {
            return makeUnexpected(std::move(decoded).error());
        }
        cached = m_indexAccessors.emplace(index, std::move(*decoded)).first;
    }
    // The copy is charged before the indices are checked, so the checks of shared accessors are
    // bounded by the budget too.
    if (Expected<void> taken = m_budget.take(cached->second.size() * sizeof(std::uint32_t), where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    for (const std::uint32_t vertex : cached->second) {
        if (vertex >= vertexCount) {
            return malformed(where, "index " + std::to_string(vertex) + " is not less than the vertex count " +
                                        std::to_string(vertexCount));
        }
    }
    return cached->second;
}

// Reads POSITION and, when present, NORMAL and TEXCOORD_0 into `primitive`.
Expected<void> Reader::readVertices(const Value& attributes, std::string_view where, ImportedPrimitive& primitive) {
    struct Attribute {
        std::string_view name;
        std::uint64_t components;
        bool allowNormalized;
        std::vector<float>* target;
    };

    const std::array<Attribute, 3> attributeList = {{{"POSITION", 3, false, &primitive.positions},
                                                     {"NORMAL", 3, false, &primitive.normals},
                                                     {"TEXCOORD_0", 2, true, &primitive.texCoords}}};
    for (const Attribute& attribute : attributeList) {
        Expected<std::optional<std::uint64_t>> index = integerMember(attributes, attribute.name, where);
        if (!index) {
            return makeUnexpected(std::move(index).error());
        }
        if (!index->has_value()) {
            if (attribute.target == &primitive.positions) {
                return malformed(where, "has no POSITION");
            }
            continue;
        }
        const std::string attributeWhere = child(where, attribute.name);
        Expected<std::vector<float>> values =
            readFloats(**index, attribute.components, attribute.allowNormalized, attributeWhere);
        if (!values) {
            return makeUnexpected(std::move(values).error());
        }
        if (attribute.target != &primitive.positions &&
            values->size() / attribute.components != primitive.positions.size() / 3) {
            return malformed(attributeWhere, "its count differs from POSITION's");
        }
        *attribute.target = std::move(*values);
    }
    return {};
}

Expected<ImportedPrimitive> Reader::primitive(const Value& object, std::string_view where,
                                              const std::vector<std::string>& materialKeys) {
    Expected<std::optional<std::uint64_t>> mode = integerMember(object, "mode", where);
    Expected<const Value*> attributes = requiredObject(object, "attributes", where);
    Expected<std::optional<std::uint64_t>> indices = integerMember(object, "indices", where);
    Expected<std::optional<std::uint64_t>> material = integerMember(object, "material", where);
    if (const Error* error = firstError(mode, attributes, indices, material)) {
        return makeUnexpected(*error);
    }
    if (const std::uint64_t primitiveMode = mode->value_or(kModeTriangles); primitiveMode != kModeTriangles) {
        return unsupported(child(where, "mode"),
                           "only triangle lists (mode 4) are imported, not mode " + std::to_string(primitiveMode));
    }
    ImportedPrimitive result;
    if (Expected<void> read = readVertices(**attributes, child(where, "attributes"), result); !read) {
        return makeUnexpected(std::move(read).error());
    }
    const std::uint64_t vertexCount = result.positions.size() / 3;
    if (indices->has_value()) {
        Expected<std::vector<std::uint32_t>> read = readIndices(**indices, vertexCount, child(where, "indices"));
        if (!read) {
            return makeUnexpected(std::move(read).error());
        }
        result.indices = std::move(*read);
    } else {
        if (vertexCount % 3 != 0 || vertexCount > std::numeric_limits<std::uint32_t>::max()) {
            return malformed(where, "without indices, the vertex count of a triangle list must be a multiple of 3");
        }
        if (Expected<void> taken = m_budget.take(vertexCount * sizeof(std::uint32_t), where); !taken) {
            return makeUnexpected(std::move(taken).error());
        }
        result.indices.resize(static_cast<std::size_t>(vertexCount));
        for (std::size_t index = 0; index < result.indices.size(); ++index) {
            result.indices[index] = static_cast<std::uint32_t>(index);
        }
    }
    if (material->has_value()) {
        if (**material >= materialKeys.size()) {
            return malformed(child(where, "material"), "there is no material " + std::to_string(**material));
        }
        // Each primitive holds its own copy of the key, which counts against the budget.
        const std::string& key = materialKeys[static_cast<std::size_t>(**material)];
        if (Expected<void> taken = m_budget.take(key.size(), where); !taken) {
            return makeUnexpected(std::move(taken).error());
        }
        result.material = key;
    }
    return result;
}

Expected<ImportedMesh> Reader::mesh(std::size_t index, const std::vector<std::string>& materialKeys) {
    const std::string where = element("meshes", index);
    Expected<const Value*> object = elementAt(m_meshes, index, "meshes");
    if (!object) {
        return makeUnexpected(std::move(object).error());
    }
    Expected<std::span<const Value>> primitives = arrayMember(**object, "primitives", where);
    if (!primitives) {
        return makeUnexpected(std::move(primitives).error());
    }
    if (primitives->empty()) {
        return malformed(where, "has no primitives");
    }
    ImportedMesh result;
    for (std::size_t primitiveIndex = 0; primitiveIndex < primitives->size(); ++primitiveIndex) {
        const std::string primitiveWhere = element(child(where, "primitives"), primitiveIndex);
        const Value& primitiveObject = (*primitives)[primitiveIndex];
        if (primitiveObject.kind() != Value::Kind::Object) {
            return malformed(primitiveWhere, "isn't an object");
        }
        Expected<ImportedPrimitive> read = primitive(primitiveObject, primitiveWhere, materialKeys);
        if (!read) {
            return makeUnexpected(std::move(read).error());
        }
        result.primitives.push_back(std::move(*read));
    }
    return result;
}

// A copy of the "name" of `object`, counted against the budget.
Expected<std::string> Reader::copyName(const Value& object, std::string_view where) {
    Expected<std::string_view> name = nameMember(object, where);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    if (Expected<void> taken = m_budget.take(name->size(), where); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    return std::string(*name);
}

// The texture slot `key` of `object`, or nullopt when it has none. Its scale or strength
// (`factorKey`) goes to `factor` when that is set; its image file goes to `references`.
Expected<std::optional<ImportedTexture>> Reader::texture(const Value& object, std::string_view key,
                                                         std::string_view where, float* factor,
                                                         std::string_view factorKey,
                                                         std::set<std::string>& references) {
    Expected<const Value*> info = objectMember(object, key, where);
    if (!info) {
        return makeUnexpected(std::move(info).error());
    }
    if (*info == nullptr) {
        return std::optional<ImportedTexture>();
    }
    const std::string infoWhere = child(where, key);
    Expected<std::uint64_t> textureIndex = requiredInteger(**info, "index", infoWhere);
    Expected<std::optional<std::uint64_t>> texCoord = integerMember(**info, "texCoord", infoWhere);
    Expected<float> factorValue =
        factor != nullptr ? floatMember(**info, factorKey, infoWhere, 1.0F) : Expected<float>(1.0F);
    if (const Error* error = firstError(textureIndex, texCoord, factorValue)) {
        return makeUnexpected(*error);
    }
    if (texCoord->value_or(0) > std::numeric_limits<std::uint32_t>::max()) {
        return malformed(child(infoWhere, "texCoord"), "is too large");
    }
    if (factor != nullptr) {
        *factor = *factorValue;
    }
    ImportedTexture result{.texCoord = static_cast<std::uint32_t>(texCoord->value_or(0))};
    Expected<const Value*> textureObject = elementAt(m_textures, *textureIndex, "textures");
    if (!textureObject) {
        return makeUnexpected(std::move(textureObject).error());
    }
    Expected<std::optional<std::uint64_t>> source =
        integerMember(**textureObject, "source", element("textures", *textureIndex));
    if (!source) {
        return makeUnexpected(std::move(source).error());
    }
    if (!source->has_value()) {
        return std::optional<ImportedTexture>(std::move(result));
    }
    Expected<const Value*> image = elementAt(m_images, **source, "images");
    if (!image) {
        return makeUnexpected(std::move(image).error());
    }
    const std::string imageWhere = element("images", **source);
    Expected<std::optional<std::string_view>> uri = stringMember(**image, "uri", imageWhere);
    if (!uri) {
        return makeUnexpected(std::move(uri).error());
    }
    if (uri->has_value() && !isDataUri(**uri)) {
        Expected<std::string> path = uriToPath(**uri, child(imageWhere, "uri"));
        if (!path) {
            return makeUnexpected(std::move(path).error());
        }
        // Many slots can use one image: every copy of its path counts, the reference only once.
        const bool newReference = !references.contains(*path);
        if (Expected<void> taken = m_budget.take(path->size() * (newReference ? 2 : 1), imageWhere); !taken) {
            return makeUnexpected(std::move(taken).error());
        }
        if (newReference) {
            references.insert(*path);
        }
        result.image = std::move(*path);
    }
    return std::optional<ImportedTexture>(std::move(result));
}

Expected<std::string> Reader::materialName(std::size_t index) {
    Expected<const Value*> object = elementAt(m_materials, index, "materials");
    if (!object) {
        return makeUnexpected(std::move(object).error());
    }
    return copyName(**object, element("materials", index));
}

Expected<ImportedMaterial> Reader::material(std::size_t index, std::set<std::string>& references) {
    const std::string where = element("materials", index);
    Expected<const Value*> object = elementAt(m_materials, index, "materials");
    if (!object) {
        return makeUnexpected(std::move(object).error());
    }
    const Value& material = **object;
    ImportedMaterial result;
    Expected<const Value*> pbr = objectMember(material, "pbrMetallicRoughness", where);
    if (!pbr) {
        return makeUnexpected(std::move(pbr).error());
    }
    const std::string pbrWhere = child(where, "pbrMetallicRoughness");
    if (*pbr != nullptr) {
        const Expected<void> baseColor = floatArrayMember(**pbr, "baseColorFactor", pbrWhere, result.baseColorFactor);
        Expected<float> metallic = floatMember(**pbr, "metallicFactor", pbrWhere, 1.0F);
        Expected<float> roughness = floatMember(**pbr, "roughnessFactor", pbrWhere, 1.0F);
        if (const Error* error = firstError(baseColor, metallic, roughness)) {
            return makeUnexpected(*error);
        }
        result.metallicFactor = *metallic;
        result.roughnessFactor = *roughness;
    }

    // The texture slots: where each lives, and its scale or strength factor if it has one.
    struct Slot {
        const Value* object;
        std::string_view key;
        std::string_view where;
        std::optional<ImportedTexture>* target;
        float* factor;
        std::string_view factorKey;
    };

    const std::array<Slot, 5> slots = {
        {{*pbr, "baseColorTexture", pbrWhere, &result.baseColorTexture, nullptr, {}},
         {*pbr, "metallicRoughnessTexture", pbrWhere, &result.metallicRoughnessTexture, nullptr, {}},
         {&material, "normalTexture", where, &result.normalTexture, &result.normalScale, "scale"},
         {&material, "occlusionTexture", where, &result.occlusionTexture, &result.occlusionStrength, "strength"},
         {&material, "emissiveTexture", where, &result.emissiveTexture, nullptr, {}}}};
    for (const Slot& slot : slots) {
        if (slot.object == nullptr) {
            continue;
        }
        Expected<std::optional<ImportedTexture>> read =
            texture(*slot.object, slot.key, slot.where, slot.factor, slot.factorKey, references);
        if (!read) {
            return makeUnexpected(std::move(read).error());
        }
        *slot.target = std::move(*read);
    }
    const Expected<void> emissive = floatArrayMember(material, "emissiveFactor", where, result.emissiveFactor);
    Expected<std::optional<std::string_view>> alphaMode = stringMember(material, "alphaMode", where);
    Expected<float> alphaCutoff = floatMember(material, "alphaCutoff", where, 0.5F);
    Expected<bool> doubleSided = boolMember(material, "doubleSided", where, false);
    if (const Error* error = firstError(emissive, alphaMode, alphaCutoff, doubleSided)) {
        return makeUnexpected(*error);
    }
    const std::string_view mode = alphaMode->value_or("OPAQUE");
    if (mode == "MASK") {
        result.alphaMode = ImportedAlphaMode::Mask;
    } else if (mode == "BLEND") {
        result.alphaMode = ImportedAlphaMode::Blend;
    } else if (mode != "OPAQUE") {
        return malformed(child(where, "alphaMode"), "must be OPAQUE, MASK or BLEND");
    }
    result.alphaCutoff = *alphaCutoff;
    result.doubleSided = *doubleSided;
    return result;
}

// The parent of every node (kNoNode for roots), after checking that the children form trees and
// that every name is a string of at most kGltfMaxNameSize bytes (kept in m_nodeNames).
Expected<std::vector<std::uint64_t>> Reader::nodeParents() {
    std::vector<std::uint64_t> parents(m_nodes.size(), kNoNode);
    m_nodeNames.assign(m_nodes.size(), std::string_view());
    m_nodePaths.assign(m_nodes.size(), std::nullopt);
    for (std::size_t index = 0; index < m_nodes.size(); ++index) {
        Expected<const Value*> node = elementAt(m_nodes, index, "nodes");
        if (!node) {
            return makeUnexpected(std::move(node).error());
        }
        const std::string where = element("nodes", index);
        Expected<std::span<const Value>> children = arrayMember(**node, "children", where);
        Expected<std::string_view> name = nameMember(**node, where);
        if (const Error* error = firstError(children, name)) {
            return makeUnexpected(*error);
        }
        m_nodeNames[index] = *name;
        for (const Value& childValue : *children) {
            const std::optional<std::uint64_t> childIndex = childValue.toNumber<std::uint64_t>();
            if (!childIndex || *childIndex >= m_nodes.size() || *childIndex == index) {
                return malformed(child(where, "children"), "must hold indices of other nodes");
            }
            if (parents[static_cast<std::size_t>(*childIndex)] != kNoNode) {
                return malformed(element("nodes", *childIndex), "has more than one parent");
            }
            parents[static_cast<std::size_t>(*childIndex)] = index;
        }
    }
    // With one parent per node, a node that can't reach a root is on a cycle. Each node is walked
    // once: 1 marks a walk in progress, 2 a node known to reach a root.
    std::vector<std::uint8_t> state(m_nodes.size(), 0);
    std::vector<std::size_t> walk;
    for (std::size_t start = 0; start < m_nodes.size(); ++start) {
        std::uint64_t node = start;
        walk.clear();
        while (node != kNoNode && state[static_cast<std::size_t>(node)] == 0) {
            state[static_cast<std::size_t>(node)] = 1;
            walk.push_back(static_cast<std::size_t>(node));
            node = parents[static_cast<std::size_t>(node)];
        }
        if (node != kNoNode && state[static_cast<std::size_t>(node)] == 1) {
            return malformed(element("nodes", node), "its children form a cycle");
        }
        for (const std::size_t visited : walk) {
            state[visited] = 2;
        }
    }
    return parents;
}

// The name path of a node: the names of its named ancestors and its own, joined by '/'. Paths are
// remembered per node, so each node's path is built once; every path counts against the budget.
Expected<std::string> Reader::nodePath(std::uint64_t node, const std::vector<std::uint64_t>& parents) {
    std::vector<std::size_t> chain;
    std::uint64_t current = node;
    while (current != kNoNode && !m_nodePaths[static_cast<std::size_t>(current)].has_value()) {
        chain.push_back(static_cast<std::size_t>(current));
        current = parents[static_cast<std::size_t>(current)];
    }
    std::string path = current == kNoNode ? std::string() : m_nodePaths[static_cast<std::size_t>(current)].value_or("");
    std::ranges::reverse(chain);
    for (const std::size_t link : chain) {
        const std::string_view name = m_nodeNames[link];
        if (!name.empty()) {
            if (!path.empty()) {
                path += '/';
            }
            path += name;
        }
        const std::string where = element("nodes", link);
        if (path.size() > kGltfMaxNameSize) {
            return failure("import.too_large", where,
                           "its name path is longer than " + std::to_string(kGltfMaxNameSize) + " bytes");
        }
        if (Expected<void> taken = m_budget.take(path.size(), where); !taken) {
            return makeUnexpected(std::move(taken).error());
        }
        m_nodePaths[link] = path;
    }
    // The returned copy counts too.
    if (Expected<void> taken = m_budget.take(path.size(), element("nodes", node)); !taken) {
        return makeUnexpected(std::move(taken).error());
    }
    return path;
}

// The name of every mesh: its own, or the name path of the first node that uses it.
Expected<std::vector<std::string>> Reader::meshNames() {
    std::vector<std::string> names(m_meshes.size());
    // A mesh is named once: by itself, or by the first node that uses it, even when that node's
    // path is empty, so later nodes never walk the tree for it again.
    std::vector<bool> named(m_meshes.size(), false);
    for (std::size_t index = 0; index < m_meshes.size(); ++index) {
        Expected<const Value*> object = elementAt(m_meshes, index, "meshes");
        if (!object) {
            return makeUnexpected(std::move(object).error());
        }
        Expected<std::string> name = copyName(**object, element("meshes", index));
        if (!name) {
            return makeUnexpected(std::move(name).error());
        }
        named[index] = !name->empty();
        names[index] = std::move(*name);
    }
    Expected<std::vector<std::uint64_t>> parents = nodeParents();
    if (!parents) {
        return makeUnexpected(std::move(parents).error());
    }
    for (std::size_t node = 0; node < m_nodes.size(); ++node) {
        const std::string where = element("nodes", node);
        Expected<std::optional<std::uint64_t>> mesh = integerMember(m_nodes[node], "mesh", where);
        if (!mesh) {
            return makeUnexpected(std::move(mesh).error());
        }
        if (!mesh->has_value()) {
            continue;
        }
        if (**mesh >= m_meshes.size()) {
            return malformed(child(where, "mesh"), "there is no mesh " + std::to_string(**mesh));
        }
        const auto meshIndex = static_cast<std::size_t>(**mesh);
        if (named[meshIndex]) {
            continue;
        }
        Expected<std::string> path = nodePath(node, *parents);
        if (!path) {
            return makeUnexpected(std::move(path).error());
        }
        names[meshIndex] = std::move(*path);
        named[meshIndex] = true;
    }
    return names;
}

// The payload of a reflected value; "import.too_large" when it can't be written.
template <typename T>
Expected<std::vector<std::byte>> payloadOf(const serialization::DocumentFormat& format, const T& value) {
    Expected<std::vector<std::byte>, serialization::Error> written = serialization::writeBinary(format, value);
    if (!written) {
        return makeError("import.too_large", written.error().toString());
    }
    return std::move(*written);
}

// The decode budget of an import: the larger of the minimum and the factor times its input, at
// most the ceiling.
std::uint64_t decodeLimit(std::span<const std::byte> source, std::span<const ImportFile> files) {
    std::uint64_t inputSize = source.size();
    for (const ImportFile& file : files) {
        inputSize += file.bytes.size();
    }
    const std::optional<std::uint64_t> scaled = checkedMultiply(inputSize, kGltfDecodeBudgetFactor);
    return std::min(kGltfMaxDecodeBudget, std::max(kGltfMinDecodeBudget, scaled.value_or(kGltfMaxDecodeBudget)));
}

// The materials of the document as objects.
Expected<std::vector<ImportedObject>> readMaterials(Reader& reader, std::set<std::string>& references) {
    std::vector<ImportedObject> materials;
    materials.reserve(reader.materialCount());
    for (std::size_t index = 0; index < reader.materialCount(); ++index) {
        Expected<std::string> name = reader.materialName(index);
        if (!name) {
            return makeUnexpected(std::move(name).error());
        }
        Expected<ImportedMaterial> material = reader.material(index, references);
        if (!material) {
            return makeUnexpected(std::move(material).error());
        }
        Expected<std::vector<std::byte>> payload = payloadOf(kImportedMaterialFormat, *material);
        if (!payload) {
            return makeUnexpected(std::move(payload).error());
        }
        materials.push_back(
            ImportedObject{.type = "material", .name = std::move(*name), .payload = std::move(*payload)});
    }
    return materials;
}

} // namespace

std::string_view GltfImporter::id() const {
    return "gltf";
}

std::uint32_t GltfImporter::version() const {
    return kImporterVersion;
}

std::span<const std::string_view> GltfImporter::extensions() const {
    return kExtensions;
}

serialization::Value GltfImporter::defaultSettings() const {
    return Value::object();
}

Expected<std::vector<std::string>> GltfImporter::dependencies(std::span<const std::byte> source,
                                                              const serialization::Value& /*settings*/) const {
    Expected<Document> document = parseDocument(source);
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    DecodeBudget budget(0);
    Reader reader(document->root, budget);
    if (Expected<void> read = reader.readArrays(); !read) {
        return makeUnexpected(std::move(read).error());
    }
    return reader.externalBufferPaths();
}

Expected<ImportOutput> GltfImporter::import(std::span<const std::byte> source, std::span<const ImportFile> files,
                                            const serialization::Value& /*settings*/) const {
    Expected<Document> document = parseDocument(source);
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    DecodeBudget budget(decodeLimit(source, files));
    Reader reader(document->root, budget);
    if (Expected<void> read = reader.readArrays(); !read) {
        return makeUnexpected(std::move(read).error());
    }
    if (Expected<void> resolved = reader.resolveBuffers(document->bin, files); !resolved) {
        return makeUnexpected(std::move(resolved).error());
    }

    ImportOutput output;
    std::set<std::string> references;
    Expected<std::vector<ImportedObject>> materials = readMaterials(reader, references);
    if (!materials) {
        return makeUnexpected(std::move(materials).error());
    }
    // Keys are counted per type, so the materials' keys alone are their keys in the whole output.
    Expected<std::vector<std::string>> materialKeys = sourceKeys(*materials);
    Expected<std::vector<std::string>> meshNames = reader.meshNames();
    if (const Error* error = firstError(materialKeys, meshNames)) {
        return makeUnexpected(*error);
    }
    for (std::size_t index = 0; index < reader.meshCount(); ++index) {
        Expected<ImportedMesh> mesh = reader.mesh(index, *materialKeys);
        if (!mesh) {
            return makeUnexpected(std::move(mesh).error());
        }
        Expected<std::vector<std::byte>> payload = payloadOf(kImportedMeshFormat, *mesh);
        if (!payload) {
            return makeUnexpected(std::move(payload).error());
        }
        output.objects.push_back(
            ImportedObject{.type = "mesh", .name = std::move((*meshNames)[index]), .payload = std::move(*payload)});
    }
    for (ImportedObject& material : *materials) {
        output.objects.push_back(std::move(material));
    }
    output.references.assign(references.begin(), references.end());
    return output;
}

} // namespace ith
