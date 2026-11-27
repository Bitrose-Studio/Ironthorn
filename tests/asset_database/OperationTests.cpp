// Folder operations, duplicates, copies from outside the project, and finding the users of assets
// (ADR-0049 rule 22).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Sidecar.h"
#include "ith/assets/AssetRef.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include "AssetTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

using ith::AssetId;
using ith::AssetProblemKind;
using ith::test::ObjectsImporter;
using ith::test::TempProject;

std::string nativeAsset(AssetId id, std::string_view data = "{}", std::string_view format = "ith.material") {
    return R"({"format": ")" + std::string(format) + R"(", "version": 1, "id": ")" + id.toString() + R"(", "data": )" +
           std::string(data) + "}";
}

// A reference as AssetRef writes it.
std::string referenceTo(AssetId id) {
    return R"({"asset": ")" + id.toString() + R"(", "subAsset": "00000000-0000-0000-0000-000000000000"})";
}

struct Fixture {
    TempProject project;
    ith::ImporterRegistry importers;
    std::unique_ptr<ith::AssetDatabase> database;

    Fixture() {
        importers.add(std::make_unique<ObjectsImporter>());
        database = std::make_unique<ith::AssetDatabase>(project.root(), importers);
    }

    AssetId import(std::string_view path, std::string_view source = "mesh Hull\n") {
        project.write(path, source);
        const auto report = database->import(path);
        REQUIRE(report.hasValue());
        return report ? report->id : AssetId();
    }

    AssetId native(std::string_view path, std::string_view data = "{}") {
        const AssetId id = AssetId::generate();
        project.write(path, nativeAsset(id, data));
        REQUIRE(database->assignNewId(path).hasValue());
        return database->findByPath(path)->id;
    }

    void scan() {
        const auto problems = database->scan();
        REQUIRE(problems.hasValue());
        if (problems) {
            CHECK(problems->empty());
        }
    }

    // A fresh database over the same project, scanned: what another session would see.
    ith::AssetDatabase rescanned() const {
        ith::AssetDatabase fresh(project.root(), importers);
        const auto problems = fresh.scan();
        CHECK(problems.hasValue());
        if (problems) {
            CHECK(problems->empty());
        }
        return fresh;
    }

    // A file outside the Content folder, as a UTF-8 path.
    std::string writeOutside(std::string_view path, std::string_view text) const {
        const std::string file = ith::fs::joinPath(project.root(), "Outside/" + std::string(path));
        CHECK(ith::fs::writeFileAtomic(file, text).hasValue());
        return file;
    }

    bool folderExists(std::string_view path) const {
        std::error_code error;
        return std::filesystem::is_directory(project.content(path), error);
    }

    template <typename T>
    static std::string errorOf(const ith::Expected<T>& result) {
        return result ? std::string() : result.error().code;
    }
};

std::vector<std::string> pathsOf(const std::vector<ith::AssetFailure>& failures) {
    std::vector<std::string> paths;
    for (const ith::AssetFailure& failure : failures) {
        paths.push_back(failure.path);
    }
    return paths;
}

} // namespace

