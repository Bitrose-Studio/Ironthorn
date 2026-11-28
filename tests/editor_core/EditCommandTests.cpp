// The command registry and the built-in Edit, Entity and Selection commands (ADR-0037 rules 1 to 4,
// 10 to 12): kinds and refusals, arguments as JSON with entities by stable id, every command with
// its undo and redo, and the selection commands.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Transactions.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using ith::EditorSession;
using ith::Entity;
using ith::Uuid;
using ith::test::editor::Aim;
using ith::test::editor::fails;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::json;
using ith::test::editor::Lamp;
using ith::test::editor::levelText;
using ith::test::editor::run;
using ith::test::editor::types;

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

void create(EditorSession& session, std::uint64_t n, const std::string& extra = "") {
    run(session, "ith.entity.create", R"({"id": )" + quoted(n) + extra + "}");
}

Entity entityOf(const EditorSession& session, std::uint64_t n) {
    return session.document().world().findByStableId(idOf(n));
}

std::vector<Entity> childrenOf(const EditorSession& session, std::uint64_t n) {
    const auto* children = session.document().world().get<const ith::Children>(entityOf(session, n));
    return children != nullptr ? children->entities : std::vector<Entity>();
}

std::vector<Entity> roots(const EditorSession& session) {
    std::vector<Entity> result;
    session.document().world().forEachRoot([&](Entity root) { result.push_back(root); });
    return result;
}

// Undoes every entry, then redoes every entry, checking the level text at each end.
void undoAllRedoAll(EditorSession& session) {
    const std::string done = levelText(session);
    std::size_t steps = 0;
    while (session.document().history().canUndo()) {
        run(session, "ith.edit.undo");
        ++steps;
    }
    CHECK(session.document().world().entityCount() == 0);
    for (std::size_t step = 0; step < steps; ++step) {
        run(session, "ith.edit.redo");
    }
    CHECK(levelText(session) == done);
}

} // namespace

