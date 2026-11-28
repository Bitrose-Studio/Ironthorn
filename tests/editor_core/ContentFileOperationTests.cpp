// The Content Browser's file operations (ADR-0049 rule 28, docs/specs/content-browser.md
// requirements 13's F2 half, 15 to 34, 36 to 39, 41 to 49 and 57), headless: new folders, rename,
// move, duplicate, delete with its preview and users, import with conflicts, dropped folders and
// referenced files, reimport and the fixes, the naming scheme, the prompts, the open level's path
// following its file, the refusals while a save is written, and the Content keymap's file keys, each
// in a temporary project with an asset database.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Sidecar.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"

#include "ContentTestSupport.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"
#include "Junction.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using ith::AssetId;
using ith::ContentKey;
using ith::serialization::Value;
using ith::test::editor::badgeOf;
using ith::test::editor::exists;
using ith::test::editor::fails;
using ith::test::editor::keyDown;
using ith::test::editor::Project;
using ith::test::editor::run;
using ith::test::editor::TempProject;

std::string jsonString(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::string idItem(AssetId id) {
    return R"({"id": )" + jsonString(id.toString()) + "}";
}

// The JSON text of a file path: '\' doubled.
std::string jsonPath(std::string_view path) {
    std::string text;
    for (const char c : path) {
        text += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    return jsonString(text);
}

// The paths of the item references in `list` (an array of {"path"}).
std::vector<std::string> pathsOf(const Value& list) {
    std::vector<std::string> paths;
    for (const Value& ref : list.elements()) {
        paths.push_back(ref.find("path") != nullptr ? ref.find("path")->asString() : ref.find("file")->asString());
    }
    return paths;
}

std::string read(const std::string& path) {
    const auto text = ith::fs::readFile(path, std::size_t{1} << 20);
    REQUIRE_MESSAGE(text.hasValue(), path);
    return *text;
}

// The names in a folder on disk, sorted.
std::vector<std::string> namesIn(const std::string& folder) {
    std::vector<std::string> names;
    if (const auto entries = ith::fs::listDirectory(folder)) {
        for (const ith::fs::DirectoryEntry& entry : *entries) {
            names.push_back(entry.name);
        }
    }
    std::ranges::sort(names);
    return names;
}

// A level file whose one entity holds a component the test registry doesn't know, which refers to
// `asset`: the level loader keeps it unresolved, and the delete preview still finds the reference.
std::string levelReferring(AssetId level, AssetId asset) {
    return R"({"format": "ith.level", "version": 1, "id": ")" + level.toString() +
           R"(", "data": {"entities": [{"id": ")" + ith::test::editor::idText(77) +
           R"(", "components": {"test::Prop": {"mesh": {"asset": ")" + asset.toString() +
           R"(", "subAsset": "00000000-0000-0000-0000-000000000000"}}}}]}})";
}

std::string nativeReferring(AssetId id, AssetId asset) {
    return R"({"format": "ith.material", "version": 1, "id": ")" + id.toString() +
           R"(", "data": {"texture": {"asset": ")" + asset.toString() + R"("}}})";
}

// The string member `key` of a result entry, or "".
std::string textOf(const Value& entry, std::string_view key) {
    const Value* value = entry.find(key);
    return value != nullptr && value->kind() == Value::Kind::String ? value->asString() : std::string();
}

std::filesystem::path nativeOf(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

bool undoFree(const Project& p) {
    return p.session->document().history().entries().empty() && !p.session->document().dirty();
}

} // namespace

