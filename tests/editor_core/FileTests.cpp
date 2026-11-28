// The editor document's files (ADR-0037 rules 15 and 16) and the queries (rule 17): Save As and
// Save with and without a job system, a reopened level equal to the saved one, the saved marker
// across a failed write, unsaved changes before New and Open, a failed open that keeps the level,
// commands refused while a level opens, opening by asset id, the level id after Save As (ADR-0050
// item 9), and what the queries return.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Importer.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using ith::EditorSession;
using ith::test::editor::fails;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::run;
using ith::test::editor::TempProject;
using ith::test::editor::types;

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

void build(EditorSession& session) {
    run(session, "ith.entity.create", R"({"id": )" + quoted(1) + R"(, "label": "Table"})");
    run(session, "ith.entity.create",
        R"({"id": )" + quoted(2) + R"(, "parent": )" + quoted(1) +
            R"(, "components": {"ith::test::editor::Lamp": {"intensity": 3}, "ith::test::editor::Aim": {"target": )" +
            quoted(1) + "}}}");
}

std::string hashOf(EditorSession& session) {
    return run(session, "ith.query.world_hash").find("hash")->asString();
}

// The level id in the header of the level file at `path` (relative to the project).
ith::Uuid fileIdOf(const TempProject& project, std::string_view path) {
    const ith::Expected<std::string> text =
        ith::fs::readFile(ith::fs::joinPath(project.root(), path), std::size_t{1} << 20);
    REQUIRE(text.hasValue());
    const auto header = ith::readAssetHeader(*text);
    REQUIRE(header.hasValue());
    CHECK(header->format == ith::kLevelFormat);
    return header->id.uuid();
}

} // namespace

