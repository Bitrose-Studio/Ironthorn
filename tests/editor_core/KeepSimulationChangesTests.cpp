// Keep Simulation Changes, headless (ADR-0053 decisions 24 to 27, docs/specs/play-in-editor.md 3.12):
// differing component values of game-world entities copied back to their editor twins by stable id
// as one undoable editor transaction, structural differences reported and not applied, references
// remapped through stable ids, the refusals, and the game world left alone. Requirement numbers are
// the spec's; the K binding (42a) is in PlayInputTests.cpp, the keymap check (45) in KeymapTests.cpp.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Hooks.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/Transactions.h"
#include "ith/math/Types.h"
#include "ith/shapes/ShapeRenderer.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ith::EditorSession;
using ith::Entity;
using ith::World;
using ith::serialization::Value;
using ith::test::editor::Aim;
using ith::test::editor::Badge;
using ith::test::editor::fails;
using ith::test::editor::Gadget;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::Lamp;
using ith::test::editor::run;
using ith::test::editor::Seal;
using ith::test::editor::TempProject;
using ith::test::editor::types;

constexpr std::string_view kKeep = "ith.play.keep_simulation_changes";

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

void create(EditorSession& session, std::uint64_t n, const std::string& extra = "") {
    run(session, "ith.entity.create", R"({"id": )" + quoted(n) + extra + "}");
}

World& game(EditorSession& session) {
    REQUIRE(session.play() != nullptr);
    return session.play()->mutableWorld();
}

const World& editor(const EditorSession& session) {
    return session.document().world();
}

Entity gameEntity(EditorSession& session, std::uint64_t n) {
    return game(session).findByStableId(idOf(n));
}

Entity editorEntity(const EditorSession& session, std::uint64_t n) {
    return editor(session).findByStableId(idOf(n));
}

std::string hashOf(EditorSession& session, std::string_view world) {
    return run(session, "ith.query.world_hash", R"({"world": ")" + std::string(world) + R"("})")
        .find("hash")
        ->asString();
}

std::string keepArgs(const std::vector<std::uint64_t>& ids) {
    std::string args = R"({"ids": [)";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        args += (i > 0 ? ", " : "") + quoted(ids[i]);
    }
    return args + "]}";
}

// The component names `kept` lists for `n`; empty when it isn't listed.
std::vector<std::string> keptOf(const Value& result, std::uint64_t n) {
    std::vector<std::string> names;
    for (const Value& entry : result.find("kept")->elements()) {
        if (entry.find("id")->asString() == idText(n)) {
            for (const Value& name : entry.find("components")->elements()) {
                names.push_back(name.asString());
            }
        }
    }
    std::ranges::sort(names);
    return names;
}

// The reason `list` ("skipped" or "ignored") gives for `n` (and `component`, for "ignored").
std::optional<std::string> reasonOf(const Value& result, std::string_view list, std::uint64_t n,
                                    std::string_view component = {}) {
    for (const Value& entry : result.find(list)->elements()) {
        const Value* type = entry.find("component");
        if (entry.find("id")->asString() == idText(n) && (component.empty() || type->asString() == component)) {
            return entry.find("reason")->asString();
        }
    }
    return std::nullopt;
}

std::size_t sizeOf(const Value& result, std::string_view list) {
    return result.find(list)->elements().size();
}

float translationX(const World& world, Entity entity) {
    const auto* local = world.get<const ith::LocalTransform>(entity);
    REQUIRE(local != nullptr);
    return local->value.translation.x;
}

const std::string kShape = R"(, "components": {"ith::ShapeRenderer": {"shape": "Cube"}})";

} // namespace

