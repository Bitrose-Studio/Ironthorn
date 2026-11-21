#include "ith/asset_database/DerivedDataCache.h"

#include "ith/asset_database/FileIo.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/Json.h"

#include <array>
#include <string>
#include <system_error>
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

DerivedDataCache::DerivedDataCache(std::filesystem::path directory) : m_directory(std::move(directory)) {}

std::filesystem::path DerivedDataCache::projectDirectory(const std::filesystem::path& project) {
    return project / "Intermediate" / "DerivedData";
}

std::filesystem::path DerivedDataCache::entryPath(const Hash128& key) const {
    const std::string name = key.toString();
    return m_directory / name.substr(0, 2) / (name + ".ddc");
}

Expected<bool> DerivedDataCache::get(const Hash128& key, const serialization::DocumentFormat& format,
                                     const TypeInfo& type, void* target) const {
    const std::filesystem::path path = entryPath(key);
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return detail::asset_files::ioError("can't check the cache entry", path, error);
        }
        return false;
    }
    Expected<std::string> bytes = detail::asset_files::readFile(path, kMaxEntrySize);
    if (!bytes) {
        if (bytes.error().code == "asset.too_large") {
            return false; // no entry this cache writes is that large
        }
        return makeUnexpected(std::move(bytes).error());
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
    const std::filesystem::path path = entryPath(key);
    Expected<void> written = detail::asset_files::writeFileAtomically(
        path, std::string_view(reinterpret_cast<const char*>(blob->data()), blob->size()));
    if (!written) {
        // On Windows the rename over an entry fails while another thread or process has it open
        // (get(), or a concurrent put() of the same key). The entry that is there is as good.
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            return {};
        }
    }
    return written;
}

} // namespace ith
