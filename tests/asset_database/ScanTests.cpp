// Scanning the Content folder, the problems it reports, and move, rename and delete (ADR-0015
// rules 5 and 6, ADR-0024).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Sidecar.h"

#include "AssetTestSupport.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using ith::AssetProblemKind;
using ith::test::ObjectsImporter;
using ith::test::TempProject;

std::string nativeAsset(ith::AssetId id, std::string_view format = "ith.material") {
    return R"({"format": ")" + std::string(format) + R"(", "version": 1, "id": ")" + id.toString() +
           R"(", "data": {}})";
}

struct Fixture {
    TempProject project;
    ith::ImporterRegistry importers;
    std::unique_ptr<ith::AssetDatabase> database;

    Fixture() {
        importers.add(std::make_unique<ObjectsImporter>());
        database = std::make_unique<ith::AssetDatabase>(project.root(), importers);
    }

    ith::AssetId import(std::string_view path, std::string_view source = "mesh Hull\n") {
        project.write(path, source);
        const auto report = database->import(path);
        REQUIRE(report.hasValue());
        return report ? report->id : ith::AssetId();
    }

    std::vector<ith::AssetProblem> scan() {
        auto problems = database->scan();
        REQUIRE(problems.hasValue());
        return problems ? std::move(*problems) : std::vector<ith::AssetProblem>{};
    }

    // A fresh database over the same project, scanned: what another session would see.
    ith::AssetDatabase rescanned() const {
        ith::AssetDatabase fresh(project.root(), importers);
        CHECK(fresh.scan().hasValue());
        return fresh;
    }

    std::string errorOf(const ith::Expected<void>& result) const {
        return result ? std::string() : result.error().code;
    }
};

std::vector<AssetProblemKind> kindsOf(const std::vector<ith::AssetProblem>& problems) {
    std::vector<AssetProblemKind> kinds;
    for (const ith::AssetProblem& problem : problems) {
        kinds.push_back(problem.kind);
    }
    return kinds;
}

// Creates a symbolic link; false when the system doesn't allow it (Windows without the privilege).
bool tryCreateSymlink(const std::filesystem::path& target, const std::filesystem::path& link, bool folder) {
    std::error_code error;
    if (folder) {
        std::filesystem::create_directory_symlink(target, link, error);
    } else {
        std::filesystem::create_symlink(target, link, error);
    }
    return !error;
}

} // namespace