TEST_SUITE("editor_core.content_files") {
    TEST_CASE("the naming helpers: the editable part, the name rule and the numbered base") {
        CHECK(ith::numberedNameBase("Rock_2") == "Rock");
        CHECK(ith::numberedNameBase("Rock_12") == "Rock");
        CHECK(ith::numberedNameBase("Rock") == "Rock");
        CHECK(ith::numberedNameBase("Rock_") == "Rock_");
        CHECK(ith::numberedNameBase("_2") == "_2");
        CHECK(ith::numberedNameBase("Rock2") == "Rock2");
        CHECK(ith::numberedNameBase("New Folder_2") == "New Folder");

        CHECK(ith::checkContentName("Rock.png").hasValue());
        CHECK(ith::checkContentName("été").hasValue());
        for (const std::string_view bad :
             {"", "a/b", "a\\b", "a:b", "name.", "name ", "CON", "com1.txt", "x.meta", "x.META", "a|b", "a\tb"}) {
            CAPTURE(bad);
            const auto checked = ith::checkContentName(bad);
            REQUIRE_FALSE(checked.hasValue());
            CHECK(checked.error().code == "editor.content.invalid_name");
        }
        CHECK(ith::checkContentName(std::string(ith::kMaxContentNameSize, 'a')).hasValue());
        CHECK_FALSE(ith::checkContentName(std::string(ith::kMaxContentNameSize + 1, 'a')).hasValue());

        const ith::ContentItem native{.kind = ith::ContentItemKind::Asset, .path = "S/Ship.ithasset", .label = "Ship"};
        CHECK(ith::nameParts(native).editable == "Ship");
        CHECK(ith::nameParts(native).extension == ".ithasset");
        const ith::ContentItem source{
            .kind = ith::ContentItemKind::Asset, .path = "S/Rock.v2.gltf", .label = "Rock.v2.gltf"};
        CHECK(ith::nameParts(source).editable == "Rock.v2");
        CHECK(ith::nameParts(source).extension == ".gltf");
        const ith::ContentItem folder{
            .kind = ith::ContentItemKind::Folder, .path = "S/My.Folder", .label = "My.Folder"};
        CHECK(ith::nameParts(folder).editable == "My.Folder");
        CHECK(ith::nameParts(folder).extension.empty());
        const ith::ContentItem bare{.kind = ith::ContentItemKind::Problem, .path = "README", .label = "README"};
        CHECK(ith::nameParts(bare).editable == "README");
    }

    TEST_CASE("New Folder twice gives New Folder and New Folder_2, each selected with its inline rename "
              "(requirement 16)") {
        Project p;
        p.native("Ship.ithasset");
        p.write("notes.txt", "x");
        p.rescan();
        CHECK(p.execute("ith.content.new_folder").find("path")->asString() == "New Folder");
        CHECK(p.selected() == std::vector<std::string>{"New Folder"});
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Rename);
        CHECK(p.model().prompt()->item == ContentKey{.id = AssetId(), .path = "New Folder"});
        const Value second = p.execute("ith.content.new_folder");
        CHECK(second.find("path")->asString() == "New Folder_2");
        CHECK(second.find("prompt")->asString() == "rename");
        CHECK(p.selected() == std::vector<std::string>{"New Folder_2"});
        const Value query = p.execute("ith.query.content");
        CHECK(query.find("prompt")->find("kind")->asString() == "rename");
        CHECK(query.find("prompt")->find("name")->asString() == "New Folder_2");
        CHECK(query.find("prompt")->find("item")->find("path")->asString() == "New Folder_2");
        CHECK(exists(p.file("New Folder_2")));

        // A name: no prompt; a parent folder.
        p.execute("ith.content.dismiss");
        CHECK(p.execute("ith.content.new_folder", R"({"name": "Meshes"})").find("prompt") == nullptr);
        CHECK(p.model().prompt() == nullptr);
        CHECK(
            p.execute("ith.content.new_folder", R"({"parent": "Meshes", "name": "Rocks"})").find("path")->asString() ==
            "Meshes/Rocks");
        CHECK(p.model().folderExists("Meshes/Rocks"));
        CHECK(std::ranges::binary_search(p.assets->folders(), std::string("Meshes/Rocks")));

        // Refusals, with nothing created.
        fails(*p.session, "ith.content.new_folder", R"({"name": "meshes"})", "editor.content.exists");
        fails(*p.session, "ith.content.new_folder", R"({"name": "NOTES.TXT"})", "editor.content.exists");
        fails(*p.session, "ith.content.new_folder", R"({"name": "a:b"})", "editor.content.invalid_name");
        fails(*p.session, "ith.content.new_folder", R"({"name": "x.meta"})", "editor.content.invalid_name");
        fails(*p.session, "ith.content.new_folder", R"({"parent": "Nowhere"})", "editor.content.not_found");
        fails(*p.session, "ith.content.new_folder", R"({"parent": "Ship.ithasset"})", "editor.content.invalid_target");
        CHECK(namesIn(p.file("")) ==
              std::vector<std::string>{"Meshes", "New Folder", "New Folder_2", "Ship.ithasset", "notes.txt"});
        CHECK(undoFree(p)); // requirement 57
    }

    TEST_CASE("rename refuses invalid and taken names, changing nothing on disk (requirements 17, 18)") {
        Project p;
        p.native("S/Ship.ithasset");
        const AssetId rock = p.imported("S/Rock.gltf");
        p.write("S/notes.txt", "x");
        p.folder("S/Sub");
        p.folder("S/Box.gltf");
        p.rescan();
        const std::vector<std::string> before = namesIn(p.file("S"));
        const std::string rockItem = idItem(rock);
        // An asset's extension stays, so its rule applies to the name with it ("com1.txt.gltf" is a device).
        for (const std::string_view bad : {"", "a/b", "a\\\\b", "a:b", "CON", "com1.txt"}) {
            CAPTURE(bad);
            fails(*p.session, "ith.content.rename", R"({"item": )" + rockItem + R"(, "name": )" + jsonString(bad) + "}",
                  "editor.content.invalid_name");
        }
        for (const std::string_view bad : {"", "name.", "name ", "CON", "com1.txt", "a:b"}) {
            CAPTURE(bad);
            fails(*p.session, "ith.content.rename", R"({"item": {"path": "S/Sub"}, "name": )" + jsonString(bad) + "}",
                  "editor.content.invalid_name");
        }
        // 205 bytes and ".gltf": 210, one more than names may have.
        fails(*p.session, "ith.content.rename",
              R"({"item": )" + rockItem + R"(, "name": )" + jsonString(std::string(205, 'r')) + "}",
              "editor.content.invalid_name");
        fails(*p.session, "ith.content.rename", R"({"item": {"path": "S/Sub"}, "name": "x.meta"})",
              "editor.content.invalid_name");
        // Taken in any ASCII case by a file, a folder and a hidden file.
        fails(*p.session, "ith.content.rename", R"({"item": {"path": "S/Sub"}, "name": "SHIP.ithasset"})",
              "editor.content.exists");
        fails(*p.session, "ith.content.rename", R"({"item": )" + rockItem + R"(, "name": "BOX"})",
              "editor.content.exists");
        fails(*p.session, "ith.content.rename", R"({"item": {"path": "S/Sub"}, "name": "Notes.TXT"})",
              "editor.content.exists");
        // The root, and nothing selected.
        fails(*p.session, "ith.content.rename", R"({"item": {"path": ""}, "name": "X"})", "editor.content.root");
        fails(*p.session, "ith.content.rename", R"({"name": "X"})", "editor.content.not_found");
        CHECK(namesIn(p.file("S")) == before);
        CHECK(p.assets->find(rock)->path == "S/Rock.gltf");
    }

    TEST_CASE("a case-only rename keeps the id and gives the file and the sidecar the new case (requirement 19)") {
        Project p;
        const AssetId rock = p.imported("S/rock.gltf");
        p.rescan();
        const Value renamed = p.execute("ith.content.rename", R"({"item": )" + idItem(rock) + R"(, "name": "Rock"})");
        CHECK(renamed.find("path")->asString() == "S/Rock.gltf");
        CHECK(namesIn(p.file("S")) == std::vector<std::string>{"Rock.gltf", "Rock.gltf.meta"});
        REQUIRE(p.assets->findByPath("S/Rock.gltf") != nullptr);
        CHECK(p.assets->findByPath("S/Rock.gltf")->id == rock);
    }

    TEST_CASE("rename keeps ids, takes the sidecar along, keeps the extension, and a folder's assets keep "
              "their ids (requirements 13, 15, 20)") {
        Project p;
        const AssetId ship = p.native("S/Ship.ithasset");
        const AssetId rock = p.imported("S/Rock.gltf");
        const AssetId inner = p.native("S/Sub/Inner.ithasset");
        p.write("S/Sub/notes.txt", "x");
        p.write("S/Loose.gltf", "glTF"); // no sidecar: a "Not imported" entry
        p.rescan();
        p.navigate("S");

        // F2 (no name): the prompt with the editable part; nothing changes.
        p.select({"S/Ship.ithasset"}, "replace");
        const Value prompt = p.execute("ith.content.rename");
        CHECK(prompt.find("name")->asString() == "Ship");
        CHECK(prompt.find("extension")->asString() == ".ithasset");
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Rename);
        CHECK(p.model().prompt()->item == ContentKey{.id = ship, .path = {}});
        // The same name does nothing and ends the prompt.
        CHECK(p.execute("ith.content.rename", R"({"name": "Ship"})").find("path")->asString() == "S/Ship.ithasset");
        CHECK(p.model().prompt() == nullptr);

        // A native asset: the extension stays; the renamed asset stays selected (requirement 15).
        CHECK(p.execute("ith.content.rename", R"({"name": "Boat"})").find("path")->asString() == "S/Boat.ithasset");
        CHECK(p.assets->find(ship)->path == "S/Boat.ithasset");
        CHECK(p.selected() == std::vector<std::string>{"S/Boat.ithasset"});
        // An imported asset: its sidecar goes along.
        p.execute("ith.content.rename", R"({"item": )" + idItem(rock) + R"(, "name": "Stone"})");
        CHECK(exists(p.file("S/Stone.gltf")));
        CHECK(exists(p.file("S/Stone.gltf.meta")));
        CHECK_FALSE(exists(p.file("S/Rock.gltf.meta")));
        CHECK(p.assets->find(rock)->path == "S/Stone.gltf");
        // A folder: everything inside goes along, files the browser doesn't show included.
        p.select({"S/Sub"}, "replace");
        p.execute("ith.content.rename", R"({"name": "Inside"})");
        CHECK(p.assets->find(inner)->path == "S/Inside/Inner.ithasset");
        CHECK(exists(p.file("S/Inside/notes.txt")));
        CHECK(p.model().folderExists("S/Inside"));
        CHECK_FALSE(p.model().folderExists("S/Sub"));
        CHECK(p.selected() == std::vector<std::string>{"S/Inside"});
        // A problem entry: its file, and it stays an entry under the new name.
        p.execute("ith.content.rename", R"({"item": {"path": "S/Loose.gltf"}, "name": "Free"})");
        CHECK(exists(p.file("S/Free.gltf")));
        REQUIRE(p.model().findByPath("S/Free.gltf") != nullptr);
        CHECK(p.model().findByPath("S/Free.gltf")->kind == ith::ContentItemKind::Problem);
        CHECK(p.model().findByPath("S/Loose.gltf") == nullptr);
        CHECK(p.labels() == std::vector<std::string>{"Inside", "Boat", "Free.gltf", "Stone.gltf"});
        CHECK(undoFree(p)); // requirement 57
    }

    TEST_CASE("an orphan sidecar can't be renamed, only deleted") {
        Project p;
        p.write("S/Gone.gltf.meta", "{}");
        p.rescan();
        REQUIRE(p.model().findByPath("S/Gone.gltf.meta") != nullptr);
        fails(*p.session, "ith.content.rename", R"({"item": {"path": "S/Gone.gltf.meta"}, "name": "X"})",
              "editor.content.invalid_target");
    }

    TEST_CASE("renaming one asset of a case conflict to a distinct name removes both badges (requirement 22)") {
        Project p;
        p.native("S/Rock.ithasset");
        p.native("S/rock.ithasset");
        const auto entries = ith::fs::listDirectory(p.file("S"));
        // Both names must exist, or the test would pass without testing anything: a file system or a
        // folder that folds case (Windows, macOS, a case-folding Linux folder) is skipped.
        if (!entries || entries->size() < 2) {
            MESSAGE("the file system ignores case; skipped");
            return;
        }
        p.rescan();
        REQUIRE(badgeOf(p.item("S/Rock.ithasset"), ith::AssetProblemKind::CaseConflict) != nullptr);
        p.execute("ith.content.rename", R"({"item": {"path": "S/rock.ithasset"}, "name": "Pebble"})");
        CHECK(badgeOf(p.item("S/Rock.ithasset"), ith::AssetProblemKind::CaseConflict) == nullptr);
        CHECK(badgeOf(p.item("S/Pebble.ithasset"), ith::AssetProblemKind::CaseConflict) == nullptr);
        CHECK(p.model().problems().empty());
    }

    TEST_CASE("the open level follows its file through renames and moves, without becoming dirty, and a save "
              "writes the new path (requirement 21)") {
        Project p;
        p.folder("Other");
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        p.rescan();
        const ith::Uuid levelId = p.session->document().levelId();
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(1) + R"("})");
        REQUIRE(p.session->document().dirty());
        CHECK(p.session->levelContentPath() == std::string("Levels/Main.ithmap"));

        p.execute("ith.content.rename", R"({"item": {"path": "Levels/Main.ithmap"}, "name": "Harbour"})");
        CHECK(p.session->document().path() == "Content/Levels/Harbour.ithmap");
        p.execute("ith.content.rename", R"({"item": {"path": "Levels"}, "name": "Maps"})");
        CHECK(p.session->document().path() == "Content/Maps/Harbour.ithmap");
        p.execute("ith.content.move", R"({"items": [{"path": "Maps"}], "folder": "Other"})");
        CHECK(p.session->document().path() == "Content/Other/Maps/Harbour.ithmap");
        p.execute("ith.content.move", R"({"items": [{"path": "Other/Maps/Harbour.ithmap"}], "folder": ""})");
        CHECK(p.session->document().path() == "Content/Harbour.ithmap");
        CHECK(p.session->document().dirty());
        CHECK(p.session->document().levelId() == levelId);
        CHECK(p.execute("ith.query.document").find("path")->asString() == "Content/Harbour.ithmap");

        run(*p.session, "ith.file.save");
        CHECK_FALSE(p.session->document().dirty());
        CHECK(read(p.file("Harbour.ithmap")).find(ith::test::editor::idText(1)) != std::string::npos);
        CHECK(namesIn(p.file("")) == std::vector<std::string>{"Harbour.ithmap", "Other"});
    }

    TEST_CASE("a level opened by its absolute path keeps an absolute path when its file is renamed") {
        Project p;
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        run(*p.session, "ith.file.new_level");
        run(*p.session, "ith.file.open_level", R"({"path": )" + jsonPath(p.file("Levels/Main.ithmap")) + "}");
        p.rescan();
        REQUIRE(p.session->document().path() == p.file("Levels/Main.ithmap"));
        p.execute("ith.content.rename", R"({"item": {"path": "Levels/Main.ithmap"}, "name": "Moved"})");
        CHECK(p.session->document().path() == p.file("Levels/Moved.ithmap"));
        CHECK_FALSE(p.session->document().dirty());
        // A level outside Content is not the browser's: nothing follows, nothing is refused.
        run(*p.session, "ith.file.save_as", R"({"path": "Elsewhere/Out.ithmap"})");
        CHECK_FALSE(p.session->levelContentPath().has_value());
        p.execute("ith.content.rename", R"({"item": {"path": "Levels/Moved.ithmap"}, "name": "Again"})");
        CHECK(p.session->document().path() == "Elsewhere/Out.ithmap");
    }

    TEST_CASE("every file operation waits for a save in flight, so a Save As target can't be taken meanwhile") {
        TempProject project;
        ith::ImporterRegistry importers;
        importers.add(std::make_unique<ith::test::editor::FakeGltfImporter>());
        ith::AssetDatabase assets(project.root(), importers);
        ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
        ith::EditorSession session(ith::test::editor::types(),
                                   {.projectDirectory = project.root(), .jobs = &jobs, .assets = &assets});
        const std::string content = ith::fs::joinPath(project.root(), "Content");
        const AssetId other = AssetId::generate();
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(content, "Levels/Other.ithmap"),
                                         ith::test::editor::nativeAsset(other, "ith.level"))
                    .hasValue());
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(content, "Levels/Loose.gltf"), "glTF").hasValue());
        run(session, "ith.content.rescan");

        // Hold the background lane, so the save's write waits behind it.
        std::mutex mutex;
        std::condition_variable wake;
        bool release = false;
        std::atomic<bool> holding{false};
        ith::JobCounter held;
        jobs.submit(
            ith::JobLane::Background, "Hold the lane",
            [&] {
                holding = true;
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return release; });
            },
            &held);
        while (!holding) {
            std::this_thread::yield();
        }
        run(session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        REQUIRE(session.document().saving());
        // The Save As target isn't the document's path yet; taking its name now would be overwritten.
        constexpr std::string_view kSaving = "editor.document.save_in_progress";
        fails(session, "ith.content.rename", R"({"item": {"path": "Levels/Other.ithmap"}, "name": "Main"})", kSaving);
        fails(session, "ith.content.new_folder", R"({"parent": "Levels"})", kSaving);
        fails(session, "ith.content.move", R"({"items": [{"path": "Levels/Other.ithmap"}], "folder": ""})", kSaving);
        fails(session, "ith.content.duplicate", R"({"items": [{"path": "Levels/Other.ithmap"}]})", kSaving);
        fails(session, "ith.content.delete", R"({"items": [{"path": "Levels"}], "confirmed": true})", kSaving);
        fails(session, "ith.content.import", R"({"files": ["Content/Levels/Loose.gltf"]})", kSaving);
        fails(session, "ith.content.reimport", R"({"items": [{"path": "Levels/Other.ithmap"}]})", kSaving);
        fails(session, "ith.content.fix", R"({"item": {"path": "Levels/Loose.gltf"}})", kSaving);
        {
            const std::scoped_lock lock(mutex);
            release = true;
        }
        wake.notify_all();
        jobs.wait(held);
        session.waitIdle();

        // The save landed with its own id, the other level kept its file and id, and the name is taken now.
        REQUIRE_FALSE(session.document().saving());
        CHECK(session.document().path() == "Content/Levels/Main.ithmap");
        const ith::AssetRecord* saved = assets.findByPath("Levels/Main.ithmap");
        REQUIRE(saved != nullptr);
        CHECK(saved->id == AssetId(session.document().levelId()));
        CHECK(saved->id != other);
        REQUIRE(assets.find(other) != nullptr);
        CHECK(assets.find(other)->path == "Levels/Other.ithmap");
        CHECK(namesIn(content + "/Levels") == std::vector<std::string>{"Loose.gltf", "Main.ithmap", "Other.ithmap"});
        fails(session, "ith.content.rename", R"({"item": {"path": "Levels/Other.ithmap"}, "name": "Main"})",
              "editor.content.exists");
    }

    TEST_CASE("move: assets with their sidecars and ids, refused targets, all or nothing, folders with "
              "everything inside, and a drop on the same folder (requirements 23 to 27)") {
        Project p;
        const AssetId ship = p.native("A/Ship.ithasset");
        const AssetId rock = p.imported("A/Rock.gltf");
        p.native("A/Sub/Deep/Inner.ithasset");
        p.write("A/Sub/notes.txt", "x");
        p.native("B/ship.ithasset");
        p.folder("C");
        p.write("A/Loose.gltf", "glTF");
        p.rescan();

        // Into itself or below it; onto an asset; a problem entry; the root.
        fails(*p.session, "ith.content.move", R"({"items": [{"path": "A"}], "folder": "A"})",
              "editor.content.invalid_target");
        fails(*p.session, "ith.content.move", R"({"items": [{"path": "A"}], "folder": "A/Sub/Deep"})",
              "editor.content.invalid_target");
        fails(*p.session, "ith.content.move", R"({"items": [{"path": "A/Rock.gltf"}], "folder": "A/Ship.ithasset"})",
              "editor.content.invalid_target");
        fails(*p.session, "ith.content.move", R"({"items": [{"path": "A/Loose.gltf"}], "folder": "C"})",
              "editor.content.invalid_target");
        fails(*p.session, "ith.content.move", R"({"items": [{"path": ""}], "folder": "C"})", "editor.content.root");
        fails(*p.session, "ith.content.move", R"({"items": [{"path": "A/Rock.gltf"}], "folder": "Nowhere"})",
              "editor.content.not_found");
        fails(*p.session, "ith.content.move", R"({"items": []})", "editor.command.invalid_args");
        // One name taken in the target, in another case: nothing moves, and the message names it.
        const auto taken =
            p.session->execute("ith.content.move", ith::test::editor::json(R"({"items": [{"path": "A/Rock.gltf"},
                {"path": "A/Ship.ithasset"}], "folder": "B"})"));
        REQUIRE_FALSE(taken.hasValue());
        CHECK(taken.error().code == "editor.content.exists");
        CHECK(taken.error().message.find("Ship.ithasset") != std::string::npos);
        CHECK(p.assets->find(rock)->path == "A/Rock.gltf");
        CHECK(p.assets->find(ship)->path == "A/Ship.ithasset");

        // Two assets onto a folder (requirement 23).
        const Value moved = p.execute(
            "ith.content.move", R"({"items": [{"path": "A/Rock.gltf"}, {"path": "A/Ship.ithasset"}], "folder": "C"})");
        CHECK(pathsOf(*moved.find("moved")) == std::vector<std::string>{"C/Rock.gltf", "C/Ship.ithasset"});
        CHECK(moved.find("failed")->elements().empty());
        CHECK(namesIn(p.file("C")) == std::vector<std::string>{"Rock.gltf", "Rock.gltf.meta", "Ship.ithasset"});
        CHECK(p.assets->find(rock)->path == "C/Rock.gltf");
        CHECK(p.assets->find(ship)->path == "C/Ship.ithasset");
        // A drop on the folder that holds them does nothing (requirement 27).
        const Value same = p.execute("ith.content.move", R"({"items": [{"path": "C/Rock.gltf"}], "folder": "C"})");
        CHECK(same.find("moved")->elements().empty());
        // A folder with files the browser doesn't show and subfolders (requirement 26); the current folder
        // and the expansion follow it.
        p.navigate("A/Sub/Deep");
        p.execute("ith.content.move", R"({"items": [{"path": "A/Sub"}], "folder": "C"})");
        CHECK(exists(p.file("C/Sub/notes.txt")));
        CHECK(exists(p.file("C/Sub/Deep/Inner.ithasset")));
        CHECK_FALSE(exists(p.file("A/Sub")));
        CHECK(p.model().currentFolder() == "C/Sub/Deep");
        CHECK(p.model().isExpanded("C/Sub"));
        CHECK(p.model().findByPath("C/Sub/Deep/Inner.ithasset") != nullptr);
        CHECK(undoFree(p));
    }

    TEST_CASE("a move that fails in the middle keeps what moved and reports the rest (requirement 28)") {
        Project p;
        p.native("A/One.ithasset");
        p.native("A/Two.ithasset");
        p.native("A/Three.ithasset");
        p.folder("B");
        p.rescan();
        // The second file goes away outside the editor, so its move fails on disk.
        REQUIRE(ith::fs::remove(p.file("A/Two.ithasset")).hasValue());
        const Value result = p.execute("ith.content.move", R"({"items": [{"path": "A/One.ithasset"},
            {"path": "A/Two.ithasset"}, {"path": "A/Three.ithasset"}], "folder": "B"})");
        CHECK(pathsOf(*result.find("moved")) == std::vector<std::string>{"B/One.ithasset"});
        REQUIRE(result.find("failed")->elements().size() == 1);
        CHECK(result.find("failed")->elements()[0].find("path")->asString() == "A/Two.ithasset");
        CHECK_FALSE(result.find("failed")->elements()[0].find("code")->asString().empty());
        CHECK(pathsOf(*result.find("skipped")) == std::vector<std::string>{"A/Three.ithasset"});
        CHECK(exists(p.file("B/One.ithasset")));
        CHECK(exists(p.file("A/Three.ithasset")));
        CHECK(p.model().findByPath("B/One.ithasset") != nullptr);
        CHECK(p.model().findByPath("A/Three.ithasset") != nullptr);
    }

    TEST_CASE("duplicate: <base>_<n> from 2, new ids, everything else copied, the copies selected "
              "(requirements 29 to 31)") {
        Project p;
        const AssetId rock = p.imported("S/Rock.gltf");
        const AssetId ship = p.native("S/Ship.ithasset");
        p.folder("S/Sub");
        p.write("S/Loose.gltf", "glTF");
        p.rescan();
        p.navigate("S");

        p.select({"S/Rock.gltf"}, "replace");
        const Value first = p.execute("ith.content.duplicate");
        CHECK(pathsOf(*first.find("created")) == std::vector<std::string>{"S/Rock_2.gltf"});
        CHECK(p.selected() == std::vector<std::string>{"S/Rock_2.gltf"});
        p.execute("ith.content.duplicate", R"({"items": [)" + idItem(rock) + "]}");
        CHECK(exists(p.file("S/Rock_3.gltf")));
        // Rock_2 gives Rock_4 while Rock_3 is taken.
        p.execute("ith.content.duplicate", R"({"items": [{"path": "S/Rock_2.gltf"}]})");
        CHECK(exists(p.file("S/Rock_4.gltf")));

        // An imported copy: a new id in its sidecar, the same importer and sub-asset table.
        const ith::AssetRecord* copy = p.assets->findByPath("S/Rock_2.gltf");
        REQUIRE(copy != nullptr);
        CHECK(copy->id != rock);
        CHECK(copy->type == "gltf");
        CHECK(copy->subAssets == p.assets->find(rock)->subAssets);
        CHECK(read(p.file("S/Rock_2.gltf")) == read(p.file("S/Rock.gltf")));
        // A native copy: a new envelope id, the data unchanged.
        const Value native = p.execute("ith.content.duplicate", R"({"items": [{"path": "S/Ship.ithasset"},
            {"path": "S/Sub"}, {"path": "S/Loose.gltf"}]})");
        CHECK(pathsOf(*native.find("created")) == std::vector<std::string>{"S/Ship_2.ithasset"});
        CHECK(pathsOf(*native.find("skipped")) == std::vector<std::string>{"S/Sub", "S/Loose.gltf"});
        const ith::AssetRecord* shipCopy = p.assets->findByPath("S/Ship_2.ithasset");
        REQUIRE(shipCopy != nullptr);
        CHECK(shipCopy->id != ship);
        const auto expected = ith::replaceHeaderId(read(p.file("S/Ship.ithasset")), shipCopy->id, "Ship.ithasset");
        REQUIRE(expected.hasValue());
        CHECK(*expected == read(p.file("S/Ship_2.ithasset")));
        CHECK(p.selected() == std::vector<std::string>{"S/Ship_2.ithasset"});
        // A Rescan finds no duplicate id.
        p.rescan();
        CHECK(std::ranges::none_of(p.model().problems(), [](const ith::AssetProblem& problem) {
            return problem.kind == ith::AssetProblemKind::DuplicateId;
        }));
        CHECK(undoFree(p));
    }

    TEST_CASE("a duplicate's name is cut to fit the name limit") {
        Project p;
        const std::string longName = std::string(ith::kMaxContentNameSize - 5, 'r') + ".gltf";
        p.imported("S/" + longName);
        p.rescan();
        const Value copy =
            p.execute("ith.content.duplicate", R"({"items": [{"path": )" + jsonString("S/" + longName) + "}]}");
        const std::string path = pathsOf(*copy.find("created")).at(0);
        CHECK(path == "S/" + std::string(ith::kMaxContentNameSize - 7, 'r') + "_2.gltf");
        CHECK(ith::isValidContentPath(path));
    }

    TEST_CASE("duplicating the open level with unsaved changes copies its saved file (requirement 32)") {
        Project p;
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        p.rescan();
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(5) + R"("})");
        REQUIRE(p.session->document().dirty());
        p.execute("ith.content.duplicate", R"({"items": [{"path": "Levels/Main.ithmap"}]})");
        CHECK(read(p.file("Levels/Main_2.ithmap")).find(ith::test::editor::idText(5)) == std::string::npos);
        CHECK(p.session->document().dirty());
        CHECK(p.session->document().path() == "Content/Levels/Main.ithmap");
    }

    TEST_CASE("delete: a preview first with the users outside the deleted set, then the files go (requirements "
              "33, 34, 36)") {
        Project p;
        const AssetId mesh = p.imported("Meshes/Rock.gltf");
        p.write("Meshes/notes.txt", "x");
        p.write("Meshes/Sub/readme.md", "x");
        const AssetId inner = AssetId::generate();
        p.write("Meshes/Inner.ithasset", nativeReferring(inner, mesh));
        const AssetId user = AssetId::generate();
        p.write("Data/User.ithasset", nativeReferring(user, mesh));
        const AssetId level = AssetId::generate();
        p.write("Levels/Main.ithmap", levelReferring(level, mesh));
        p.rescan();
        const std::string userText = read(p.file("Data/User.ithasset"));

        // The asset alone: every user, the other native asset inside its folder too.
        const Value preview = p.execute("ith.content.delete", R"({"items": [)" + idItem(mesh) + "]}");
        CHECK_FALSE(preview.find("deleted")->asBool());
        const Value& uses = *preview.find("preview")->find("uses");
        REQUIRE(uses.elements().size() == 1);
        CHECK(uses.elements()[0].find("asset")->find("path")->asString() == "Meshes/Rock.gltf");
        CHECK(pathsOf(*uses.elements()[0].find("users")) ==
              std::vector<std::string>{"Data/User.ithasset", "Levels/Main.ithmap", "Meshes/Inner.ithasset"});
        CHECK(exists(p.file("Meshes/Rock.gltf")));
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Delete);
        CHECK(p.execute("ith.query.content").find("prompt")->find("kind")->asString() == "delete");

        // The folder: users inside the deleted set are not listed; files the browser doesn't show are counted.
        const Value folder = p.execute("ith.content.delete", R"({"items": [{"path": "Meshes"}]})");
        const Value& folderPreview = *folder.find("preview");
        CHECK(folderPreview.find("count")->toNumber<int>() == 1);
        CHECK(folderPreview.find("assets")->toNumber<int>() == 2);
        CHECK(folderPreview.find("otherFiles")->toNumber<int>() == 2);
        CHECK(folderPreview.find("items")->elements()[0].find("kind")->asString() == "folder");
        CHECK(pathsOf(*folderPreview.find("uses")->elements()[0].find("users")) ==
              std::vector<std::string>{"Data/User.ithasset", "Levels/Main.ithmap"});

        // Confirmed: the asset's file and sidecar go; the users' files stay as they were.
        const Value deleted =
            p.execute("ith.content.delete", R"({"items": [)" + idItem(mesh) + R"(], "confirmed": true})");
        CHECK(pathsOf(*deleted.find("deleted")) == std::vector<std::string>{"Meshes/Rock.gltf"});
        CHECK(deleted.find("failed")->elements().empty());
        CHECK_FALSE(exists(p.file("Meshes/Rock.gltf")));
        CHECK_FALSE(exists(p.file("Meshes/Rock.gltf.meta")));
        CHECK(p.assets->find(mesh) == nullptr);
        CHECK(read(p.file("Data/User.ithasset")) == userText);
        CHECK(p.model().prompt() == nullptr);

        // A folder with everything in it.
        p.navigate("Meshes/Sub");
        p.execute("ith.content.delete", R"({"items": [{"path": "Meshes"}], "confirmed": true})");
        CHECK_FALSE(exists(p.file("Meshes")));
        CHECK(p.assets->find(inner) == nullptr);
        CHECK(p.model().currentFolder().empty()); // the nearest folder that is left (requirement 39)
        CHECK_FALSE(p.model().folderExists("Meshes"));
        CHECK(undoFree(p));
    }

    TEST_CASE("the delete preview lists the open level when only its unsaved state refers to the asset "
              "(requirement 34)") {
        Project p;
        const AssetId mesh = p.imported("Meshes/Rock.gltf");
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        p.rescan();
        const Value none = p.execute("ith.content.delete", R"({"items": [)" + idItem(mesh) + "]}");
        CHECK(none.find("preview")->find("uses")->elements().empty());
        run(*p.session, "ith.entity.create",
            R"({"id": ")" + ith::test::editor::idText(3) + R"(", "components": {"ith::test::editor::Gadget":
                {"asset": {"asset": ")" +
                mesh.toString() + R"(", "subAsset": "00000000-0000-0000-0000-000000000000"}}}})");
        const Value used = p.execute("ith.content.delete", R"({"items": [)" + idItem(mesh) + "]}");
        const Value& uses = *used.find("preview")->find("uses");
        REQUIRE(uses.elements().size() == 1);
        const Value& level = uses.elements()[0].find("users")->elements().at(0);
        CHECK(level.find("path")->asString() == "Levels/Main.ithmap");
        CHECK(level.find("openLevel")->asBool());
        CHECK(level.find("unsaved")->asBool());
        CHECK(level.find("id")->asString() == p.session->document().levelId().toString());
    }

    TEST_CASE("delete refuses the root and the open level, and removes orphan sidecars and problem entries "
              "(requirements 37, 38)") {
        Project p;
        p.write("S/Gone.gltf.meta", "{}");
        p.write("S/Loose.gltf", "glTF");
        p.native("S/Keep.ithasset");
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        p.rescan();
        fails(*p.session, "ith.content.delete", R"({"items": [{"path": ""}], "confirmed": true})",
              "editor.content.root");
        fails(*p.session, "ith.content.delete", R"({"items": [{"path": "Levels/Main.ithmap"}], "confirmed": true})",
              "editor.content.level_open");
        fails(*p.session, "ith.content.delete", R"({"items": [{"path": "S/Keep.ithasset"}, {"path": "Levels"}],
              "confirmed": true})",
              "editor.content.level_open");
        fails(*p.session, "ith.content.delete", R"({"items": [{"path": "S/Keep.ithasset"}], "confirmed": 1})",
              "editor.command.invalid_args");
        CHECK(exists(p.file("Levels/Main.ithmap")));
        CHECK(exists(p.file("S/Keep.ithasset")));

        const Value deleted = p.execute("ith.content.delete", R"({"items": [{"path": "S/Gone.gltf.meta"},
            {"path": "S/Loose.gltf"}], "confirmed": true})");
        CHECK(pathsOf(*deleted.find("deleted")) == std::vector<std::string>{"S/Gone.gltf.meta", "S/Loose.gltf"});
        CHECK(namesIn(p.file("S")) == std::vector<std::string>{"Keep.ithasset"});
        CHECK(p.model().problems().empty());
        CHECK(p.model().findByPath("S/Gone.gltf.meta") == nullptr);
        // Nothing selected: nothing to delete, no prompt.
        CHECK(p.execute("ith.content.delete").kind() == Value::Kind::Null);
        CHECK(p.model().prompt() == nullptr);
    }

    TEST_CASE("import: copies with their dependencies and referenced files, selected in the destination "
              "(requirements 41, 42, 44)") {
        Project p;
        TempProject outside;
        const auto source = [&outside](std::string_view name) { return ith::fs::joinPath(outside.root(), name); };
        REQUIRE(ith::fs::writeFileAtomic(source("Rock.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Ship.gltf"), "dep:Ship.bin\nref:tex/Hull.png\n").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Ship.bin"), "bytes").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("tex/Hull.png"), "png").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("notes.txt"), "x").hasValue());
        p.folder("Meshes");
        p.write("Here/InPlace.gltf", "glTF");
        p.rescan();
        REQUIRE(badgeOf(p.item("Here/InPlace.gltf"), ith::AssetProblemKind::MissingSidecar) != nullptr);

        const Value result = p.execute("ith.content.import",
                                       R"({"files": [)" + jsonPath(source("Rock.glb")) + ", " +
                                           jsonPath(source("Ship.gltf")) + ", " + jsonPath(source("notes.txt")) + ", " +
                                           jsonPath(p.file("Here/InPlace.gltf")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*result.find("imported")) == std::vector<std::string>{"Meshes/Rock.glb", "Meshes/Ship.gltf",
                                                                            "Meshes/tex/Hull.png",
                                                                            "Here/InPlace.gltf"});
        REQUIRE(result.find("skipped")->elements().size() == 1);
        CHECK(result.find("skipped")->elements()[0].find("reason")->asString() == "no importer for .txt");
        CHECK(result.find("failed")->elements().empty());
        CHECK(exists(p.file("Meshes/Rock.glb.meta")));
        CHECK(exists(p.file("Meshes/Ship.bin")));
        CHECK(exists(p.file("Meshes/tex/Hull.png.meta")));
        CHECK_FALSE(exists(p.file("Meshes/notes.txt")));
        // A file already inside Content is imported in place, not copied, and is no longer "Not imported".
        CHECK(exists(p.file("Here/InPlace.gltf.meta")));
        CHECK(p.model().findByPath("Here/InPlace.gltf")->kind == ith::ContentItemKind::Asset);
        CHECK(p.model().problems().empty());
        // The destination is current, with the imported assets there selected.
        CHECK(p.model().currentFolder() == "Meshes");
        CHECK(p.selected() == std::vector<std::string>{"Meshes/Rock.glb", "Meshes/Ship.gltf"});
        CHECK(undoFree(p));
    }

    TEST_CASE("import conflicts: nothing without a choice, then Keep Both, Skip and Replace (requirement 43)") {
        Project p;
        TempProject outside;
        const auto source = [&outside](std::string_view name) { return ith::fs::joinPath(outside.root(), name); };
        p.write("Meshes/Rock.glb", "obj:A\nobj:B\n");
        const ith::Expected<ith::ImportReport> first = p.assets->import("Meshes/Rock.glb");
        REQUIRE(first.hasValue());
        const AssetId rock = first->id;
        p.native("Meshes/Ship.ithasset");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(source("ROCK.glb"), "obj:B\nobj:C\n").hasValue());
        REQUIRE(
            ith::fs::writeFileAtomic(source("Ship.ithasset"), nativeReferring(AssetId::generate(), rock)).hasValue());
        const std::string files = "[" + jsonPath(source("ROCK.glb")) + ", " + jsonPath(source("Ship.ithasset")) + "]";

        const Value asked = p.execute("ith.content.import", R"({"files": )" + files + R"(, "folder": "Meshes"})");
        CHECK_FALSE(asked.find("done")->asBool());
        const Value& conflicts = *asked.find("conflicts");
        REQUIRE(conflicts.elements().size() == 2);
        CHECK(conflicts.elements()[0].find("target")->asString() == "Meshes/Rock.glb");
        CHECK(conflicts.elements()[0].find("replace")->asBool());
        CHECK(conflicts.elements()[1].find("target")->asString() == "Meshes/Ship.ithasset");
        CHECK_FALSE(conflicts.elements()[1].find("replace")->asBool());
        CHECK(namesIn(p.file("Meshes")) == std::vector<std::string>{"Rock.glb", "Rock.glb.meta", "Ship.ithasset"});
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::ImportConflicts);

        // Skip leaves both out.
        const Value skipped =
            p.execute("ith.content.import", R"({"files": )" + files + R"(, "folder": "Meshes", "conflict": "skip"})");
        CHECK(skipped.find("imported")->elements().empty());
        CHECK(skipped.find("skipped")->elements().size() == 2);
        CHECK(p.model().prompt() == nullptr);
        // Keep Both gives the next free names.
        const Value both = p.execute("ith.content.import",
                                     R"({"files": )" + files + R"(, "folder": "Meshes", "conflict": "keep_both"})");
        CHECK(pathsOf(*both.find("imported")) ==
              std::vector<std::string>{"Meshes/ROCK_2.glb", "Meshes/Ship_2.ithasset"});
        // Replace: the imported file's source is overwritten and reimported, keeping its id and the ids of
        // matching sub-assets; the native asset can't be replaced and is skipped with the reason.
        const std::vector<ith::SubAssetEntry> before = p.assets->find(rock)->subAssets;
        const Value replaced = p.execute("ith.content.import",
                                         R"({"files": )" + files + R"(, "folder": "Meshes", "conflict": "replace"})");
        REQUIRE(replaced.find("imported")->elements().size() == 1);
        CHECK(replaced.find("imported")->elements()[0].find("id")->asString() == rock.toString());
        CHECK(replaced.find("imported")->elements()[0].find("replaced")->asBool());
        CHECK(read(p.file("Meshes/Rock.glb")) == "obj:B\nobj:C\n");
        REQUIRE(replaced.find("skipped")->elements().size() == 1);
        CHECK(replaced.find("skipped")->elements()[0].find("reason")->asString().find("can't be replaced") !=
              std::string::npos);
        const ith::AssetRecord* after = p.assets->find(rock);
        REQUIRE(after != nullptr);
        const auto idOf = [](const std::vector<ith::SubAssetEntry>& table, std::string_view key) {
            const auto found =
                std::ranges::find_if(table, [key](const ith::SubAssetEntry& entry) { return entry.key == key; });
            return found != table.end() ? found->id : ith::SubAssetId();
        };
        CHECK(idOf(after->subAssets, "mesh:B") == idOf(before, "mesh:B"));
        CHECK_FALSE(idOf(after->subAssets, "mesh:C").isNil());
        // Per-file answers of the dialog override the default.
        const Value chosen =
            p.execute("ith.content.import", R"({"files": )" + files +
                                                R"(, "folder": "Meshes", "conflict": "skip", "choices": {)" +
                                                jsonPath(source("ROCK.glb")) + R"(: "keep_both"}})");
        CHECK(pathsOf(*chosen.find("imported")) == std::vector<std::string>{"Meshes/ROCK_3.glb"});
        fails(*p.session, "ith.content.import", R"({"files": )" + files + R"(, "conflict": "merge"})",
              "editor.command.invalid_args");
        fails(*p.session, "ith.content.import", R"({"files": )" + files + R"(, "choices": {"x": "merge"}})",
              "editor.command.invalid_args");
    }

    TEST_CASE("import: dropped folders with subfolders, native assets and their ids, failed imports and "
              "refusals (requirements 45 to 47)") {
        Project p;
        TempProject outside;
        const auto source = [&outside](std::string_view name) { return ith::fs::joinPath(outside.root(), name); };
        const AssetId known = p.native("Known.ithasset");
        p.folder("Meshes");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(source("Pack/Rocks/A.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Pack/B.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Pack/readme.txt"), "x").hasValue());
        REQUIRE(ith::fs::createDirectories(source("Pack/Empty")).hasValue());
        const Value pack =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(source("Pack")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*pack.find("imported")) ==
              std::vector<std::string>{"Meshes/Pack/B.glb", "Meshes/Pack/Rocks/A.glb"});
        CHECK(pathsOf(*pack.find("skipped")) == std::vector<std::string>{source("Pack/readme.txt")});
        CHECK(p.model().folderExists("Meshes/Pack/Rocks"));
        CHECK(p.model().folderExists("Meshes/Pack/Empty"));

        // A native asset keeps its id unless the database has it (requirement 46).
        const AssetId fresh = AssetId::generate();
        REQUIRE(
            ith::fs::writeFileAtomic(source("Fresh.ithasset"), ith::test::editor::nativeAsset(fresh, "ith.material"))
                .hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Copy.ithasset"), ith::test::editor::nativeAsset(known, "ith.material"))
                    .hasValue());
        const Value natives =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(source("Fresh.ithasset")) + ", " +
                                                jsonPath(source("Copy.ithasset")) + R"(], "folder": ""})");
        REQUIRE(natives.find("imported")->elements().size() == 2);
        CHECK(natives.find("imported")->elements()[0].find("id")->asString() == fresh.toString());
        CHECK_FALSE(natives.find("imported")->elements()[0].find("newId")->asBool());
        CHECK(natives.find("imported")->elements()[1].find("id")->asString() != known.toString());
        CHECK(natives.find("imported")->elements()[1].find("newId")->asBool());
        CHECK(p.assets->find(known)->path == "Known.ithasset");

        // A failed import keeps the copy as a "Not imported" entry and reports the importer's error
        // (requirement 47); a missing file fails alone.
        REQUIRE(ith::fs::writeFileAtomic(source("Bad.gltf"), "fail\n").hasValue());
        const Value bad =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(source("Bad.gltf")) + ", " +
                                                jsonPath(source("Missing.glb")) + R"(], "folder": "Meshes"})");
        REQUIRE(bad.find("failed")->elements().size() == 2);
        CHECK(bad.find("failed")->elements()[0].find("code")->asString() == "fs.not_found");
        CHECK(bad.find("failed")->elements()[1].find("code")->asString() == "import.malformed");
        CHECK(exists(p.file("Meshes/Bad.gltf")));
        REQUIRE(p.model().findByPath("Meshes/Bad.gltf") != nullptr);
        CHECK(badgeOf(p.item("Meshes/Bad.gltf"), ith::AssetProblemKind::MissingSidecar) != nullptr);

        fails(*p.session, "ith.content.import", R"({"folder": "Meshes"})", "editor.command.invalid_args");
        fails(*p.session, "ith.content.import", R"({"files": [""]})", "editor.command.invalid_args");
        fails(*p.session, "ith.content.import", R"({"files": [], "folder": "Nowhere"})", "editor.content.not_found");
        fails(*p.session, "ith.content.import", R"({"files": [], "folder": "Known.ithasset"})",
              "editor.content.invalid_target");
    }

    TEST_CASE("reimport skips native assets, reports the cache and missing sub-assets; native assets alone are "
              "refused (requirement 48)") {
        Project p;
        p.write("S/Rock.glb", "obj:A\nobj:B\n");
        const ith::Expected<ith::ImportReport> first = p.assets->import("S/Rock.glb");
        REQUIRE(first.hasValue());
        p.native("S/Ship.ithasset");
        p.rescan();
        p.navigate("S");
        p.execute("ith.content.select_all");
        const Value unchanged = p.execute("ith.content.reimport");
        REQUIRE(unchanged.find("reimported")->elements().size() == 1);
        CHECK(unchanged.find("reimported")->elements()[0].find("fromCache")->asBool());
        CHECK(pathsOf(*unchanged.find("skipped")) == std::vector<std::string>{"S/Ship.ithasset"});

        p.write("S/Rock.glb", "obj:A\n");
        const Value changed = p.execute("ith.content.reimport", R"({"items": [{"path": "S/Rock.glb"}]})");
        const Value& entry = changed.find("reimported")->elements().at(0);
        CHECK_FALSE(entry.find("fromCache")->asBool());
        REQUIRE(entry.find("missing")->elements().size() == 1);
        CHECK(entry.find("missing")->elements()[0].asString() == "mesh:B");

        fails(*p.session, "ith.content.reimport", R"({"items": [{"path": "S/Ship.ithasset"}]})",
              "editor.content.not_imported");
    }

    TEST_CASE("fix imports a \"Not imported\" file in place and gives an unresolved duplicate a new id; both "
              "become assets (requirement 49)") {
        Project p;
        const AssetId shared = AssetId::generate();
        p.native("S/A.ithasset", "ith.material", shared);
        p.native("S/B.ithasset", "ith.material", shared);
        p.write("S/Loose.gltf", "glTF");
        p.write("S/notes.txt", "x");
        p.rescan(); // a first scan: the duplicate stays unresolved
        p.navigate("S");
        REQUIRE(p.model().findByPath("S/B.ithasset") != nullptr);
        REQUIRE(p.model().findByPath("S/B.ithasset")->kind == ith::ContentItemKind::Problem);

        p.select({"S/Loose.gltf"}, "replace");
        const Value imported = p.execute("ith.content.fix");
        CHECK(imported.find("path")->asString() == "S/Loose.gltf");
        CHECK(p.model().findByPath("S/Loose.gltf")->kind == ith::ContentItemKind::Asset);
        CHECK(exists(p.file("S/Loose.gltf.meta")));
        CHECK(p.selected() == std::vector<std::string>{"S/Loose.gltf"}); // the entry stays selected as an asset

        const Value assigned = p.execute("ith.content.fix", R"({"item": {"path": "S/B.ithasset"}})");
        CHECK(assigned.find("id")->asString() != shared.toString());
        CHECK(p.model().findByPath("S/B.ithasset")->kind == ith::ContentItemKind::Asset);
        CHECK(p.model().problems().empty());

        fails(*p.session, "ith.content.fix", R"({"item": {"path": "S/A.ithasset"}})", "editor.content.invalid_target");
        fails(*p.session, "ith.content.fix", R"({"item": {"path": "S/Nothing"}})", "editor.content.not_found");
        CHECK(undoFree(p));
    }

    TEST_CASE("F2, Delete and Ctrl + D are the content file operations in the Content context") {
        Project p;
        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const ith::Keymap keymap = ith::Keymap::load(config, p.session->commands(), warnings);
        for (const auto& [chord, id] :
             {std::pair{"Keyboard.F2", "ith.content.rename"}, std::pair{"Keyboard.Delete", "ith.content.delete"},
              std::pair{"Ctrl+Keyboard.D", "ith.content.duplicate"}}) {
            CAPTURE(id);
            REQUIRE(keymap.commandFor(*ith::parseChord(chord), ith::KeymapContext::Content) != nullptr);
            CHECK(*keymap.commandFor(*ith::parseChord(chord), ith::KeymapContext::Content) == id);
            CHECK(p.session->commands().find(id) != nullptr);
            CHECK(std::ranges::none_of(warnings, [&](const ith::Error& warning) {
                return warning.message == "no command " + std::string(id);
            }));
        }
        // With the focus elsewhere they stay the entity commands.
        CHECK(*keymap.commandFor(*ith::parseChord("Keyboard.Delete")) == "ith.edit.delete");
        p.native("S/A.ithasset");
        p.rescan();
        p.navigate("S");
        ith::KeymapConsumer shortcuts(*p.session, keymap);
        p.model().setKeyboardFocus(true);
        p.select({"S/A.ithasset"}, "replace");
        shortcuts.onInputEvent(keyDown(ith::Key::Delete));
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Delete);
        CHECK(exists(p.file("S/A.ithasset")));
    }

    TEST_CASE("with the focus in the folder tree, rename and delete without a target act on the current folder "
              "(spec 3.6, 3.8, 3.11)") {
        Project p;
        p.native("S/A.ithasset");
        p.native("S/Sub/B.ithasset");
        p.rescan();
        p.navigate("S");
        p.select({"S/A.ithasset"}, "replace");
        const ContentKey folderKey{.id = AssetId(), .path = "S"};

        SUBCASE("tree focus: the current folder") {
            p.model().setKeyboardFocus(true);
            p.model().setTreeFocus(true);
            REQUIRE(p.model().treeFocus());
            const Value prompt = p.execute("ith.content.rename");
            CHECK(textOf(prompt, "path") == "S");
            CHECK(textOf(prompt, "name") == "S");
            REQUIRE(p.model().prompt() != nullptr);
            CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Rename);
            CHECK(p.model().prompt()->item == folderKey);

            const Value preview = p.execute("ith.content.delete");
            CHECK_FALSE(preview.find("deleted")->asBool());
            CHECK(pathsOf(*preview.find("preview")->find("items")) == std::vector<std::string>{"S"});
            REQUIRE(p.model().prompt() != nullptr);
            CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Delete);

            // An explicit target still wins over the tree's folder.
            const Value item = p.execute("ith.content.rename", R"({"item": {"path": "S/A.ithasset"}})");
            CHECK(textOf(item, "path") == "S/A.ithasset");
            const Value items = p.execute("ith.content.delete", R"({"items": [{"path": "S/Sub"}]})");
            CHECK(pathsOf(*items.find("preview")->find("items")) == std::vector<std::string>{"S/Sub"});

            // The answers without a target rename and delete the folder itself.
            const Value renamed = p.execute("ith.content.rename", R"({"name": "T"})");
            CHECK(textOf(renamed, "path") == "T");
            CHECK(exists(p.file("T/A.ithasset")));
            CHECK_FALSE(exists(p.file("S")));
            CHECK(p.model().currentFolder() == "T");
            const Value deleted = p.execute("ith.content.delete", R"({"confirmed": true})");
            CHECK(pathsOf(*deleted.find("deleted")) == std::vector<std::string>{"T"});
            CHECK_FALSE(exists(p.file("T")));
            CHECK(undoFree(p));
        }

        SUBCASE("content focus: the content selection") {
            p.model().setKeyboardFocus(true);
            p.model().setTreeFocus(false);
            CHECK(textOf(p.execute("ith.content.rename"), "path") == "S/A.ithasset");
            CHECK(pathsOf(*p.execute("ith.content.delete").find("preview")->find("items")) ==
                  std::vector<std::string>{"S/A.ithasset"});
        }

        SUBCASE("the tree's focus counts only while the browser has the keyboard focus") {
            p.model().setKeyboardFocus(true);
            p.model().setTreeFocus(true);
            p.model().setKeyboardFocus(true); // a new report starts in the asset view
            CHECK_FALSE(p.model().treeFocus());
            p.model().setTreeFocus(true);
            p.model().setScreenRects({});
            p.model().pointerPressed(5.0f, 5.0f); // outside the browser
            CHECK_FALSE(p.model().treeFocus());
            CHECK(textOf(p.execute("ith.content.rename"), "path") == "S/A.ithasset");
        }

        SUBCASE("revealInTree expands the folders above a folder, and ignores what isn't one") {
            p.model().setExpanded("S", false);
            const auto rows = [&p] {
                std::vector<std::string> paths;
                for (const ith::ContentFolderRow& row : p.model().treeRows()) {
                    paths.push_back(row.path);
                }
                return paths;
            };
            CHECK(rows() == std::vector<std::string>{"", "S"});
            const auto generation = p.model().generation();
            p.model().revealInTree("S/Nothing");
            p.model().revealInTree("S/A.ithasset");
            CHECK(p.model().generation() == generation);
            p.model().revealInTree("S/Sub");
            CHECK(p.model().isExpanded("S"));
            CHECK(rows() == std::vector<std::string>{"", "S", "S/Sub"});
            const auto expanded = p.model().generation();
            p.model().revealInTree("S/Sub"); // already shown: nothing changes
            CHECK(p.model().generation() == expanded);
        }

        SUBCASE("tree focus on the root: refused, nothing changes") {
            p.navigate("");
            p.select({"S"}, "replace");
            p.model().setKeyboardFocus(true);
            p.model().setTreeFocus(true);
            fails(*p.session, "ith.content.rename", "{}", "editor.content.root");
            fails(*p.session, "ith.content.rename", R"({"name": "X"})", "editor.content.root");
            fails(*p.session, "ith.content.delete", "{}", "editor.content.root");
            fails(*p.session, "ith.content.delete", R"({"confirmed": true})", "editor.content.root");
            CHECK(p.model().prompt() == nullptr);
            CHECK(exists(p.file("S/A.ithasset")));
        }
    }

    TEST_CASE("without an asset database the file operations fail") {
        ith::EditorSession session(ith::test::editor::types());
        for (const std::string_view id :
             {"ith.content.new_folder", "ith.content.rename", "ith.content.move", "ith.content.duplicate",
              "ith.content.delete", "ith.content.import", "ith.content.reimport", "ith.content.fix"}) {
            CAPTURE(id);
            fails(session, id, "{}", "editor.document.no_asset_database");
        }
    }

    TEST_CASE("dismiss ends a pending prompt first, then clears the selection, then closes the drawer") {
        Project p;
        p.native("S/A.ithasset");
        p.rescan();
        p.navigate("S");
        p.execute("ith.content.toggle_drawer", R"({"open": true})");
        p.select({"S/A.ithasset"}, "replace");
        p.execute("ith.content.rename");
        REQUIRE(p.model().prompt() != nullptr);
        const Value first = p.execute("ith.content.dismiss");
        CHECK(p.model().prompt() == nullptr);
        CHECK(first.find("selection")->elements().size() == 1);
        CHECK(first.find("drawerOpen")->asBool());
        CHECK(p.execute("ith.content.dismiss").find("selection")->elements().empty());
        CHECK(p.model().drawerOpen());
        CHECK_FALSE(p.execute("ith.content.dismiss").find("drawerOpen")->asBool());
    }

    TEST_CASE("a pending rename follows its item through a move, and goes when the item goes") {
        Project p;
        p.folder("A/Sub");
        p.folder("B");
        p.rescan();
        p.execute("ith.content.rename", R"({"item": {"path": "A/Sub"}})");
        REQUIRE(p.model().prompt() != nullptr);
        p.execute("ith.content.move", R"({"items": [{"path": "A"}], "folder": "B"})");
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->item == ContentKey{.id = AssetId(), .path = "B/A/Sub"});
        CHECK(p.execute("ith.query.content").find("prompt")->find("path")->asString() == "B/A/Sub");
        // The rename's answer reaches the moved folder.
        CHECK(p.execute("ith.content.rename", R"({"item": {"path": "B/A/Sub"}, "name": "Inner"})")
                  .find("path")
                  ->asString() == "B/A/Inner");
        // A rename whose item is deleted ends.
        p.execute("ith.content.rename", R"({"item": {"path": "B/A/Inner"}})");
        p.execute("ith.content.delete", R"({"items": [{"path": "B/A"}], "confirmed": true})");
        CHECK(p.model().prompt() == nullptr);
        // And one whose item went outside the editor ends at the next Rescan.
        p.execute("ith.content.rename", R"({"item": {"path": "B"}})");
        REQUIRE(ith::fs::remove(p.file("B")).hasValue());
        p.rescan();
        CHECK(p.model().prompt() == nullptr);
    }

    TEST_CASE("an import of more than 4096 files and folders imports nothing") {
        Project p;
        p.rescan();
        std::string files = "[";
        for (int i = 0; i <= 4096; ++i) {
            files += (i > 0 ? ", " : "") + jsonString("Missing/" + std::to_string(i) + ".glb");
        }
        files += "]";
        fails(*p.session, "ith.content.import", R"({"files": )" + files + "}", "editor.content.too_many_files");
        CHECK(p.model().problems().empty());
    }

    TEST_CASE("a dropped folder merges into a folder of the same name in another case, on every system") {
        Project p;
        TempProject outside;
        p.folder("Meshes/pack");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(outside.root(), "Pack/Rocks/A.glb"), "glTF").hasValue());
        const Value result =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(ith::fs::joinPath(outside.root(), "Pack")) +
                                                R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*result.find("imported")) == std::vector<std::string>{"Meshes/pack/Rocks/A.glb"});
        CHECK(namesIn(p.file("Meshes")) == std::vector<std::string>{"pack"});
        CHECK(p.model().folderExists("Meshes/pack/Rocks"));
    }

    TEST_CASE("import: links inside a dropped folder are skipped, a link given on its own is imported, and a "
              "referenced file missing beside the source is reported") {
        Project p;
        TempProject outside;
        const auto source = [&outside](std::string_view name) { return ith::fs::joinPath(outside.root(), name); };
        p.folder("Meshes");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(source("Real.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Pack/B.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Ship.gltf"), "ref:tex/Gone.png\n").hasValue());
        const Value missing = p.execute("ith.content.import",
                                        R"({"files": [)" + jsonPath(source("Ship.gltf")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*missing.find("imported")) == std::vector<std::string>{"Meshes/Ship.gltf"});
        REQUIRE(missing.find("skipped")->elements().size() == 1);
        CHECK(textOf(missing.find("skipped")->elements()[0], "reason").find("not beside it") != std::string::npos);

        std::error_code error;
        std::filesystem::create_symlink(nativeOf(source("Real.glb")), nativeOf(source("Pack/Link.glb")), error);
        if (error) {
            MESSAGE("symbolic links can't be created here; the link cases are skipped");
            return;
        }
        std::filesystem::create_symlink(nativeOf(source("Real.glb")), nativeOf(source("Alone.glb")), error);
        REQUIRE_FALSE(error);
        const Value pack =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(source("Pack")) + ", " +
                                                jsonPath(source("Alone.glb")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*pack.find("imported")) == std::vector<std::string>{"Meshes/Pack/B.glb", "Meshes/Alone.glb"});
        CHECK(pathsOf(*pack.find("skipped")) == std::vector<std::string>{source("Pack/Link.glb")});
        CHECK(read(p.file("Meshes/Alone.glb")) == "glTF");
    }