TEST_SUITE("asset_database.folders") {
    TEST_CASE("createFolder creates a folder with its parents and lists them") {
        Fixture fixture;
        fixture.scan();
        REQUIRE(fixture.database->createFolder("Meshes").hasValue());
        REQUIRE(fixture.database->createFolder("Levels/Deep/Deeper").hasValue());
        CHECK(fixture.folderExists("Meshes"));
        CHECK(fixture.folderExists("Levels/Deep/Deeper"));
        CHECK(fixture.database->folders() ==
              std::vector<std::string>{"Levels", "Levels/Deep", "Levels/Deep/Deeper", "Meshes"});
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("createFolder refuses taken names in any case, bad names and folders that are too deep") {
        Fixture fixture;
        fixture.import("Meshes/Ship.objects");
        fixture.project.write("notes.txt", "hidden from the browser");
        fixture.scan();
        const auto code = [&](std::string_view path) { return Fixture::errorOf(fixture.database->createFolder(path)); };
        CHECK(code("Meshes") == "asset.path_conflict");
        CHECK(code("meshes") == "asset.path_conflict");
        CHECK(code("NOTES.TXT") == "asset.path_conflict");
        CHECK(code("Meshes/ship.objects") == "asset.path_conflict");
        CHECK(code("Meshes/Ship.objects.META") == "asset.invalid_path"); // a sidecar name
        CHECK(code("meshes/New") == "asset.path_conflict");              // a parent in another case
        CHECK(code("") == "asset.invalid_path");
        CHECK(code("a.meta") == "asset.invalid_path");
        CHECK(code("CON") == "asset.invalid_path");
        CHECK(code("a/../b") == "asset.invalid_path");
        CHECK(code("Name.") == "asset.invalid_path");
        CHECK(code("notes.txt/Inside") == "asset.io"); // a parent is a file

        std::string deep;
        for (std::size_t level = 0; level < ith::kMaxFolderDepth; ++level) {
            deep += deep.empty() ? "d" : "/d";
        }
        CHECK(code(deep + "/e") == "asset.invalid_path");
        REQUIRE(fixture.database->createFolder(deep).hasValue());
        CHECK(fixture.database->folders().size() == ith::kMaxFolderDepth + 1); // "Meshes" and the chain
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("moveFolder moves everything inside, keeps ids and updates records and folders") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Sea/Ship.objects");
        const AssetId steel = fixture.native("Meshes/Steel.ithasset");
        const AssetId rock = fixture.import("Rocks/Rock.objects");
        fixture.project.write("Meshes/notes.txt", "not an asset");
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Meshes/Empty")).hasValue());
        fixture.project.write("MeshesAndMore/Other.txt", "a folder whose name starts the same");
        fixture.scan();

        REQUIRE(fixture.database->moveFolder("Meshes", "Vehicles/Meshes").hasValue());
        CHECK(fixture.project.exists("Vehicles/Meshes/Sea/Ship.objects"));
        CHECK(fixture.project.exists("Vehicles/Meshes/Sea/Ship.objects.meta"));
        CHECK(fixture.project.exists("Vehicles/Meshes/notes.txt"));
        CHECK_FALSE(fixture.folderExists("Meshes"));
        CHECK(fixture.database->find(ship)->path == "Vehicles/Meshes/Sea/Ship.objects");
        CHECK(fixture.database->find(steel)->path == "Vehicles/Meshes/Steel.ithasset");
        CHECK(fixture.database->find(rock)->path == "Rocks/Rock.objects");
        CHECK(fixture.database->findByPath("Meshes/Steel.ithasset") == nullptr);
        CHECK(fixture.database->records().size() == 3);
        CHECK(fixture.database->folders() == std::vector<std::string>{"MeshesAndMore", "Rocks", "Vehicles",
                                                                      "Vehicles/Meshes", "Vehicles/Meshes/Empty",
                                                                      "Vehicles/Meshes/Sea"});
        const ith::AssetDatabase fresh = fixture.rescanned();
        CHECK(fresh.folders() == fixture.database->folders());
        CHECK(fresh.find(ship)->path == "Vehicles/Meshes/Sea/Ship.objects");
        CHECK(fresh.find(steel)->path == "Vehicles/Meshes/Steel.ithasset");
    }

    TEST_CASE("moveFolder renames a folder to another case of its own name") {
        Fixture fixture;
        const AssetId ship = fixture.import("meshes/Ship.objects");
        fixture.scan();
        REQUIRE(fixture.database->moveFolder("meshes", "Meshes").hasValue());
        CHECK(fixture.database->find(ship)->path == "Meshes/Ship.objects");
        CHECK(fixture.database->folders() == std::vector<std::string>{"Meshes"});
        const ith::AssetDatabase fresh = fixture.rescanned();
        CHECK(fresh.folders() == std::vector<std::string>{"Meshes"});
        CHECK(fresh.find(ship)->path == "Meshes/Ship.objects");
        CHECK(fixture.database->moveFolder("Meshes", "Meshes").hasValue()); // the same path: nothing
    }

    TEST_CASE("moveFolder refuses targets inside the folder, taken names and missing folders, changing nothing") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Sea/Ship.objects");
        fixture.import("Rocks/Rock.objects");
        fixture.project.write("Taken.txt", "a file");
        fixture.scan();
        const std::vector<std::string> folders = fixture.database->folders();
        const auto code = [&](std::string_view from, std::string_view to) {
            return Fixture::errorOf(fixture.database->moveFolder(from, to));
        };
        CHECK(code("Meshes", "Meshes/Sea/Inner") == "asset.invalid_target");
        CHECK(code("Meshes", "Meshes/Inner") == "asset.invalid_target");
        CHECK(code("Meshes", "meshes/Inner") == "asset.invalid_target"); // inside, in another case
        CHECK(code("Meshes", "Rocks") == "asset.path_conflict");
        CHECK(code("Meshes", "ROCKS") == "asset.path_conflict");
        CHECK(code("Meshes", "taken.TXT") == "asset.path_conflict");
        CHECK(code("Meshes/Sea", "Meshes") == "asset.path_conflict");   // its own parent
        CHECK(code("Meshes", "rocks/Meshes") == "asset.path_conflict"); // a parent in another case
        CHECK(code("Nothing", "Other") == "asset.not_found");
        CHECK(code("meshes", "Other") == "asset.not_found");    // the folder in another case
        CHECK(code("Taken.txt", "Other") == "asset.not_found"); // a file is no folder
        CHECK(code("", "Other") == "asset.invalid_path");       // the Content folder
        CHECK(code("Meshes", "") == "asset.invalid_path");
        CHECK(code("Meshes", "x.meta") == "asset.invalid_path");

        CHECK(fixture.database->folders() == folders);
        CHECK(fixture.database->find(ship)->path == "Meshes/Sea/Ship.objects");
        CHECK(fixture.project.exists("Meshes/Sea/Ship.objects"));
        CHECK(fixture.project.exists("Meshes/Sea/Ship.objects.meta"));
    }

    TEST_CASE("moveFolder refuses a move that nests folders too deep") {
        Fixture fixture;
        std::string deep;
        for (std::size_t level = 0; level + 1 < ith::kMaxFolderDepth; ++level) {
            deep += deep.empty() ? "d" : "/d";
        }
        REQUIRE(fixture.database->createFolder("A/B").hasValue());
        REQUIRE(fixture.database->createFolder(deep).hasValue());
        CHECK(Fixture::errorOf(fixture.database->moveFolder("A", deep + "/A")) == "asset.invalid_path");
        CHECK(fixture.database->moveFolder("A/B", deep + "/B").hasValue()); // exactly kMaxFolderDepth
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("removeFolder deletes everything below it and erases its records") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Sea/Ship.objects");
        const AssetId steel = fixture.native("Meshes/Steel.ithasset");
        const AssetId rock = fixture.import("MeshesToo/Rock.objects");
        fixture.project.write("Meshes/notes.txt", "not an asset");
        fixture.project.write("Meshes/Orphan.objects.meta", "{}");
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Meshes/Empty/Inner")).hasValue());
        REQUIRE(fixture.database->scan().hasValue()); // reports the orphan
        CHECK(fixture.database->folders().size() == 5);

        const auto report = fixture.database->removeFolder("Meshes");
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->failures.empty());
            CHECK(report->orphans.empty());
        }
        CHECK_FALSE(fixture.folderExists("Meshes"));
        CHECK(fixture.database->find(ship) == nullptr);
        CHECK(fixture.database->find(steel) == nullptr);
        CHECK(fixture.database->find(rock) != nullptr);
        CHECK(fixture.database->folders() == std::vector<std::string>{"MeshesToo"});
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("removeFolder drops records whose file was already gone, and refuses what is no folder") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Ship.objects");
        fixture.project.write("File.txt", "a file");
        fixture.scan();
        REQUIRE(ith::fs::remove(fixture.project.contentPath("Meshes/Ship.objects")).hasValue());
        const auto report = fixture.database->removeFolder("Meshes");
        REQUIRE(report.hasValue());
        CHECK(fixture.database->find(ship) == nullptr);
        CHECK(fixture.database->folders().empty());

        CHECK(Fixture::errorOf(fixture.database->removeFolder("Meshes")) == "asset.not_found");
        CHECK(Fixture::errorOf(fixture.database->removeFolder("File.txt")) == "asset.not_found");
        CHECK(Fixture::errorOf(fixture.database->removeFolder("")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->removeFolder("../Content")) == "asset.invalid_path");
    }

    TEST_CASE("removeFolder removes links without following them") {
        Fixture fixture;
        const AssetId rock = fixture.import("Real/Rock.objects");
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Doomed")).hasValue());
        std::error_code error;
        std::filesystem::create_directory_symlink(fixture.project.content("Real"),
                                                  fixture.project.content("Doomed/Linked"), error);
        if (error) {
            MESSAGE("symbolic links can't be created here; skipped");
            return;
        }
        fixture.scan();
        const auto report = fixture.database->removeFolder("Doomed");
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->failures.empty());
        }
        CHECK_FALSE(fixture.folderExists("Doomed"));
        CHECK(fixture.project.exists("Real/Rock.objects"));
        CHECK(fixture.database->find(rock) != nullptr);
    }