TEST_SUITE("asset_database.scan") {
    TEST_CASE("a scan finds native assets, levels and imported files") {
        Fixture fixture;
        const ith::AssetId material = ith::AssetId::generate();
        const ith::AssetId level = ith::AssetId::generate();
        fixture.project.write("Materials/Steel.ithasset", nativeAsset(material));
        fixture.project.write("Maps/Harbour.ITHMAP", nativeAsset(level, ith::kLevelFormat));
        const ith::AssetId ship = fixture.import("Meshes/Ship.objects", "mesh Hull\nmesh Sail\n");
        fixture.project.write("Readme.txt", "not an asset");
        fixture.project.write("Data.json", R"({"format": "x", "version": 1, "id": "", "data": {}})");

        const ith::AssetDatabase database = fixture.rescanned();
        const std::vector<const ith::AssetRecord*> records = database.records();
        REQUIRE(records.size() == 3);
        CHECK(records[0]->path == "Maps/Harbour.ITHMAP");
        CHECK(records[0]->id == level);
        CHECK(records[0]->type == ith::kLevelFormat);
        CHECK(records[1]->path == "Materials/Steel.ithasset");
        CHECK(records[1]->id == material);
        CHECK(records[1]->type == "ith.material");
        CHECK_FALSE(records[1]->imported);
        CHECK(records[2]->path == "Meshes/Ship.objects");
        CHECK(records[2]->id == ship);
        CHECK(records[2]->imported);
        CHECK(records[2]->subAssets.size() == 2);
        // A plain .json file is a source file like any other, ignored without an importer.
        CHECK(fixture.scan().empty());
    }

    TEST_CASE("a native asset whose extension doesn't fit its format is reported but kept") {
        Fixture fixture;
        const ith::AssetId level = ith::AssetId::generate();
        const ith::AssetId material = ith::AssetId::generate();
        fixture.project.write("Harbour.ithasset", nativeAsset(level, ith::kLevelFormat));
        fixture.project.write("Steel.ithmap", nativeAsset(material));
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        CHECK(kindsOf(problems) ==
              std::vector<AssetProblemKind>{AssetProblemKind::FormatMismatch, AssetProblemKind::FormatMismatch});
        REQUIRE(problems.size() == 2);
        CHECK(problems[0].path == "Harbour.ithasset");
        CHECK(problems[1].path == "Steel.ithmap");
        CHECK(fixture.database->records().size() == 2);

        CHECK(ith::formatMatchesExtension("a.ithmap", ith::kLevelFormat));
        CHECK(ith::formatMatchesExtension("a.IthAsset", "test.prefab"));
        CHECK_FALSE(ith::formatMatchesExtension("a.ITHMAP", "test.prefab"));
        CHECK_FALSE(ith::formatMatchesExtension("a.ithasset", ith::kLevelFormat));
        CHECK(ith::isNativeAssetPath("Maps/A.IthMap"));
        CHECK_FALSE(ith::isNativeAssetPath("A.json"));
    }

    TEST_CASE("a scan fails when Content can't be listed") {
        TempProject project;
        ith::ImporterRegistry importers;
        ith::AssetDatabase database(project.root() + "/Nowhere", importers);
        const auto problems = database.scan();
        REQUIRE_FALSE(problems.hasValue());
        if (!problems) {
            CHECK(problems.error().code == "asset.io");
        }
        CHECK(database.folders().empty());
    }

    TEST_CASE("a scan lists every folder below Content, empty ones included, sorted") {
        Fixture fixture;
        CHECK(fixture.database->folders().empty()); // until the first scan
        fixture.import("Meshes/Ship.objects");
        fixture.project.write("Levels/Main.ithmap", nativeAsset(ith::AssetId::generate(), ith::kLevelFormat));
        fixture.project.write("Docs/notes.txt", "not an asset");
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Meshes/Rocks/Small")).hasValue());
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath("Empty")).hasValue());
        CHECK(fixture.scan().empty());
        CHECK(fixture.database->folders() ==
              std::vector<std::string>{"Docs", "Empty", "Levels", "Meshes", "Meshes/Rocks", "Meshes/Rocks/Small"});

        // A folder deleted outside the database goes at the next scan.
        REQUIRE(ith::fs::remove(fixture.project.contentPath("Empty")).hasValue());
        CHECK(fixture.scan().empty());
        CHECK(fixture.database->folders() ==
              std::vector<std::string>{"Docs", "Levels", "Meshes", "Meshes/Rocks", "Meshes/Rocks/Small"});
    }

    TEST_CASE("an import and a new id add the folders of their file") {
        Fixture fixture;
        fixture.import("New/Deep/Ship.objects"); // no scan yet
        CHECK(fixture.database->folders() == std::vector<std::string>{"New", "New/Deep"});
        // A folder made outside the database after the scan comes with the file given a new id there.
        CHECK(fixture.scan().empty());
        fixture.project.write("Later/Steel.ithasset", nativeAsset(ith::AssetId::generate()));
        REQUIRE(fixture.database->assignNewId("Later/Steel.ithasset").hasValue());
        CHECK(fixture.database->folders() == std::vector<std::string>{"Later", "New", "New/Deep"});
    }

    TEST_CASE("folders deeper than kMaxFolderDepth are reported, not entered") {
        Fixture fixture;
        std::string path;
        for (std::size_t level = 0; level < ith::kMaxFolderDepth + 1; ++level) {
            path += (path.empty() ? "d" : "/d");
        }
        // A file at the deepest allowed level is found; the folder below it is a problem.
        std::string allowed = path.substr(0, path.size() - 2);
        fixture.project.write(allowed + "/Steel.ithasset", nativeAsset(ith::AssetId::generate()));
        REQUIRE(ith::fs::createDirectories(fixture.project.contentPath(path + "/e")).hasValue());
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].kind == AssetProblemKind::InvalidFile);
        CHECK(problems[0].path == path);
        CHECK(fixture.database->folders().size() == ith::kMaxFolderDepth);
        CHECK(fixture.database->findByPath(allowed + "/Steel.ithasset") != nullptr);
    }

    TEST_CASE("a move adds the folders it creates to the list") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("Ship.objects");
        CHECK(fixture.scan().empty());
        CHECK(fixture.database->folders().empty());
        REQUIRE(fixture.database->move(id, "Vehicles/Sea/Ship.objects").hasValue());
        CHECK(fixture.database->folders() == std::vector<std::string>{"Vehicles", "Vehicles/Sea"});
        REQUIRE(fixture.database->move(id, "Vehicles/Air/Ship.objects").hasValue());
        // The emptied folder stays: it is still on disk.
        CHECK(fixture.database->folders() == std::vector<std::string>{"Vehicles", "Vehicles/Air", "Vehicles/Sea"});
        CHECK(fixture.rescanned().folders() == fixture.database->folders());
    }

    TEST_CASE("a file copied with its sidecar gets a new id when the database knows the original") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("B/Ship.objects");
        CHECK(fixture.scan().empty());
        // The copy sorts first, but the database knows where the original is.
        fixture.project.write("A/Ship.objects", fixture.project.read("B/Ship.objects").value_or(""));
        fixture.project.write("A/Ship.objects.meta", fixture.project.read("B/Ship.objects.meta").value_or(""));

        const std::vector<ith::AssetProblem> problems = fixture.scan();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].kind == AssetProblemKind::DuplicateId);
        CHECK(problems[0].path == "A/Ship.objects");
        CHECK(problems[0].otherPath == "B/Ship.objects");
        const ith::AssetRecord* original = fixture.database->findByPath("B/Ship.objects");
        const ith::AssetRecord* copy = fixture.database->findByPath("A/Ship.objects");
        REQUIRE(original != nullptr);
        REQUIRE(copy != nullptr);
        if (original == nullptr || copy == nullptr) {
            return;
        }
        CHECK(original->id == id);
        CHECK(copy->id != id);
        // The new id is written to the copy's sidecar, so the next scan is clean.
        const auto sidecar = ith::readSidecar(fixture.project.read("A/Ship.objects.meta").value_or(""));
        REQUIRE(sidecar.hasValue());
        CHECK(sidecar->id == copy->id);
        // A scan rebuilds the records, so keep the id, not the pointer.
        const ith::AssetId copyId = copy->id;
        CHECK(fixture.scan().empty());
        CHECK(fixture.database->findByPath("A/Ship.objects")->id == copyId);
    }

    TEST_CASE("a cold scan reports a duplicated id, writes nothing and leaves the second file out") {
        Fixture fixture;
        const ith::AssetId id = ith::AssetId::generate();
        fixture.project.write("b.ithasset", nativeAsset(id));
        fixture.project.write("a.ithasset", nativeAsset(id));
        const std::optional<std::string> before = fixture.project.read("b.ithasset");

        const std::vector<ith::AssetProblem> problems = fixture.scan();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].kind == AssetProblemKind::DuplicateId);
        CHECK(problems[0].path == "b.ithasset");
        CHECK(problems[0].otherPath == "a.ithasset");
        CHECK(fixture.database->findByPath("a.ithasset")->id == id);
        CHECK(fixture.database->findByPath("b.ithasset") == nullptr);
        CHECK(fixture.project.read("b.ithasset") == before);
        // The same on the next scan: nothing was decided for the user.
        CHECK(fixture.scan().size() == 1);

        // The user gives the copy a new id.
        const ith::Expected<ith::AssetId> newId = fixture.database->assignNewId("b.ithasset");
        REQUIRE(newId.hasValue());
        if (!newId) {
            return;
        }
        CHECK(*newId != id);
        CHECK(fixture.database->findByPath("b.ithasset")->id == *newId);
        const auto header = ith::readAssetHeader(fixture.project.read("b.ithasset").value_or(""));
        REQUIRE(header.hasValue());
        CHECK(header->id == *newId);
        CHECK(fixture.scan().empty());
    }

    TEST_CASE("assignNewId works on imported files and refuses other files") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("Ship.objects");
        const ith::Expected<ith::AssetId> newId = fixture.database->assignNewId("Ship.objects");
        REQUIRE(newId.hasValue());
        CHECK(newId.valueOr(id) != id);
        CHECK(fixture.database->find(id) == nullptr);
        CHECK(fixture.rescanned().findByPath("Ship.objects")->id == newId.valueOr(id));

        fixture.project.write("Loose.objects", "mesh A\n");
        CHECK(fixture.database->assignNewId("Loose.objects").error().code == "asset.not_found");
        CHECK(fixture.database->assignNewId("Missing.ithasset").error().code == "asset.not_found");
        CHECK(fixture.database->assignNewId("../x.ithasset").error().code == "asset.invalid_path");
    }

    TEST_CASE("orphan sidecars, files without sidecars and unreadable files are listed") {
        Fixture fixture;
        fixture.import("Gone.objects");
        std::error_code error;
        std::filesystem::remove(fixture.project.content("Gone.objects"), error);
        fixture.project.write("New.objects", "mesh Hull\n");
        fixture.project.write("Bad.ithasset", "{ not json");
        fixture.project.write("Bad.objects", "mesh Hull\n");
        fixture.project.write("Bad.objects.meta",
                              R"({"format": "ith.asset_meta", "version": 9, "id": "", "data": {}})");

        const std::vector<ith::AssetProblem> problems = fixture.scan();
        CHECK(kindsOf(problems) ==
              std::vector<AssetProblemKind>{AssetProblemKind::InvalidFile, AssetProblemKind::InvalidFile,
                                            AssetProblemKind::OrphanSidecar, AssetProblemKind::MissingSidecar});
        REQUIRE(problems.size() == 4);
        CHECK(problems[0].path == "Bad.ithasset");
        CHECK(problems[1].path == "Bad.objects.meta");
        CHECK(problems[1].message.find("serialization.newer_version") != std::string::npos);
        CHECK(problems[2].path == "Gone.objects.meta");
        CHECK(problems[3].path == "New.objects");
        // A scan doesn't create sidecars; the file gets one when it is imported.
        CHECK_FALSE(fixture.project.exists("New.objects.meta"));
        CHECK(fixture.database->records().empty());
        CHECK(fixture.database->import("New.objects").hasValue());
        CHECK(fixture.project.exists("New.objects.meta"));
    }

    TEST_CASE("leftover temporary files are reported") {
        Fixture fixture;
        fixture.import("Rock.objects");
        fixture.project.write("Rock.objects.meta.tmp-" + ith::Uuid::generate().toString(), "{");
        fixture.project.write("Notes.tmp-not-a-uuid", "kept quiet");
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].kind == AssetProblemKind::InvalidFile);
        CHECK(problems[0].path.starts_with("Rock.objects.meta.tmp-"));
        CHECK(fixture.database->records().size() == 1);
    }

    TEST_CASE("a sidecar whose name differs from its file's in case is a conflict, not a pair") {
        Fixture fixture;
        fixture.import("Rock.objects");
        std::error_code error;
        std::filesystem::rename(fixture.project.content("Rock.objects.meta"),
                                fixture.project.content("rock.objects.meta"), error);
        REQUIRE_FALSE(error);
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        CHECK(kindsOf(problems) == std::vector<AssetProblemKind>{AssetProblemKind::MissingSidecar,
                                                                 AssetProblemKind::OrphanSidecar,
                                                                 AssetProblemKind::CaseConflict});
        REQUIRE(problems.size() == 3);
        CHECK(problems[2].path == "rock.objects.meta");
        CHECK(problems[2].otherPath == "Rock.objects");
        CHECK(fixture.database->records().empty());

        // Importing would replace or duplicate that sidecar, so it is refused until it is renamed.
        const auto imported = fixture.database->import("Rock.objects");
        REQUIRE_FALSE(imported.hasValue());
        if (!imported) {
            CHECK(imported.error().code == "asset.path_conflict");
        }
        CHECK(fixture.project.exists("rock.objects.meta"));
        CHECK_FALSE(fixture.project.exists("Rock.objects.meta"));
    }

    TEST_CASE("a sidecar suffix in another case is not a sidecar") {
        Fixture fixture;
        fixture.import("Rock.objects");
        std::error_code error;
        std::filesystem::rename(fixture.project.content("Rock.objects.meta"),
                                fixture.project.content("Rock.objects.META"), error);
        REQUIRE_FALSE(error);
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        CHECK(kindsOf(problems) == std::vector<AssetProblemKind>{AssetProblemKind::MissingSidecar,
                                                                 AssetProblemKind::OrphanSidecar,
                                                                 AssetProblemKind::CaseConflict});
        CHECK(fixture.database->records().empty());
    }

    TEST_CASE("paths that differ only in case are found") {
        const std::vector<std::string> paths = {"Meshes", "Meshes/Rock.gltf", "meshes", "meshes/rock.gltf", "Other"};
        const auto conflicts = ith::findCaseConflicts(paths);
        REQUIRE(conflicts.size() == 2);
        CHECK(conflicts[0] == std::pair<std::string, std::string>{"Meshes", "meshes"});
        CHECK(conflicts[1] == std::pair<std::string, std::string>{"Meshes/Rock.gltf", "meshes/rock.gltf"});
        CHECK(ith::findCaseConflicts({"a", "b", "c"}).empty());
        // Only ASCII letters are folded.
        CHECK(ith::findCaseConflicts({"\xc3\x89"
                                      "corce.png",
                                      "\xc3\xa9"
                                      "corce.png"})
                  .empty());
    }

    TEST_CASE("valid content paths") {
        CHECK(ith::isValidContentPath("Ship.gltf"));
        CHECK(ith::isValidContentPath("Meshes/Vehicles/Ship.gltf"));
        CHECK(ith::isValidContentPath(".hidden"));
        CHECK(ith::isValidContentPath("Ünïcode/Кора.png"));
        CHECK(ith::isValidContentPath("CONSOLE.png"));
        CHECK(ith::isValidContentPath("COM0.png"));
        CHECK(ith::isValidContentPath("Ship.metadata"));
        CHECK(ith::isValidContentPath(std::string(ith::kMaxContentNameSize, 'a')));
        for (const std::string_view path : {"",
                                            "/Ship.gltf",
                                            "Ship.gltf/",
                                            "a//b",
                                            "./a",
                                            "a/../b",
                                            "..",
                                            "a\\b",
                                            "C:/a",
                                            "a?b",
                                            "a*b",
                                            "a<b",
                                            "a|b",
                                            "a\"b",
                                            "dot./a",
                                            "space /a",
                                            "Ship.gltf.meta",
                                            "Ship.gltf.META",
                                            "CON",
                                            "con.png",
                                            "Meshes/aux.objects",
                                            "NUL/x.objects",
                                            "Com1.txt",
                                            "lpt9",
                                            "prn.tar.gz"}) {
            CHECK_MESSAGE(!ith::isValidContentPath(path), path);
        }
        CHECK_FALSE(ith::isValidContentPath(std::string_view("a\tb")));
        CHECK_FALSE(ith::isValidContentPath(std::string(ith::kMaxContentNameSize + 1, 'a')));
        CHECK_FALSE(ith::isValidContentPath("Bad\xff\xfe.objects"));
        CHECK_FALSE(ith::isValidContentPath("\xed\xa0\x80.png")); // a surrogate in UTF-8
    }
}

