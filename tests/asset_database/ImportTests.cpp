// Import and reimport (ADR-0015 rule 9): the P1 exit criterion "asset reimport keeps IDs".

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Sidecar.h"

#include "AssetTestSupport.h"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using ith::test::ObjectsImporter;
using ith::test::TempProject;

// The sub-asset ids of an import by key, with the missing ones.
struct Ids {
    std::map<std::string, ith::SubAssetId> present;
    std::map<std::string, ith::SubAssetId> missing;
};

Ids idsOf(const ith::ImportReport& report) {
    Ids ids;
    for (const ith::ImportedSubAsset& subAsset : report.subAssets) {
        ids.present.emplace(subAsset.key, subAsset.id);
    }
    for (const ith::SubAssetEntry& entry : report.missing) {
        ids.missing.emplace(entry.key, entry.id);
    }
    return ids;
}

std::vector<std::string> keysOf(const ith::ImportReport& report) {
    std::vector<std::string> keys;
    for (const ith::ImportedSubAsset& subAsset : report.subAssets) {
        keys.push_back(subAsset.key);
    }
    return keys;
}

std::string payloadText(const ith::ImportedSubAsset& subAsset) {
    return {reinterpret_cast<const char*>(subAsset.payload.data()), subAsset.payload.size()};
}

// A project with the test importer and a database over it.
struct Fixture {
    TempProject project;
    std::atomic<int> calls{0};
    ith::ImporterRegistry importers;
    std::unique_ptr<ith::AssetDatabase> database;

    explicit Fixture(std::uint32_t importerVersion = 1) {
        importers.add(std::make_unique<ObjectsImporter>(&calls, importerVersion));
        database = std::make_unique<ith::AssetDatabase>(project.directory(), importers);
    }

    ith::ImportReport import(std::string_view path) {
        ith::Expected<ith::ImportReport> report = database->import(path);
        REQUIRE_MESSAGE(report.hasValue(), (report ? "" : report.error().message));
        return report ? std::move(*report) : ith::ImportReport{};
    }

    ith::Sidecar sidecar(std::string_view path) const {
        const std::optional<std::string> text = project.read(std::string(path) + ".meta");
        REQUIRE(text.has_value());
        const auto read = ith::readSidecar(text.value_or(""));
        REQUIRE(read.hasValue());
        return read ? *read : ith::Sidecar{};
    }
};

} // namespace

