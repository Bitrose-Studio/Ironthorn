// Click picking without a GPU (ADR-0042 rules 21 and 22): the id texel an entity writes and the
// entity a texel names, and how a read-back texel changes the selection: a plain click replaces it
// (empty space clears it), Ctrl toggles, Shift adds, and stale hits and entities without a stable id
// change nothing.

#include "ith/editor_core/ViewportPick.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Uuid.h"

#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using ith::Entity;
using ith::PickMode;
using ith::PickTexel;
using ith::Uuid;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::run;
using ith::test::editor::types;

// Creates entity `n` and returns its id texel.
PickTexel create(ith::EditorSession& session, std::uint64_t n) {
    run(session, "ith.entity.create", R"({"id": ")" + idText(n) + R"("})");
    const Entity entity = session.document().world().findByStableId(idOf(n));
    REQUIRE_FALSE(entity.isNull());
    return ith::pickTexel(entity);
}

void pick(ith::EditorSession& session, PickTexel texel, PickMode mode) {
    const ith::Expected<void> applied = ith::applyPick(session, texel, mode);
    REQUIRE_MESSAGE(applied.hasValue(), (applied ? std::string() : applied.error().message));
}

std::vector<Uuid> selected(const ith::EditorSession& session) {
    return session.document().selection().entities();
}

} // namespace

TEST_SUITE("editor_core.viewport_pick") {
    TEST_CASE("an entity's texel is its slot index + 1 and generation, and names it again") {
        const Entity entity = Entity::fromParts(41, 7, 3);
        const PickTexel texel = ith::pickTexel(entity);
        CHECK(texel == PickTexel{.index = 42, .generation = 7});
        CHECK(ith::pickedEntity(texel, 3) == entity);
        // Slot 0 is still an entity: only index 0 is empty.
        CHECK(ith::pickTexel(Entity::fromParts(0, 0, 1)) == PickTexel{.index = 1, .generation = 0});
        CHECK(ith::pickedEntity({.index = 1, .generation = 0}, 1) == Entity::fromParts(0, 0, 1));
        CHECK(ith::pickTexel(Entity::fromParts(5, Entity::kMaxGeneration, 1)).generation == Entity::kMaxGeneration);
        // The null handle writes the empty texel, which names no entity.
        CHECK(ith::pickTexel(Entity()) == PickTexel{});
        CHECK(ith::pickedEntity({}, 1).isNull());
        CHECK(ith::pickedEntity({.index = 0, .generation = 9}, 1).isNull());
    }

    TEST_CASE("a plain click replaces the selection, Ctrl toggles and Shift adds as primary") {
        ith::EditorSession session(types());
        const PickTexel first = create(session, 1);
        const PickTexel second = create(session, 2);
        run(session, "ith.edit.select_none");
        const std::uint64_t before = session.document().changes().selection();

        pick(session, first, PickMode::Replace);
        CHECK(selected(session) == std::vector<Uuid>{idOf(1)});
        CHECK(session.document().changes().selection() > before);
        pick(session, second, PickMode::Add);
        CHECK(selected(session) == std::vector<Uuid>{idOf(1), idOf(2)});
        CHECK(session.document().selection().primary() == idOf(2));
        pick(session, first, PickMode::Add); // already selected: moves to the end, primary
        CHECK(selected(session) == std::vector<Uuid>{idOf(2), idOf(1)});
        pick(session, first, PickMode::Toggle);
        CHECK(selected(session) == std::vector<Uuid>{idOf(2)});
        pick(session, first, PickMode::Toggle);
        CHECK(selected(session) == std::vector<Uuid>{idOf(2), idOf(1)});
        pick(session, second, PickMode::Replace);
        CHECK(selected(session) == std::vector<Uuid>{idOf(2)});
    }

    TEST_CASE("empty space clears the selection on a plain click and changes nothing with Ctrl or Shift") {
        ith::EditorSession session(types());
        create(session, 1);
        run(session, "ith.selection.set", R"({"entities": [")" + idText(1) + R"("]})");
        pick(session, {}, PickMode::Toggle);
        pick(session, {}, PickMode::Add);
        CHECK(selected(session) == std::vector<Uuid>{idOf(1)});
        pick(session, {}, PickMode::Replace);
        CHECK(selected(session).empty());
        const std::uint64_t cleared = session.document().changes().selection();
        pick(session, {}, PickMode::Replace); // nothing left to clear: no change reported
        CHECK(session.document().changes().selection() == cleared);
    }

    TEST_CASE("a stale hit and an entity without a stable id change nothing") {
        ith::EditorSession session(types());
        const PickTexel kept = create(session, 1);
        const PickTexel doomed = create(session, 2);
        run(session, "ith.edit.delete", R"({"ids": [")" + idText(2) + R"("]})");
        // The despawned entity's slot may be reused, with a higher generation: the old texel is stale.
        const PickTexel reused = create(session, 3);
        CHECK_FALSE(reused == doomed);
        run(session, "ith.selection.set", R"({"entities": [")" + idText(1) + R"("]})");
        const std::uint64_t before = session.document().changes().selection();

        pick(session, doomed, PickMode::Replace);
        pick(session, doomed, PickMode::Toggle);
        // A slot that was never used, too.
        pick(session, {.index = 1000, .generation = 0}, PickMode::Replace);
        CHECK(selected(session) == std::vector<Uuid>{idOf(1)});

        // An entity without a StableId, spawned outside the document's commands.
        const Entity anonymous = session.document().mutableWorld().spawn(ith::LocalTransform{});
        REQUIRE(session.document().world().get<const ith::StableId>(anonymous) == nullptr);
        pick(session, ith::pickTexel(anonymous), PickMode::Replace);
        pick(session, ith::pickTexel(anonymous), PickMode::Toggle);
        CHECK(selected(session) == std::vector<Uuid>{idOf(1)});
        CHECK(session.document().changes().selection() == before);
        pick(session, kept, PickMode::Toggle);
        CHECK(selected(session).empty());
    }
}