TEST_SUITE("asset_database.links") {
    TEST_CASE("dangling links are skipped and folder links are not followed") {
        Fixture fixture;
        fixture.import("Real/Rock.objects");
        if (!tryCreateSymlink(fixture.project.content("Real"), fixture.project.content("Linked"), true) ||
            !tryCreateSymlink("does-not-exist.objects", fixture.project.content("Dangling.objects"), false)) {
            MESSAGE("symbolic links can't be created here; skipped");
            return;
        }
        CHECK(fixture.scan().empty());
        const std::vector<const ith::AssetRecord*> records = fixture.database->records();
        REQUIRE(records.size() == 1);
        CHECK(records[0]->path == "Real/Rock.objects");
        CHECK(fixture.database->folders() == std::vector<std::string>{"Real"});
    }
}

#ifndef _WIN32
TEST_SUITE("asset_database.posix") {
    TEST_CASE("an unreadable folder is a problem, not a failed scan") {
        Fixture fixture;
        fixture.import("Rock.objects");
        fixture.project.write("Locked/Inner.objects", "mesh A\n");
        ::chmod(fixture.project.content("Locked").c_str(), 0);
        std::error_code error;
        const bool listable = std::filesystem::directory_iterator(fixture.project.content("Locked"), error) !=
                              std::filesystem::directory_iterator();
        if (!error || listable) {
            MESSAGE("the folder is still readable (running as root?); skipped");
        } else {
            const std::vector<ith::AssetProblem> problems = fixture.scan();
            REQUIRE(problems.size() == 1);
            CHECK(problems[0].kind == AssetProblemKind::InvalidFile);
            CHECK(problems[0].path == "Locked");
            CHECK(fixture.database->records().size() == 1);
        }
        ::chmod(fixture.project.content("Locked").c_str(), 0755);
    }

    TEST_CASE("a file name that is not UTF-8 is a problem") {
        Fixture fixture;
        fixture.import("Rock.objects");
        std::error_code error;
        std::filesystem::copy_file(fixture.project.content("Rock.objects"),
                                   fixture.project.directory() / "Content" / std::string("Bad\xff\xfe.objects"), error);
        if (error) {
            MESSAGE("the file system refuses names that are not UTF-8; skipped");
            return;
        }
        const std::vector<ith::AssetProblem> problems = fixture.scan();
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].kind == AssetProblemKind::InvalidFile);
        CHECK(fixture.database->records().size() == 1);
        CHECK(fixture.database->import("Bad\xff\xfe.objects").error().code == "asset.invalid_path");
    }
}
#endif