#ifdef _WIN32
    TEST_CASE("removeFolder keeps a sidecar, in any case, whose file stays") {
        Fixture fixture;
        fixture.project.write("Meshes/Data.bin", "data");
        fixture.project.write("Meshes/DATA.BIN.META", "{}"); // no record: paired by name only
        fixture.project.write("Meshes/Gone.bin.meta", "{}"); // an orphan: deleted
        std::optional<std::ifstream> held(std::in_place, fixture.project.content("Meshes/Data.bin"));
        REQUIRE(held->is_open());
        const auto report = fixture.database->removeFolder("Meshes");
        held.reset();
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(pathsOf(report->failures) == std::vector<std::string>{"Meshes", "Meshes/Data.bin"});
            CHECK(report->orphans.empty());
        }
        CHECK(fixture.project.exists("Meshes/Data.bin"));
        CHECK(fixture.project.exists("Meshes/DATA.BIN.META"));
        CHECK_FALSE(fixture.project.exists("Meshes/Gone.bin.meta"));
    }

    TEST_CASE("removeFolder goes on after a failure and reports a sidecar it left as an orphan") {
        // Windows refuses to delete a file that is open without delete sharing, as std::ifstream
        // opens it.
        Fixture fixture;
        const AssetId held = fixture.import("Meshes/Held.objects");
        const AssetId orphaned = fixture.import("Meshes/Orphaned.objects");
        const AssetId other = fixture.import("Meshes/Sub/Other.objects");
        fixture.scan();
        std::optional<std::ifstream> holdFile(std::in_place, fixture.project.content("Meshes/Held.objects"));
        std::optional<std::ifstream> holdSidecar(std::in_place,
                                                 fixture.project.content("Meshes/Orphaned.objects.meta"));
        REQUIRE(holdFile->is_open());
        REQUIRE(holdSidecar->is_open());

        const auto report = fixture.database->removeFolder("Meshes");
        holdFile.reset();
        holdSidecar.reset();
        REQUIRE(report.hasValue());
        if (!report) {
            return;
        }
        CHECK(pathsOf(report->failures) ==
              std::vector<std::string>{"Meshes", "Meshes/Held.objects", "Meshes/Orphaned.objects.meta"});
        for (const ith::AssetFailure& failure : report->failures) {
            CHECK(failure.error.code == "asset.io");
        }
        REQUIRE(report->orphans.size() == 1);
        CHECK(report->orphans[0].kind == AssetProblemKind::OrphanSidecar);
        CHECK(report->orphans[0].path == "Meshes/Orphaned.objects.meta");

        // The held asset keeps its file, sidecar and record; the others are gone.
        CHECK(fixture.project.exists("Meshes/Held.objects"));
        CHECK(fixture.project.exists("Meshes/Held.objects.meta"));
        CHECK(fixture.database->find(held) != nullptr);
        CHECK(fixture.database->find(orphaned) == nullptr);
        CHECK(fixture.database->find(other) == nullptr);
        CHECK_FALSE(fixture.folderExists("Meshes/Sub"));
        CHECK(fixture.database->folders() == std::vector<std::string>{"Meshes"});

        // A scan finds the same: the held asset and the orphan.
        const auto problems = fixture.database->scan();
        REQUIRE(problems.hasValue());
        if (problems) {
            REQUIRE(problems->size() == 1);
            CHECK((*problems)[0].kind == AssetProblemKind::OrphanSidecar);
        }
        CHECK(fixture.database->find(held) != nullptr);
    }

    TEST_CASE("a folder move that fails changes nothing") {
        // Windows refuses to rename a folder with a file open inside it.
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Ship.objects");
        fixture.scan();
        std::optional<std::ifstream> held(std::in_place, fixture.project.content("Meshes/Ship.objects"));
        REQUIRE(held->is_open());
        const auto moved = fixture.database->moveFolder("Meshes", "Vehicles");
        held.reset();
        if (moved) {
            MESSAGE("the folder moved with a file open inside it; skipped");
            return;
        }
        CHECK(moved.error().code == "asset.io");
        CHECK(fixture.database->find(ship)->path == "Meshes/Ship.objects");
        CHECK(fixture.project.exists("Meshes/Ship.objects"));
        CHECK(fixture.database->folders() == std::vector<std::string>{"Meshes"});
    }
#else
    TEST_CASE("removeFolder goes on after a failure and keeps what it couldn't delete") {
        Fixture fixture;
        const AssetId locked = fixture.import("Meshes/Locked/Inner.objects");
        const AssetId loose = fixture.import("Meshes/Free.objects");
        fixture.scan();
        ::chmod(fixture.project.content("Meshes/Locked").c_str(), 0555);
        const bool enforced = ::access(fixture.project.content("Meshes/Locked").c_str(), W_OK) != 0;
        const auto report = fixture.database->removeFolder("Meshes");
        ::chmod(fixture.project.content("Meshes/Locked").c_str(), 0755);
        REQUIRE(report.hasValue());
        if (!enforced) {
            MESSAGE("the folder is still writable (running as root?); skipped");
            return;
        }
        if (!report) {
            return;
        }
        CHECK(pathsOf(report->failures) ==
              std::vector<std::string>{"Meshes", "Meshes/Locked", "Meshes/Locked/Inner.objects"});
        CHECK(report->orphans.empty());
        CHECK(fixture.database->find(locked) != nullptr);
        CHECK(fixture.database->find(loose) == nullptr);
        CHECK(fixture.project.exists("Meshes/Locked/Inner.objects.meta"));
        CHECK(fixture.database->folders() == std::vector<std::string>{"Meshes", "Meshes/Locked"});
    }
#endif
}

TEST_SUITE("asset_database.duplicate") {
    TEST_CASE("a duplicated imported asset has a new id in a copied sidecar and the same source") {
        Fixture fixture;
        const AssetId ship = fixture.import("Ship.objects", "mesh Hull\nmesh Sail\n");
        const std::optional<std::string> sidecarText = fixture.project.read("Ship.objects.meta");
        REQUIRE(sidecarText.has_value());
        const auto original = ith::readSidecar(sidecarText.value_or(""));
        REQUIRE(original.hasValue());

        const auto copy = fixture.database->duplicate(ship, "Copies/Ship_2.objects");
        REQUIRE(copy.hasValue());
        if (!copy || !original) {
            return;
        }
        CHECK(*copy != ship);
        CHECK(fixture.project.read("Copies/Ship_2.objects") == fixture.project.read("Ship.objects"));
        const auto copied = ith::readSidecar(fixture.project.read("Copies/Ship_2.objects.meta").value_or(""));
        REQUIRE(copied.hasValue());
        if (copied) {
            CHECK(copied->id == *copy);
            ith::Sidecar expected = *original;
            expected.id = *copy;
            CHECK(*copied == expected);
        }
        const ith::AssetRecord* record = fixture.database->find(*copy);
        REQUIRE(record != nullptr);
        CHECK(record->path == "Copies/Ship_2.objects");
        CHECK(record->imported);
        CHECK(record->subAssets == fixture.database->find(ship)->subAssets);
        CHECK(fixture.database->folders() == std::vector<std::string>{"Copies"});
        CHECK(fixture.rescanned().find(*copy) != nullptr); // no duplicate id
    }

    TEST_CASE("a duplicated native asset has a new header id and the same data") {
        Fixture fixture;
        const AssetId level = AssetId::generate();
        fixture.project.write("Maps/Harbour.ithmap",
                              nativeAsset(level, R"({"entities": [1, 2, 3]})", ith::kLevelFormat));
        fixture.scan();
        const auto copy = fixture.database->duplicate(level, "Maps/Harbour_2.ithmap");
        REQUIRE(copy.hasValue());
        if (!copy) {
            return;
        }
        const std::string text = fixture.project.read("Maps/Harbour_2.ithmap").value_or("");
        const auto header = ith::readAssetHeader(text);
        REQUIRE(header.hasValue());
        if (header) {
            CHECK(header->id == *copy);
            CHECK(header->format == ith::kLevelFormat);
        }
        const auto copied = ith::serialization::parseJson(text);
        const auto original = ith::serialization::parseJson(fixture.project.read("Maps/Harbour.ithmap").value_or(""));
        REQUIRE(copied.hasValue());
        REQUIRE(original.hasValue());
        if (copied && original) {
            CHECK(*copied->find("data") == *original->find("data"));
        }
        CHECK(fixture.database->find(*copy)->type == ith::kLevelFormat);
        CHECK(fixture.rescanned().find(*copy) != nullptr);
    }

    TEST_CASE("duplicate refuses taken targets, other extensions and unknown ids, writing nothing") {
        Fixture fixture;
        const AssetId ship = fixture.import("Ship.objects");
        fixture.import("Taken.objects");
        fixture.project.write("Orphan.objects.meta", "{}");
        const auto code = [&](std::string_view path) {
            return Fixture::errorOf(fixture.database->duplicate(ship, path));
        };
        CHECK(code("Ship.objects") == "asset.path_conflict");
        CHECK(code("ship.objects") == "asset.path_conflict"); // its own name in another case
        CHECK(code("TAKEN.objects") == "asset.path_conflict");
        CHECK(code("Orphan.objects") == "asset.path_conflict");
        CHECK(code("Ship_2.json") == "asset.extension_changed");
        CHECK(code("../Ship_2.objects") == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->duplicate(AssetId::generate(), "X.objects")) == "asset.not_found");
        CHECK(fixture.database->records().size() == 2);
    }

    TEST_CASE("a duplicate whose file is gone or whose write fails changes nothing") {
        Fixture fixture;
        const AssetId ship = fixture.import("Ship.objects");
        const AssetId steel = fixture.native("Steel.ithasset");

#ifndef ITH_SHIPPING // the write fault hook is not in Shipping builds
        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::BeforeRename);
        const auto failed = fixture.database->duplicate(ship, "Copies/Ship_2.objects");
        const auto failedNative = fixture.database->duplicate(steel, "Steel_2.ithasset");
        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::None);
        CHECK(Fixture::errorOf(failed) == "asset.io");
        CHECK(Fixture::errorOf(failedNative) == "asset.io");
        CHECK_FALSE(fixture.project.exists("Copies/Ship_2.objects"));
        CHECK_FALSE(fixture.project.exists("Copies/Ship_2.objects.meta"));
        CHECK_FALSE(fixture.project.exists("Steel_2.ithasset"));
        CHECK(fixture.database->records().size() == 2);
