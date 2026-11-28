// Play In Editor's session, headless (ADR-0053 decisions 3 and 6 to 15, docs/specs/play-in-editor.md):
// starting Play and Simulate on a copy of the editor world, the refusals and their codes, the game
// world's contents, Time through Step and a fake clock, Pause, the active world of the selection
// commands, queries and view models, value edits of the game world that record nothing, what is
// refused while playing, and Stop leaving the editor as it was. Requirement numbers are the spec's.

#include "ith/editor_core/PlaySession.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Query.h"
#include "ith/ecs/Schedule.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/OutlinerModel.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/gameplay/Time.h"
#include "ith/math/Transform.h"
#include "ith/platform_events/Event.h"
#include "ith/shapes/ShapeRenderer.h"

#include "AssertCapture.h"
#include "ContentTestSupport.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::EditorSession;
using ith::Entity;
using ith::PlayMode;
using ith::PlaySession;
using ith::Stage;
using ith::SystemContext;
using ith::SystemDescriptor;
using ith::Time;
using ith::Uuid;
using ith::World;
using ith::serialization::Value;
using ith::test::editor::Aim;
using ith::test::editor::fails;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::Lamp;
using ith::test::editor::levelText;
using ith::test::editor::run;
using ith::test::editor::Score;
using ith::test::editor::TempProject;
using ith::test::editor::types;

constexpr std::uint64_t kSecond = 1'000'000'000;

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

void create(EditorSession& session, std::uint64_t n, const std::string& label, const std::string& extra = "") {
    run(session, "ith.entity.create", R"({"id": )" + quoted(n) + R"(, "label": ")" + label + "\"" + extra + "}");
}

// The level of requirement 7: Crate with its child Lid, and Lamp, disabled; each with an EditorLabel, a
// LocalTransform and a ShapeRenderer, and a Lamp component on the Lamp.
void threeEntities(EditorSession& session) {
    const std::string shape = R"(, "components": {"ith::ShapeRenderer": {"shape": "Cube"}})";
    create(session, 1, "Crate", shape);
    create(session, 2, "Lid", R"(, "parent": )" + quoted(1) + shape);
    create(session, 3, "Lamp",
           R"(, "components": {"ith::ShapeRenderer": {"shape": "Sphere"}, "ith::test::editor::Lamp": {}})");
    run(session, "ith.entity.set_enabled", R"({"ids": [)" + quoted(3) + R"(], "enabled": false})");
}

PlaySession& play(EditorSession& session) {
    REQUIRE(session.play() != nullptr);
    return *session.play();
}

const Time& timeOf(EditorSession& session) {
    const Time* time = play(session).time();
    REQUIRE(time != nullptr);
    return *time;
}

Entity gameEntity(EditorSession& session, std::uint64_t n) {
    return play(session).world().findByStableId(idOf(n));
}

std::string stateOf(EditorSession& session) {
    return run(session, "ith.query.play").find("state")->asString();
}

double translationX(const World& world, Entity entity) {
    const auto* local = world.get<const ith::LocalTransform>(entity);
    REQUIRE(local != nullptr);
    return local->value.translation.x;
}

// A game-world system that counts its runs.
SystemDescriptor counting(int* runs, std::string name = "test::count", Stage stage = Stage::PrePhysics) {
    return SystemDescriptor{.name = std::move(name), .stage = stage, .run = [runs](SystemContext&) { ++*runs; }};
}

// A game-world system that moves every entity one unit along x per tick.
SystemDescriptor mover() {
    return SystemDescriptor{.name = "test::move",
                            .stage = Stage::PrePhysics,
                            .access = ith::accessTo<ith::LocalTransform>(),
                            .run = [](SystemContext& context) {
                                context.query<ith::LocalTransform>().forEach(
                                    [](ith::LocalTransform& local) { local.value.translation.x += 1.0f; });
                            }};
}

// Counts the errors of the PlayInEditor log category while it lives.
class PlayErrors {
public:
    PlayErrors() { REQUIRE(ith::addLogSink(m_buffer.sink()).hasValue()); }

    PlayErrors(const PlayErrors&) = delete;
    PlayErrors& operator=(const PlayErrors&) = delete;
    PlayErrors(PlayErrors&&) = delete;
    PlayErrors& operator=(PlayErrors&&) = delete;

    ~PlayErrors() { ith::removeLogSink(m_buffer.sink()); }

    [[nodiscard]] std::size_t count() {
        std::vector<ith::LogEntry> entries;
        m_next = m_buffer.read(m_next, entries);
        for (const ith::LogEntry& entry : entries) {
            if (entry.level == ith::LogLevel::Error && std::string_view(entry.category->name()) == "PlayInEditor") {
                ++m_count;
            }
        }
        return m_count;
    }

private:
    ith::LogRingBuffer m_buffer{64};
    std::uint64_t m_next = 0;
    std::size_t m_count = 0;
};

// The row of the Details property with `key`, or nullopt.
std::optional<std::size_t> rowOf(const ith::DetailsModel& details, std::string_view key) {
    for (std::size_t i = 0; i < details.rows().size(); ++i) {
        if (details.rows()[i].key == key) {
            return i;
        }
    }
    return std::nullopt;
}
} // namespace