TEST_SUITE("editor_core.files") {
    TEST_CASE("save as, then open in a new session, gives the same world; with and without a job system") {
        for (const bool withJobs : {false, true}) {
            CAPTURE(withJobs);
            TempProject project;
            ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
            const ith::EditorSessionOptions options{.projectDirectory = project.root(),
                                                    .jobs = withJobs ? &jobs : nullptr};
            std::string hash;
            {
                EditorSession session(types(), options);
                build(session);
                hash = hashOf(session);
                CHECK(session.document().dirty());
                run(session, "ith.file.save_as", R"({"path": "Content/Levels/Room.ithmap"})");
                session.waitIdle();
                CHECK_FALSE(session.document().dirty());
                CHECK_FALSE(session.document().saving());
                CHECK(session.document().path() == "Content/Levels/Room.ithmap");
                CHECK_FALSE(session.lastFileError().has_value());
            }
            const auto info = ith::fs::fileInfo(ith::fs::joinPath(project.root(), "Content/Levels/Room.ithmap"));
            REQUIRE(info.hasValue());
            REQUIRE(info->has_value());

            EditorSession reopened(types(), options);
            run(reopened, "ith.file.open_level", R"({"path": "Content/Levels/Room.ithmap"})");
            reopened.waitIdle();
            REQUIRE_FALSE(reopened.lastFileError().has_value());
            CHECK(hashOf(reopened) == hash);
            CHECK_FALSE(reopened.document().dirty());
            CHECK(reopened.document().history().entries().empty());
        }
    }

    TEST_CASE("a save in flight: edits go on, a second save and an open are refused, the marker holds") {
        TempProject project;
        ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
        EditorSession session(types(), {.projectDirectory = project.root(), .jobs = &jobs});
        build(session);
        run(session, "ith.file.save_as", R"({"path": "Content/Room.ithmap"})");
        CHECK(session.document().saving()); // finishes only through the main-thread queue
        fails(session, "ith.file.save", "{}", "editor.document.save_in_progress");
        fails(session, "ith.file.open_level", R"({"path": "Content/Room.ithmap"})", "editor.document.save_in_progress");
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Edited while saving"})");
        session.waitIdle();
        CHECK_FALSE(session.document().saving());
        CHECK(session.document().dirty()); // the rename came after the saved state
        run(session, "ith.edit.undo");
        CHECK_FALSE(session.document().dirty());
    }

    TEST_CASE("a failed write puts the saved marker back, so the level stays dirty") {
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        build(session);
        // A directory where the file should go: the write fails.
        REQUIRE(ith::fs::createDirectories(ith::fs::joinPath(project.root(), "Content/Taken.ithmap")).hasValue());
        run(session, "ith.file.save_as", R"({"path": "Content/Taken.ithmap"})");
        REQUIRE(session.lastFileError().has_value());
        CHECK(session.document().dirty());
        CHECK(session.document().path().empty());
        const ith::serialization::Value document = run(session, "ith.query.document");
        CHECK(document.find("lastError")->find("code")->asString() == session.lastFileError()->code);
    }

    TEST_CASE("unsaved changes before New and Open: refused, saved first, or discarded") {
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        build(session);
        fails(session, "ith.file.new_level", "{}", "editor.document.unsaved_changes");
        fails(session, "ith.file.new_level", R"({"unsaved": "save"})", "editor.document.no_path");
        fails(session, "ith.file.new_level", R"({"unsaved": "maybe"})", "editor.command.invalid_args");
        run(session, "ith.file.save_as", R"({"path": "Content/A.ithmap"})");
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Saved first"})");
        run(session, "ith.file.new_level", R"({"unsaved": "save"})");
        CHECK(session.document().world().entityCount() == 0);
        run(session, "ith.file.open_level", R"({"path": "Content/A.ithmap"})");
        CHECK(run(session, "ith.query.entity", R"({"id": )" + quoted(1) + "}").find("label")->asString() ==
              "Saved first");
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Dropped"})");
        run(session, "ith.file.new_level", R"({"unsaved": "discard"})");
        run(session, "ith.file.open_level", R"({"path": "Content/A.ithmap"})");
        CHECK(run(session, "ith.query.entity", R"({"id": )" + quoted(1) + "}").find("label")->asString() ==
              "Saved first");
        session.document().setPlaying(true);
        fails(session, "ith.file.new_level", "{}", "editor.command.playing");
        session.document().setPlaying(false);
    }

    TEST_CASE("a failed open keeps the level; while a level opens, edits are refused") {
        TempProject project;
        ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
        EditorSession session(types(), {.projectDirectory = project.root(), .jobs = &jobs});
        build(session);
        run(session, "ith.file.save_as", R"({"path": "Content/B.ithmap"})");
        session.waitIdle();
        const std::string hash = hashOf(session);

        run(session, "ith.file.open_level", R"({"path": "Content/Missing.ithmap"})");
        CHECK(session.document().opening());
        fails(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "x"})", "editor.document.busy");
        fails(session, "ith.edit.undo", "{}", "editor.document.busy");
        run(session, "ith.query.document"); // queries still run
        session.waitIdle();
        REQUIRE(session.lastFileError().has_value());
        CHECK(session.lastFileError()->code == "fs.not_found");
        CHECK(hashOf(session) == hash);
        CHECK(session.document().path() == "Content/B.ithmap");

        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(project.root(), "Content/Broken.ithmap"), "{ not json")
                    .hasValue());
        run(session, "ith.file.open_level", R"({"path": "Content/Broken.ithmap"})");
        session.waitIdle();
        REQUIRE(session.lastFileError().has_value());
        CHECK(session.lastFileError()->code == "serialization.syntax");
        CHECK(hashOf(session) == hash);
    }

    TEST_CASE("open by asset id goes through the asset database") {
        TempProject project;
        EditorSession first(types(), {.projectDirectory = project.root()});
        build(first);
        run(first, "ith.file.save_as", R"({"path": "Content/Levels/C.ithmap"})");
        const std::string levelId = first.document().levelId().toString();
        fails(first, "ith.file.open_level", R"({"id": ")" + levelId + R"("})", "editor.document.no_asset_database");

        const ith::ImporterRegistry importers;
        ith::AssetDatabase assets(project.root(), importers);
        REQUIRE(assets.scan().hasValue());
        EditorSession second(types(), {.projectDirectory = project.root(), .assets = &assets});
        run(second, "ith.file.open_level", R"({"id": ")" + levelId + R"("})");
        CHECK(second.document().path() == "Content/Levels/C.ithmap");
        CHECK(second.document().levelId() == first.document().levelId());
        fails(second, "ith.file.open_level", R"({"id": )" + quoted(77) + "}", "editor.asset.not_found");
        fails(second, "ith.file.open_level", "{}", "editor.command.invalid_args");
    }

    TEST_CASE("Save As to a new path gives the copy a new level id; the original keeps its own; a Save keeps it") {
        for (const bool withJobs : {false, true}) {
            CAPTURE(withJobs);
            TempProject project;
            ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
            EditorSession session(types(), {.projectDirectory = project.root(), .jobs = withJobs ? &jobs : nullptr});
            build(session);
            const ith::Uuid first = session.document().levelId();
            // A level without a file keeps its id on its first file.
            run(session, "ith.file.save_as", R"({"path": "Content/A.ithmap"})");
            session.waitIdle();
            CHECK(session.document().levelId() == first);
            CHECK(fileIdOf(project, "Content/A.ithmap") == first);

            run(session, "ith.file.save_as", R"({"path": "Content/B.ithmap"})");
            if (withJobs) {
                // Until the write has finished, the document keeps its id and path.
                CHECK(session.document().levelId() == first);
                CHECK(session.document().path() == "Content/A.ithmap");
            }
            session.waitIdle();
            REQUIRE_FALSE(session.lastFileError().has_value());
            const ith::Uuid copy = session.document().levelId();
            CHECK(copy != first);
            CHECK_FALSE(copy.isNil());
            CHECK(session.document().path() == "Content/B.ithmap");
            CHECK_FALSE(session.document().dirty());
            CHECK(fileIdOf(project, "Content/A.ithmap") == first);
            CHECK(fileIdOf(project, "Content/B.ithmap") == copy);
            CHECK(run(session, "ith.query.document").find("levelId")->asString() == copy.toString());

            // Save, and Save As to its own path written another way, keep the id.
            run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Renamed"})");
            run(session, "ith.file.save");
            session.waitIdle();
            run(session, "ith.file.save_as",
                R"({"path": ")" + ith::fs::joinPath(project.root(), "Content/B.ithmap") + R"("})");
            session.waitIdle();
            CHECK(session.document().levelId() == copy);
            CHECK(fileIdOf(project, "Content/B.ithmap") == copy);
        }
    }

    TEST_CASE("Save As over another level takes its id; over a file that is not a level it gets a new one") {
        for (const bool withJobs : {false, true}) {
            CAPTURE(withJobs);
            TempProject project;
            ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
            EditorSession session(types(), {.projectDirectory = project.root(), .jobs = withJobs ? &jobs : nullptr});
            run(session, "ith.file.save_as", R"({"path": "Content/Target.ithmap"})");
            session.waitIdle();
            const ith::Uuid target = session.document().levelId();

            run(session, "ith.file.new_level");
            build(session);
            run(session, "ith.file.save_as", R"({"path": "Content/Mine.ithmap"})");
            session.waitIdle();
            const ith::Uuid mine = session.document().levelId();

            run(session, "ith.file.save_as", R"({"path": "Content/Target.ithmap"})");
            session.waitIdle();
            REQUIRE_FALSE(session.lastFileError().has_value());
            CHECK(session.document().levelId() == target);
            CHECK(session.document().path() == "Content/Target.ithmap");
            CHECK(fileIdOf(project, "Content/Target.ithmap") == target);
            CHECK(fileIdOf(project, "Content/Mine.ithmap") == mine);
            // The overwritten file has the saved content.
            EditorSession reopened(types(), {.projectDirectory = project.root()});
            run(reopened, "ith.file.open_level", R"({"path": "Content/Target.ithmap"})");
            CHECK(reopened.document().levelId() == target);
            CHECK(hashOf(reopened) == hashOf(session));
            // Byte for byte what a save of the level under that id writes.
            const ith::Expected<std::string> written =
                ith::fs::readFile(ith::fs::joinPath(project.root(), "Content/Target.ithmap"), std::size_t{1} << 20);
            REQUIRE(written.hasValue());
            CHECK(*written == ith::test::editor::levelText(session));

            // An untitled level saved over a level takes its id too.
            run(session, "ith.file.new_level");
            run(session, "ith.file.save_as", R"({"path": "Content/Mine.ithmap"})");
            session.waitIdle();
            CHECK(session.document().levelId() == mine);

            // Another native asset, or a file that isn't a document, is replaced under a new id.
            for (const std::string& text :
                 {std::string(R"({"format": "ith.material", "version": 1, "id": ")" + idText(9) + R"(", "data": {}})"),
                  std::string("not a document")}) {
                CAPTURE(text);
                REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(project.root(), "Content/Other.ithmap"), text)
                            .hasValue());
                run(session, "ith.file.save_as", R"({"path": "Content/Mine.ithmap"})");
                session.waitIdle();
                run(session, "ith.file.save_as", R"({"path": "Content/Other.ithmap"})");
                session.waitIdle();
                REQUIRE_FALSE(session.lastFileError().has_value());
                CHECK(session.document().levelId() != mine);
                CHECK(session.document().levelId() != idOf(9));
                CHECK(fileIdOf(project, "Content/Other.ithmap") == session.document().levelId());
            }
        }
    }

    TEST_CASE("a failed Save As changes neither the level id nor the path; with and without a job system") {
        for (const bool withJobs : {false, true}) {
            CAPTURE(withJobs);
            TempProject project;
            ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
            EditorSession session(types(), {.projectDirectory = project.root(), .jobs = withJobs ? &jobs : nullptr});
            build(session);
            run(session, "ith.file.save_as", R"({"path": "Content/A.ithmap"})");
            session.waitIdle();
            const ith::Uuid id = session.document().levelId();
            run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Unsaved"})");
            // A directory where the file should go: the save fails (reading it already).
            REQUIRE(ith::fs::createDirectories(ith::fs::joinPath(project.root(), "Content/Taken.ithmap")).hasValue());
            run(session, "ith.file.save_as", R"({"path": "Content/Taken.ithmap"})");
            session.waitIdle();
            REQUIRE(session.lastFileError().has_value());
            CHECK(session.lastFileError()->code == "fs.is_a_directory");
            CHECK(session.document().levelId() == id);
            CHECK(session.document().path() == "Content/A.ithmap");
            CHECK(session.document().dirty());
            CHECK(fileIdOf(project, "Content/A.ithmap") == id);
            // The next Save goes to the level's own file, under its own id.
            run(session, "ith.file.save");
            session.waitIdle();
            REQUIRE_FALSE(session.lastFileError().has_value());
            CHECK(session.document().levelId() == id);
            CHECK_FALSE(session.document().dirty());
        }
    }