TEST_SUITE("asset_database.operations") {
    TEST_CASE("move and rename keep the id and take the sidecar along") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("Ship.objects", "mesh Hull\n");
        const std::vector<ith::SubAssetEntry> subAssets = fixture.database->find(id)->subAssets;

        REQUIRE(fixture.database->move(id, "Vehicles/Sea/Ship.objects").hasValue());
        CHECK(fixture.project.exists("Vehicles/Sea/Ship.objects"));
        CHECK(fixture.project.exists("Vehicles/Sea/Ship.objects.meta"));
        CHECK_FALSE(fixture.project.exists("Ship.objects"));
        CHECK_FALSE(fixture.project.exists("Ship.objects.meta"));
        CHECK(fixture.database->find(id)->path == "Vehicles/Sea/Ship.objects");
        CHECK(fixture.database->findByPath("Ship.objects") == nullptr);

        REQUIRE(fixture.database->rename(id, "Boat.objects").hasValue());
        CHECK(fixture.database->findByPath("Vehicles/Sea/Boat.objects") == fixture.database->find(id));

        // Another session sees the same identity at the new path, and reimport keeps the ids.
        ith::AssetDatabase fresh = fixture.rescanned();
        const ith::AssetRecord* record = fresh.findByPath("Vehicles/Sea/Boat.objects");
        REQUIRE(record != nullptr);
        if (record != nullptr) {
            CHECK(record->id == id);
            CHECK(record->subAssets == subAssets);
        }
        const auto report = fresh.reimport(id);
        REQUIRE(report.hasValue());
        if (report) {
            CHECK(report->subAssets.at(0).id == subAssets.at(0).id);
        }
    }

    TEST_CASE("a native asset moves without a sidecar") {
        Fixture fixture;
        const ith::AssetId id = ith::AssetId::generate();
        fixture.project.write("Steel.ithasset", nativeAsset(id));
        CHECK(fixture.scan().empty());
        REQUIRE(fixture.database->move(id, "Materials/Steel.ithasset").hasValue());
        CHECK(fixture.rescanned().findByPath("Materials/Steel.ithasset") != nullptr);
        CHECK(fixture.database->reimport(id).error().code == "asset.not_imported");
    }

    TEST_CASE("renaming to another case of the same name works") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("rock.objects");
        REQUIRE(fixture.database->rename(id, "Rock.objects").hasValue());
        CHECK(fixture.project.exists("Rock.objects"));
        CHECK(fixture.project.exists("Rock.objects.meta"));
        CHECK_FALSE(fixture.project.exists("rock.objects"));
        CHECK(fixture.scan().empty());
        CHECK(fixture.database->findByPath("Rock.objects")->id == id);
    }

    TEST_CASE("moves that would conflict or change the type are refused, with nothing changed") {
        Fixture fixture;
        const ith::AssetId ship = fixture.import("Ship.objects");
        fixture.import("Meshes/Boat.objects");
        fixture.project.write("Taken.objects", "mesh A\n");
        fixture.project.write("Orphan.objects.meta", "{}");
        fixture.project.write("Upper.objects.META", "{}");

        const auto code = [&](std::string_view newPath) {
            return fixture.errorOf(fixture.database->move(ship, newPath));
        };
        CHECK(code("Meshes/Boat.objects") == "asset.path_conflict");
        CHECK(code("Meshes/boat.objects") == "asset.path_conflict");
        CHECK(code("meshes/Ship.objects") == "asset.path_conflict"); // the folder is "Meshes"
        CHECK(code("Taken.objects") == "asset.path_conflict");
        CHECK(code("Orphan.objects") == "asset.path_conflict");
        CHECK(code("Upper.objects") == "asset.path_conflict");
        CHECK(code("Ship.json") == "asset.extension_changed");
        CHECK(code("../Ship.objects") == "asset.invalid_path");
        CHECK(code("CON.objects") == "asset.invalid_path");
        CHECK(code("Ship.objects") == "");
        CHECK(fixture.errorOf(fixture.database->rename(ship, "a/b.objects")) == "asset.invalid_path");
        CHECK(fixture.errorOf(fixture.database->move(ith::AssetId::generate(), "X.objects")) == "asset.not_found");

        CHECK(fixture.project.exists("Ship.objects"));
        CHECK(fixture.project.exists("Ship.objects.meta"));
        CHECK(fixture.database->find(ship)->path == "Ship.objects");
    }

