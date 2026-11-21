#pragma once

// The local derived-data cache, DDC (ADR-0015 rule 11).
//
// Derived data (imported assets now; cooked data and compiled shaders later) is stored under a
// key that hashes everything it was derived from, so a change to any input is a miss and
// unchanged inputs are a hit. Entries are binary archive blobs (ADR-0010), one file per key in a
// per-project directory that is never committed:
//
//   ith::DerivedDataCache cache(ith::DerivedDataCache::projectDirectory(project));
//   const ith::Expected<ith::Hash128> key = ith::derivedDataKey({.source = ith::hash128(bytes), ...});
//   Imported value;
//   ith::Expected<bool> hit = cache.get(*key, kImportedFormat, value);
//   if (hit && !*hit) {
//       value = importAgain();
//       static_cast<void>(cache.put(*key, kImportedFormat, value));
//   }
//
// The cache is thread-safe: it holds no state but its directory, writes are atomic (a temporary
// file renamed over the entry) and a damaged or outdated entry reads as a miss. A shared cache
// comes later behind the same interface.

#include "ith/asset_database/Hash128.h"
#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace ith {

// Everything a derived-data key covers.
struct DerivedDataInputs {
    // The hash of the source's content (hash128 of its bytes).
    Hash128 source;
    // The content hashes of every other file the result depends on, in a fixed order.
    std::span<const Hash128> dependencies;
    // The id and version of the importer or other tool that derives the data.
    std::string_view importerId;
    std::uint32_t importerVersion = 0;
    // The settings and the overrides (JSON values; null when there are none).
    const serialization::Value* settings = nullptr;
    const serialization::Value* overrides = nullptr;
    // The target platform, or "any" for platform-independent data such as importer output.
    std::string_view platform;
    // The output format: its version and the schema hash of its root type
    // (serialization::schemaHash), so a changed format or layout is a miss.
    std::uint32_t outputFormatVersion = 0;
    std::uint64_t outputSchemaHash = 0;
};

// The key of the derived data described by `inputs`: a hash over all of them, each part length-
// prefixed so that no two different inputs give the same bytes. Fails with the errors of
// serialization::writeJson when the settings or overrides can't be written.
[[nodiscard]] Expected<Hash128> derivedDataKey(const DerivedDataInputs& inputs);

class DerivedDataCache {
public:
    // The largest entry written or read, in bytes. A larger file in the cache is a miss.
    static constexpr std::size_t kMaxEntrySize = std::size_t{1} << 30;

    // A cache in `directory`, which is created when the first entry is written.
    explicit DerivedDataCache(std::filesystem::path directory);

    // The cache directory of the project in `project`: "Intermediate/DerivedData" inside
    // it. Projects keep "Intermediate/" out of version control.
    [[nodiscard]] static std::filesystem::path projectDirectory(const std::filesystem::path& project);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }

    // Reads the entry `key` into `target`, a value of `type`. Returns true on a hit, false on a
    // miss: no entry, or an entry that isn't a valid blob of `format` and `type` (damaged, or
    // written by another version), which the next put() replaces. On a miss `target` is unchanged.
    // Fails with "asset.io" when an existing entry can't be read.
    [[nodiscard]] Expected<bool> get(const Hash128& key, const serialization::DocumentFormat& format,
                                     const TypeInfo& type, void* target) const;

    // Writes the value of `type` at `object` as the entry `key`, replacing an existing one. When the
    // replacement fails but an entry exists (on Windows, another reader or writer has it open),
    // that entry is kept and the call succeeds: entries are addressed by their inputs, so it holds
    // the same data unless it is damaged, which get() reports as a miss. Fails with "asset.io", or
    // with "asset.cache_write" when the value can't be written as a blob of at most kMaxEntrySize.
    [[nodiscard]] Expected<void> put(const Hash128& key, const serialization::DocumentFormat& format,
                                     const TypeInfo& type, const void* object) const;

    // get() for a reflected type T.
    template <typename T>
    [[nodiscard]] Expected<bool> get(const Hash128& key, const serialization::DocumentFormat& format, T& target) const {
        return get(key, format, typeOf<T>(), &target);
    }

    // put() for a reflected type T.
    template <typename T>
    [[nodiscard]] Expected<void> put(const Hash128& key, const serialization::DocumentFormat& format,
                                     const T& value) const {
        return put(key, format, typeOf<T>(), &value);
    }

    // The file of the entry `key`: "<directory>/<first two digits>/<32 hex digits>.ddc".
    [[nodiscard]] std::filesystem::path entryPath(const Hash128& key) const;

private:
    std::filesystem::path m_directory;
};

} // namespace ith