#ifndef ITH_SHIPPING
    TEST_CASE("a Save As over another level whose write fails keeps the id, the path, the dirty state and that file") {
        // The write fault is per thread, so the write runs on this one: no job system.
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        run(session, "ith.file.save_as", R"({"path": "Content/Target.ithmap"})");
        const ith::Uuid target = session.document().levelId();
        const std::string targetText =
            *ith::fs::readFile(ith::fs::joinPath(project.root(), "Content/Target.ithmap"), std::size_t{1} << 20);
        run(session, "ith.file.new_level");
        build(session);
        run(session, "ith.file.save_as", R"({"path": "Content/Mine.ithmap"})");
        const ith::Uuid mine = session.document().levelId();
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Unsaved"})");

        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::BeforeRename);
        run(session, "ith.file.save_as", R"({"path": "Content/Target.ithmap"})");
        ith::fs::testing::setWriteFault(ith::fs::testing::WriteFault::None);
        REQUIRE(session.lastFileError().has_value());
        CHECK(session.lastFileError()->code == "fs.io");
        CHECK(session.document().levelId() == mine);
        CHECK(session.document().levelId() != target);
        CHECK(session.document().path() == "Content/Mine.ithmap");
        CHECK(session.document().dirty());
        CHECK(*ith::fs::readFile(ith::fs::joinPath(project.root(), "Content/Target.ithmap"), std::size_t{1} << 20) ==
              targetText);
        CHECK(fileIdOf(project, "Content/Mine.ithmap") == mine);
    }
