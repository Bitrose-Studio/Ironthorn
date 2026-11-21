#include "ith/asset_database/Sidecar.h"

#include "ith/asset_database/AssetHeader.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace serialization = ith::serialization;

ith::Sidecar sampleSidecar() {
    ith::Sidecar sidecar;
    sidecar.id = ith::AssetId(ith::Uuid(0x0123456789abcdefull, 0x1122334455667788ull));
    sidecar.importer = "test.objects";
    sidecar.importerVersion = 3;
    sidecar.settings.set("scale", serialization::Value::integer(2));
    sidecar.subAssets = {
        {.key = "material:Steel", .id = ith::SubAssetId(ith::Uuid(1, 1))},
        {.key = "mesh:#0", .id = ith::SubAssetId(ith::Uuid(1, 2)), .missing = true},
        {.key = "mesh:Body/Hull", .id = ith::SubAssetId(ith::Uuid(1, 3))},
    };
    serialization::Value overrides = serialization::Value::object();
    overrides.set("00000000-0000-0001-0000-000000000003:Mesh.castShadows", serialization::Value::boolean(false));
    sidecar.overrides = overrides;
    return sidecar;
}

constexpr std::string_view kSampleText = R"({
  "format": "ith.asset_meta",
  "version": 1,
  "id": "01234567-89ab-cdef-1122-334455667788",
  "data": {
    "importer": "test.objects",
    "importerVersion": 3,
    "settings": {
      "scale": 2
    },
    "subAssets": [
      {
        "key": "material:Steel",
        "id": "00000000-0000-0001-0000-000000000001",
        "missing": false
      },
      {
        "key": "mesh:#0",
        "id": "00000000-0000-0001-0000-000000000002",
        "missing": true
      },
      {
        "key": "mesh:Body/Hull",
        "id": "00000000-0000-0001-0000-000000000003",
        "missing": false
      }
    ],
    "overrides": {
      "00000000-0000-0001-0000-000000000003:Mesh.castShadows": false
    }
  }
}
)";

// Reads `text` as a sidecar and returns the error code, or "" when it reads.
std::string readError(std::string_view text) {
    const auto read = ith::readSidecar(text, "test.meta");
    return read ? std::string() : read.error().code;
}

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string result(text);
    const std::size_t at = result.find(from);
    REQUIRE(at != std::string::npos);
    if (at != std::string::npos) {
        result.replace(at, from.size(), to);
    }
    return result;
}

} // namespace

