#include "ith/asset_database/DerivedDataCache.h"

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/Json.h"

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace ith {
namespace {

// Appends `value` as 8 little-endian bytes.
void appendNumber(std::string& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes += static_cast<char>((value >> (index * 8)) & 0xff);
    }
}

void appendText(std::string& bytes, std::string_view text) {
    appendNumber(bytes, text.size());
    bytes += text;
}

void appendHash(std::string& bytes, const Hash128& hash) {
    appendNumber(bytes, hash.high);
    appendNumber(bytes, hash.low);
}

// Appends a JSON value as its written text, or an empty text for none.
Expected<void> appendJson(std::string& bytes, const serialization::Value* value) {
    if (value == nullptr) {
        appendText(bytes, {});
        return {};
    }
    Expected<std::string, serialization::Error> text = serialization::writeJson(*value);
    if (!text) {
        return makeError(text.error().code, text.error().toString());
    }
    appendText(bytes, *text);
    return {};
}

} // namespace

Expected<Hash128> derivedDataKey(const DerivedDataInputs& inputs) {
    // A version of this layout, so that a change to it can't produce an old key.
    constexpr std::uint64_t kKeyLayout = 2;
    std::string bytes;
    appendNumber(bytes, kKeyLayout);
    appendHash(bytes, inputs.source);
    appendNumber(bytes, inputs.dependencies.size());
    for (const Hash128& dependency : inputs.dependencies) {
        appendHash(bytes, dependency);
    }
    appendText(bytes, inputs.importerId);
    appendNumber(bytes, inputs.importerVersion);
    if (Expected<void> settings = appendJson(bytes, inputs.settings); !settings) {
        return makeUnexpected(std::move(settings).error());
    }
    if (Expected<void> overrides = appendJson(bytes, inputs.overrides); !overrides) {
        return makeUnexpected(std::move(overrides).error());
    }
    appendText(bytes, inputs.platform);
    appendNumber(bytes, inputs.outputFormatVersion);
    appendNumber(bytes, inputs.outputSchemaHash);
    return hash128(bytes);
}

DerivedDataCache::DerivedDataCache(std::string_view directory) : m_directory(fs::normalizePath(directory)) {}

std::string DerivedDataCache::projectDirectory(std::string_view project) {
    return fs::joinPath(project, "Intermediate/DerivedData");
}

std::string DerivedDataCache::entryPath(const Hash128& key) const {
    const std::string name = key.toString();
    return fs::joinPath(m_directory, name.substr(0, 2) + "/" + name + ".ddc");
}

Expected<bool> DerivedDataCache::get(const Hash128& key, const serialization::DocumentFormat& format,
                                     const TypeInfo& type, void* target) const {
    const std::string path = entryPath(key);
    Expected<std::string> bytes = fs::readFile(path, kMaxEntrySize);
    if (!bytes) {
        const std::string& code = bytes.error().code;
        if (code == "fs.not_found" || code == "fs.too_large") {
            return false; // no entry, or none this cache writes: that large
        }
        return makeError("asset.io", std::move(bytes).error().message);
    }
    const std::span<const std::byte> blob = std::as_bytes(std::span<const char>(bytes->data(), bytes->size()));
    return serialization::readBinary(blob, format, type, target).hasValue();
}

Expected<void> DerivedDataCache::put(const Hash128& key, const serialization::DocumentFormat& format,
                                     const TypeInfo& type, const void* object) const {
    Expected<std::vector<std::byte>, serialization::Error> blob = serialization::writeBinary(format, type, object);
    if (!blob) {
        return makeError("asset.cache_write", blob.error().toString());
    }
    if (blob->size() > kMaxEntrySize) {
        return makeError("asset.cache_write", "the entry has " + std::to_string(blob->size()) +
                                                  " bytes; the limit is " + std::to_string(kMaxEntrySize));
    }
    const std::string path = entryPath(key);
    Expected<void> written =
        fs::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(blob->data()), blob->size()),
                            {.durability = fs::Durability::Atomic});
    if (written) {
        return {};
    }
    // On Windows the rename over an entry fails while another process has it open without delete
    // sharing. Entries are addressed by their inputs, so the entry that is there is as good.
    if (written.error().code == "fs.busy") {
        if (Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path); info && info->has_value()) {
            return {};
        }
    }
    return makeError("asset.io", std::move(written).error().message);
}

} // namespace ith