#endif

#ifndef _WIN32
    TEST_CASE("a Save As onto a FIFO or an unreadable file fails at once without writing; with a job system") {
        TempProject project;
        ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
        EditorSession session(types(), {.projectDirectory = project.root(), .jobs = &jobs});
        build(session);
        run(session, "ith.file.save_as", R"({"path": "Content/Mine.ithmap"})");
        session.waitIdle();
        const ith::Uuid mine = session.document().levelId();

        const std::string fifo = ith::fs::joinPath(project.root(), "Content/Pipe.ithmap");
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
        run(session, "ith.file.save_as", R"({"path": "Content/Pipe.ithmap"})");
        session.waitIdle(); // returns: the read doesn't wait for a writer
        REQUIRE(session.lastFileError().has_value());
        CHECK(session.lastFileError()->code == "fs.unsupported");
        CHECK(session.document().levelId() == mine);
        CHECK(session.document().path() == "Content/Mine.ithmap");
        const auto info = ith::fs::fileInfo(fifo);
        REQUIRE(info.hasValue());
        REQUIRE(info->has_value());
        CHECK((*info)->kind == ith::fs::FileKind::Other); // still the FIFO

        // A level file that can't be read: refused, not replaced under a new id.
        run(session, "ith.file.save_as", R"({"path": "Content/Locked.ithmap"})");
        session.waitIdle();
        const std::string locked = ith::fs::joinPath(project.root(), "Content/Locked.ithmap");
        const ith::Uuid lockedId = session.document().levelId();
        run(session, "ith.file.open_level", R"({"path": "Content/Mine.ithmap"})");
        session.waitIdle();
        REQUIRE(::chmod(locked.c_str(), 0) == 0);
        if (ith::fs::readFile(locked, std::size_t{1} << 20).hasValue()) {
            MESSAGE("the file is still readable (running as root?); skipped");
        } else {
            run(session, "ith.file.save_as", R"({"path": "Content/Locked.ithmap"})");
            session.waitIdle();
            REQUIRE(session.lastFileError().has_value());
            CHECK(session.lastFileError()->code == "fs.access_denied");
            CHECK(session.document().levelId() == mine);
            CHECK(session.document().path() == "Content/Mine.ithmap");
        }
        REQUIRE(::chmod(locked.c_str(), 0600) == 0);
        CHECK(fileIdOf(project, "Content/Locked.ithmap") == lockedId);
    }