#endif

        REQUIRE(ith::fs::remove(fixture.project.contentPath("Ship.objects")).hasValue());
        CHECK(Fixture::errorOf(fixture.database->duplicate(ship, "Ship_2.objects")) == "asset.not_found");
        fixture.project.write("Steel.ithasset", "not json");
        CHECK(Fixture::errorOf(fixture.database->duplicate(steel, "Steel_2.ithasset")) == "serialization.syntax");
        CHECK_FALSE(fixture.project.exists("Ship_2.objects"));
        CHECK_FALSE(fixture.project.exists("Steel_2.ithasset"));
    }
}

TEST_SUITE("asset_database.copy_in") {
    TEST_CASE("copyIn copies a source file with the files its importer reads, and doesn't import it") {
        Fixture fixture;
        const std::string external =
            fixture.writeOutside("Ship.objects", "include Parts/Hull.txt\ninclude Missing.txt\nmesh Body\n");
        fixture.writeOutside("Parts/Hull.txt", "hull bytes");
        fixture.scan();

        const auto report = fixture.database->copyIn(external, "Meshes/Ship.objects", false);
        REQUIRE(report.hasValue());
        if (!report) {
            return;
        }
        CHECK(report->id.isNil());
        CHECK(report->dependencies == std::vector<std::string>{"Meshes/Parts/Hull.txt"});
        CHECK(fixture.project.read("Meshes/Ship.objects") ==
              "include Parts/Hull.txt\ninclude Missing.txt\nmesh Body\n");
        CHECK(fixture.project.read("Meshes/Parts/Hull.txt") == "hull bytes");
        CHECK_FALSE(fixture.project.exists("Meshes/Ship.objects.meta"));
        CHECK(fixture.database->records().empty());
        CHECK(fixture.database->folders() == std::vector<std::string>{"Meshes", "Meshes/Parts"});
    }

    TEST_CASE("copyIn copies a dependency only when no entry of its name is there") {
        Fixture fixture;
        const std::string external = fixture.writeOutside("Ship.objects", "include hull.txt\ninclude b.txt\n");
        fixture.writeOutside("hull.txt", "new hull");
        fixture.writeOutside("b.txt", "b");
        fixture.project.write("Meshes/Hull.TXT", "old hull");
        const auto report = fixture.database->copyIn(external, "Meshes/Ship.objects", false);
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->dependencies == std::vector<std::string>{"Meshes/b.txt"});
        }
        CHECK(fixture.project.read("Meshes/Hull.TXT") == "old hull");
        CHECK_FALSE(fixture.project.exists("Meshes/hull.txt"));
    }

    TEST_CASE("copyIn skips dependencies that would leave the folder, are native assets or are malformed") {
        Fixture fixture;
        const std::string external = fixture.writeOutside(
            "Deep/Ship.objects", "include ../Secret.txt\ninclude Level.ithmap\ninclude Ship.objects\nmesh A\n");
        fixture.writeOutside("Secret.txt", "secret");
        fixture.writeOutside("Deep/Level.ithmap", nativeAsset(AssetId::generate(), "{}", ith::kLevelFormat));
        const auto report = fixture.database->copyIn(external, "Ship.objects", false);
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->dependencies.empty());
        }
        CHECK_FALSE(fixture.project.exists("Secret.txt"));
        CHECK_FALSE(fixture.project.exists("Level.ithmap"));

        // A source the importer will refuse is copied all the same; its import reports the error.
        const std::string broken = fixture.writeOutside("Broken.objects", "!broken\n");
        CHECK(fixture.database->copyIn(broken, "Broken.objects", false).hasValue());
        CHECK(fixture.project.exists("Broken.objects"));
    }

    TEST_CASE("copyIn copies files no importer reads as they are") {
        Fixture fixture;
        const std::string external = fixture.writeOutside("Rock.png", "png bytes");
        const auto report = fixture.database->copyIn(external, "Textures/Rock.png", false);
        REQUIRE(report.hasValue());
        CHECK(fixture.project.read("Textures/Rock.png") == "png bytes");
        CHECK(fixture.database->records().empty());
        CHECK(fixture.database->folders() == std::vector<std::string>{"Textures"});
    }

    TEST_CASE("a native asset from outside keeps its id unless the database has it") {
        Fixture fixture;
        const AssetId steel = fixture.native("Steel.ithasset");
        const AssetId fresh = AssetId::generate();
        const std::string known = fixture.writeOutside("Steel.ithasset", nativeAsset(steel, R"({"shine": 2})"));
        const std::string unknown = fixture.writeOutside("Iron.ithasset", nativeAsset(fresh));

        const auto copied = fixture.database->copyIn(unknown, "Iron.ithasset", false);
        REQUIRE(copied.hasValue());
        if (copied) {
            CHECK(copied->id == fresh);
            CHECK_FALSE(copied->newId);
        }
        CHECK(fixture.database->find(fresh)->path == "Iron.ithasset");

        const auto renewed = fixture.database->copyIn(known, "Steel_2.ithasset", false);
        REQUIRE(renewed.hasValue());
        if (!renewed) {
            return;
        }
        CHECK(renewed->newId);
        CHECK(renewed->id != steel);
        CHECK(fixture.database->find(renewed->id)->path == "Steel_2.ithasset");
        CHECK(fixture.database->find(steel)->path == "Steel.ithasset");
        const auto header = ith::readAssetHeader(fixture.project.read("Steel_2.ithasset").value_or(""));
        REQUIRE(header.hasValue());
        if (header) {
            CHECK(header->id == renewed->id);
        }
        CHECK(fixture.rescanned().records().size() == 3);
    }

    TEST_CASE("replace overwrites an imported asset's source and keeps its sidecar") {
        Fixture fixture;
        const AssetId ship = fixture.import("Ship.objects", "mesh Hull\n");
        const std::optional<std::string> sidecar = fixture.project.read("Ship.objects.meta");
        const std::string external = fixture.writeOutside("Ship.objects", "mesh Hull\nmesh Sail\n");
        const auto report = fixture.database->copyIn(external, "Ship.objects", true);
        REQUIRE(report.hasValue());
        CHECK(fixture.project.read("Ship.objects") == "mesh Hull\nmesh Sail\n");
        CHECK(fixture.project.read("Ship.objects.meta") == sidecar);
        const auto reimported = fixture.database->reimport(ship);
        REQUIRE(reimported.hasValue());
        if (reimported) {
            CHECK(reimported->id == ship);
            CHECK(reimported->subAssets.size() == 2);
        }
    }

    TEST_CASE("copyIn refusals change nothing") {
        Fixture fixture;
        fixture.import("Ship.objects");
        fixture.native("Steel.ithasset");
        fixture.project.write("Plain.objects", "mesh A\n"); // no sidecar: not an asset
        const std::string source = fixture.writeOutside("Ship.objects", "mesh B\n");
        const std::string material = fixture.writeOutside("Steel.ithasset", nativeAsset(AssetId::generate()));
        const std::string broken = fixture.writeOutside("Broken.ithasset", "{ not json");
        const auto code = [&](const std::string& external, std::string_view target, bool replace) {
            return Fixture::errorOf(fixture.database->copyIn(external, target, replace));
        };
        CHECK(code(source, "Ship.objects", false) == "asset.path_conflict");
        CHECK(code(source, "SHIP.objects", false) == "asset.path_conflict");
        CHECK(code(source, "Ship.json", false) == "asset.extension_changed");
        CHECK(code(source, "../Ship.objects", false) == "asset.invalid_path");
        CHECK(code(source, "Other.objects", true) == "asset.not_found");
        CHECK(code(source, "Plain.objects", true) == "asset.not_found");
        CHECK(code(material, "Steel.ithasset", true) == "asset.not_imported");
        CHECK(code(fixture.project.root() + "/Outside/Nothing.objects", "New.objects", false) == "asset.not_found");
        CHECK(code(broken, "Broken.ithasset", false) == "serialization.syntax");
        CHECK(fixture.project.read("Ship.objects") == "mesh Hull\n");
        CHECK(fixture.project.read("Plain.objects") == "mesh A\n");
        CHECK_FALSE(fixture.project.exists("New.objects"));
        CHECK_FALSE(fixture.project.exists("Broken.ithasset"));
        CHECK(fixture.database->records().size() == 2);
    }

    TEST_CASE("when the file can't be written, the dependencies written before it are deleted again") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Ship.objects", "mesh Hull\n");
        // The asset's file becomes a folder behind the database's back, so writing over it fails.
        REQUIRE(ith::fs::remove(fixture.project.contentPath("Meshes/Ship.objects")).hasValue());
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Meshes/Ship.objects")).hasValue());
        const std::string external = fixture.writeOutside("Ship.objects", "include Parts/Hull.txt\nmesh Hull\n");
        fixture.writeOutside("Parts/Hull.txt", "hull");

        const auto report = fixture.database->copyIn(external, "Meshes/Ship.objects", true);
        CHECK(Fixture::errorOf(report) == "asset.io");
        CHECK_FALSE(fixture.project.exists("Meshes/Parts/Hull.txt"));
        CHECK(fixture.database->find(ship) != nullptr);