TEST_SUITE("editor_core.commands") {
    TEST_CASE("the registry refuses unknown commands, bad arguments, edits while playing or mid-interaction") {
        EditorSession session(types());
        fails(session, "ith.no_such_command", "{}", "editor.command.unknown");
        fails(session, "ith.entity.create", "[1]", "editor.command.invalid_args");
        fails(session, "ith.edit.undo", "{}", "editor.command.disabled"); // nothing to undo
        create(session, 1);

        session.document().setPlaying(true);
        fails(session, "ith.edit.rename", R"({"label": "x"})", "editor.command.playing");
        fails(session, "ith.edit.undo", "{}", "editor.command.playing"); // ADR-0053 decision 14
        run(session, "ith.edit.select_none");                            // the selection stays available
        session.document().setPlaying(false);

        {
            ith::TransactionScope drag = session.document().transactions().begin("Drag");
            fails(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "x"})",
                  "editor.command.interaction_open");
            fails(session, "ith.edit.select_all", "{}", "editor.command.interaction_open");
            drag.cancel();
        }
        CHECK(session.commands().canExecute(session, "ith.edit.select_all"));
        CHECK(session.commands().find("ith.edit.delete")->displayName == "Delete");
    }

    TEST_CASE("create takes an id, a label, a parent and components, and selects the new entity") {
        EditorSession session(types());
        create(session, 1);
        const ith::serialization::Value result = run(
            session, "ith.entity.create",
            R"({"id": )" + quoted(2) + R"(, "label": "Lamp", "parent": )" + quoted(1) +
                R"(, "components": {"ith::test::editor::Lamp": {"intensity": 3}, "ith::test::editor::Aim": {"target": )" +
                quoted(1) + "}}}");
        CHECK(result.find("id")->asString() == idText(2));
        const Entity lamp = entityOf(session, 2);
        const ith::World& world = session.document().world();
        CHECK(world.get<const ith::EditorLabel>(lamp)->name == "Lamp");
        CHECK(world.get<const ith::ChildOf>(lamp)->parent == entityOf(session, 1));
        CHECK(world.get<const Lamp>(lamp)->intensity == 3.0f);
        CHECK(world.get<const Aim>(lamp)->target == entityOf(session, 1));
        CHECK(world.has<ith::LocalTransform>(lamp));
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(2)});

        fails(session, "ith.entity.create", R"({"id": )" + quoted(2) + "}", "editor.command.invalid_args");
        fails(session, "ith.entity.create", R"({"parent": )" + quoted(9) + "}", "editor.entity.not_found");
        fails(session, "ith.entity.create", R"({"components": {"ith::NoSuchType": {}}})",
              "editor.command.invalid_args");
        fails(session, "ith.entity.create", R"({"components": {"ith::StableId": {}}})", "editor.command.invalid_args");
        fails(session, "ith.entity.create", R"({"components": {"ith::test::editor::Lamp": {"wattage": 3}}})",
              "serialization.unknown_field");
        CHECK(world.entityCount() == 2); // the failures changed nothing
        run(session, "ith.edit.undo");
        CHECK(entityOf(session, 2).isNull());
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1)}); // as before the create
        run(session, "ith.edit.redo");
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(2)});
    }

    TEST_CASE("rename, add, remove and set a property, on the selection or on given ids") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2); // selected
        run(session, "ith.edit.rename", R"({"label": "Second"})");
        CHECK(session.document().world().get<const ith::EditorLabel>(entityOf(session, 2))->name == "Second");
        run(session, "ith.entity.add_component",
            R"({"ids": [)" + quoted(1) + "," + quoted(2) +
                R"(], "type": "ith::test::editor::Lamp", "value": {"intensity": 2}})");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + "," + quoted(2) +
                R"(], "path": "ith::test::editor::Lamp.steps", "value": [1, 2]})");
        CHECK(session.document().world().get<const Lamp>(entityOf(session, 1))->steps ==
              std::vector<std::int32_t>{1, 2});
        fails(session, "ith.entity.set_property", R"({"path": "ith::test::editor::Lamp.serial", "value": 3})",
              "editor.command.read_only");
        fails(session, "ith.entity.set_property", R"({"path": "ith::test::editor::Lamp.nothing", "value": 3})",
              "reflection.path.unknown_field");
        fails(session, "ith.entity.set_property", R"({"path": "ith::test::editor::Lamp.intensity"})",
              "editor.command.invalid_args");
        run(session, "ith.entity.remove_component", R"({"type": "ith::test::editor::Lamp"})");
        CHECK_FALSE(session.document().world().has<Lamp>(entityOf(session, 2)));
        fails(session, "ith.entity.remove_component", R"({"type": "ith::test::editor::Lamp"})",
              "ecs.patch.missing_component");
        run(session, "ith.edit.select_none");
        fails(session, "ith.edit.rename", R"({"label": "x"})", "editor.command.invalid_args");
        undoAllRedoAll(session);
    }

    TEST_CASE("attach, detach and move keep the order, and undo puts everything back") {
        EditorSession session(types());
        for (std::uint64_t n = 1; n <= 4; ++n) {
            create(session, n);
        }
        run(session, "ith.entity.attach",
            R"({"ids": [)" + quoted(3) + "," + quoted(2) + R"(], "parent": )" + quoted(1) + "}");
        CHECK(childrenOf(session, 1) == std::vector<Entity>{entityOf(session, 3), entityOf(session, 2)});
        run(session, "ith.entity.move_before", R"({"id": )" + quoted(2) + R"(, "before": )" + quoted(3) + "}");
        CHECK(childrenOf(session, 1) == std::vector<Entity>{entityOf(session, 2), entityOf(session, 3)});
        run(session, "ith.entity.move_before", R"({"id": )" + quoted(4) + R"(, "before": )" + quoted(1) + "}");
        CHECK(roots(session) == std::vector<Entity>{entityOf(session, 4), entityOf(session, 1)});
        fails(session, "ith.entity.attach", R"({"ids": [)" + quoted(1) + R"(], "parent": )" + quoted(2) + "}",
              "ecs.patch.refused_value"); // a cycle
        run(session, "ith.entity.detach", R"({"ids": [)" + quoted(2) + "]}");
        CHECK(roots(session) == std::vector<Entity>{entityOf(session, 4), entityOf(session, 1), entityOf(session, 2)});
        undoAllRedoAll(session);
    }

    TEST_CASE("delete detaches the other children in place; delete with children removes the subtree") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2, R"(, "parent": )" + quoted(1));
        create(session, 3, R"(, "parent": )" + quoted(1));
        create(session, 4, R"(, "parent": )" + quoted(3));
        create(session, 5);
        const std::string before = levelText(session);

        run(session, "ith.edit.delete", R"({"ids": [)" + quoted(1) + "]}");
        CHECK(entityOf(session, 1).isNull());
        CHECK(roots(session) == std::vector<Entity>{entityOf(session, 5), entityOf(session, 2), entityOf(session, 3)});
        CHECK(childrenOf(session, 3) == std::vector<Entity>{entityOf(session, 4)});
        run(session, "ith.edit.undo");
        CHECK(levelText(session) == before);

        run(session, "ith.edit.delete_with_children", R"({"ids": [)" + quoted(1) + "]}");
        CHECK(session.document().world().entityCount() == 1);
        run(session, "ith.edit.undo");
        CHECK(levelText(session) == before);
        run(session, "ith.edit.redo");
        CHECK(session.document().world().entityCount() == 1);
        run(session, "ith.edit.undo");
        undoAllRedoAll(session);
    }

    TEST_CASE("duplicate copies each entity next to itself with a new id and selects the copies") {
        EditorSession session(types());
        create(session, 1, R"(, "label": "Crate", "components": {"ith::test::editor::Lamp": {"intensity": 4}})");
        create(session, 2);
        const ith::serialization::Value result = run(session, "ith.edit.duplicate", R"({"ids": [)" + quoted(1) + "]}");
        const auto copyId = Uuid::parse(result.find("ids")->elements().front().asString());
        REQUIRE(copyId.hasValue());
        const Entity copy = session.document().world().findByStableId(*copyId);
        REQUIRE_FALSE(copy.isNull());
        CHECK(session.document().world().get<const ith::EditorLabel>(copy)->name == "Crate");
        CHECK(session.document().world().get<const Lamp>(copy)->intensity == 4.0f);
        CHECK(roots(session) == std::vector<Entity>{entityOf(session, 1), copy, entityOf(session, 2)});
        CHECK(session.document().selection().entities() == std::vector<Uuid>{*copyId});
        undoAllRedoAll(session);
    }

    TEST_CASE("duplicate copies a selected parent and child as a hierarchy; unselected children stay behind") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2,
               R"(, "parent": )" + quoted(1) + R"(, "components": {"ith::test::editor::Aim": {"target": )" + quoted(1) +
                   "}}");
        create(session, 3,
               R"(, "parent": )" + quoted(1) + R"(, "components": {"ith::test::editor::Aim": {"target": )" + quoted(1) +
                   "}}");
        // The child first: the copies are made in Outliner order whatever the order of the ids.
        const ith::serialization::Value result =
            run(session, "ith.edit.duplicate", R"({"ids": [)" + quoted(2) + "," + quoted(1) + "]}");
        REQUIRE(result.find("ids")->elements().size() == 2);
        const Entity parentCopy =
            session.document().world().findByStableId(*Uuid::parse(result.find("ids")->elements()[0].asString()));
        const Entity childCopy =
            session.document().world().findByStableId(*Uuid::parse(result.find("ids")->elements()[1].asString()));
        const ith::World& world = session.document().world();
        CHECK(world.get<const ith::ChildOf>(childCopy)->parent == parentCopy);
        CHECK(world.get<const Aim>(childCopy)->target == parentCopy); // a reference inside the set
        CHECK(world.get<const ith::Children>(parentCopy)->entities == std::vector<Entity>{childCopy});
        CHECK(childrenOf(session, 1).size() == 2); // entity 3 was not selected and is not copied
        CHECK(roots(session) == std::vector<Entity>{entityOf(session, 1), parentCopy});
        undoAllRedoAll(session);
    }

    TEST_CASE("selection commands: set with modes, select all in Outliner order, select none") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2, R"(, "parent": )" + quoted(1));
        create(session, 3);
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(3) + "," + quoted(1) + "]}");
        CHECK(session.document().selection().primary() == idOf(1));
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(3) + R"(], "mode": "toggle"})");
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + R"(], "mode": "add"})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + R"(], "mode": "remove"})");
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(2)});
        fails(session, "ith.selection.set", R"({"entities": [)" + quoted(9) + "]}", "editor.entity.not_found");
        fails(session, "ith.selection.set", R"({"entities": [], "mode": "swap"})", "editor.command.invalid_args");
        run(session, "ith.edit.select_all");
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1), idOf(2), idOf(3)});
        run(session, "ith.edit.select_none");
        CHECK(session.document().selection().empty());
        CHECK(session.document().history().entries().size() == entries); // selecting records nothing
    }
}