TEST_SUITE("asset_database.import") {
    TEST_CASE("the first import creates the sidecar with new ids") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull = steel\nmaterial Paint\n");
        const ith::ImportReport report = fixture.import("Ship.objects");
        CHECK(report.created);
        CHECK_FALSE(report.fromCache);
        CHECK_FALSE(report.id.isNil());
        CHECK(keysOf(report) == std::vector<std::string>{"mesh:Hull", "material:Paint"});
        REQUIRE(report.subAssets.size() == 2);
        CHECK(payloadText(report.subAssets[0]) == "steel@1");
        CHECK(report.missing.empty());

        const ith::Sidecar sidecar = fixture.sidecar("Ship.objects");
        CHECK(sidecar.id == report.id);
        CHECK(sidecar.importer == "test.objects");
        CHECK(sidecar.importerVersion == 1);
        REQUIRE(sidecar.subAssets.size() == 2);
        CHECK(sidecar.subAssets[0].key == "material:Paint");
        CHECK(sidecar.subAssets[0].id == report.subAssets[1].id);

        const ith::AssetRecord* record = fixture.database->find(report.id);
        REQUIRE(record != nullptr);
        if (record != nullptr) {
            CHECK(record->path == "Ship.objects");
            CHECK(record->type == "test.objects");
            CHECK(record->imported);
            CHECK(record->subAssets == sidecar.subAssets);
        }
        CHECK(fixture.database->findByPath("Ship.objects") == record);
    }

    TEST_CASE("reimport keeps ids when objects are reordered") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull\nmesh Sail\nmaterial Paint\n");
        const Ids before = idsOf(fixture.import("Ship.objects"));
        fixture.project.write("Ship.objects", "material Paint\nmesh Sail\nmesh Hull\n");
        const ith::ImportReport report = fixture.import("Ship.objects");
        CHECK_FALSE(report.created);
        CHECK(idsOf(report).present == before.present);
        CHECK(report.missing.empty());
    }

    TEST_CASE("a renamed object gets a new id and the old one is kept as missing, then restored") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull\nmesh Sail\n");
        const Ids before = idsOf(fixture.import("Ship.objects"));
        fixture.project.write("Ship.objects", "mesh Hull\nmesh Jib\n");
        const Ids renamed = idsOf(fixture.import("Ship.objects"));
        CHECK(renamed.present.at("mesh:Hull") == before.present.at("mesh:Hull"));
        REQUIRE(renamed.present.contains("mesh:Jib"));
        CHECK(renamed.present.at("mesh:Jib") != before.present.at("mesh:Sail"));
        CHECK(renamed.missing == std::map<std::string, ith::SubAssetId>{{"mesh:Sail", before.present.at("mesh:Sail")}});
        CHECK(fixture.sidecar("Ship.objects").subAssets.size() == 3);

        // Renaming it back brings the old id back.
        fixture.project.write("Ship.objects", "mesh Sail\nmesh Hull\n");
        const Ids restored = idsOf(fixture.import("Ship.objects"));
        CHECK(restored.present == before.present);
        CHECK(restored.missing.at("mesh:Jib") == renamed.present.at("mesh:Jib"));
    }

    TEST_CASE("added objects get new ids and removed ones are marked missing") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull\nmaterial Paint\n");
        const Ids before = idsOf(fixture.import("Ship.objects"));
        fixture.project.write("Ship.objects", "mesh Hull\nmesh Mast\nanimation Sway\n");
        const ith::ImportReport report = fixture.import("Ship.objects");
        const Ids after = idsOf(report);
        CHECK(after.present.size() == 3);
        CHECK(after.present.at("mesh:Hull") == before.present.at("mesh:Hull"));
        CHECK(after.missing.at("material:Paint") == before.present.at("material:Paint"));
        CHECK_FALSE(after.present.at("mesh:Mast").isNil());
        CHECK_FALSE(after.present.at("animation:Sway").isNil());
        const ith::Sidecar sidecar = fixture.sidecar("Ship.objects");
        CHECK(sidecar.subAssets.size() == 4);
        for (const ith::SubAssetEntry& entry : sidecar.subAssets) {
            CHECK(entry.missing == (entry.key == "material:Paint"));
        }
    }

    TEST_CASE("unnamed objects fall back to their index among the unnamed objects of their type") {
        Fixture fixture;
        fixture.project.write("Rocks.objects", "mesh\nmesh\nmaterial\n");
        const ith::ImportReport first = fixture.import("Rocks.objects");
        CHECK(keysOf(first) == std::vector<std::string>{"mesh:#0", "mesh:#1", "material:#0"});
        // A named object added in front doesn't shift the unnamed ones.
        fixture.project.write("Rocks.objects", "mesh Boulder\nmaterial\nmesh\nmesh\n");
        const Ids second = idsOf(fixture.import("Rocks.objects"));
        const Ids before = idsOf(first);
        CHECK(second.present.at("mesh:#0") == before.present.at("mesh:#0"));
        CHECK(second.present.at("mesh:#1") == before.present.at("mesh:#1"));
        CHECK(second.present.at("material:#0") == before.present.at("material:#0"));
        CHECK(second.missing.empty());
    }

    TEST_CASE("source keys escape names and number repeated names") {
        const std::vector<ith::ImportedObject> objects = {
            {.type = "mesh", .name = "Cube"},
            {.type = "mesh", .name = "Cube"},
            {.type = "mesh", .name = "Cube#1"},
            {.type = "mesh", .name = "50%/#0"},
            {.type = "mesh"},
            {.type = "material", .name = "Cube"},
        };
        const auto keys = ith::sourceKeys(objects);
        REQUIRE(keys.hasValue());
        if (keys) {
            CHECK(*keys == std::vector<std::string>{"mesh:Cube", "mesh:Cube#1", "mesh:Cube%231", "mesh:50%25/%230",
                                                    "mesh:#0", "material:Cube"});
        }
        const std::vector<ith::ImportedObject> badType = {{.type = "Mesh", .name = "Cube"}};
        const auto bad = ith::sourceKeys(badType);
        REQUIRE_FALSE(bad.hasValue());
        if (!bad) {
            CHECK(bad.error().code == "import.invalid_output");
        }
    }

    TEST_CASE("reimport uses the saved settings and keeps the sidecar's overrides") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull = steel\n");
        const ith::ImportReport first = fixture.import("Ship.objects");
        ith::Sidecar sidecar = fixture.sidecar("Ship.objects");
        sidecar.settings.set("scale", ith::serialization::Value::integer(4));
        sidecar.overrides.set("x:Mesh.lod", ith::serialization::Value::integer(2));
        const auto text = ith::writeSidecar(sidecar);
        REQUIRE(text.hasValue());
        fixture.project.write("Ship.objects.meta", text ? *text : "");

        const ith::ImportReport report = fixture.database->reimport(first.id).valueOr(ith::ImportReport{});
        REQUIRE(report.subAssets.size() == 1);
        CHECK(payloadText(report.subAssets[0]) == "steel@4");
        CHECK(report.subAssets[0].id == first.subAssets[0].id);
        CHECK_FALSE(report.fromCache);
        CHECK(fixture.sidecar("Ship.objects") == sidecar);
    }

    TEST_CASE("an unchanged reimport comes from the cache and leaves the sidecar alone") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull = steel\n");
        const ith::ImportReport first = fixture.import("Ship.objects");
        CHECK(fixture.calls == 1);
        const std::optional<std::string> sidecarText = fixture.project.read("Ship.objects.meta");
        const ith::ImportReport second = fixture.import("Ship.objects");
        CHECK(second.fromCache);
        CHECK(fixture.calls == 1);
        CHECK(second.id == first.id);
        REQUIRE(second.subAssets.size() == 1);
        CHECK(payloadText(second.subAssets[0]) == "steel@1");
        CHECK(fixture.project.read("Ship.objects.meta") == sidecarText);

        // A changed source is imported again.
        fixture.project.write("Ship.objects", "mesh Hull = oak\n");
        const ith::ImportReport third = fixture.import("Ship.objects");
        CHECK_FALSE(third.fromCache);
        CHECK(fixture.calls == 2);
        CHECK(third.subAssets[0].id == first.subAssets[0].id);
    }

    TEST_CASE("a new importer version imports again and is saved in the sidecar") {
        TempProject project;
        project.write("Ship.objects", "mesh Hull\n");
        std::atomic<int> calls{0};
        ith::AssetId id;
        ith::SubAssetId hull;
        {
            ith::ImporterRegistry importers;
            importers.add(std::make_unique<ObjectsImporter>(&calls, 1));
            ith::AssetDatabase database(project.directory(), importers);
            const auto report = database.import("Ship.objects");
            REQUIRE(report.hasValue());
            id = report ? report->id : ith::AssetId();
            hull = report ? report->subAssets.at(0).id : ith::SubAssetId();
        }
        ith::ImporterRegistry importers;
        importers.add(std::make_unique<ObjectsImporter>(&calls, 2));
        ith::AssetDatabase database(project.directory(), importers);
        REQUIRE(database.scan().hasValue());
        const auto report = database.reimport(id);
        REQUIRE(report.hasValue());
        if (report) {
            CHECK_FALSE(report->fromCache);
            CHECK(report->subAssets.at(0).id == hull);
        }
        CHECK(calls == 2);
        const auto sidecar = ith::readSidecar(project.read("Ship.objects.meta").value_or(""));
        REQUIRE(sidecar.hasValue());
        if (sidecar) {
            CHECK(sidecar->importerVersion == 2);
        }
    }

    TEST_CASE("import failures leave files and database unchanged") {
        Fixture fixture;
        fixture.project.write("Broken.objects", "mesh Hull\n!oops\n");
        const auto broken = fixture.database->import("Broken.objects");
        REQUIRE_FALSE(broken.hasValue());
        if (!broken) {
            CHECK(broken.error().code == "import.malformed");
        }
        CHECK_FALSE(fixture.project.exists("Broken.objects.meta"));
        CHECK(fixture.database->records().empty());

        fixture.project.write("Bad.objects", "Mesh Hull\n");
        const auto badType = fixture.database->import("Bad.objects");
        REQUIRE_FALSE(badType.hasValue());
        if (!badType) {
            CHECK(badType.error().code == "import.invalid_output");
        }

        const auto code = [&](std::string_view path) {
            const auto report = fixture.database->import(path);
            return report ? std::string() : report.error().code;
        };
        fixture.project.write("notes.txt", "hello");
        CHECK(code("notes.txt") == "asset.no_importer");
        CHECK(code("Missing.objects") == "asset.not_found");
        CHECK(code("broken.objects") == "asset.not_found"); // the file is "Broken.objects"
        CHECK(code("../Ship.objects") == "asset.invalid_path");
        CHECK(code("Ship.objects.meta") == "asset.invalid_path");

        fixture.project.write("Odd.objects", "mesh A\n");
        fixture.project.write("Odd.objects.meta", "{}");
        CHECK(code("Odd.objects") == "serialization.not_a_document");
        CHECK(fixture.project.read("Odd.objects.meta") == std::optional<std::string>("{}"));

        CHECK_FALSE(fixture.database->reimport(ith::AssetId::generate()).hasValue());
    }

    TEST_CASE("files the importer needs are read from the source's folder and are part of the cache key") {
        Fixture fixture;
        fixture.project.write("Ships/Ship.objects", "include Parts/Hull.txt\nmesh Hull\nrefer Textures/Rock.png\n");
        fixture.project.write("Ships/Parts/Hull.txt", "first");
        const ith::ImportReport first = fixture.import("Ships/Ship.objects");
        CHECK(keysOf(first) == std::vector<std::string>{"file:Parts/Hull.txt", "mesh:Hull"});
        REQUIRE(first.subAssets.size() == 2);
        CHECK(payloadText(first.subAssets[0]) == "first");
        CHECK(first.references == std::vector<std::string>{"Ships/Textures/Rock.png"});
        CHECK(fixture.calls == 1);

        // Unchanged inputs come from the cache, references included.
        const ith::ImportReport cached = fixture.import("Ships/Ship.objects");
        CHECK(cached.fromCache);
        CHECK(cached.references == first.references);
        CHECK(fixture.calls == 1);

        // A changed dependency is a miss and keeps the ids.
        fixture.project.write("Ships/Parts/Hull.txt", "second");
        const ith::ImportReport changed = fixture.import("Ships/Ship.objects");
        CHECK_FALSE(changed.fromCache);
        CHECK(fixture.calls == 2);
        REQUIRE(changed.subAssets.size() == 2);
        CHECK(payloadText(changed.subAssets[0]) == "second");
        CHECK(idsOf(changed).present == idsOf(first).present);
    }

    TEST_CASE("missing, escaping or miscased dependencies fail the import and change nothing") {
        Fixture fixture;
        const auto code = [&](std::string_view path) {
            const auto report = fixture.database->import(path);
            return report ? std::string() : report.error().code;
        };
        fixture.project.write("Ships/Missing.objects", "include Hull.txt\n");
        CHECK(code("Ships/Missing.objects") == "asset.not_found");
        fixture.project.write("Ships/hull.txt", "x");
        CHECK(code("Ships/Missing.objects") == "asset.not_found"); // the file is "hull.txt"

        fixture.project.write("Secret.txt", "x");
        fixture.project.write("Ships/Escape.objects", "include ../Secret.txt\n");
        CHECK(code("Ships/Escape.objects") == "import.invalid_output");
        fixture.project.write("Ships/Absolute.objects", "include /Secret.txt\n");
        CHECK(code("Ships/Absolute.objects") == "import.invalid_output");
        fixture.project.write("Ships/Drive.objects", "include C:/Secret.txt\n");
        CHECK(code("Ships/Drive.objects") == "import.invalid_output");
        fixture.project.write("Ships/BadReference.objects", "refer ../Rock.png\n");
        CHECK(code("Ships/BadReference.objects") == "import.invalid_output");

        // Dependencies are checked before the importer runs; references only after it ran.
        CHECK(fixture.calls == 1);
        CHECK(fixture.database->records().empty());
        CHECK_FALSE(fixture.project.exists("Ships/Missing.objects.meta"));
        CHECK_FALSE(fixture.project.exists("Ships/Escape.objects.meta"));
    }

    TEST_CASE("a file moved outside the editor keeps its id on import; a copy gets a new one") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull\n");
        const ith::ImportReport first = fixture.import("Ship.objects");
        std::error_code error;
        std::filesystem::create_directories(fixture.project.content("Sea"), error);
        std::filesystem::rename(fixture.project.content("Ship.objects"), fixture.project.content("Sea/Ship.objects"),
                                error);
        std::filesystem::rename(fixture.project.content("Ship.objects.meta"),
                                fixture.project.content("Sea/Ship.objects.meta"), error);
        REQUIRE_FALSE(error);

        // No rescan: the database still has the old path, which no longer exists.
        const ith::ImportReport moved = fixture.import("Sea/Ship.objects");
        CHECK(moved.id == first.id);
        CHECK(moved.subAssets.at(0).id == first.subAssets.at(0).id);
        CHECK(fixture.database->find(first.id)->path == "Sea/Ship.objects");
        CHECK(fixture.database->findByPath("Ship.objects") == nullptr);

        // A copy, while the original is still there, gets a new id written to its sidecar.
        fixture.project.write("Copy/Ship.objects", fixture.project.read("Sea/Ship.objects").value_or(""));
        fixture.project.write("Copy/Ship.objects.meta", fixture.project.read("Sea/Ship.objects.meta").value_or(""));
        const ith::ImportReport copy = fixture.import("Copy/Ship.objects");
        CHECK(copy.id != first.id);
        CHECK(fixture.sidecar("Copy/Ship.objects").id == copy.id);
        CHECK(fixture.database->find(first.id)->path == "Sea/Ship.objects");
    }

    TEST_CASE("the sidecar's warnings are reported and a rewritten sidecar is clean") {
        Fixture fixture;
        fixture.project.write("Ship.objects", "mesh Hull\n");
        fixture.import("Ship.objects");
        std::string text = fixture.project.read("Ship.objects.meta").value_or("");
        // An unknown key and CRLF line endings, as an editor or a git setting might leave them.
        const std::size_t data = text.find("\"data\": {");
        REQUIRE(data != std::string::npos);
        text.insert(data + 9, "\"note\": 1,");
        std::string crlf;
        for (const char character : text) {
            if (character == '\n') {
                crlf += '\r';
            }
            crlf += character;
        }
        fixture.project.write("Ship.objects.meta", crlf);

        const ith::ImportReport report = fixture.import("Ship.objects");
        REQUIRE(report.warnings.size() == 1);
        CHECK(report.warnings[0].code == "serialization.unknown_field");
        CHECK(report.warnings[0].path == "$.note");
        const std::string rewritten = fixture.project.read("Ship.objects.meta").value_or("\r");
        CHECK(rewritten.find('\r') == std::string::npos);
        CHECK(rewritten.find("note") == std::string::npos);
    }

    TEST_CASE("files larger than the limit are refused before they are read") {
        const TempProject project;
        project.write("Big.bin", std::string(100, 'x'));
        const auto small = ith::detail::asset_files::readFile(project.content("Big.bin"), 99);
        REQUIRE_FALSE(small.hasValue());
        if (!small) {
            CHECK(small.error().code == "asset.too_large");
        }
        CHECK(ith::detail::asset_files::readFile(project.content("Big.bin"), 100).hasValue());
        CHECK(ith::detail::asset_files::readFile(project.content("None.bin"), 100).error().code == "asset.io");
    }
}