#ifndef ITH_SHIPPING
        // A write fault before the first file leaves nothing either (the hook is not in Shipping).
        const std::string other = fixture.writeOutside("Other.objects", "include Parts/Hull.txt\nmesh A\n");
        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::AfterFlush);
        CHECK(Fixture::errorOf(fixture.database->copyIn(other, "Other.objects", false)) == "asset.io");
        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::None);
        CHECK_FALSE(fixture.project.exists("Other.objects"));
        CHECK_FALSE(fixture.project.exists("Parts/Hull.txt"));
#endif
    }
}

TEST_SUITE("asset_database.references") {
    TEST_CASE("AssetRef and SoftAssetRef are written in the shape collectAssetReferences finds") {
        const AssetId mesh = AssetId::generate();
        const AssetId level = AssetId::generate();
        const ith::AssetRef hard = ith::AssetRef::to(mesh, ith::SubAssetId::generate());
        const ith::SoftAssetRef soft = ith::SoftAssetRef::to(level);
        const auto hardValue = ith::serialization::toValue(ith::typeOf<ith::AssetRef>(), &hard);
        const auto softValue = ith::serialization::toValue(ith::typeOf<ith::SoftAssetRef>(), &soft);
        REQUIRE(hardValue.hasValue());
        REQUIRE(softValue.hasValue());
        if (!hardValue || !softValue) {
            return;
        }
        for (const ith::serialization::Value* value : {&*hardValue, &*softValue}) {
            REQUIRE(value->kind() == ith::serialization::Value::Kind::Object);
            REQUIRE(value->members().size() == 2);
            CHECK(value->members()[0].key == "asset");
            CHECK(value->members()[1].key == "subAsset");
        }
        CHECK(ith::collectAssetReferences(*hardValue) == std::vector<AssetId>{mesh});
        CHECK(ith::collectAssetReferences(*softValue) == std::vector<AssetId>{level});
        const ith::AssetRef none;
        const auto noneValue = ith::serialization::toValue(ith::typeOf<ith::AssetRef>(), &none);
        REQUIRE(noneValue.hasValue());
        if (noneValue) {
            CHECK(ith::collectAssetReferences(*noneValue).empty());
        }
    }

    TEST_CASE("references are found at any depth, sorted and unique, and nothing else counts") {
        const AssetId a(ith::Uuid(1, 10));
        const AssetId b(ith::Uuid(1, 11));
        const AssetId c(ith::Uuid(2, 1));
        const std::string text = R"({
            "components": [
                {"mesh": )" + referenceTo(b) +
                                 R"(},
                [[{"deep": {"asset": ")" +
                                 a.toString() + R"("}}]],
                {"asset": ")" + c.toString() +
                                 R"(", "asset": ")" + a.toString() + R"("},
                {"asset": ")" + b.toString() +
                                 R"("}
            ],
            "plain": {"asset": ")" +
                                 ith::Uuid(3, 3).toString() + R"("},
            "nil": {"asset": "00000000-0000-0000-0000-000000000000"},
            "number": {"asset": 12},
            "object": {"asset": {"id": ")" +
                                 ith::Uuid(4, 4).toString() + R"("}},
            "array": {"asset": [")" +
                                 ith::Uuid(5, 5).toString() + R"("]},
            "braces": {"asset": "{)" +
                                 ith::Uuid(6, 6).toString() + R"(}"},
            "text": {"asset": "Meshes/Ship.gltf"},
            "otherKey": {"Asset": ")" +
                                 ith::Uuid(7, 7).toString() + R"("},
            "string": ")" + ith::Uuid(8, 8).toString() +
                                 R"("
        })";
        const auto value = ith::serialization::parseJson(text);
        REQUIRE(value.hasValue());
        if (!value) {
            return;
        }
        // The member "asset" in an object counts at any depth; its object member is walked too.
        CHECK(ith::collectAssetReferences(*value) == std::vector<AssetId>{a, b, c, AssetId(ith::Uuid(3, 3))});
        CHECK(ith::collectAssetReferences(ith::serialization::Value()).empty());
        CHECK(ith::collectAssetReferences(ith::serialization::Value::string(a.toString())).empty());

        // Uppercase hexadecimal is a UUID too (Uuid::parse).
        ith::serialization::Value upper = ith::serialization::Value::object();
        std::string upperText = a.toString();
        std::ranges::transform(upperText, upperText.begin(), [](char character) {
            return character >= 'a' && character <= 'f' ? static_cast<char>(character - 'a' + 'A') : character;
        });
        upper.set("asset", ith::serialization::Value::string(upperText));
        CHECK(ith::collectAssetReferences(upper) == std::vector<AssetId>{a});
    }

    TEST_CASE("a value deeper than the parser allows is walked without recursion") {
        ith::serialization::Value value = ith::serialization::Value::object();
        const AssetId id = AssetId::generate();
        value.set("asset", ith::serialization::Value::string(id.toString()));
        for (int level = 0; level < 1000; ++level) {
            ith::serialization::Value outer = ith::serialization::Value::array();
            outer.elements().push_back(std::move(value));
            value = std::move(outer);
        }
        CHECK(ith::collectAssetReferences(value) == std::vector<AssetId>{id});
        // Taken apart one level at a time: destroying the value recurses once per level.
        while (value.kind() == ith::serialization::Value::Kind::Array) {
            ith::serialization::Value inner = std::move(value.elements().front());
            value = std::move(inner);
        }
    }

    TEST_CASE("findUsers lists the native assets and sidecars outside the ids that refer to each id") {
        Fixture fixture;
        const AssetId ship = fixture.import("Meshes/Ship.objects");
        const AssetId rock = fixture.import("Meshes/Rock.objects");
        const AssetId unused = fixture.import("Meshes/Unused.objects");
        // A level that uses the ship and a sub-asset of the rock.
        const AssetId level = fixture.native(
            "Maps/Harbour.ithasset", R"({"entities": [{"mesh": )" + referenceTo(ship) + R"(}, {"mesh": {"asset": ")" +
                                         rock.toString() + R"(", "subAsset": ")" +
                                         ith::SubAssetId::generate().toString() + R"("}}]})");
        // A material that uses the ship, and is itself deleted with it.
        const AssetId material =
            fixture.native("Materials/Steel.ithasset", R"({"preview": )" + referenceTo(ship) + "}");
        // A sidecar whose overrides use the ship.
        const AssetId barge = fixture.import("Meshes/Barge.objects");
        const std::string bargeSidecar = fixture.project.read("Meshes/Barge.objects.meta").value_or("");
        auto sidecar = ith::readSidecar(bargeSidecar);
        REQUIRE(sidecar.hasValue());
        if (!sidecar) {
            return;
        }
        sidecar->overrides.set("hull", *ith::serialization::parseJson(referenceTo(ship)));
        const auto rewritten = ith::writeSidecar(*sidecar);
        REQUIRE(rewritten.hasValue());
        fixture.project.write("Meshes/Barge.objects.meta", rewritten ? *rewritten : std::string());
        // A level that refers to itself is no user of itself.
        const AssetId self = AssetId::generate();
        fixture.project.write("Maps/Self.ithasset", nativeAsset(self, R"({"me": )" + referenceTo(self) + "}"));
        fixture.scan();

        const std::vector<AssetId> ids = {ship, rock, unused, material, self};
        const ith::FindUsersReport report = fixture.database->findUsers(ids);
        CHECK(report.unreadable.empty());
        REQUIRE(report.uses.size() == ids.size());
        CHECK(report.uses[0].id == ship);
        CHECK(report.uses[0].users == std::vector<AssetId>{level, barge}); // by path: Maps/, Meshes/
        CHECK(report.uses[1].id == rock);
        CHECK(report.uses[1].users == std::vector<AssetId>{level});
        CHECK(report.uses[2].users.empty());
        CHECK(report.uses[3].users.empty());
        CHECK(report.uses[4].users.empty());

        // Without the material in the set, it is a user of the ship.
        const std::vector<AssetId> shipOnly = {ship};
        const ith::FindUsersReport alone = fixture.database->findUsers(shipOnly);
        REQUIRE(alone.uses.size() == 1);
        CHECK(alone.uses[0].users == std::vector<AssetId>{level, material, barge});
        CHECK(fixture.database->findUsers({}).uses.empty());
    }

    TEST_CASE("findUsers lists files it can't read and goes on") {
        Fixture fixture;
        const AssetId ship = fixture.import("Ship.objects");
        const AssetId user = fixture.native("User.ithasset", R"({"mesh": )" + referenceTo(ship) + "}");
        const AssetId broken = fixture.native("Broken.ithasset");
        const AssetId gone = fixture.native("Gone.ithasset");
        fixture.project.write("Broken.ithasset", "{ not json");
        REQUIRE(ith::fs::remove(fixture.project.contentPath("Gone.ithasset")).hasValue());
        const std::vector<AssetId> ids = {ship};
        const ith::FindUsersReport report = fixture.database->findUsers(ids);
        REQUIRE(report.uses.size() == 1);
        CHECK(report.uses[0].users == std::vector<AssetId>{user});
        CHECK(pathsOf(report.unreadable) == std::vector<std::string>{"Broken.ithasset", "Gone.ithasset"});
        if (report.unreadable.size() == 2) {
            CHECK(report.unreadable[0].error.code == "serialization.syntax");
            CHECK(report.unreadable[1].error.code == "asset.io");
        }
        CHECK(fixture.database->find(broken) != nullptr);
        CHECK(fixture.database->find(gone) != nullptr);
    }
}

