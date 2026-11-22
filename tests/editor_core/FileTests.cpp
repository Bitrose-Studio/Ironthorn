// The editor document's files (ADR-0037 rules 15 and 16) and the queries (rule 17): Save As and
// Save with and without a job system, a reopened level equal to the saved one, the saved marker
// across a failed write, unsaved changes before New and Open, a failed open that keeps the level,
// commands refused while a level opens, opening by asset id, and what the queries return.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>

namespace {

using ith::EditorSession;
using ith::test::editor::fails;
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
        fails(session, "ith.file.new_level", "{}", "editor.command.disabled");
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
