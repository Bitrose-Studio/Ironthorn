// Transactions and the undo history (ADR-0037 rules 5 to 7, 9, 12 and 15): live patches and one
// entry per interaction, cancel and scopes destroyed without commit, nested scopes that join,
// merged sets, the selection before and after, stable ids across despawn and respawn, the limits,
// the saved marker, and the change counters.

#include "ith/ecs/EntityPatch.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Transactions.h"
#include "ith/editor_core/UndoHistory.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include "AssertCapture.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <vector>

namespace {

using ith::ComponentChange;
using ith::Entity;
using ith::EntityPatch;
using ith::OwnedValue;
using ith::Uuid;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::Lamp;
using ith::test::editor::levelText;
using ith::test::editor::run;
using ith::test::editor::types;

EntityPatch intensity(Entity entity, float value) {
    const auto path = ith::PropertyPath::parse(ith::typeOf<Lamp>(), "intensity");
    REQUIRE(path.hasValue());
    ith::Patch patch(ith::typeOf<Lamp>());
    patch.set(*path, OwnedValue::of(value));
    EntityPatch result{.entity = entity};
    result.changes.push_back(ComponentChange::makePatch(std::move(patch)));
    return result;
}

Entity lampOf(ith::EditorSession& session, std::uint64_t n) {
    run(session, "ith.entity.create",
        R"({"id": ")" + idText(n) + R"(", "components": {"ith::test::editor::Lamp": {}}})");
    return session.document().world().findByStableId(idOf(n));
}

float intensityOf(const ith::EditorSession& session, Entity entity) {
    return session.document().world().get<const Lamp>(entity)->intensity;
}

} // namespace