#ifdef _WIN32
    TEST_CASE("a move whose sidecar can't follow puts the file back") {
        // Windows refuses to rename a file that is open without delete sharing, as std::ifstream
        // opens it.
        Fixture fixture;
        const ith::AssetId ship = fixture.import("Ship.objects");
        std::ifstream held(fixture.project.content("Ship.objects.meta"));
        REQUIRE(held.is_open());
        const ith::Expected<void> moved = fixture.database->move(ship, "Sea/Ship.objects");
        REQUIRE_FALSE(moved.hasValue());
        if (!moved) {
            CHECK(moved.error().code == "asset.io");
        }
        CHECK(fixture.project.exists("Ship.objects"));
        CHECK_FALSE(fixture.project.exists("Sea/Ship.objects"));
        CHECK(fixture.database->find(ship)->path == "Ship.objects");
    }
#endif

    TEST_CASE("remove deletes the file and its sidecar") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("Ship.objects");
        REQUIRE(fixture.database->remove(id).hasValue());
        CHECK(fixture.database->find(id) == nullptr);
        CHECK_FALSE(fixture.project.exists("Ship.objects"));
        CHECK_FALSE(fixture.project.exists("Ship.objects.meta"));
        CHECK(fixture.errorOf(fixture.database->remove(id)) == "asset.not_found");
        CHECK(fixture.scan().empty());
    }
}

TEST_SUITE("asset_database.utf8") {
    TEST_CASE("non-ASCII paths are UTF-8 on every platform") {
        Fixture fixture;
        const ith::AssetId id = fixture.import("Ünïcode/Кора.objects");
        CHECK(fixture.project.exists("Ünïcode/Кора.objects.meta"));
        REQUIRE(fixture.database->rename(id, "Écorce.objects").hasValue());
        const ith::AssetDatabase fresh = fixture.rescanned();
        const ith::AssetRecord* record = fresh.findByPath("Ünïcode/Écorce.objects");
        REQUIRE(record != nullptr);
        if (record != nullptr) {
            CHECK(record->id == id);
        }
    }
}
