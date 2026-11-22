// A randomized model test of commands, undo and redo (ADR-0037 rule 20): thousands of random
// commands (create, rename, attach, detach, move, add and remove components, set properties,
// delete, delete with children, duplicate) mixed with undo and redo. The model is the list of level
// texts after each entry: after every undo and redo the world must be exactly the one the model
// holds for that position, and undoing everything then redoing everything must give the last world.
// The P2 step 7 sequence (spawn, rename, reparent, edit, delete, undo all, redo all) runs first, by
// hand.

#include "ith/ecs/World.h"
#include "ith/editor_core/EditorSession.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest prints std::string operands through an ostream
#include <random>
#include <string>
#include <vector>

namespace {

using ith::EditorSession;
using ith::test::editor::idText;
using ith::test::editor::json;
using ith::test::editor::levelText;
using ith::test::editor::run;
using ith::test::editor::types;

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

// The world after each undoable entry, by history position.
struct Model {
    std::vector<std::string> states; // states[0]: the empty level
    std::size_t cursor = 0;

    void done(std::string text) {
        states.resize(cursor + 1);
        states.push_back(std::move(text));
        ++cursor;
    }
};

} // namespace

TEST_SUITE("editor_core.model") {
    TEST_CASE("spawn, rename, reparent, edit, delete, undo all and redo all give the same world") {
        EditorSession session(types());
        const std::string empty = levelText(session);
        run(session, "ith.entity.create", R"({"id": )" + quoted(1) + "}");
        run(session, "ith.entity.create", R"({"id": )" + quoted(2) + "}");
        run(session, "ith.edit.rename", R"({"id": )" + quoted(1) + R"(, "label": "Table"})");
        run(session, "ith.entity.attach", R"({"ids": [)" + quoted(2) + R"(], "parent": )" + quoted(1) + "}");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(2) + R"(], "path": "ith::LocalTransform.value.translation.x", "value": 2.5})");
        run(session, "ith.edit.delete", R"({"ids": [)" + quoted(1) + "]}");
        const std::string done = levelText(session);
        while (session.document().history().canUndo()) {
            run(session, "ith.edit.undo");
        }
        CHECK(levelText(session) == empty);
        while (session.document().history().canRedo()) {
            run(session, "ith.edit.redo");
        }
        CHECK(levelText(session) == done);
    }

    TEST_CASE("random commands, undo and redo match the model at every step") {
        std::mt19937 random(20261122u);
        EditorSession session(types());
        // Every entry kept, so undoing all goes back to the empty level.
        session.document().history().setLimits({.maxEntries = 1000000, .maxBytes = std::size_t{1} << 40});
        Model model;
        model.states.push_back(levelText(session));
        std::uint64_t nextId = 1;

        const auto pick = [&](std::uint64_t below) {
            return std::uniform_int_distribution<std::uint64_t>(0, below - 1)(random);
        };
        const auto someId = [&] { return quoted(1 + pick(nextId)); };

        for (int step = 0; step < 3000; ++step) {
            const std::uint64_t choice = pick(16);
            if (choice < 3 && session.document().history().canUndo()) {
                run(session, "ith.edit.undo");
                --model.cursor;
                REQUIRE(levelText(session) == model.states[model.cursor]);
                continue;
            }
            if (choice < 5 && session.document().history().canRedo()) {
                run(session, "ith.edit.redo");
                ++model.cursor;
                REQUIRE(levelText(session) == model.states[model.cursor]);
                continue;
            }
            std::string id;
            std::string args;
            switch (pick(10)) {
                case 0:
                case 1:
                    id = "ith.entity.create";
                    args = R"({"id": )" + quoted(nextId++) + (pick(2) == 0 ? "" : R"(, "parent": )" + someId()) + "}";
                    break;
                case 2:
                    id = "ith.edit.rename";
                    args = R"({"id": )" + someId() + R"(, "label": "n)" + std::to_string(step) + R"("})";
                    break;
                case 3:
                    id = "ith.entity.attach";
                    args = R"({"ids": [)" + someId() + R"(], "parent": )" + someId() + "}";
                    break;
                case 4:
                    id = pick(2) == 0 ? "ith.entity.detach" : "ith.entity.move_before";
                    args = id == "ith.entity.detach" ? R"({"ids": [)" + someId() + "]}"
                                                     : R"({"id": )" + someId() + R"(, "before": )" + someId() + "}";
                    break;
                case 5:
                    id = pick(2) == 0 ? "ith.entity.add_component" : "ith.entity.remove_component";
                    args = R"({"ids": [)" + someId() + R"(], "type": "ith::test::editor::Lamp"})";
                    break;
                case 6:
                    id = "ith.entity.set_property";
                    args = R"({"ids": [)" + someId() +
                           R"(], "path": "ith::LocalTransform.value.translation.y", "value": )" +
                           std::to_string(static_cast<int>(pick(100))) + "}";
                    break;
                case 7:
                    id = pick(3) == 0 ? "ith.edit.delete_with_children" : "ith.edit.delete";
                    args = R"({"ids": [)" + someId() + "]}";
                    break;
                case 8:
                    id = "ith.edit.duplicate";
                    args = R"({"ids": [)" + someId() + "]}";
                    break;
                default:
                    id = "ith.entity.set_property";
                    args = R"({"ids": [)" + someId() + R"(], "path": "ith::test::editor::Lamp.steps", "value": [)" +
                           std::to_string(step) + "]}";
                    break;
            }
            const std::string before = levelText(session);
            const std::size_t entries = session.document().history().cursor();
            const auto result = session.execute(id, json(args));
            if (!result.hasValue()) {
                // Refused (an unknown id, a cycle, a missing component): nothing changed.
                REQUIRE(levelText(session) == before);
                REQUIRE(session.document().history().cursor() == entries);
                continue;
            }
            if (session.document().history().cursor() == entries) {
                REQUIRE(levelText(session) == before); // a command that changed nothing
                continue;
            }
            model.done(levelText(session));
        }

        while (session.document().history().canUndo()) {
            run(session, "ith.edit.undo");
            --model.cursor;
        }
        CHECK(levelText(session) == model.states[0]);
        while (session.document().history().canRedo()) {
            run(session, "ith.edit.redo");
            ++model.cursor;
        }
        CHECK(levelText(session) == model.states.back());
    }
}