TEST_SUITE("editor_core.transactions") {
    TEST_CASE("a drag applies live and becomes one entry that keeps the first old value") {
        ith::EditorSession session(types());
        const Entity lamp = lampOf(session, 1);
        ith::Transactions& transactions = session.document().transactions();
        const std::size_t before = session.document().history().cursor();
        {
            ith::TransactionScope drag = transactions.begin("Drag");
            for (int frame = 1; frame <= 100; ++frame) {
                const EntityPatch patch = intensity(lamp, static_cast<float>(frame));
                REQUIRE(drag.apply(std::span(&patch, 1)));
                CHECK(intensityOf(session, lamp) == static_cast<float>(frame)); // a live preview
            }
            drag.commit();
        }
        REQUIRE(session.document().history().cursor() == before + 1);
        const ith::UndoEntry& entry = session.document().history().entries().back();
        CHECK(entry.name == "Drag");
        REQUIRE(entry.patches.size() == 1); // 99 later sets dropped
        REQUIRE(transactions.undo());
        CHECK(intensityOf(session, lamp) == 1.0f);
        REQUIRE(transactions.redo());
        CHECK(intensityOf(session, lamp) == 100.0f);
    }

    TEST_CASE("cancel, and a scope destroyed without commit, put the world back and record nothing") {
        ith::EditorSession session(types());
        const Entity lamp = lampOf(session, 1);
        const std::string text = levelText(session);
        const std::size_t entries = session.document().history().entries().size();
        {
            ith::TransactionScope scope = session.document().transactions().begin("Drag");
            const EntityPatch patch = intensity(lamp, 9.0f);
            REQUIRE(scope.apply(std::span(&patch, 1)));
            scope.cancel();
            CHECK_FALSE(scope.active());
            CHECK_FALSE(scope.apply(std::span(&patch, 1)));
        }
        CHECK(levelText(session) == text);
        {
            ith::TransactionScope scope = session.document().transactions().begin("Drag");
            const EntityPatch patch = intensity(lamp, 9.0f);
            REQUIRE(scope.apply(std::span(&patch, 1)));
        }
        CHECK(levelText(session) == text);
        CHECK(session.document().history().entries().size() == entries);
        CHECK_FALSE(session.document().transactions().isOpen());
    }

    TEST_CASE("nested scopes join the outermost; an inner cancel cancels the whole transaction") {
        ith::EditorSession session(types());
        const Entity lamp = lampOf(session, 1);
        const std::size_t entries = session.document().history().entries().size();
        ith::Transactions& transactions = session.document().transactions();
        {
            ith::TransactionScope outer = transactions.begin("Duplicate and move");
            {
                ith::TransactionScope inner = transactions.begin("Inner");
                const EntityPatch patch = intensity(lamp, 2.0f);
                REQUIRE(inner.apply(std::span(&patch, 1)));
                inner.commit();
            }
            CHECK(transactions.isOpen());
            const EntityPatch patch = intensity(lamp, 3.0f);
            REQUIRE(outer.apply(std::span(&patch, 1)));
            outer.commit();
        }
        REQUIRE(session.document().history().entries().size() == entries + 1);
        CHECK(session.document().history().entries().back().name == "Duplicate and move");

        {
            ith::TransactionScope outer = transactions.begin("Cancelled");
            {
                ith::TransactionScope inner = transactions.begin("Inner");
                const EntityPatch patch = intensity(lamp, 7.0f);
                REQUIRE(inner.apply(std::span(&patch, 1)));
                inner.cancel();
            }
            outer.commit();
        }
        CHECK(intensityOf(session, lamp) == 3.0f);
        CHECK(session.document().history().entries().size() == entries + 1);
    }

    TEST_CASE("a failed apply changes nothing and the transaction goes on") {
        ith::EditorSession session(types());
        const Entity lamp = lampOf(session, 1);
        const Entity bare = session.document().mutableWorld().spawn(Lamp{});
        ith::TransactionScope scope = session.document().transactions().begin("Edit");
        const EntityPatch unstable = intensity(bare, 4.0f);
        const auto refused = scope.apply(std::span(&unstable, 1));
        REQUIRE_FALSE(refused.hasValue());
        CHECK(refused.error().code == "editor.no_stable_id");
        EntityPatch missing{.entity = lamp};
        missing.changes.push_back(ComponentChange::makeRemove(ith::typeOf<ith::ChildOf>()));
        CHECK_FALSE(scope.apply(std::span(&missing, 1)));
        const EntityPatch fine = intensity(lamp, 5.0f);
        CHECK(scope.apply(std::span(&fine, 1)));
        scope.commit();
        CHECK(intensityOf(session, lamp) == 5.0f);
    }

    TEST_CASE("undo restores the selection before, redo the selection after") {
        ith::EditorSession session(types());
        lampOf(session, 1);
        lampOf(session, 2); // selects 2
        run(session, "ith.selection.set", R"({"entities": [")" + idText(1) + R"("]})");
        run(session, "ith.edit.delete"); // deletes 1; the selection is pruned
        CHECK(session.document().selection().empty());
        run(session, "ith.edit.undo");
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
        run(session, "ith.selection.set", R"({"entities": [")" + idText(2) + R"("]})");
        run(session, "ith.edit.redo");
        CHECK(session.document().selection().empty());
        CHECK(session.document().world().findByStableId(idOf(1)).isNull());
    }

    TEST_CASE("entries name entities by stable id, so older entries work after a respawn") {
        ith::EditorSession session(types());
        lampOf(session, 1);
        run(session, "ith.entity.set_property",
            R"({"ids": [")" + idText(1) + R"("], "path": "ith::test::editor::Lamp.intensity", "value": 6})");
        run(session, "ith.edit.delete", R"({"ids": [")" + idText(1) + R"("]})");
        run(session, "ith.edit.undo"); // a new handle
        run(session, "ith.edit.undo"); // the set_property entry names the respawned entity
        const Entity lamp = session.document().world().findByStableId(idOf(1));
        REQUIRE_FALSE(lamp.isNull());
        CHECK(intensityOf(session, lamp) == 1.0f);
        run(session, "ith.edit.redo");
        run(session, "ith.edit.redo");
        CHECK(session.document().world().findByStableId(idOf(1)).isNull());
    }

    TEST_CASE("change counters: values for a value edit, structure for the rest, selection and document") {
        ith::EditorSession session(types());
        const Entity lamp = lampOf(session, 1);
        ith::EditorChanges& changes = session.document().changes();
        static_cast<void>(changes.takeTouched());
        const std::uint64_t structure = changes.structure();
        const std::uint64_t values = changes.values();
        run(session, "ith.entity.set_property",
            R"({"ids": [")" + idText(1) + R"("], "path": "ith::test::editor::Lamp.intensity", "value": 2})");
        CHECK(changes.structure() == structure);
        CHECK(changes.values() > values);
        CHECK(changes.takeTouched() == std::vector<Uuid>{idOf(1)});
        run(session, "ith.edit.rename", R"({"label": "Desk lamp"})");
        CHECK(changes.structure() > structure);
        const std::uint64_t selection = changes.selection();
        run(session, "ith.edit.select_none");
        CHECK(changes.selection() > selection);
        static_cast<void>(lamp);
    }
}