TEST_SUITE("asset_database.names") {
    TEST_CASE("findCaseVariant finds any entry by name without ASCII case") {
        Fixture fixture;
        fixture.import("Meshes/Ship.objects");
        fixture.project.write("Meshes/notes.txt", "hidden");
        const std::string meshes = fixture.project.contentPath("Meshes");
        const auto find = [&](std::string_view name) {
            const auto variant = ith::findCaseVariant(meshes, name);
            REQUIRE(variant.hasValue());
            return variant ? variant->value_or("-") : std::string("error");
        };
        CHECK(find("Ship.objects") == "Ship.objects");
        CHECK(find("SHIP.OBJECTS") == "Ship.objects");
        CHECK(find("ship.objects.meta") == "Ship.objects.meta");
        CHECK(find("Notes.txt") == "notes.txt");
        CHECK(find("Other") == "-");
        CHECK(find("Ünïcode") == "-");
        const auto missing = ith::findCaseVariant(fixture.project.contentPath("Nowhere"), "x");
        REQUIRE(missing.hasValue());
        CHECK_FALSE(missing->has_value());
        const auto file = ith::findCaseVariant(fixture.project.contentPath("Meshes/notes.txt"), "x");
        REQUIRE(file.hasValue());
        CHECK_FALSE(file->has_value());
    }
}