TEST_SUITE("asset_database.sidecar") {
    TEST_CASE("a sidecar is written as a stable JSON document with the id in its header") {
        const auto text = ith::writeSidecar(sampleSidecar());
        REQUIRE(text.hasValue());
        if (text) {
            CHECK(*text == kSampleText);
        }
    }

    TEST_CASE("a sidecar round-trips") {
        const auto read = ith::readSidecar(kSampleText);
        REQUIRE(read.hasValue());
        if (!read) {
            return;
        }
        CHECK(*read == sampleSidecar());
        const auto written = ith::writeSidecar(*read);
        REQUIRE(written.hasValue());
        if (written) {
            CHECK(*written == kSampleText);
        }
    }

    TEST_CASE("an unsorted table is read sorted, but not written") {
        const std::string unsorted = replaced(replaced(kSampleText, "material:Steel", "zz:Steel"), "mesh:#0", "aa:#0");
        const auto read = ith::readSidecar(unsorted);
        REQUIRE(read.hasValue());
        if (!read) {
            return;
        }
        REQUIRE(read->subAssets.size() == 3);
        CHECK(read->subAssets.front().key == "aa:#0");
        CHECK(read->subAssets.back().key == "zz:Steel");

        ith::Sidecar sidecar = sampleSidecar();
        std::swap(sidecar.subAssets[0], sidecar.subAssets[2]);
        const auto written = ith::writeSidecar(sidecar);
        REQUIRE_FALSE(written.hasValue());
        if (!written) {
            CHECK(written.error().code == "asset.sidecar_invalid");
        }
    }

    TEST_CASE("unknown keys are dropped with warnings") {
        std::string text =
            replaced(kSampleText, R"("importer": "test.objects",)", R"("importer": "test.objects", "comment": "x",)");
        text = replaced(text, R"("version": 1,)", R"("version": 1, "extra": 1,)");
        std::vector<serialization::Error> warnings;
        const auto read = ith::readSidecar(text, "a.meta", &warnings);
        REQUIRE(read.hasValue());
        REQUIRE(warnings.size() == 2);
        CHECK(warnings[0].code == "serialization.unknown_field");
        CHECK(warnings[0].path.empty());
        CHECK(warnings[1].path == "$.comment");
        CHECK(warnings[1].source == "a.meta");
    }

    TEST_CASE("versions newer than the engine's are refused") {
        CHECK(readError(replaced(kSampleText, R"("version": 1)", R"("version": 2)")) == "serialization.newer_version");
        CHECK(readError(replaced(kSampleText, R"("version": 1)", R"("version": 0)")) ==
              "serialization.version_too_old");
        CHECK(readError(replaced(kSampleText, R"("version": 1)", R"("version": "1")")) ==
              "serialization.not_a_document");
    }

    TEST_CASE("broken headers and fields are reported") {
        CHECK(readError("") == "serialization.syntax");
        CHECK(readError("[]") == "serialization.not_a_document");
        CHECK(readError(replaced(kSampleText, "ith.asset_meta", "ith.material")) == "serialization.wrong_format");
        CHECK(readError(replaced(kSampleText, R"("id": "01234567)", R"("idx": "01234567)")) ==
              "serialization.not_a_document");
        CHECK(readError(replaced(kSampleText, "01234567-89ab", "nope-89ab")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, "01234567-89ab-cdef-1122-334455667788",
                                 "00000000-0000-0000-0000-000000000000")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, R"("importer": "test.objects",)", "")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, R"("importerVersion": 3)", R"("importerVersion": -3)")) ==
              "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, "{\n      \"scale\": 2\n    }", "3")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, "material:Steel", "mesh:#0")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, "material:Steel", "Material:Steel")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, "00000000-0000-0001-0000-000000000001",
                                 "00000000-0000-0001-0000-000000000002")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, R"("missing": true)", R"("missing": 1)")) == "asset.sidecar_invalid");
        CHECK(readError(replaced(kSampleText, R"("importer": "test.objects",)",
                                 R"("importer": "test.objects", "importer": "x",)")) ==
              "serialization.duplicate_field");
    }

    TEST_CASE("an error names the field") {
        const auto read = ith::readSidecar(replaced(kSampleText, R"("missing": true)", R"("missing": null)"), "x.meta");
        REQUIRE_FALSE(read.hasValue());
        if (!read) {
            CHECK(read.error().path == "$.subAssets[1].missing");
            CHECK(read.error().source == "x.meta");
        }
    }

    TEST_CASE("damaged sidecars fail cleanly") {
        // Every prefix and every single-byte change of a valid sidecar either reads or fails with
        // an error; the libFuzzer target covers far more in sanitizer builds.
        for (std::size_t length = 0; length + 1 < kSampleText.size(); ++length) {
            const auto read = ith::readSidecar(kSampleText.substr(0, length));
            CHECK_FALSE(read.hasValue());
        }
        for (std::size_t index = 0; index < kSampleText.size(); ++index) {
            for (const char replacement : {'\0', '"', '{', ']', '9', '\xff'}) {
                std::string damaged(kSampleText);
                damaged[index] = replacement;
                const auto read = ith::readSidecar(damaged);
                if (!read) {
                    CHECK_FALSE(read.error().code.empty());
                }
            }
        }
    }

    TEST_CASE("valid source keys") {
        CHECK(ith::isValidSourceKey("mesh:Body/Hull"));
        CHECK(ith::isValidSourceKey("skin_2:#0"));
        CHECK_FALSE(ith::isValidSourceKey("mesh:"));
        CHECK_FALSE(ith::isValidSourceKey(":Body"));
        CHECK_FALSE(ith::isValidSourceKey("Mesh:Body"));
        CHECK_FALSE(ith::isValidSourceKey("mesh"));
        CHECK_FALSE(ith::isValidSourceKey("me sh:Body"));
    }

    TEST_CASE("reconciling keeps ids by key, adds new ids and marks missing rows") {
        const ith::SubAssetId hull = ith::SubAssetId::generate();
        const ith::SubAssetId gone = ith::SubAssetId::generate();
        const ith::SubAssetId back = ith::SubAssetId::generate();
        const std::vector<ith::SubAssetEntry> table = {
            {.key = "mesh:Back", .id = back, .missing = true},
            {.key = "mesh:Gone", .id = gone},
            {.key = "mesh:Hull", .id = hull},
        };
        const std::vector<std::string> keys = {"mesh:New", "mesh:Hull", "mesh:Back"};
        const std::vector<ith::SubAssetEntry> result = ith::reconcileSubAssets(table, keys);
        REQUIRE(result.size() == 4);
        CHECK(result[0] == ith::SubAssetEntry{.key = "mesh:Back", .id = back});
        CHECK(result[1] == ith::SubAssetEntry{.key = "mesh:Gone", .id = gone, .missing = true});
        CHECK(result[2] == ith::SubAssetEntry{.key = "mesh:Hull", .id = hull});
        CHECK(result[3].key == "mesh:New");
        CHECK_FALSE(result[3].missing);
        CHECK_FALSE(result[3].id.isNil());
    }
}