#ifndef _WIN32
    TEST_CASE("import skips a given path whose name holds a backslash on POSIX") {
        Project p;
        TempProject outside;
        p.rescan();
        const std::string odd = outside.root() + R"(/odd\name.glb)";
        const Value result = p.execute("ith.content.import", R"({"files": [)" + jsonPath(odd) + R"(], "folder": ""})");
        CHECK(result.find("imported")->elements().empty());
        REQUIRE(result.find("skipped")->elements().size() == 1);
        CHECK(textOf(result.find("skipped")->elements()[0], "reason").find("can't be used") != std::string::npos);
    }
#endif

    TEST_CASE("after a confirmed delete the user still loads, keeping the reference to the deleted id "
              "(requirement 35, as far as levels go today)") {
        Project p;
        const AssetId mesh = p.imported("Meshes/Rock.gltf");
        const AssetId level = AssetId::generate();
        p.write("Levels/Main.ithmap", levelReferring(level, mesh));
        p.rescan();
        const std::string before = read(p.file("Levels/Main.ithmap"));
        p.execute("ith.content.delete", R"({"items": [)" + idItem(mesh) + R"(], "confirmed": true})");
        CHECK(read(p.file("Levels/Main.ithmap")) == before);
        p.execute("ith.content.open", R"({"item": {"path": "Levels/Main.ithmap"}})");
        CHECK(p.session->document().path() == "Content/Levels/Main.ithmap");
        CHECK(ith::test::editor::levelText(*p.session).find(mesh.toString()) != std::string::npos);
    }

    TEST_CASE("folders of one import that differ only in case merge into the first spelling, on every system") {
        Project p;
        TempProject first;
        TempProject second;
        p.folder("Meshes");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(first.root(), "Foo/A.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(second.root(), "foo/B.glb"), "glTF").hasValue());
        const Value result = p.execute(
            "ith.content.import", R"({"files": [)" + jsonPath(ith::fs::joinPath(first.root(), "Foo")) + ", " +
                                      jsonPath(ith::fs::joinPath(second.root(), "foo")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*result.find("imported")) == std::vector<std::string>{"Meshes/Foo/A.glb", "Meshes/Foo/B.glb"});
        CHECK(namesIn(p.file("Meshes")) == std::vector<std::string>{"Foo"});
        // Siblings inside one dropped folder, where the file system keeps both.
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(first.root(), "Pack/Sub/C.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(first.root(), "Pack/sub/D.glb"), "glTF").hasValue());
        if (namesIn(ith::fs::joinPath(first.root(), "Pack")).size() < 2) {
            MESSAGE("the file system ignores case; the sibling case is skipped");
            return;
        }
        const Value siblings =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(ith::fs::joinPath(first.root(), "Pack")) +
                                                R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*siblings.find("imported")) ==
              std::vector<std::string>{"Meshes/Pack/Sub/C.glb", "Meshes/Pack/Sub/D.glb"});
        CHECK(namesIn(p.file("Meshes/Pack")) == std::vector<std::string>{"Sub"});
    }

    TEST_CASE("conflict choices name files inside dropped folders, by absolute path, relative path or with '\\'") {
        Project p;
        const std::string outside = ith::fs::joinPath(p.project.root(), "Outside");
        p.write("Meshes/Pack/B.glb", "glTF");
        REQUIRE(p.assets->import("Meshes/Pack/B.glb").hasValue());
        p.write("Meshes/Pack/C.glb", "glTF");
        REQUIRE(p.assets->import("Meshes/Pack/C.glb").hasValue());
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(outside, "Pack/B.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(outside, "Pack/C.glb"), "glTF").hasValue());
        const Value asked = p.execute("ith.content.import", R"({"files": ["Outside/Pack"], "folder": "Meshes"})");
        REQUIRE(asked.find("conflicts")->elements().size() == 2);
        CHECK(textOf(asked.find("conflicts")->elements()[0], "file") == ith::fs::joinPath(outside, "Pack/B.glb"));
        // B by its absolute path, C relative to the project and spelled with '\'.
        const Value chosen =
            p.execute("ith.content.import", R"({"files": ["Outside/Pack"], "folder": "Meshes", "choices": {)" +
                                                jsonPath(ith::fs::joinPath(outside, "Pack/B.glb")) +
                                                R"(: "keep_both", "Outside\\Pack\\C.glb": "skip"}})");
        CHECK(pathsOf(*chosen.find("imported")) == std::vector<std::string>{"Meshes/Pack/B_2.glb"});
        CHECK(pathsOf(*chosen.find("skipped")) == std::vector<std::string>{ith::fs::joinPath(outside, "Pack/C.glb")});
    }

#ifdef _WIN32
    TEST_CASE("on Windows, a rename that changes only the case outside ASCII is refused and changes nothing") {
        Project p;
        const AssetId id = p.native("S/\xc3\xa9t\xc3\xa9.ithasset"); // "été"
        p.rescan();
        const auto result = p.session->execute(
            "ith.content.rename", ith::test::editor::json(R"({"item": )" + idItem(id) + R"(, "name": "Été"})"));
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "editor.content.exists");
        CAPTURE(result.error().message);
        CHECK(result.error().message.find("same name to the file system") != std::string::npos);
        CHECK(p.assets->find(id)->path == "S/\xc3\xa9t\xc3\xa9.ithasset");
        CHECK(namesIn(p.file("S")) == std::vector<std::string>{"\xc3\xa9t\xc3\xa9.ithasset"});
    }

    TEST_CASE("on Windows, a junction inside a dropped folder is skipped and one dropped on its own is imported") {
        Project p;
        TempProject outside;
        const auto source = [&outside](std::string_view name) { return ith::fs::joinPath(outside.root(), name); };
        p.folder("Meshes");
        p.rescan();
        REQUIRE(ith::fs::writeFileAtomic(source("Real/R.glb"), "glTF").hasValue());
        REQUIRE(ith::fs::writeFileAtomic(source("Pack/B.glb"), "glTF").hasValue());
        if (!ith::test::makeJunction(source("Pack/Inner"), source("Real")) ||
            !ith::test::makeJunction(source("Alone"), source("Real"))) {
            MESSAGE("junctions can't be made here; skipped");
            return;
        }
        const Value result =
            p.execute("ith.content.import", R"({"files": [)" + jsonPath(source("Pack")) + ", " +
                                                jsonPath(source("Alone")) + R"(], "folder": "Meshes"})");
        CHECK(pathsOf(*result.find("imported")) == std::vector<std::string>{"Meshes/Pack/B.glb", "Meshes/Alone/R.glb"});
        CHECK(pathsOf(*result.find("skipped")) == std::vector<std::string>{source("Pack/Inner")});
    }
#endif

    TEST_CASE("a level opened under another spelling of its path follows a rename of its folder") {
        Project p;
        run(*p.session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        run(*p.session, "ith.file.new_level");
        run(*p.session, "ith.file.open_level", R"({"path": "content/levels/MAIN.ithmap"})");
        if (p.session->lastFileError().has_value()) {
            MESSAGE("the file system keeps case, so the other spelling is another file; skipped");
            return;
        }
        p.rescan();
        REQUIRE(p.session->levelContentPath() == std::string("Levels/Main.ithmap"));
        p.execute("ith.content.rename", R"({"item": {"path": "Levels"}, "name": "Maps"})");
        CHECK(p.session->document().path() == "Content/Maps/Main.ithmap");
        CHECK_FALSE(p.session->document().dirty());
    }
}