TEST_SUITE("asset_database.safety") {
    TEST_CASE("moveFolder and removeFolder take an element of folders() as their argument") {
        Fixture fixture;
        // Short names, held inside the strings themselves: an insertion into the list that moves
        // its elements would change what a reference into it reads.
        const std::vector<std::string> names = {"a", "b", "c", "d", "e", "f", "g", "h"};
        std::vector<AssetId> ids;
        for (const std::string& name : names) {
            ids.push_back(fixture.import(name + "/x.objects"));
        }
        fixture.scan();
        const std::size_t index = 5;
        REQUIRE(fixture.database->folders()[index] == "f");
        REQUIRE(fixture.database->moveFolder(fixture.database->folders()[index], "Fresh/Sub").hasValue());
        CHECK(fixture.database->find(ids[index])->path == "Fresh/Sub/x.objects");
        CHECK(fixture.database->find(ids[4])->path == "e/x.objects");
        CHECK(fixture.project.exists("Fresh/Sub/x.objects"));
        CHECK_FALSE(fixture.folderExists("f"));
        CHECK(fixture.database->folders() ==
              std::vector<std::string>{"Fresh", "Fresh/Sub", "a", "b", "c", "d", "e", "g", "h"});
        CHECK(fixture.rescanned().folders() == fixture.database->folders());

        const auto removed = fixture.database->removeFolder(fixture.database->folders()[0]);
        REQUIRE(removed.hasValue());
        CHECK_FALSE(fixture.folderExists("Fresh"));
        CHECK(fixture.database->find(ids[index]) == nullptr);
    }

    TEST_CASE("moveFolder drops stale entries at its target, so no folder is listed twice") {
        Fixture fixture;
        REQUIRE(fixture.database->createFolder("B/X").hasValue());
        REQUIRE(fixture.database->createFolder("A/Y").hasValue());
        // B goes outside the database; folders() still lists it.
        REQUIRE(ith::fs::remove(fixture.project.contentPath("B/X")).hasValue());
        REQUIRE(ith::fs::remove(fixture.project.contentPath("B")).hasValue());
        REQUIRE(fixture.database->moveFolder("A", "B").hasValue());
        CHECK(fixture.database->folders() == std::vector<std::string>{"B", "B/Y"});
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("paths through a link to a folder are refused and nothing outside changes") {
        Fixture fixture;
        const std::filesystem::path outside = fixture.project.directory() / "Outside";
        fixture.writeOutside("Docs/Keep.txt", "outside");
        const AssetId ship = fixture.import("Ship.objects");
        REQUIRE(fixture.database->createFolder("Real").hasValue());
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, fixture.project.content("Ext"), error);
        if (error) {
            MESSAGE("symbolic links can't be created here; skipped");
            return;
        }
        const std::string external = fixture.writeOutside("New.objects", "mesh A\n");
        CHECK(Fixture::errorOf(fixture.database->removeFolder("Ext/Docs")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->removeFolder("Ext")) == "asset.not_found"); // the link itself
        CHECK(Fixture::errorOf(fixture.database->createFolder("Ext/New")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->moveFolder("Ext/Docs", "Moved")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->moveFolder("Real", "Ext/Real")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->duplicate(ship, "Ext/Copy.objects")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->copyIn(external, "Ext/Copy.objects", false)) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->move(ship, "Ext/Ship.objects")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->import("Ext/New.objects")) == "asset.not_found");

        CHECK(std::filesystem::exists(outside / "Docs" / "Keep.txt"));
        CHECK_FALSE(std::filesystem::exists(outside / "New"));
        CHECK_FALSE(std::filesystem::exists(outside / "Real"));
        CHECK_FALSE(std::filesystem::exists(outside / "Copy.objects"));
        CHECK_FALSE(std::filesystem::exists(outside / "Ship.objects"));
        CHECK_FALSE(std::filesystem::exists(outside / "New.objects.meta"));
        CHECK(fixture.folderExists("Real"));
        CHECK(fixture.project.exists("Ship.objects"));
    }

    TEST_CASE("names the file system folds beyond ASCII case are taken where it folds them") {
        Fixture fixture;
        const AssetId steel = fixture.native("été.ithasset");
        const AssetId ship = fixture.import("Ship.objects");
        REQUIRE(fixture.database->createFolder("ébène").hasValue());
        const std::string external = fixture.writeOutside("Other.ithasset", nativeAsset(AssetId::generate()));
        std::error_code error;
        const bool folds = std::filesystem::exists(fixture.project.content("Été.ithasset"), error);
        const auto duplicated = fixture.database->duplicate(steel, "Été.ithasset");
        const auto copied = fixture.database->copyIn(external, "ÉTÉ.ithasset", false);
        const auto created = fixture.database->createFolder("Ébène");
        const auto moved = fixture.database->moveFolder("ébène", "Ébène");
        const auto shipMoved = fixture.database->move(ship, "Ébène/Ship.objects");
        if (folds) { // Windows and macOS: the names are aliases of the existing entries
            CHECK(Fixture::errorOf(duplicated) == "asset.path_conflict");
            CHECK(Fixture::errorOf(copied) == "asset.path_conflict");
            CHECK(Fixture::errorOf(created) == "asset.path_conflict");
            CHECK(Fixture::errorOf(moved) == "asset.path_conflict");
            CHECK(Fixture::errorOf(shipMoved) == "asset.path_conflict");
            CHECK(fixture.database->records().size() == 2);
            CHECK(fixture.database->folders() == std::vector<std::string>{"ébène"});
        } else { // Linux: distinct names
            CHECK(duplicated.hasValue());
            CHECK(copied.hasValue());
            CHECK(created.hasValue());
            CHECK(Fixture::errorOf(moved) == "asset.path_conflict"); // "Ébène" exists now
            CHECK(shipMoved.hasValue());
            CHECK(fixture.database->records().size() == 4);
        }
        CHECK(fixture.database->find(steel)->path == "été.ithasset");
        const auto header = ith::readAssetHeader(fixture.project.read("été.ithasset").value_or(""));
        REQUIRE(header.hasValue());
        if (header) {
            CHECK(header->id == steel);
        }
        // The name rule sees the alias too.
        const auto found = ith::findCaseVariant(fixture.project.contentPath(""), "ÉTÉ.ithasset");
        REQUIRE(found.hasValue());
        if (found) {
            CHECK(found->has_value() == (folds || copied.hasValue()));
        }
    }

    TEST_CASE("copyIn refuses a native document whose format doesn't fit the extension") {
        Fixture fixture;
        const std::string material = fixture.writeOutside("Steel.ithmap", nativeAsset(AssetId::generate()));
        const std::string level =
            fixture.writeOutside("Harbour.ithasset", nativeAsset(AssetId::generate(), "{}", ith::kLevelFormat));
        CHECK(Fixture::errorOf(fixture.database->copyIn(material, "Steel.ithmap", false)) == "asset.format_mismatch");
        CHECK(Fixture::errorOf(fixture.database->copyIn(level, "Harbour.ithasset", false)) == "asset.format_mismatch");
        CHECK_FALSE(fixture.project.exists("Steel.ithmap"));
        CHECK_FALSE(fixture.project.exists("Harbour.ithasset"));
        CHECK(fixture.database->records().empty());
    }

    TEST_CASE("every name of the longest accepted size can be written with its sidecar") {
        Fixture fixture;
        const std::string longest = std::string(ith::kMaxContentNameSize - 8, 'a') + ".objects";
        REQUIRE(longest.size() == ith::kMaxContentNameSize);
        const AssetId id = fixture.import("Long/" + longest);
        const std::string other = std::string(ith::kMaxContentNameSize - 8, 'b') + ".objects";
        CHECK(fixture.database->duplicate(id, "Long/" + other).hasValue());
        CHECK(fixture.project.exists("Long/" + other + ".meta"));
        CHECK(fixture.database->createFolder(std::string(ith::kMaxContentNameSize, 'c')).hasValue());
        CHECK(Fixture::errorOf(fixture.database->duplicate(id, "Long/b" + other)) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->createFolder(std::string(ith::kMaxContentNameSize + 1, 'c'))) ==
              "asset.invalid_path");
        CHECK(fixture.rescanned().records().size() == 2);
    }
}