TEST_SUITE("editor_core.keep_simulation_changes") {
    TEST_CASE("kept values reach the editor world at once as one undo entry, undone after Stop (37)") {
        const TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        create(session, 1, kShape);
        create(session, 2, kShape);
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        run(session, "ith.file.save_as", R"({"path": "Content/Level.ithmap"})"); // clean, to see the dirty state
        session.waitIdle();
        REQUIRE_FALSE(session.lastFileError().has_value());
        const std::string editorHash = hashOf(session, "editor");
        const std::size_t entries = session.document().history().entries().size();
        const std::vector<ith::Uuid> editorSelection = session.document().selection().entities();

        run(session, "ith.play.simulate", R"({"paused": true})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        run(session, "ith.entity.set_property",
            R"({"path": "ith::LocalTransform.value.translation", "value": {"x": 4, "y": 0, "z": 0}})");
        run(session, "ith.entity.set_property",
            R"({"path": "ith::ShapeRenderer.color", "value": {"x": 1, "y": 0, "z": 0}})");
        // A Transient value (WorldTransform) that differs is not kept.
        game(session).get<ith::WorldTransform>(gameEntity(session, 1))->matrix[3][0] = 42.0f;
        const std::string gameHash = hashOf(session, "game");
        const std::uint64_t seenValues = session.document().changes().values();
        CHECK_FALSE(session.document().dirty());

        const Value result = run(session, kKeep, keepArgs({1}));
        CHECK(keptOf(result, 1) == std::vector<std::string>{"ith::LocalTransform", "ith::ShapeRenderer"});
        CHECK(sizeOf(result, "skipped") == 0);
        CHECK(sizeOf(result, "ignored") == 0); // EditorLabel is EditorOnly: not "removed during play"
        CHECK(sizeOf(result, "warnings") == 0);
        // At once, before Stop: the editor world has both values, its value counter moved for the
        // entity (the viewports that draw it redraw), and the game world is unchanged.
        CHECK(translationX(editor(session), editorEntity(session, 1)) == 4.0f);
        CHECK(editor(session).get<const ith::ShapeRenderer>(editorEntity(session, 1))->color ==
              ith::Vec3{1.0f, 0.0f, 0.0f});
        CHECK(editor(session).get<const ith::WorldTransform>(editorEntity(session, 1))->matrix[3][0] != 42.0f);
        CHECK(session.document().changes().values() > seenValues);
        const auto touched = session.document().changes().touchedSince(seenValues);
        REQUIRE(touched.has_value());
        CHECK(std::ranges::find(*touched, idOf(1)) != touched->end());
        CHECK(hashOf(session, "game") == gameHash);
        CHECK(hashOf(session, "editor") != editorHash);
        REQUIRE(session.document().history().entries().size() == entries + 1);
        CHECK(session.document().history().entries().back().name == "Keep Simulation Changes");
        CHECK(session.document().dirty());
        // The editor selection is kept, the play selection too.
        CHECK(session.document().selection().entities() == editorSelection);
        CHECK(session.play()->selection().entities() == std::vector<ith::Uuid>{idOf(1)});
        // Undo is still refused while playing.
        fails(session, "ith.edit.undo", "{}", "editor.command.playing");

        run(session, "ith.play.stop");
        CHECK(translationX(editor(session), editorEntity(session, 1)) == 4.0f);
        CHECK(session.document().history().entries().size() == entries + 1);
        CHECK(session.document().dirty());
        // One Undo restores both values and the selection; Redo keeps them again.
        run(session, "ith.edit.undo");
        CHECK(hashOf(session, "editor") == editorHash);
        CHECK(translationX(editor(session), editorEntity(session, 1)) == 0.0f);
        CHECK(editor(session).get<const ith::ShapeRenderer>(editorEntity(session, 1))->color ==
              ith::ShapeRenderer{}.color);
        CHECK(session.document().selection().entities() == editorSelection);
        CHECK_FALSE(session.document().dirty());
        run(session, "ith.edit.redo");
        CHECK(translationX(editor(session), editorEntity(session, 1)) == 4.0f);
    }

    TEST_CASE("ids only one world has are skipped with their reasons, changing nothing (38)") {
        EditorSession session(types());
        create(session, 1, kShape);
        create(session, 2, kShape);
        const std::string editorHash = hashOf(session, "editor");
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.play.simulate", R"({"paused": true})");
        // As a game system would: spawn an entity with a new stable id, despawn a level entity.
        static_cast<void>(game(session).spawn(ith::StableId{.id = idOf(9)}, ith::LocalTransform{}));
        REQUIRE(game(session).despawn(gameEntity(session, 2)));

        const Value result = run(session, kKeep, keepArgs({9, 2}));
        CHECK(reasonOf(result, "skipped", 9) == "not in the level");
        CHECK(reasonOf(result, "skipped", 2) == "editor.play.not_found");
        CHECK(sizeOf(result, "skipped") == 2);
        CHECK(sizeOf(result, "kept") == 0);
        CHECK(hashOf(session, "editor") == editorHash);
        CHECK(session.document().history().entries().size() == entries);
    }

    TEST_CASE("added and removed components, a reparent and a Disabled change are reported, not kept (39)") {
        EditorSession session(types());
        create(session, 1, kShape);
        create(session, 2, kShape);
        create(session, 3, R"(, "parent": )" + quoted(1));
        create(session, 4);
        const std::string editorHash = hashOf(session, "editor");
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& world = game(session);
        static_cast<void>(world.add(gameEntity(session, 1), Lamp{.intensity = 3.0f})); // added
        REQUIRE(world.remove<ith::ShapeRenderer>(gameEntity(session, 2)));             // removed
        static_cast<void>(world.add(gameEntity(session, 3), ith::ChildOf{.parent = gameEntity(session, 2)}));
        static_cast<void>(world.add(gameEntity(session, 4), ith::ChildOf{.parent = gameEntity(session, 1)}));
        static_cast<void>(world.add(gameEntity(session, 2), ith::Disabled{}));

        const Value result = run(session, kKeep, keepArgs({1, 2, 3, 4}));
        CHECK(reasonOf(result, "ignored", 1, "ith::test::editor::Lamp") == "added during play");
        CHECK(reasonOf(result, "ignored", 2, "ith::ShapeRenderer") == "removed during play");
        CHECK(reasonOf(result, "ignored", 3, "ith::ChildOf") == "parent changed");
        CHECK(reasonOf(result, "ignored", 4, "ith::ChildOf") == "parent changed"); // was a root
        CHECK(reasonOf(result, "ignored", 2, "ith::Disabled") == "enabled state changed");
        // DisabledInHierarchy and Children are Transient: never reported.
        CHECK(sizeOf(result, "ignored") == 5);
        CHECK(sizeOf(result, "kept") == 0);
        CHECK(hashOf(session, "editor") == editorHash);
        CHECK(session.document().history().entries().size() == entries);

        // A Disabled removed during play, and a child made a root, are reported too.
        run(session, "ith.play.stop");
        run(session, "ith.entity.set_enabled", R"({"ids": [)" + quoted(4) + R"(], "enabled": false})");
        run(session, "ith.play.simulate", R"({"paused": true})");
        REQUIRE(game(session).remove<ith::Disabled>(gameEntity(session, 4)));
        REQUIRE(game(session).remove<ith::ChildOf>(gameEntity(session, 3)));
        const Value again = run(session, kKeep, keepArgs({3, 4}));
        CHECK(reasonOf(again, "ignored", 4, "ith::Disabled") == "enabled state changed");
        CHECK(reasonOf(again, "ignored", 3, "ith::ChildOf") == "parent changed");
        CHECK(sizeOf(again, "ignored") == 2);
    }

    TEST_CASE("entity references are kept as the editor twin; one to a spawned entity becomes null, warned (40)") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2);
        create(session, 3, R"(, "components": {"ith::test::editor::Aim": {"target": )" + quoted(1) + "}}");
        create(session, 4, R"(, "components": {"ith::test::editor::Aim": {}})");
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& world = game(session);
        const Entity spawned = world.spawn(ith::LocalTransform{});
        world.get<Aim>(gameEntity(session, 4))->target = gameEntity(session, 2); // a level entity
        world.get<Aim>(gameEntity(session, 3))->target = spawned;                // no stable id

        const Value result = run(session, kKeep, keepArgs({3, 4}));
        CHECK(keptOf(result, 4) == std::vector<std::string>{"ith::test::editor::Aim"});
        CHECK(keptOf(result, 3) == std::vector<std::string>{"ith::test::editor::Aim"});
        CHECK(editor(session).get<const Aim>(editorEntity(session, 4))->target == editorEntity(session, 2));
        CHECK(editor(session).get<const Aim>(editorEntity(session, 3))->target.isNull());
        REQUIRE(sizeOf(result, "warnings") == 1);
        const Value& warning = result.find("warnings")->elements()[0];
        CHECK(warning.find("code")->asString() == "editor.play.reference_dropped");
        CHECK(warning.find("message")->asString().starts_with(idText(3)));
        // The game world still points at its own entities.
        CHECK(game(session).get<const Aim>(gameEntity(session, 3))->target == spawned);
        CHECK(game(session).get<const Aim>(gameEntity(session, 4))->target == gameEntity(session, 2));
        // Undo after Stop restores the old references.
        run(session, "ith.play.stop");
        run(session, "ith.edit.undo");
        CHECK(editor(session).get<const Aim>(editorEntity(session, 3))->target == editorEntity(session, 1));
        CHECK(editor(session).get<const Aim>(editorEntity(session, 4))->target.isNull());
    }

    TEST_CASE("nothing new to keep records nothing: a second call, an unchanged entity (41)") {
        EditorSession session(types());
        create(session, 1, kShape);
        create(session, 2, kShape);
        run(session, "ith.play.simulate", R"({"paused": true})");
        const std::size_t entries = session.document().history().entries().size();
        const Value unchanged = run(session, kKeep, keepArgs({2}));
        CHECK(sizeOf(unchanged, "kept") == 0);
        CHECK(sizeOf(unchanged, "ignored") == 0);
        CHECK(session.document().history().entries().size() == entries);

        game(session).get<ith::LocalTransform>(gameEntity(session, 1))->value.translation.x = 2.0f;
        CHECK(sizeOf(run(session, kKeep, keepArgs({1, 2})), "kept") == 1);
        CHECK(session.document().history().entries().size() == entries + 1);
        const std::uint64_t values = session.document().changes().values();
        const Value second = run(session, kKeep, keepArgs({1, 2}));
        CHECK(sizeOf(second, "kept") == 0);
        CHECK(session.document().history().entries().size() == entries + 1);
        CHECK(session.document().changes().values() == values);
    }

    TEST_CASE("without ids it keeps the play selection; repeated ids count once (42)") {
        EditorSession session(types());
        create(session, 1, kShape);
        create(session, 2, kShape);
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        run(session, "ith.play.simulate", R"({"paused": true})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        for (const std::uint64_t n : {std::uint64_t{1}, std::uint64_t{2}}) {
            game(session).get<ith::LocalTransform>(gameEntity(session, n))->value.translation.x = 5.0f;
        }
        const Value result = run(session, kKeep);
        CHECK(keptOf(result, 2) == std::vector<std::string>{"ith::LocalTransform"});
        CHECK(keptOf(result, 1).empty());
        CHECK(translationX(editor(session), editorEntity(session, 1)) == 0.0f);
        CHECK(translationX(editor(session), editorEntity(session, 2)) == 5.0f);
        // The editor selection stays the one from before Play.
        CHECK(session.document().selection().entities() == std::vector<ith::Uuid>{idOf(1)});

        const Value twice = run(session, kKeep, keepArgs({1, 1}));
        CHECK(sizeOf(twice, "kept") == 1);
        CHECK(keptOf(twice, 1) == std::vector<std::string>{"ith::LocalTransform"});

        // An empty play selection keeps nothing and records nothing.
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.edit.select_none");
        CHECK(sizeOf(run(session, kKeep), "kept") == 0);
        CHECK(session.document().history().entries().size() == entries);
    }

    TEST_CASE("an immutable component is kept as an Add that replaces it, undone in one step") {
        EditorSession session(types());
        create(session, 1, R"(, "components": {"ith::test::editor::Badge": {"rank": 1}})");
        run(session, "ith.play.play", R"({"paused": true})");
        static_cast<void>(game(session).add(gameEntity(session, 1), Badge{.rank = 5}));
        const Value result = run(session, kKeep, keepArgs({1}));
        CHECK(keptOf(result, 1) == std::vector<std::string>{"ith::test::editor::Badge"});
        CHECK(editor(session).get<const Badge>(editorEntity(session, 1))->rank == 5);
        run(session, "ith.play.stop");
        run(session, "ith.edit.undo");
        CHECK(editor(session).get<const Badge>(editorEntity(session, 1))->rank == 1);
    }

    TEST_CASE("refused without a session, during an interaction and with bad ids, changing nothing (2)") {
        EditorSession session(types());
        create(session, 1, R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        const std::string editorHash = hashOf(session, "editor");
        const std::size_t entries = session.document().history().entries().size();
        fails(session, kKeep, "{}", "editor.play.not_running");
        fails(session, kKeep, keepArgs({1}), "editor.play.not_running");

        // Running and not paused works as well as paused.
        run(session, "ith.play.simulate");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        fails(session, kKeep, R"({"ids": "x"})", "editor.command.invalid_args");
        fails(session, kKeep, R"({"ids": [3]})", "editor.command.invalid_args");
        fails(session, kKeep, R"({"ids": ["not an id"]})", "editor.command.invalid_args");

        // A Details drag holds the game world's transaction open: Keep waits for it.
        ith::DetailsModel details(session);
        details.update();
        std::optional<std::size_t> row;
        for (std::size_t i = 0; i < details.rows().size(); ++i) {
            if (details.rows()[i].key == "ith::test::editor::Lamp.intensity") {
                row = i;
            }
        }
        REQUIRE(row.has_value());
        REQUIRE(details.beginEdit(*row).hasValue());
        const float dragged = 6.0f;
        REQUIRE(details.previewValue(&dragged).hasValue());
        fails(session, kKeep, "{}", "editor.command.interaction_open");
        details.commitEdit();
        const Value result = run(session, kKeep);
        CHECK(keptOf(result, 1) == std::vector<std::string>{"ith::test::editor::Lamp"});
        CHECK(editor(session).get<const Lamp>(editorEntity(session, 1))->intensity == 6.0f);
        CHECK(session.document().history().entries().size() == entries + 1);

        run(session, "ith.play.stop");
        run(session, "ith.edit.undo");
        CHECK(hashOf(session, "editor") == editorHash);
    }

    TEST_CASE("a reparented entity's LocalTransform is reported as \"parent changed\", not kept") {
        EditorSession session(types());
        create(session, 1, R"(, "components": {"ith::LocalTransform": {"value": {"translation": {"x": 10}}}})");
        create(session, 2);
        create(session, 3, R"(, "parent": )" + quoted(1));
        const std::string editorHash = hashOf(session, "editor");
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& world = game(session);
        // As a game would: attach 2 under the parent at x = 10 and move 3 to the root, each keeping its
        // place in the world, so their LocalTransforms change with the parent.
        static_cast<void>(world.add(gameEntity(session, 2), ith::ChildOf{.parent = gameEntity(session, 1)}));
        world.get<ith::LocalTransform>(gameEntity(session, 2))->value.translation.x = -10.0f;
        REQUIRE(world.remove<ith::ChildOf>(gameEntity(session, 3)));
        world.get<ith::LocalTransform>(gameEntity(session, 3))->value.translation.x = 10.0f;

        const Value result = run(session, kKeep, keepArgs({2, 3}));
        CHECK(reasonOf(result, "ignored", 2, "ith::ChildOf") == "parent changed");
        CHECK(reasonOf(result, "ignored", 2, "ith::LocalTransform") == "parent changed");
        CHECK(reasonOf(result, "ignored", 3, "ith::ChildOf") == "parent changed");
        CHECK(reasonOf(result, "ignored", 3, "ith::LocalTransform") == "parent changed");
        CHECK(sizeOf(result, "ignored") == 4);
        CHECK(sizeOf(result, "kept") == 0);
        CHECK(translationX(editor(session), editorEntity(session, 2)) == 0.0f);
        CHECK(translationX(editor(session), editorEntity(session, 3)) == 0.0f);
        CHECK(hashOf(session, "editor") == editorHash);

        // With the parent as it was, a moved LocalTransform is kept.
        world.get<ith::LocalTransform>(gameEntity(session, 1))->value.translation.x = 12.0f;
        CHECK(keptOf(run(session, kKeep, keepArgs({1})), 1) == std::vector<std::string>{"ith::LocalTransform"});
    }

    TEST_CASE("an immutable Add is applied before the sets, so its hook can't overwrite a kept value") {
        // Seal's hook, set once before any World stores the type; only this test uses Seal.
        static const bool kHooked = ith::setComponentHooks<Seal>({.onInsert = [](ith::HookContext& context) {
            const auto* seal = context.value<const Seal>();
            if (auto* lamp = context.get<Lamp>(context.entity()); lamp != nullptr && seal != nullptr) {
                lamp->intensity = static_cast<float>(seal->level);
            }
        }});
        REQUIRE(kHooked);
        EditorSession session(types());
        create(session, 1,
               R"(, "components": {"ith::test::editor::Lamp": {}, "ith::test::editor::Seal": {"level": 1}})");
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& world = game(session);
        static_cast<void>(world.add(gameEntity(session, 1), Seal{.level = 5})); // its hook writes the Lamp: 5
        world.get<Lamp>(gameEntity(session, 1))->intensity = 7.0f;              // then the game changes it

        const Value result = run(session, kKeep, keepArgs({1}));
        CHECK(keptOf(result, 1) == std::vector<std::string>{"ith::test::editor::Lamp", "ith::test::editor::Seal"});
        // The editor matches the game: the Add's hook ran first, the kept Lamp after it.
        CHECK(editor(session).get<const Seal>(editorEntity(session, 1))->level == 5);
        CHECK(editor(session).get<const Lamp>(editorEntity(session, 1))->intensity == 7.0f);
        CHECK(game(session).get<const Lamp>(gameEntity(session, 1))->intensity == 7.0f);
    }

    TEST_CASE("a reference to an editor entity without a stable id survives a Keep of another field") {
        EditorSession session(types());
        create(session, 1, R"(, "components": {"ith::test::editor::Gadget": {}})");
        // An entity without a stable id, as code that owns the world may make; Gadget.link points at it.
        World& level = session.document().mutableWorld();
        const Entity anonymous = level.spawn(ith::LocalTransform{});
        level.get<Gadget>(editorEntity(session, 1))->link = anonymous;
        run(session, "ith.play.simulate", R"({"paused": true})");
        game(session).get<Gadget>(gameEntity(session, 1))->power = 60.0f;

        const Value result = run(session, kKeep, keepArgs({1}));
        CHECK(keptOf(result, 1) == std::vector<std::string>{"ith::test::editor::Gadget"});
        CHECK(sizeOf(result, "warnings") == 0);
        CHECK(editor(session).get<const Gadget>(editorEntity(session, 1))->power == 60.0f);
        CHECK(editor(session).get<const Gadget>(editorEntity(session, 1))->link == anonymous);
    }

    TEST_CASE("a dropped reference warns only when its value is kept: not for an equal value or a ChildOf") {
        EditorSession session(types());
        create(session, 1);
        create(session, 2, R"(, "parent": )" + quoted(1));
        create(session, 3, R"(, "components": {"ith::test::editor::Aim": {}})");
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& world = game(session);
        const Entity spawned = world.spawn(ith::LocalTransform{});
        // Maps to null, as the editor value already is: equal, nothing kept, no warning.
        world.get<Aim>(gameEntity(session, 3))->target = spawned;
        // A parent that maps to null: reported as "parent changed", no warning.
        static_cast<void>(world.add(gameEntity(session, 2), ith::ChildOf{.parent = spawned}));

        const Value result = run(session, kKeep, keepArgs({2, 3}));
        CHECK(sizeOf(result, "kept") == 0);
        CHECK(sizeOf(result, "warnings") == 0);
        CHECK(reasonOf(result, "ignored", 2, "ith::ChildOf") == "parent changed");
    }

    TEST_CASE("run from inside a command that holds an editor transaction open, it is refused, recording nothing") {
        EditorSession session(types());
        create(session, 1, R"(, "components": {"ith::test::editor::Lamp": {}})");
        // A test-only Action that opens an editor-world transaction and runs Keep inside it.
        session.commands().add({.id = "test.keep_nested",
                                .displayName = "Keep Nested",
                                .kind = ith::CommandKind::Action,
                                .execute = [](EditorSession& self, const Value& /*args*/) -> ith::Expected<Value> {
                                    const ith::TransactionScope outer = self.document().transactions().begin("Outer");
                                    ith::Expected<Value> inner = self.execute("ith.play.keep_simulation_changes");
                                    if (!inner) {
                                        return ith::makeUnexpected(std::move(inner).error());
                                    }
                                    return Value::object();
                                }});
        run(session, "ith.play.simulate", R"({"paused": true})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        game(session).get<Lamp>(gameEntity(session, 1))->intensity = 4.0f;
        const std::size_t entries = session.document().history().entries().size();
        fails(session, "test.keep_nested", "{}", "editor.command.interaction_open");
        CHECK(session.document().history().entries().size() == entries);
        CHECK(editor(session).get<const Lamp>(editorEntity(session, 1))->intensity == 1.0f);
        CHECK_FALSE(session.document().transactions().isOpen());
    }
}