TEST_SUITE("editor_core.undo_history") {
    TEST_CASE("the entry limit drops the oldest; the newest stays even alone over the byte budget") {
        ith::EditorSession session(types());
        ith::UndoHistory& history = session.document().history();
        history.setLimits({.maxEntries = 3});
        for (std::uint64_t n = 1; n <= 5; ++n) {
            lampOf(session, n);
        }
        REQUIRE(history.entries().size() == 3);
        CHECK(history.entries().front().name == "Create Entity");
        run(session, "ith.edit.undo");
        run(session, "ith.edit.undo");
        run(session, "ith.edit.undo");
        CHECK_FALSE(history.canUndo());
        CHECK(session.document().world().entityCount() == 2); // the dropped entries stay done

        history.setLimits({.maxEntries = 10, .maxBytes = 1});
        CHECK(history.entries().size() == 1);
        lampOf(session, 9);
        CHECK(history.entries().size() == 1);
        CHECK(history.canUndo());
        CHECK(history.bytes() > 1);
    }

    TEST_CASE("the saved marker: clean again after undoing back, dirty for good once it can't come back") {
        ith::EditorSession session(types());
        ith::UndoHistory& history = session.document().history();
        CHECK_FALSE(session.document().dirty());
        lampOf(session, 1);
        CHECK(session.document().dirty());
        history.markSaved();
        CHECK_FALSE(session.document().dirty());
        lampOf(session, 2);
        CHECK(session.document().dirty());
        run(session, "ith.edit.undo");
        CHECK_FALSE(session.document().dirty());
        run(session, "ith.edit.undo");
        CHECK(session.document().dirty());
        run(session, "ith.edit.redo");
        CHECK_FALSE(session.document().dirty());
        // A new change after an undo discards the redo tail.
        run(session, "ith.edit.undo");
        lampOf(session, 3);
        run(session, "ith.edit.undo");
        CHECK(session.document().dirty()); // back to before the saved state: not the saved one
        run(session, "ith.edit.redo");
        CHECK(session.document().dirty());
        // Dropped by the limit: dirty until the next save.
        history.setLimits({.maxEntries = 1});
        run(session, "ith.edit.undo");
        CHECK(session.document().dirty());
    }

    TEST_CASE("reset empties the history and the world and makes the document clean") {
        ith::EditorSession session(types());
        lampOf(session, 1);
        const Uuid level = session.document().levelId();
        session.document().reset();
        CHECK(session.document().world().entityCount() == 0);
        CHECK(session.document().history().entries().empty());
        CHECK_FALSE(session.document().dirty());
        CHECK(session.document().levelId() != level);
    }

    TEST_CASE("an entry's size counts the heap memory of its values") {
        ith::EditorSession session(types());
        lampOf(session, 1);
        const std::size_t small = session.document().history().entries().back().bytes;
        run(session, "ith.entity.set_property",
            R"({"ids": [")" + idText(1) + R"("], "path": "ith::test::editor::Lamp.note", "value": ")" +
                std::string(4096, 'x') + R"("})");
        run(session, "ith.entity.set_property",
            R"({"ids": [")" + idText(1) + R"("], "path": "ith::test::editor::Lamp.note", "value": ""})");
        CHECK(session.document().history().entries().back().bytes > small + 4000);
    }
}