TEST_SUITE("asset_database.safety") {
    TEST_CASE("moveFolder drops stale records at its target") {
        Fixture fixture;
        const AssetId stale = fixture.import("B/x.objects");
        const AssetId moving = fixture.import("A/y.objects");
        fixture.scan();
        // B goes outside the database; its record stays until a scan.
        REQUIRE(ith::fs::remove(fixture.project.contentPath("B/x.objects")).hasValue());
        REQUIRE(ith::fs::remove(fixture.project.contentPath("B/x.objects.meta")).hasValue());
        REQUIRE(ith::fs::remove(fixture.project.contentPath("B")).hasValue());
        REQUIRE(fixture.database->moveFolder("A", "B").hasValue());
        CHECK(fixture.database->find(stale) == nullptr);
        CHECK(fixture.database->find(moving)->path == "B/y.objects");
        CHECK(fixture.database->records().size() == 1);
        CHECK(fixture.database->folders() == std::vector<std::string>{"B"});
    }

    TEST_CASE("an asset whose folder became a link after the scan is not moved, deleted or given an id") {
        Fixture fixture;
        const AssetId ship = fixture.import("Sub/Ship.objects");
        fixture.scan();
        const std::filesystem::path outside = fixture.project.directory() / "Outside" / "Sub";
        std::error_code error;
        std::filesystem::create_directories(outside.parent_path(), error);
        REQUIRE_FALSE(error);
        std::filesystem::rename(fixture.project.content("Sub"), outside, error);
        REQUIRE_FALSE(error);
        std::filesystem::create_directory_symlink(outside, fixture.project.content("Sub"), error);
        if (error) {
            MESSAGE("symbolic links can't be created here; skipped");
            return;
        }
        CHECK(Fixture::errorOf(fixture.database->remove(ship)) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->move(ship, "Ship.objects")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->rename(ship, "Boat.objects")) == "asset.invalid_path");
        CHECK(Fixture::errorOf(fixture.database->assignNewId("Sub/Ship.objects")) == "asset.invalid_path");
        CHECK(std::filesystem::exists(outside / "Ship.objects"));
        CHECK(std::filesystem::exists(outside / "Ship.objects.meta"));
        CHECK_FALSE(fixture.project.exists("Ship.objects"));
        CHECK(fixture.database->find(ship) != nullptr);
    }

    TEST_CASE("Windows device names with superscript digits and the console names are refused") {
        for (const std::string_view name : {"COM\xC2\xB9", "com\xC2\xB2.txt", "LPT\xC2\xB3", "lpt\xC2\xB9.objects",
                                            "CONIN$", "conin$.txt", "CONOUT$", "Conout$.objects"}) {
            CAPTURE(name);
            CHECK_FALSE(ith::isValidContentPath(name));
            CHECK_FALSE(ith::isValidContentPath("Folder/" + std::string(name)));
        }
        // Other superscripts, other numbers and longer names are ordinary names.
        for (const std::string_view name : {"COM\xE2\x81\xB4", "COM0", "COM10", "CONIN", "CONOUT$1", "LPT\xC2\xB9x"}) {
            CAPTURE(name);
            CHECK(ith::isValidContentPath(name));
        }
    }
}

#ifndef _WIN32
TEST_SUITE("asset_database.posix") {
    TEST_CASE("a name holding a backslash is reported and never followed") {
        // "..\.." is a legal POSIX name; joined into a path, the file system would read it as two
        // steps up, out of Content.
        Fixture fixture;
        const std::string canary = fixture.project.root() + "/canary.txt";
        REQUIRE(ith::fs::writeFileAtomic(canary, "alive").hasValue());
        fixture.project.write("Pack/Keep.txt", "x");
        const std::filesystem::path escape = fixture.project.content("Pack") / "..\\..";
        REQUIRE(::mkdir(escape.c_str(), 0755) == 0);
        const std::string inner = (escape / "inner.txt").string();
        const int descriptor = ::open(inner.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        REQUIRE(descriptor >= 0);
        ::close(descriptor);

        const auto problems = fixture.database->scan();
        REQUIRE(problems.hasValue());
        if (problems) {
            REQUIRE(problems->size() == 1);
            CHECK((*problems)[0].kind == AssetProblemKind::InvalidFile);
            CHECK((*problems)[0].path == "Pack");
        }
        CHECK(fixture.database->folders() == std::vector<std::string>{"Pack"});

        const auto report = fixture.database->removeFolder("Pack");
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(pathsOf(report->failures) == std::vector<std::string>{"Pack", "Pack"});
        }
        CHECK(ith::fs::readFile(canary, 64).valueOr(std::string()) == "alive");
        CHECK(std::filesystem::exists(inner));
        CHECK_FALSE(fixture.project.exists("Pack/Keep.txt"));
        ::unlink(inner.c_str());
        ::rmdir(escape.c_str());
    }

    TEST_CASE("removeFolder pairs a sidecar with its exact file before one in another case") {
        Fixture fixture;
        const AssetId lower = fixture.import("Pack/a.objects");
        // import() refuses a sidecar in another case, so the second pair is copied in and scanned:
        // the copy gets a new id.
        fixture.project.write("Pack/A.objects", "mesh Hull\n");
        fixture.project.write("Pack/A.objects.meta", fixture.project.read("Pack/a.objects.meta").value_or(""));
        REQUIRE(fixture.database->scan().hasValue()); // reports the case conflict and the copy
        const ith::AssetRecord* copy = fixture.database->findByPath("Pack/A.objects");
        REQUIRE(copy != nullptr);
        const AssetId upper = copy != nullptr ? copy->id : AssetId();
        CHECK(upper != lower);
        const auto report = fixture.database->removeFolder("Pack");
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->failures.empty());
            CHECK(report->orphans.empty());
        }
        CHECK_FALSE(fixture.folderExists("Pack"));
        CHECK(fixture.database->find(lower) == nullptr);
        CHECK(fixture.database->find(upper) == nullptr);
        CHECK(fixture.database->folders().empty());
    }
}
#endif
