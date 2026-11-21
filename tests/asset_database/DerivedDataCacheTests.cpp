// The derived-data cache (ADR-0015 rule 11): keys, entries and the 128-bit hash behind them.

#include "ith/asset_database/DerivedDataCache.h"

#include "ith/asset_database/Hash128.h"
#include "ith/asset_database/ImportedAssetData.h"

#include "AssetTestSupport.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using ith::test::TempProject;

ith::Hash128 keyOf(const ith::DerivedDataInputs& inputs) {
    const ith::Expected<ith::Hash128> key = ith::derivedDataKey(inputs);
    REQUIRE(key.hasValue());
    return key.valueOr(ith::Hash128{});
}

ith::ImportedAssetData sampleData() {
    ith::ImportedAssetData data;
    data.objects.push_back({.key = "mesh:Hull", .payload = {1, 2, 3}});
    data.objects.push_back({.key = "mesh:#0", .payload = {}});
    return data;
}

bool sameData(const ith::ImportedAssetData& a, const ith::ImportedAssetData& b) {
    if (a.objects.size() != b.objects.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.objects.size(); ++index) {
        if (a.objects[index].key != b.objects[index].key || a.objects[index].payload != b.objects[index].payload) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_SUITE("asset_database.hash128") {
    TEST_CASE("hashes are deterministic and tell inputs apart") {
        CHECK(ith::hash128("abc") == ith::hash128("abc"));
        CHECK(ith::hash128("abc") != ith::hash128("abd"));
        CHECK(ith::hash128("") != ith::hash128(std::string(1, '\0')));
        const std::array<std::byte, 3> bytes = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        CHECK(ith::hash128(bytes) == ith::hash128("abc"));

        std::set<ith::Hash128> seen;
        for (int index = 0; index < 1000; ++index) {
            seen.insert(ith::hash128(std::to_string(index)));
        }
        CHECK(seen.size() == 1000);
    }

    TEST_CASE("the hash is XXH3-128 with seed 0") {
        // Known answers: a changed algorithm or a misconfigured build fails here instead of
        // silently changing every cache key.
        CHECK(ith::hash128(std::string_view()).toString() == "99aa06d3014798d86001c324468d497f");
        CHECK(ith::hash128("Ironthorn").toString() == "09fd04e5ef7861bcfb5f55a4847fdce7");
    }

    TEST_CASE("a hash prints as 32 hexadecimal digits") {
        CHECK(ith::Hash128{.high = 0x0123456789abcdefull, .low = 0xfedcba9876543210ull}.toString() ==
              "0123456789abcdeffedcba9876543210");
        CHECK(ith::Hash128{}.toString() == std::string(32, '0'));
    }
}

TEST_SUITE("asset_database.ddc") {
    TEST_CASE("the key changes when any input changes") {
        serialization::Value settings = serialization::Value::object();
        settings.set("scale", serialization::Value::integer(1));
        const serialization::Value overrides = serialization::Value::object();
        const std::array<ith::Hash128, 1> dependencies = {ith::hash128("texture")};
        const ith::DerivedDataInputs base{.source = ith::hash128("source"),
                                          .dependencies = dependencies,
                                          .importerId = "gltf",
                                          .importerVersion = 1,
                                          .settings = &settings,
                                          .overrides = &overrides,
                                          .platform = "windows",
                                          .outputFormatVersion = 1,
                                          .outputSchemaHash = 7};
        const ith::Hash128 key = keyOf(base);
        CHECK(keyOf(base) == key);

        serialization::Value otherSettings = serialization::Value::object();
        otherSettings.set("scale", serialization::Value::integer(2));
        serialization::Value otherOverrides = serialization::Value::object();
        otherOverrides.set("x:Mesh.lod", serialization::Value::integer(1));
        const std::array<ith::Hash128, 1> otherDependencies = {ith::hash128("texture2")};

        const std::vector<std::function<void(ith::DerivedDataInputs&)>> changes = {
            [](ith::DerivedDataInputs& in) { in.source = ith::hash128("source2"); },
            [&](ith::DerivedDataInputs& in) { in.dependencies = otherDependencies; },
            [](ith::DerivedDataInputs& in) { in.dependencies = {}; },
            [](ith::DerivedDataInputs& in) { in.importerId = "gltf2"; },
            [](ith::DerivedDataInputs& in) { in.importerVersion = 2; },
            [&](ith::DerivedDataInputs& in) { in.settings = &otherSettings; },
            [&](ith::DerivedDataInputs& in) { in.overrides = &otherOverrides; },
            [](ith::DerivedDataInputs& in) { in.overrides = nullptr; },
            [](ith::DerivedDataInputs& in) { in.platform = "linux"; },
            [](ith::DerivedDataInputs& in) { in.outputFormatVersion = 2; },
            [](ith::DerivedDataInputs& in) { in.outputSchemaHash = 8; },
        };
        std::set<ith::Hash128> keys = {key};
        for (const auto& change : changes) {
            ith::DerivedDataInputs changed = base;
            change(changed);
            keys.insert(keyOf(changed));
        }
        CHECK(keys.size() == changes.size() + 1);

        // Length prefixes keep neighbouring text inputs apart.
        ith::DerivedDataInputs a = base;
        a.importerId = "ab";
        a.platform = "c";
        ith::DerivedDataInputs b = base;
        b.importerId = "a";
        b.platform = "bc";
        CHECK(keyOf(a) != keyOf(b));
    }

    TEST_CASE("entries round-trip, and missing or damaged entries are misses") {
        const TempProject project;
        const ith::DerivedDataCache cache(ith::DerivedDataCache::projectDirectory(project.directory()));
        CHECK(cache.directory() == project.directory() / "Intermediate" / "DerivedData");
        const ith::Hash128 key = ith::hash128("key");
        const std::string name = key.toString();
        CHECK(cache.entryPath(key) == cache.directory() / name.substr(0, 2) / (name + ".ddc"));

        ith::ImportedAssetData read;
        const ith::Expected<bool> miss = cache.get(key, ith::kImportedAssetFormat, read);
        REQUIRE(miss.hasValue());
        CHECK_FALSE(miss.valueOr(true));

        const ith::ImportedAssetData data = sampleData();
        REQUIRE(cache.put(key, ith::kImportedAssetFormat, data).hasValue());
        const ith::Expected<bool> hit = cache.get(key, ith::kImportedAssetFormat, read);
        REQUIRE(hit.hasValue());
        CHECK(hit.valueOr(false));
        CHECK(sameData(read, data));

        // Another format, or a damaged file, is a miss that leaves the target alone.
        constexpr ith::serialization::DocumentFormat kOther{.name = "test.other", .version = 1};
        ith::ImportedAssetData untouched = sampleData();
        untouched.objects.pop_back();
        CHECK_FALSE(cache.get(key, kOther, untouched).valueOr(true));
        CHECK(untouched.objects.size() == 1);

        const ith::Expected<std::string> blob =
            ith::detail::asset_files::readFile(cache.entryPath(key), ith::DerivedDataCache::kMaxEntrySize);
        REQUIRE(blob.hasValue());
        if (!blob) {
            return;
        }
        REQUIRE(ith::detail::asset_files::writeFileAtomically(cache.entryPath(key), blob->substr(0, blob->size() - 1))
                    .hasValue());
        CHECK_FALSE(cache.get(key, ith::kImportedAssetFormat, untouched).valueOr(true));
        CHECK(untouched.objects.size() == 1);

        // put() replaces a damaged entry.
        REQUIRE(cache.put(key, ith::kImportedAssetFormat, data).hasValue());
        CHECK(cache.get(key, ith::kImportedAssetFormat, read).valueOr(false));
    }

    TEST_CASE("put succeeds when an entry exists that can't be replaced") {
        // On Windows a file open for reading can't be renamed over; the existing entry, addressed
        // by the same inputs, is kept. Elsewhere the entry is simply replaced.
        const TempProject project;
        const ith::DerivedDataCache cache(ith::DerivedDataCache::projectDirectory(project.directory()));
        const ith::Hash128 key = ith::hash128("open");
        const ith::ImportedAssetData data = sampleData();
        REQUIRE(cache.put(key, ith::kImportedAssetFormat, data).hasValue());
        {
            std::ifstream held(cache.entryPath(key), std::ios::binary);
            REQUIRE(held.is_open());
            CHECK(cache.put(key, ith::kImportedAssetFormat, data).hasValue());
        }
        ith::ImportedAssetData read;
        CHECK(cache.get(key, ith::kImportedAssetFormat, read).valueOr(false));
        CHECK(sameData(read, data));
        // No temporary file is left behind.
        std::error_code error;
        int files = 0;
        for (std::filesystem::recursive_directory_iterator entry(cache.directory(), error);
             !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error)) {
            files += entry->is_regular_file(error) ? 1 : 0;
        }
        CHECK(files == 1);
    }
}