#endif

    TEST_CASE("two unscanned files with one id: the open one stays the level's file through a Rescan and a Save") {
        TempProject project;
        const ith::ImporterRegistry importers;
        ith::AssetDatabase assets(project.root(), importers);
        EditorSession session(types(), {.projectDirectory = project.root(), .assets = &assets});
        const std::string shared = idText(5);
        const std::string level =
            R"({"format": "ith.level", "version": 1, "id": ")" + shared + R"(", "data": {"entities": []}})";
        const std::string a = ith::fs::joinPath(project.root(), "Content/Maps/A.ithmap");
        REQUIRE(ith::fs::writeFileAtomic(a, level).hasValue());
        REQUIRE(ith::fs::writeFileAtomic(ith::fs::joinPath(project.root(), "Content/Maps/B.ithmap"), level).hasValue());
        run(session, "ith.file.open_level", R"({"path": "Content/Maps/B.ithmap"})");
        REQUIRE(session.document().levelId() == idOf(5));

        // A, first in byte order, keeps the id; B is left out as a duplicate, but it is still there.
        run(session, "ith.content.rescan");
        REQUIRE(assets.findByPath("Maps/B.ithmap") == nullptr);
        CHECK(session.document().path() == "Content/Maps/B.ithmap");
        CHECK_FALSE(session.document().dirty());

        build(session);
        run(session, "ith.file.save");
        REQUIRE_FALSE(session.lastFileError().has_value());
        CHECK(*ith::fs::readFile(a, std::size_t{1} << 20) == level);
        CHECK(session.document().path() == "Content/Maps/B.ithmap");
        // The save's scan gave B, the copy, a new id, which the level took.
        const ith::AssetRecord* b = assets.findByPath("Maps/B.ithmap");
        REQUIRE(b != nullptr);
        CHECK(b->id != ith::AssetId(idOf(5)));
        CHECK(ith::AssetId(session.document().levelId()) == b->id);
        CHECK(fileIdOf(project, "Content/Maps/B.ithmap") == session.document().levelId());
    }

    TEST_CASE("queries: document, selection, entity, find, history and the world hash") {
        EditorSession session(types());
        build(session);
        const ith::serialization::Value document = run(session, "ith.query.document");
        CHECK(document.find("dirty")->asBool());
        CHECK(document.find("entities")->toNumber<int>() == 2);
        CHECK(document.find("lastError")->kind() == ith::serialization::Value::Kind::Null);

        const ith::serialization::Value selection = run(session, "ith.query.selection");
        CHECK(selection.find("primary")->asString() == idText(2));

        const ith::serialization::Value lamp = run(session, "ith.query.entity", R"({"id": )" + quoted(2) + "}");
        CHECK(lamp.find("parent")->asString() == idText(1));
        CHECK(lamp.find("label")->asString() == "Entity");
        const ith::serialization::Value* components = lamp.find("components");
        REQUIRE(components != nullptr);
        CHECK(components->find("ith::test::editor::Aim")->find("target")->asString() == idText(1));
        CHECK(components->find("ith::EditorLabel") != nullptr);
        CHECK(components->find("ith::ChildOf") != nullptr);
        CHECK(components->find("ith::StableId") == nullptr);
        CHECK(components->find("ith::WorldTransform") == nullptr); // Transient
        fails(session, "ith.query.entity", R"({"id": )" + quoted(9) + "}", "editor.entity.not_found");

        const ith::serialization::Value found = run(session, "ith.query.find", R"({"label": "Table"})");
        REQUIRE(found.find("ids")->elements().size() == 1);
        const ith::serialization::Value history = run(session, "ith.query.history");
        CHECK(history.find("entries")->elements().size() == 2);
        CHECK(history.find("canUndo")->asBool());

        const std::string before = hashOf(session);
        CHECK(before.size() == 16);
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Other"})");
        CHECK(hashOf(session) != before);
        run(session, "ith.edit.undo");
        CHECK(hashOf(session) == before);
    }
}