TEST_SUITE("asset_database.native_header") {
    constexpr std::string_view kNative = R"({"format": "ith.material", "version": 2,
        "id": "01234567-89ab-cdef-1122-334455667788", "data": {"roughness": 0.5}})";

    TEST_CASE("the header of a native asset is read without its data") {
        const auto header = ith::readAssetHeader(kNative, "Steel.json");
        REQUIRE(header.hasValue());
        if (header) {
            CHECK(header->format == "ith.material");
            CHECK(header->version == 2);
            CHECK(header->id.toString() == "01234567-89ab-cdef-1122-334455667788");
        }
    }

    TEST_CASE("broken native headers are reported") {
        const auto code = [](std::string_view text) {
            const auto header = ith::readAssetHeader(text);
            return header ? std::string() : header.error().code;
        };
        CHECK(code("{") == "serialization.syntax");
        CHECK(code(R"({"format": "a", "version": 1, "data": {}})") == "serialization.not_a_document");
        CHECK(code(R"({"format": "a", "version": -1, "id": "", "data": {}})") == "serialization.not_a_document");
        CHECK(code(R"({"format": "a", "version": 1, "id": 5, "data": {}})") == "asset.invalid_id");
        CHECK(code(R"({"format": "a", "version": 1, "id": "00000000-0000-0000-0000-000000000000", "data": {}})") ==
              "asset.invalid_id");
    }

    TEST_CASE("a header key given twice is refused, so a replaced id can't leave the old one behind") {
        const std::string twice =
            R"({"format": "ith.material", "version": 1, "id": "01234567-89ab-cdef-1122-334455667788",
            "id": "11234567-89ab-cdef-1122-334455667788", "data": {}})";
        const auto header = ith::readAssetHeader(twice);
        REQUIRE_FALSE(header.hasValue());
        if (!header) {
            CHECK(header.error().code == "serialization.duplicate_field");
        }
        CHECK_FALSE(ith::replaceHeaderId(twice, ith::AssetId::generate()).hasValue());
        CHECK(
            ith::readAssetHeader(
                R"({"format": "a", "format": "b", "version": 1, "id": "01234567-89ab-cdef-1122-334455667788", "data": {}})")
                .error()
                .code == "serialization.duplicate_field");
    }

    TEST_CASE("the header id can be replaced") {
        const ith::AssetId id = ith::AssetId::generate();
        const auto text = ith::replaceHeaderId(kNative, id);
        REQUIRE(text.hasValue());
        if (!text) {
            return;
        }
        const auto header = ith::readAssetHeader(*text);
        REQUIRE(header.hasValue());
        if (header) {
            CHECK(header->id == id);
            CHECK(header->format == "ith.material");
        }
        CHECK(text->find("\"roughness\": 0.5") != std::string::npos);
    }
}