TEST_SUITE("editor_core.play") {
    TEST_CASE("Play, Simulate and the play query report the session's state (requirement 1)") {
        EditorSession session(types());
        Value stopped = run(session, "ith.query.play");
        CHECK(stopped.find("state")->asString() == "stopped");
        CHECK(stopped.find("viewport")->kind() == Value::Kind::Null);
        CHECK(stopped.find("frame")->toNumber<std::uint64_t>() == 0U);

        const Value started = run(session, "ith.play.play");
        CHECK(started.find("state")->asString() == "running");
        CHECK(started.find("possessed")->asBool());
        CHECK_FALSE(started.find("paused")->asBool());
        CHECK(started.find("viewport")->asString() == "ith.viewport.1");
        const Value playing = run(session, "ith.query.play");
        CHECK(playing.find("state")->asString() == "running");
        CHECK(playing.find("possessed")->asBool());
        CHECK_FALSE(playing.find("captured")->asBool());
        CHECK(playing.find("viewport")->asString() == "ith.viewport.1");
        CHECK(run(session, "ith.query.document").find("playing")->asBool());
        CHECK(play(session).mode() == PlayMode::Play);

        CHECK(run(session, "ith.play.stop").find("state")->asString() == "stopped");
        CHECK(session.play() == nullptr);
        CHECK_FALSE(session.document().playing());
        const Value simulated = run(session, "ith.play.simulate", R"({"paused": true})");
        CHECK_FALSE(simulated.find("possessed")->asBool());
        CHECK(simulated.find("paused")->asBool());
        CHECK(play(session).mode() == PlayMode::Simulate);

        // Possess and Eject switch the flag, and toggle without an argument.
        CHECK(run(session, "ith.play.possess_or_eject").find("possessed")->asBool());
        CHECK(run(session, "ith.play.possess_or_eject", R"({"possess": true})").find("possessed")->asBool());
        CHECK_FALSE(run(session, "ith.play.possess_or_eject", R"({"possess": false})").find("possessed")->asBool());
        CHECK_FALSE(run(session, "ith.play.free_mouse").find("captured")->asBool());
        fails(session, "ith.play.possess_or_eject", R"({"possess": 1})", "editor.command.invalid_args");
        fails(session, "ith.play.play", R"({"paused": "yes"})", "editor.play.running");
        run(session, "ith.play.stop");
        fails(session, "ith.play.play", R"({"paused": "yes"})", "editor.command.invalid_args");
        CHECK(session.play() == nullptr);
    }

    TEST_CASE("play commands are refused in the wrong state with their own codes (requirement 2)") {
        EditorSession session(types());
        for (const std::string_view command :
             {"ith.play.stop", "ith.play.pause", "ith.play.step", "ith.play.possess_or_eject", "ith.play.free_mouse"}) {
            CAPTURE(command);
            fails(session, command, "{}", "editor.play.not_running");
            const auto allowed = session.commands().canExecute(session, command);
            REQUIRE_FALSE(allowed.hasValue());
            CHECK(allowed.error().code == "editor.play.not_running");
        }
    }

    TEST_CASE("play commands during a session: Play and Simulate refused, Step only while paused (requirement 2)") {
        EditorSession session(types());
        run(session, "ith.play.simulate");
        fails(session, "ith.play.play", "{}", "editor.play.running");
        fails(session, "ith.play.simulate", "{}", "editor.play.running");
        fails(session, "ith.play.step", "{}", "editor.play.not_paused");
        CHECK(run(session, "ith.play.pause").find("paused")->asBool());
        run(session, "ith.play.step");
        CHECK(timeOf(session).frame == 1);
    }

    TEST_CASE(
        "Play is refused while a level opens and while an interaction is open, starting nothing (requirement 3)") {
        EditorSession session(types());
        session.document().setOpening(true);
        fails(session, "ith.play.play", "{}", "editor.document.busy");
        CHECK(session.startPlay(PlayMode::Play).error().code == "editor.document.busy");
        session.document().setOpening(false);
        {
            ith::TransactionScope drag = session.document().transactions().begin("Drag");
            fails(session, "ith.play.play", "{}", "editor.command.interaction_open");
            fails(session, "ith.play.simulate", "{}", "editor.command.interaction_open");
            CHECK(session.startPlay(PlayMode::Simulate).error().code == "editor.command.interaction_open");
            drag.cancel();
        }
        CHECK(session.play() == nullptr);
        CHECK_FALSE(session.document().playing());
        CHECK(stateOf(session) == "stopped");
    }

    TEST_CASE("a game-system list with a cycle refuses Play with the schedule's error, logged (requirement 4)") {
        int runs = 0;
        SystemDescriptor first = counting(&runs, "test::first");
        first.before = {"test::second"};
        SystemDescriptor second = counting(&runs, "test::second");
        second.before = {"test::first"};
        EditorSession session(types(), {.gameSystems = {first, second}});
        PlayErrors errors;
        fails(session, "ith.play.play", "{}", "ecs.schedule.cycle");
        CHECK(errors.count() == 1);
        CHECK(stateOf(session) == "stopped");
        CHECK_FALSE(run(session, "ith.query.document").find("playing")->asBool());
        CHECK(runs == 0);
    }

    TEST_CASE("the game world is a copy without editor-only data under another world id (requirements 7 and 8)") {
        EditorSession session(types());
        threeEntities(session);
        run(session, "ith.entity.add_component",
            R"({"ids": [)" + quoted(1) + R"(], "type": "ith::test::editor::Aim", "value": {"target": )" + quoted(3) +
                "}}");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        run(session, "ith.play.play");
        const World& editor = session.document().world();
        const World& game = play(session).world();
        CHECK(game.id() != editor.id());
        CHECK(game.entityCount() == 3);
        CHECK(run(session, "ith.query.play").find("entities")->toNumber<std::uint64_t>() == 3U);
        for (std::uint64_t n = 1; n <= 3; ++n) {
            CAPTURE(n);
            const Entity entity = gameEntity(session, n);
            REQUIRE_FALSE(entity.isNull());
            CHECK(entity.worldId() == game.id());
            CHECK_FALSE(game.has<ith::EditorLabel>(entity));
            CHECK(game.has<ith::LocalTransform>(entity));
            CHECK(game.has<ith::ShapeRenderer>(entity));
        }
        // The same hierarchy, order and enabled state.
        CHECK(game.get<const ith::ChildOf>(gameEntity(session, 2))->parent == gameEntity(session, 1));
        std::vector<Uuid> roots;
        game.forEachRoot([&](Entity root) { roots.push_back(game.get<const ith::StableId>(root)->id); });
        CHECK(roots == std::vector<Uuid>{idOf(1), idOf(3)});
        CHECK(game.has<ith::Disabled>(gameEntity(session, 3)));
        CHECK_FALSE(game.has<ith::Disabled>(gameEntity(session, 1)));
        // Requirement 8: the reference points at the game world's copy.
        CHECK(game.get<const Aim>(gameEntity(session, 1))->target == gameEntity(session, 3));
        // The game world has a Time, and the play selection starts as the editor selection.
        CHECK(timeOf(session).frame == 0);
        CHECK(play(session).selection().entities() == std::vector<Uuid>{idOf(2)});
    }

    TEST_CASE("an unsaved, untitled level plays without reading or writing a file (requirement 9)") {
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        threeEntities(session);
        REQUIRE(session.document().path().empty());
        REQUIRE(session.document().dirty());
        const auto before = ith::fs::listDirectory(project.root());
        REQUIRE(before.hasValue());
        run(session, "ith.play.simulate", R"({"paused": true})");
        run(session, "ith.play.step", R"({"frames": 3})");
        CHECK(play(session).world().entityCount() == 3);
        run(session, "ith.play.stop");
        const auto after = ith::fs::listDirectory(project.root());
        REQUIRE(after.hasValue());
        CHECK(after->size() == before->size());
        CHECK(session.document().dirty());
    }

    TEST_CASE("game-world systems run only on the game world, and first see every value as added (10, 11)") {
        int runs = 0;
        std::vector<std::size_t> added;
        const SystemDescriptor seesAdded{
            .name = "test::added",
            .stage = Stage::PrePhysics,
            .access = ith::accessTo<const ith::LocalTransform>(),
            .run = [&added](SystemContext& context) {
                std::size_t count = 0;
                context.query<const ith::LocalTransform, ith::Added<ith::LocalTransform>, ith::IncludeDisabled>()
                    .forEach([&count](const ith::LocalTransform&) { ++count; });
                added.push_back(count);
            }};
        EditorSession session(types(), {.gameSystems = {counting(&runs), mover(), seesAdded}});
        threeEntities(session);
        const std::string editorLevel = levelText(session);
        run(session, "ith.play.simulate", R"({"paused": true})");
        CHECK(run(session, "ith.play.step", R"({"frames": 3})").find("frame")->toNumber<std::uint64_t>() == 3U);
        CHECK(runs == 3);
        // The editor world was never run: the mover moved only the game world.
        CHECK(levelText(session) == editorLevel);
        CHECK(translationX(play(session).world(), gameEntity(session, 1)) == doctest::Approx(3.0));
        // Requirement 11: the first run sees the three entities as added, as after a level load.
        REQUIRE(added.size() == 3);
        CHECK(added[0] == 3);
        CHECK(added[1] == 0);
    }

    TEST_CASE("two sessions in a row each start with a fresh Time and the editor world's resources (12)") {
        const SystemDescriptor scorer{.name = "test::score",
                                      .stage = Stage::PrePhysics,
                                      .access = ith::accessTo<Score>(),
                                      .run = [](SystemContext& context) {
                                          if (Score* score = context.resource<Score>()) {
                                              score->points += 10;
                                          }
                                      }};
        EditorSession session(types(), {.gameSystems = {scorer}});
        session.document().mutableWorld().insertResource(Score{.points = 1});
        for (int round = 0; round < 2; ++round) {
            CAPTURE(round);
            run(session, "ith.play.play", R"({"paused": true})");
            CHECK(timeOf(session).frame == 0);
            CHECK(timeOf(session).elapsed == 0.0);
            CHECK(play(session).world().resource<const Score>()->points == 1);
            run(session, "ith.play.step", R"({"frames": 2})");
            CHECK(play(session).world().resource<const Score>()->points == 21);
            CHECK(play(session).identity() == static_cast<std::uint64_t>(round + 1));
            run(session, "ith.play.stop");
        }
        CHECK(session.document().world().resource<const Score>()->points == 1);
    }

    TEST_CASE("Step advances Time exactly; a dilation written in one tick applies to the next (requirement 13)") {
        struct Seen {
            std::uint64_t frame = 0;
            float delta = 0.0f;
        };

        std::vector<Seen> early;
        std::vector<Seen> late;
        const auto recorder = [](std::vector<Seen>* seen, std::string name, Stage stage) {
            return SystemDescriptor{.name = std::move(name),
                                    .stage = stage,
                                    .access = ith::accessTo<const Time>(),
                                    .run = [seen](SystemContext& context) {
                                        const Time* time = context.resource<const Time>();
                                        seen->push_back({.frame = time->frame, .delta = time->delta});
                                    }};
        };
        const SystemDescriptor slower{.name = "test::slow",
                                      .stage = Stage::PostPhysics,
                                      .access = ith::accessTo<Time>(),
                                      .run = [](SystemContext& context) {
                                          Time* time = context.resource<Time>();
                                          if (time->frame == 2) {
                                              time->dilation = 0.5f;
                                          }
                                      }};
        EditorSession session(types(), {.gameSystems = {recorder(&early, "test::early", Stage::Input),
                                                        recorder(&late, "test::late", Stage::PostUpdate), slower}});
        run(session, "ith.play.play", R"({"paused": true})");
        const Value stepped = run(session, "ith.play.step", R"({"frames": 2, "deltaSeconds": 0.02})");
        CHECK(stepped.find("frame")->toNumber<std::uint64_t>() == 2U);
        CHECK(stepped.find("elapsed")->toNumber<double>() == doctest::Approx(0.04));
        CHECK(timeOf(session).delta == doctest::Approx(0.02));
        CHECK(timeOf(session).elapsed == doctest::Approx(0.04));

        run(session, "ith.play.step", R"({"deltaSeconds": 0.02})");
        CHECK(timeOf(session).frame == 3);
        CHECK(timeOf(session).delta == doctest::Approx(0.01));
        CHECK(timeOf(session).unscaledDelta == doctest::Approx(0.02));
        CHECK(timeOf(session).elapsed == doctest::Approx(0.05));
        // Every system of a tick, whatever its stage, sees the tick's final Time.
        REQUIRE(early.size() == 3);
        REQUIRE(late.size() == 3);
        for (std::size_t tick = 0; tick < 3; ++tick) {
            CAPTURE(tick);
            CHECK(early[tick].frame == tick + 1);
            CHECK(late[tick].frame == tick + 1);
            CHECK(early[tick].delta == late[tick].delta);
        }
        CHECK(early[2].delta == doctest::Approx(0.01));
    }

    TEST_CASE("Step's arguments are checked (requirement 14)") {
        EditorSession session(types());
        run(session, "ith.play.simulate", R"({"paused": true})");
        for (const std::string_view args :
             {R"({"deltaSeconds": 0})", R"({"deltaSeconds": 0.2})", R"({"deltaSeconds": -0.01})",
              R"({"deltaSeconds": "fast"})", R"({"frames": 0})", R"({"frames": 1001})", R"({"frames": 1.5})",
              R"({"frames": -1})"}) {
            CAPTURE(args);
            fails(session, "ith.play.step", args, "editor.command.invalid_args");
        }
        CHECK(timeOf(session).frame == 0);
        run(session, "ith.play.step", R"({"frames": 1000, "deltaSeconds": 0.1})");
        CHECK(timeOf(session).frame == 1000);
        CHECK(timeOf(session).unscaledElapsed == doctest::Approx(100.0));
    }

    TEST_CASE("the frame clock: the first call after a start or a resume only takes the time (15, 16)") {
        int runs = 0;
        EditorSession session(types(), {.gameSystems = {counting(&runs)}});
        run(session, "ith.play.play");
        PlaySession& game = play(session);
        game.tick(10 * kSecond);
        CHECK(timeOf(session).frame == 0);
        CHECK(runs == 0);
        // A 2-second gap is clamped to kMaxTickDelta.
        game.tick(12 * kSecond);
        CHECK(timeOf(session).frame == 1);
        CHECK(runs == 1);
        CHECK(timeOf(session).unscaledDelta == doctest::Approx(ith::kMaxTickDelta));
        // Running, every call advances one frame.
        game.tick(12 * kSecond + kSecond / 50);
        CHECK(timeOf(session).frame == 2);
        CHECK(timeOf(session).unscaledDelta == doctest::Approx(0.02));
        // A clock that went back counts as no time.
        game.tick(11 * kSecond);
        CHECK(timeOf(session).frame == 3);
        CHECK(timeOf(session).unscaledDelta == 0.0f);

        // Paused: tick calls advance nothing.
        CHECK(run(session, "ith.play.pause").find("paused")->asBool());
        game.tick(13 * kSecond);
        game.tick(15 * kSecond);
        CHECK(timeOf(session).frame == 3);
        CHECK(runs == 3);
        // Five seconds later a Resume: the first call only records its time, the next is measured from it.
        CHECK_FALSE(run(session, "ith.play.pause").find("paused")->asBool());
        game.tick(20 * kSecond);
        CHECK(timeOf(session).frame == 3);
        game.tick(20 * kSecond + kSecond / 40);
        CHECK(timeOf(session).frame == 4);
        CHECK(timeOf(session).unscaledDelta == doctest::Approx(0.025));
        // pause with an argument sets the state; resuming a running session changes nothing.
        CHECK_FALSE(run(session, "ith.play.pause", R"({"paused": false})").find("paused")->asBool());
        game.tick(20 * kSecond + kSecond / 20);
        CHECK(timeOf(session).frame == 5);
        CHECK(run(session, "ith.play.pause", R"({"paused": true})").find("paused")->asBool());
        CHECK(run(session, "ith.play.pause", R"({"paused": true})").find("paused")->asBool());
        fails(session, "ith.play.pause", R"({"paused": 0})", "editor.command.invalid_args");
    }

    TEST_CASE("paused, a value edit shows in the game world's WorldTransform after propagation (requirement 17)") {
        EditorSession session(types());
        create(session, 1, "Crate");
        run(session, "ith.play.simulate", R"({"paused": true})");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) +
                R"(], "path": "ith::LocalTransform.value.translation", "value": {"x": 4, "y": 0, "z": 0}})");
        ith::propagateTransforms(play(session).mutableWorld());
        const auto* placed = play(session).world().get<const ith::WorldTransform>(gameEntity(session, 1));
        REQUIRE(placed != nullptr);
        CHECK(placed->matrix[3][0] == doctest::Approx(4.0));
        CHECK(timeOf(session).frame == 0);
        CHECK(translationX(session.document().world(), session.document().world().findByStableId(idOf(1))) == 0.0);
    }

    TEST_CASE("during play the selection commands and queries act on the game world (requirements 28, 32)") {
        EditorSession session(types());
        threeEntities(session);
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        run(session, "ith.play.simulate", R"({"paused": true})");

        CHECK(run(session, "ith.query.selection").find("primary")->asString() == idText(1));
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(3) + R"(], "mode": "add"})");
        CHECK(run(session, "ith.query.selection").find("entities")->elements().size() == 2);
        // The editor selection is unchanged.
        CHECK(run(session, "ith.query.selection", R"({"world": "editor"})").find("entities")->elements().size() == 1);
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
        run(session, "ith.edit.select_all");
        CHECK(play(session).selection().entities().size() == 3);
        run(session, "ith.edit.select_none");
        CHECK(play(session).selection().empty());
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1)});
        fails(session, "ith.query.selection", R"({"world": "other"})", "editor.command.invalid_args");

        // Labels come from the editor twins; find matches them.
        CHECK(run(session, "ith.query.find", R"({"label": "Lid"})").find("ids")->elements().at(0).asString() ==
              idText(2));
        CHECK(run(session, "ith.query.find", R"({"label": "Lid", "world": "editor"})").find("ids")->elements().size() ==
              1);
        const Value lamp = run(session, "ith.query.entity", R"({"id": )" + quoted(3) + "}");
        CHECK(lamp.find("label")->asString() == "Lamp");
        CHECK(lamp.find("components")->find("ith::editor::EditorLabel") == nullptr);
        CHECK(run(session, "ith.query.entity", R"({"id": )" + quoted(3) + R"(, "world": "editor"})")
                  .find("components")
                  ->find("ith::EditorLabel") != nullptr);
        // The game world's hash is its own (no editor-only data); the editor's is unchanged.
        CHECK(run(session, "ith.query.world_hash").find("hash")->asString() !=
              run(session, "ith.query.world_hash", R"({"world": "editor"})").find("hash")->asString());

        run(session, "ith.play.stop");
        fails(session, "ith.query.entity", R"({"id": )" + quoted(3) + R"(, "world": "game"})",
              "editor.play.not_running");
        CHECK(run(session, "ith.query.selection").find("entities")->elements().size() == 1);
    }

    TEST_CASE("an entity spawned during play shows as \"Spawned <n>\", unselectable, and is counted (28, 29)") {
        bool spawn = false;
        const SystemDescriptor spawner{
            .name = "test::spawn", .stage = Stage::PrePhysics, .run = [&spawn](SystemContext& context) {
                if (spawn) {
                    static_cast<void>(context.commands().spawn(ith::LocalTransform{}, ith::WorldTransform{}));
                    spawn = false;
                }
            }};
        EditorSession session(types(), {.gameSystems = {spawner, mover()}});
        threeEntities(session);
        ith::OutlinerModel outliner(session);
        outliner.update();
        const std::uint64_t editorBuilds = outliner.rebuilds();
        run(session, "ith.play.simulate", R"({"paused": true})");
        outliner.update();
        CHECK(outliner.rebuilds() == editorBuilds + 1); // another world
        REQUIRE(outliner.entries().size() == 3);
        CHECK(outliner.entries()[0].label == "Crate"); // from the editor twin
        CHECK(outliner.entries()[1].label == "Lid");

        // A tick that only changes values doesn't rebuild the Outliner (requirement 29).
        run(session, "ith.play.step");
        outliner.update();
        CHECK(outliner.rebuilds() == editorBuilds + 1);

        spawn = true;
        run(session, "ith.play.step");
        outliner.update();
        CHECK(outliner.rebuilds() == editorBuilds + 2);
        REQUIRE(outliner.entries().size() == 4);
        const ith::OutlinerEntry& spawned = outliner.entries().back();
        CHECK(spawned.spawned);
        CHECK(spawned.id.isNil());
        CHECK(spawned.label.starts_with("Spawned "));
        CHECK(outliner.click(spawned.id, ith::OutlinerClick::Replace).error().code == "editor.outliner.not_selectable");
        CHECK(run(session, "ith.query.play").find("spawned")->toNumber<std::uint64_t>() == 1U);
        CHECK(run(session, "ith.query.play").find("entities")->toNumber<std::uint64_t>() == 4U);

        // Selection through the Outliner selects in the game world.
        REQUIRE(outliner.click(idOf(2), ith::OutlinerClick::Replace).hasValue());
        CHECK(play(session).selection().entities() == std::vector<Uuid>{idOf(2)});
        outliner.update();
        CHECK(outliner.tree().selected().size() == 1);

        run(session, "ith.play.stop");
        outliner.update();
        CHECK(outliner.rebuilds() == editorBuilds + 3);
        CHECK(outliner.entries().size() == 3);
    }

    TEST_CASE("view models key on the world: Play and Stop rebuild them with matching counters (29a)") {
        EditorSession session(types());
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        ith::DetailsModel details(session);
        details.update();
        const std::uint64_t editorRefreshes = details.refreshes();
        run(session, "ith.play.simulate", R"({"paused": true})");
        // The game world's values are its own: change one, then look again from the editor and back.
        play(session).mutableWorld().get<Lamp>(gameEntity(session, 1))->intensity = 7.0f;
        details.update();
        CHECK(details.refreshes() == editorRefreshes + 1);
        REQUIRE(details.entities() == std::vector<Uuid>{idOf(1)});
        run(session, "ith.play.stop");
        details.update();
        CHECK(details.refreshes() == editorRefreshes + 2);
        CHECK(details.entities() == std::vector<Uuid>{idOf(1)});
    }

    TEST_CASE("value commands change the game world only, recording nothing (requirement 30)") {
        EditorSession session(types());
        threeEntities(session);
        const std::string hash = run(session, "ith.query.world_hash").find("hash")->asString();
        const std::size_t entries = session.document().history().entries().size();
        const std::size_t cursor = session.document().history().cursor();
        const bool dirty = session.document().dirty();
        run(session, "ith.play.play", R"({"paused": true})");
        const std::string gameHash = run(session, "ith.query.world_hash").find("hash")->asString();

        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) +
                R"(], "path": "ith::LocalTransform.value.translation", "value": {"x": 2, "y": 0, "z": 0}})");
        CHECK(translationX(play(session).world(), gameEntity(session, 1)) == doctest::Approx(2.0));
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(3) + R"(], "path": "ith::test::editor::Lamp.steps", "value": [1, 2]})");
        run(session, "ith.entity.insert_element",
            R"({"ids": [)" + quoted(3) + R"(], "path": "ith::test::editor::Lamp.steps", "value": 9})");
        run(session, "ith.entity.erase_element",
            R"({"ids": [)" + quoted(3) + R"(], "path": "ith::test::editor::Lamp.steps", "index": 0})");
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 3))->steps == std::vector<std::int32_t>{2, 9});
        run(session, "ith.entity.reset_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::LocalTransform.value.translation"})");
        CHECK(translationX(play(session).world(), gameEntity(session, 1)) == 0.0);
        // An invalid value is refused as when editing.
        fails(session, "ith.entity.set_property",
              R"({"ids": [)" + quoted(1) + R"(], "path": "ith::LocalTransform.value.translation", "value": 3})",
              "serialization.wrong_type");
        CHECK(run(session, "ith.query.world_hash").find("hash")->asString() != gameHash);

        CHECK(run(session, "ith.query.world_hash", R"({"world": "editor"})").find("hash")->asString() == hash);
        CHECK(session.document().history().entries().size() == entries);
        CHECK(session.document().history().cursor() == cursor);
        CHECK(session.document().dirty() == dirty);
        run(session, "ith.play.stop");
        CHECK(run(session, "ith.query.world_hash").find("hash")->asString() == hash);
    }

    TEST_CASE("a Details drag on the game world previews live, Esc cancels it, nothing is recorded (30)") {
        EditorSession session(types());
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        const std::size_t entries = session.document().history().entries().size();
        run(session, "ith.play.simulate", R"({"paused": true})");
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
        // The play commands run during it (ADR-0053 rules 6, 7 and 10): Step keeps the game going, and
        // Play and Simulate say first that a session runs. Other commands wait for it.
        run(session, "ith.play.step");
        fails(session, "ith.play.play", "{}", "editor.play.running");
        fails(session, "ith.play.simulate", "{}", "editor.play.running");
        fails(session, "ith.edit.select_none", "{}", "editor.command.interaction_open");
        const float dragged = 5.0f;
        REQUIRE(details.previewValue(&dragged).hasValue());
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 1))->intensity == 5.0f);
        // Esc through the keymap consumer cancels the drag on the game world.
        const ith::Keymap noBindings;
        ith::KeymapConsumer keymap(session, noBindings);
        CHECK(keymap.onInputEvent(ith::test::editor::keyDown(ith::Key::Escape)) == ith::InputReply::Consumed);
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 1))->intensity == 2.0f);
        details.commitEdit();
        // A committed drag records nothing.
        REQUIRE(details.beginEdit(*row).hasValue());
        REQUIRE(details.previewValue(&dragged).hasValue());
        details.commitEdit();
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 1))->intensity == 5.0f);
        CHECK(session.document().history().entries().size() == entries);
        CHECK(
            session.document().world().get<const Lamp>(session.document().world().findByStableId(idOf(1)))->intensity ==
            2.0f);

        // ith.play.stop during a drag succeeds; the model drops the drag, before and after update().
        REQUIRE(details.beginEdit(*row).hasValue());
        REQUIRE(details.previewValue(&dragged).hasValue());
        CHECK(run(session, "ith.play.stop").find("state")->asString() == "stopped");
        CHECK(session.play() == nullptr);
        CHECK_FALSE(details.editing());
        CHECK(details.editingKey().empty());
        details.update();
        CHECK_FALSE(details.editing());
        CHECK(details.previewValue(&dragged).error().code == "editor.details.not_editing");
        CHECK(
            session.document().world().get<const Lamp>(session.document().world().findByStableId(idOf(1)))->intensity ==
            2.0f);
    }

    TEST_CASE("while playing, structural edits, history and level files are refused, changing nothing (31)") {
        TempProject project;
        EditorSession session(types(), {.projectDirectory = project.root()});
        threeEntities(session);
        run(session, "ith.file.save_as", R"({"path": "Content/Level.ithmap"})");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::LocalTransform.value.translation", "value": {"x": 1}})");
        const std::string hash = run(session, "ith.query.world_hash").find("hash")->asString();
        run(session, "ith.play.play", R"({"paused": true})");
        const std::string gameHash = run(session, "ith.query.world_hash").find("hash")->asString();
        const std::string one = quoted(1);
        const std::string three = quoted(3);
        for (const auto& [command, args] : std::vector<std::pair<std::string_view, std::string>>{
                 {"ith.entity.create", R"({"label": "New"})"},
                 {"ith.edit.rename", R"({"id": )" + one + R"(, "label": "x"})"},
                 {"ith.entity.attach", R"({"ids": [)" + three + R"(], "parent": )" + one + "}"},
                 {"ith.entity.detach", R"({"ids": [)" + quoted(2) + "]}"},
                 {"ith.entity.move_before", R"({"id": )" + three + "}"},
                 {"ith.entity.add_component", R"({"ids": [)" + one + R"(], "type": "ith::test::editor::Lamp"})"},
                 {"ith.entity.remove_component", R"({"ids": [)" + one + R"(], "type": "ith::ShapeRenderer"})"},
                 {"ith.entity.set_enabled", R"({"ids": [)" + three + R"(], "enabled": true})"},
                 {"ith.edit.delete", R"({"ids": [)" + one + "]}"},
                 {"ith.edit.delete_with_children", R"({"ids": [)" + one + "]}"},
                 {"ith.edit.duplicate", R"({"ids": [)" + one + "]}"},
                 {"ith.viewport.snap_to_floor", R"({"ids": [)" + one + "]}"},
                 {"ith.edit.undo", "{}"},
                 {"ith.edit.redo", "{}"},
                 {"ith.file.new_level", R"({"unsaved": "discard"})"},
                 {"ith.file.open_level", R"({"path": "Content/Level.ithmap", "unsaved": "discard"})"},
                 {"ith.file.save", "{}"},
                 {"ith.file.save_as", R"({"path": "Content/Other.ithmap"})"}}) {
            CAPTURE(command);
            fails(session, command, args, "editor.command.playing");
        }
        CHECK(run(session, "ith.query.world_hash").find("hash")->asString() == gameHash);
        CHECK(run(session, "ith.query.world_hash", R"({"world": "editor"})").find("hash")->asString() == hash);
        CHECK_FALSE(ith::test::editor::exists(ith::fs::joinPath(project.root(), "Content/Other.ithmap")));
        CHECK(session.document().dirty());
        run(session, "ith.play.stop");
        run(session, "ith.file.save");
        session.waitIdle();
        CHECK_FALSE(session.document().dirty());
    }

    TEST_CASE("while playing, the Content Browser's file operations work and opening a level is refused (31, 31a)") {
        ith::test::editor::Project p;
        p.native("Levels/A.ithmap", "ith.level");
        p.native("S/Ship.ithasset");
        p.write("S/Loose.gltf", "glTF");
        p.write("S/Mesh.gltf", "glTF");
        p.rescan();
        p.execute("ith.content.import", R"({"files": ["Content/S/Mesh.gltf"]})");
        run(*p.session, "ith.play.simulate", R"({"paused": true})");

        fails(*p.session, "ith.content.open", R"({"item": {"path": "Levels/A.ithmap"}})", "editor.command.playing");
        CHECK(p.execute("ith.content.new_folder", R"({"name": "Props"})").find("path")->asString() == "Props");
        p.execute("ith.content.rename", R"({"item": {"path": "S/Ship.ithasset"}, "name": "Boat"})");
        p.execute("ith.content.move", R"({"items": [{"path": "S/Boat.ithasset"}], "folder": "Props"})");
        p.execute("ith.content.duplicate", R"({"items": [{"path": "Props/Boat.ithasset"}]})");
        p.execute("ith.content.reimport", R"({"items": [{"path": "S/Mesh.gltf"}]})");
        p.execute("ith.content.fix", R"({"item": {"path": "S/Loose.gltf"}})");
        p.execute("ith.content.delete", R"({"items": [{"path": "Props/Boat_2.ithasset"}], "confirmed": true})");
        CHECK(ith::test::editor::exists(p.file("Props/Boat.ithasset")));
        CHECK_FALSE(ith::test::editor::exists(p.file("Props/Boat_2.ithasset")));
        CHECK(ith::test::editor::exists(p.file("S/Loose.gltf.meta")));
        CHECK(p.session->play() != nullptr);
        CHECK(stateOf(*p.session) == "running");
    }

    TEST_CASE("Stop leaves the editor exactly as before Play (requirements 34 and 35)") {
        EditorSession session(types(), {.gameSystems = {mover()}});
        threeEntities(session);
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        run(session, "ith.edit.undo"); // a redoable entry too
        const std::string hash = run(session, "ith.query.world_hash").find("hash")->asString();
        const Value history = run(session, "ith.query.history");
        const bool dirty = session.document().dirty();
        const std::vector<Uuid> selection = session.document().selection().entities();
        const std::uint8_t editorId = session.document().world().id();

        run(session, "ith.play.play", R"({"paused": true})");
        run(session, "ith.play.step", R"({"frames": 10})");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::LocalTransform.value.translation", "value": {"x": 9}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        run(session, "ith.play.possess_or_eject");
        const std::uint64_t first = play(session).identity();
        run(session, "ith.play.stop");

        CHECK(session.play() == nullptr);
        CHECK_FALSE(session.document().playing());
        CHECK(run(session, "ith.query.world_hash").find("hash")->asString() == hash);
        CHECK(run(session, "ith.query.history") == history);
        CHECK(session.document().dirty() == dirty);
        CHECK(session.document().selection().entities() == selection);
        CHECK(session.document().world().id() == editorId);
        // The next Play gets a fresh world.
        run(session, "ith.play.play", R"({"paused": true})");
        CHECK(play(session).identity() != first);
        CHECK(timeOf(session).frame == 0);
        CHECK(translationX(play(session).world(), gameEntity(session, 1)) == 0.0);
        run(session, "ith.edit.select_none");
        // A session still running when the EditorSession goes is destroyed with it.
    }

    TEST_CASE("Play, Simulate and Stop refuse to run from inside another command, freeing nothing") {
        EditorSession session(types());
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Lamp": {}})");
        // An Edit command that runs on the game world and tries to stop it from inside its transaction.
        session.commands().add({.id = "test.edit.stop_inside",
                                .displayName = "Stop Inside",
                                .playEditable = true,
                                .execute = [](EditorSession& inner, const Value&) -> ith::Expected<Value> {
                                    ith::Expected<Value> stopped = inner.execute("ith.play.stop");
                                    REQUIRE_FALSE(stopped.hasValue());
                                    CHECK(stopped.error().code == "editor.command.nested");
                                    return Value::string(stopped.error().code);
                                }});
        // An Action that tries to start a session from inside a command.
        session.commands().add({.id = "test.action.play_inside",
                                .displayName = "Play Inside",
                                .kind = ith::CommandKind::Action,
                                .execute = [](EditorSession& inner, const Value&) -> ith::Expected<Value> {
                                    for (const std::string_view id : {"ith.play.play", "ith.play.simulate"}) {
                                        ith::Expected<Value> started = inner.execute(id);
                                        REQUIRE_FALSE(started.hasValue());
                                        CHECK(started.error().code == "editor.command.nested");
                                    }
                                    return Value();
                                }});
        run(session, "test.action.play_inside");
        CHECK(session.play() == nullptr);

        run(session, "ith.play.simulate", R"({"paused": true})");
        CHECK(run(session, "test.edit.stop_inside").asString() == "editor.command.nested");
        REQUIRE(session.play() != nullptr);
        CHECK_FALSE(play(session).transactions().isOpen());
        // The session is whole: it still steps and edits, and stops from the top level.
        run(session, "ith.play.step");
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::test::editor::Lamp.intensity", "value": 3})");
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 1))->intensity == 3.0f);
        run(session, "ith.play.stop");
        CHECK(session.play() == nullptr);
        CHECK(session.commands().depth() == 0);
    }

    TEST_CASE("the registry asserts on play flags given to the wrong kind of command") {
        EditorSession session(types());
        const auto nothing = [](EditorSession&, const Value&) -> ith::Expected<Value> { return Value(); };
        {
            ith::test::AssertCapture capture;
            session.commands().add({.id = "test.action.editable",
                                    .kind = ith::CommandKind::Action,
                                    .playEditable = true,
                                    .execute = nothing});
            session.commands().add({.id = "test.edit.during", .runsDuringInteraction = true, .execute = nothing});
#ifdef ITH_ENABLE_ASSERTS
            CHECK(capture.count() == 2);
#endif
        }
        CHECK(session.commands().find("test.action.editable") == nullptr);
        CHECK(session.commands().find("test.edit.during") == nullptr);
    }

    TEST_CASE("a system despawning the dragged entity: Esc and Stop are clean (foreign writers)") {
        bool despawn = false;
        const SystemDescriptor despawner{.name = "test::despawn",
                                         .stage = Stage::PrePhysics,
                                         .access = ith::accessTo<const ith::StableId>(),
                                         .run = [&despawn](SystemContext& context) {
                                             if (!despawn) {
                                                 return;
                                             }
                                             despawn = false;
                                             context.query<const ith::StableId>().forEach(
                                                 [&context](Entity entity, const ith::StableId& id) {
                                                     if (id.id == idOf(1)) {
                                                         context.commands().despawn(entity);
                                                     }
                                                 });
                                         }};
        EditorSession session(types(), {.gameSystems = {despawner}});
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        create(session, 2, "Box", R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + ", " + quoted(2) + "]}");
        run(session, "ith.play.simulate", R"({"paused": true})");
        ith::DetailsModel details(session);
        details.update();
        const std::optional<std::size_t> row = rowOf(details, "ith::test::editor::Lamp.intensity");
        REQUIRE(row.has_value());
        REQUIRE(details.beginEdit(*row).hasValue());
        const float dragged = 6.0f;
        REQUIRE(details.previewValue(&dragged).hasValue());
        // The game keeps ticking during the drag, and a system despawns one dragged entity.
        despawn = true;
        run(session, "ith.play.step");
        CHECK(gameEntity(session, 1).isNull());
        // Esc: the inverse no longer applies; it is dropped and logged, not asserted.
        {
            ith::test::AssertCapture capture;
            const ith::Keymap noBindings;
            ith::KeymapConsumer keymap(session, noBindings);
            CHECK(keymap.onInputEvent(ith::test::editor::keyDown(ith::Key::Escape)) == ith::InputReply::Consumed);
            CHECK(capture.count() == 0);
        }
        CHECK_FALSE(play(session).transactions().isOpen());
        details.update();
        details.commitEdit(); // the release ends the cancelled drag; it records nothing
        CHECK_FALSE(details.editing());
        // Another drag, still open at Stop, is dropped without a replay.
        REQUIRE(details.beginEdit(*row).hasValue());
        REQUIRE(details.previewValue(&dragged).hasValue());
        CHECK(play(session).world().get<const Lamp>(gameEntity(session, 2))->intensity == 6.0f);
        run(session, "ith.play.stop");
        details.update();
        CHECK_FALSE(details.editing());
        CHECK(session.document().world().entityCount() == 2);
        CHECK(
            session.document().world().get<const Lamp>(session.document().world().findByStableId(idOf(2)))->intensity ==
            2.0f);
    }

    TEST_CASE("Details refreshes on a tick only when a shown value changed (requirement 47)") {
        bool brighten = false;
        const SystemDescriptor brightener{.name = "test::brighten",
                                          .stage = Stage::PrePhysics,
                                          .access = ith::accessTo<Lamp>(),
                                          .run = [&brighten](SystemContext& context) {
                                              if (brighten) {
                                                  context.query<Lamp>().forEach(
                                                      [](Lamp& lamp) { lamp.intensity += 1.0f; });
                                              }
                                          }};
        EditorSession session(types(), {.gameSystems = {brightener}});
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Lamp": {"intensity": 2}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        run(session, "ith.play.simulate", R"({"paused": true})");
        ith::DetailsModel details(session);
        details.update();
        const std::uint64_t base = details.refreshes();
        // Transform propagation writes only the Transient WorldTransform, which Details doesn't show.
        for (int tick = 0; tick < 50; ++tick) {
            run(session, "ith.play.step");
            details.update();
        }
        CHECK(details.refreshes() == base);
        // A system changing a shown value: one refresh per tick, showing the new value.
        brighten = true;
        for (int tick = 0; tick < 5; ++tick) {
            run(session, "ith.play.step");
            details.update();
        }
        CHECK(details.refreshes() == base + 5);
        const std::optional<std::size_t> row = rowOf(details, "ith::test::editor::Lamp.intensity");
        REQUIRE(row.has_value());
        CHECK(*static_cast<const float*>(details.value(*row)) == 7.0f);
    }

    TEST_CASE("Details reports nothing from the previous world's handles before update() (Play, Stop)") {
        EditorSession session(types());
        create(session, 1, "Crate", R"(, "components": {"ith::test::editor::Aim": {"target": )" + quoted(1) + "}}");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        ith::DetailsModel details(session);
        details.update();
        const Entity editorCrate = session.document().world().findByStableId(idOf(1));
        REQUIRE_FALSE(details.addableComponents().empty());
        CHECK(details.entityLabel(editorCrate) == "Crate");
        const ith::TypeInfo* lamp = session.types().find("ith::test::editor::Lamp");
        REQUIRE(lamp != nullptr);

        run(session, "ith.play.simulate", R"({"paused": true})");
        // No update(): the model's handles are the editor world's.
        CHECK(details.addableComponents().empty());
        CHECK(details.addComponent(*lamp).error().code == "editor.details.stale");
        CHECK(details.entityLabel(editorCrate) == "None");
        CHECK(details.beginEdit(0).error().code == "editor.details.stale");
        CHECK_FALSE(details.editing());
        details.update();
        CHECK_FALSE(details.addableComponents().empty());
        CHECK(details.entityLabel(gameEntity(session, 1)) == "Crate");

        const Entity gameCrate = gameEntity(session, 1);
        run(session, "ith.play.stop");
        CHECK(details.addableComponents().empty());
        CHECK(details.addComponent(*lamp).error().code == "editor.details.stale");
        CHECK(details.entityLabel(gameCrate) == "None");
        details.update();
        CHECK(details.entityLabel(editorCrate) == "Crate");
    }

    TEST_CASE("a spawned entity in a reused slot gets another Outliner row key") {
        EditorSession session(types());
        create(session, 1, "Crate");
        run(session, "ith.play.simulate", R"({"paused": true})");
        ith::OutlinerModel outliner(session);
        World& game = play(session).mutableWorld();
        const Entity first = game.spawn(ith::LocalTransform{});
        run(session, "ith.play.step"); // the session follows the structure after a tick
        outliner.update();
        REQUIRE(outliner.entries().size() == 2);
        REQUIRE(outliner.entries()[1].spawned);
        const std::uint64_t firstKey = outliner.tree().items()[1].key;

        REQUIRE(game.despawn(first));
        const Entity second = game.spawn(ith::LocalTransform{});
        REQUIRE(second.index() == first.index());
        REQUIRE(second.generation() != first.generation());
        run(session, "ith.play.step");
        outliner.update();
        REQUIRE(outliner.entries().size() == 2);
        CHECK(outliner.tree().items()[1].key != firstKey);
    }
    TEST_CASE("bumpAllValues makes every older generation ask for a full refresh") {
        ith::EditorChanges changes;
        changes.bumpValues(idOf(1));
        const std::uint64_t seen = changes.values();
        changes.bumpValues(idOf(2));
        REQUIRE(changes.touchedSince(seen).has_value());
        changes.bumpAllValues();
        CHECK(changes.values() == seen + 2);
        CHECK_FALSE(changes.touchedSince(seen).has_value());
        CHECK_FALSE(changes.touchedSince(changes.values() - 1).has_value());
        CHECK(changes.touchedSince(changes.values())->empty());
        changes.bumpValues(idOf(3));
        CHECK(*changes.touchedSince(changes.values() - 1) == std::vector<Uuid>{idOf(3)});
    }
}
