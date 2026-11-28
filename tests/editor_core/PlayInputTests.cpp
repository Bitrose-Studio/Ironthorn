// Play In Editor's views and input, headless (ADR-0053 decisions 5, 10 and 16 to 22,
// docs/specs/play-in-editor.md 3.6 to 3.8): the selected viewport, the play camera saved at the start
// and restored at Stop, the spectator on the Game layer, Possess and Eject with one saved pose, the
// Play In Editor consumer and its Escape rule, capture on click, the keymap's capture rule and its
// gate while the mouse is free, focus loss, and picks checked against the world each viewport draws.
// Everything runs through a real Input with the editor's consumers and the engine's Keymap.json.
// Requirement numbers are the spec's.

#include "ith/editor_core/PlayInput.h"

#include "ith/config/Config.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/ViewportGizmo.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/ViewportPick.h"
#include "ith/editor_core/Viewports.h"
#include "ith/file_system/Path.h"
#include "ith/input/Input.h"
#include "ith/platform_events/Event.h"

#include "AssertCapture.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <utility>
#include <vector>

namespace {

using ith::CameraInput;
using ith::EditorCamera;
using ith::EditorSession;
using ith::Entity;
using ith::Event;
using ith::EventType;
using ith::InputReply;
using ith::Key;
using ith::KeyModifiers;
using ith::MouseButton;
using ith::PickMode;
using ith::Uuid;
using ith::Vec3;
using ith::ViewportTool;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::run;
using ith::test::editor::TempProject;
using ith::test::editor::types;

constexpr ith::WindowId kWindow = 1;
constexpr ith::DeviceId kGamepad = 7;
// A frame time below ith::kMaxFlyStepSeconds, which the spectator moves for in full.
constexpr float kFrame = 0.025f;
static_assert(kFrame < ith::kMaxFlyStepSeconds);

// What reaches the Game layer: on top of it, it records every event and consumes none.
class GameRecorder final : public ith::InputConsumer {
public:
    InputReply onInputEvent(const Event& event) override {
        m_seen.push_back(event.type == EventType::KeyDown || event.type == EventType::KeyUp ? event.key : Key::None);
        m_types.push_back(event.type);
        return InputReply::NotConsumed;
    }

    [[nodiscard]] bool sawKey(Key key) const { return std::ranges::find(m_seen, key) != m_seen.end(); }

    [[nodiscard]] bool saw(EventType type) const { return std::ranges::find(m_types, type) != m_types.end(); }

    [[nodiscard]] bool empty() const { return m_types.empty(); }

    void clear() {
        m_seen.clear();
        m_types.clear();
    }

private:
    std::vector<Key> m_seen;
    std::vector<EventType> m_types;
};

// A stand-in for the UI's consumer: a panel at (600, 500) to (800, 700) takes the presses on it, and
// with `keyboard` (a text field or a popup has the keyboard) every key press.
class PanelsStandIn final : public ith::InputConsumer {
public:
    InputReply onInputEvent(const Event& event) override {
        if (event.type == EventType::KeyDown && keyboard) {
            return InputReply::Consumed;
        }
        const bool onPanel = event.x >= 600.0f && event.x < 800.0f && event.y >= 500.0f && event.y < 700.0f;
        if ((event.type == EventType::MouseButtonDown || event.type == EventType::MouseMoved) && onPanel) {
            return InputReply::Consumed;
        }
        return InputReply::NotConsumed;
    }

    bool keyboard = false;
};

// An editor window's stack as ADR-0053 rule 19 orders it, top first: the Play In Editor consumer, the
// UI stand-in, the viewports, the keymap; then the Game layer: the recorder above the spectator.
// Viewport 1's image is at (100, 100) to (500, 400), viewport 2's at (600, 100) to (800, 300). The
// keymap is the engine's Keymap.json.
struct Rig {
    explicit Rig(ith::EditorSessionOptions options = {}) : session(types(), std::move(options)) {
        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        keymap = ith::Keymap::load(config, session.commands(), warnings);
        session.setPlayInput(&input, kWindow);
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Game, .consumer = &spectator});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Game, .consumer = &game});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &viewports});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &panels});
        input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &playKeys});
        ith::ViewportState& first = session.viewports().at(0);
        first.imagePosition = {100.0f, 100.0f};
        first.imageSize = {400.0f, 300.0f};
        first.imageHovered = true;
        ith::ViewportState& second = session.viewports().at(1);
        second.imagePosition = {600.0f, 100.0f};
        second.imageSize = {200.0f, 200.0f};
        second.imageHovered = true;
        events({Event{.type = EventType::WindowFocusGained}});
        // Rotate is the tool, so W running translate_tool shows.
        session.viewports().setTool(ViewportTool::Rotate);
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    // The binding's contract: Stop, then unbind, before the Input goes (it is destroyed before the
    // session here).
    ~Rig() {
        session.stopPlay();
        session.setPlayInput(nullptr, ith::kNoWindow);
    }

    // Routes one frame's events.
    void events(std::vector<Event> list) {
        timeNs += 16'000'000;
        for (Event& event : list) {
            event.timestampNs = timeNs;
            event.window = kWindow;
        }
        input.processEvents(list, timeNs);
    }

    // A key pressed and released in one frame.
    void tap(Key key, KeyModifiers modifiers = KeyModifiers::None) {
        events({Event{.type = EventType::KeyDown, .key = key, .modifiers = modifiers},
                Event{.type = EventType::KeyUp, .key = key, .modifiers = modifiers}});
    }

    // An LMB press and release at (x, y) in one frame.
    void click(float x, float y) {
        events(
            {Event{.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y},
             Event{.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y}});
    }

    [[nodiscard]] bool captured() const { return input.mouseCaptured(kWindow); }

    [[nodiscard]] std::string state() { return run(session, "ith.query.play").find("state")->asString(); }

    EditorCamera& camera(std::size_t index = 0) { return session.viewports().at(index).camera; }

    EditorSession session;
    ith::Keymap keymap;
    ith::Input input;
    ith::KeymapConsumer shortcuts{session, keymap};
    ith::ViewportConsumer viewports{session};
    PanelsStandIn panels;
    ith::PlayInputConsumer playKeys{session, keymap};
    ith::SpectatorConsumer spectator{session};
    GameRecorder game;
    std::uint64_t timeNs = 1'000'000'000;
};

Event press(MouseButton button, float x, float y, KeyModifiers modifiers = KeyModifiers::None) {
    return {
        .type = EventType::MouseButtonDown, .modifiers = modifiers, .mouseButton = button, .clicks = 1, .x = x, .y = y};
}

Event release(MouseButton button, float x, float y) {
    return {.type = EventType::MouseButtonUp, .mouseButton = button, .x = x, .y = y};
}

Event moved(float x, float y, float deltaX, float deltaY) {
    return {.type = EventType::MouseMoved, .x = x, .y = y, .deltaX = deltaX, .deltaY = deltaY};
}

// A camera away from the default, so a restore is visible.
EditorCamera movedCamera(float x) {
    EditorCamera camera;
    camera.position = Vec3{x, 2.0f, 7.0f};
    camera.yaw = 0.25f;
    camera.pitch = -0.125f;
    camera.speedStep = 3;
    return camera;
}

// The camera after one updateCamera call.
EditorCamera expected(EditorCamera camera, const CameraInput& input, float deltaSeconds = 0.0f) {
    ith::updateCamera(camera, input, deltaSeconds);
    return camera;
}

void createShape(EditorSession& session, std::uint64_t n, float x) {
    run(session, "ith.entity.create",
        R"({"id": ")" + idText(n) + R"(", "components": {"ith::ShapeRenderer": {"shape": "Cube"}, )" +
            R"("ith::LocalTransform": {"value": {"translation": {"x": )" + std::to_string(x) + "}}}}}");
}

std::vector<Uuid> playSelection(EditorSession& session) {
    REQUIRE(session.play() != nullptr);
    return session.play()->selection().entities();
}

} // namespace

TEST_SUITE("editor_core.play_input") {
    TEST_CASE("a session plays in the selected viewport: the last one pressed, ith.viewport.1 at first (1)") {
        Rig rig;
        CHECK(rig.session.viewports().selected() == 0);
        CHECK(run(rig.session, "ith.play.simulate").find("viewport")->asString() == "ith.viewport.1");
        run(rig.session, "ith.play.stop");
        // A press on viewport 2's image selects it; a press elsewhere doesn't.
        rig.events({press(MouseButton::Right, 700.0f, 200.0f), release(MouseButton::Right, 700.0f, 200.0f)});
        rig.events({press(MouseButton::Left, 50.0f, 50.0f), release(MouseButton::Left, 50.0f, 50.0f)});
        CHECK(rig.session.viewports().selected() == 1);
        CHECK(run(rig.session, "ith.play.play").find("viewport")->asString() == "ith.viewport.2");
        CHECK(run(rig.session, "ith.query.play").find("viewport")->asString() == "ith.viewport.2");
        CHECK(&rig.session.drawnWorld(1) == &rig.session.play()->world());
        CHECK(&rig.session.drawnWorld(0) == &rig.session.document().world());
        run(rig.session, "ith.play.stop");
        CHECK(&rig.session.drawnWorld(1) == &rig.session.document().world());
    }

    TEST_CASE("Simulate flies the play viewport's own camera, restored bit for bit at Stop and never saved (18, 21)") {
        TempProject project;
        Rig rig({.projectDirectory = project.root()});
        run(rig.session, "ith.file.save_as", R"({"path": "Content/Level.ithmap"})");
        const Uuid level = rig.session.document().levelId();
        rig.camera(0) = movedCamera(1.0f);
        rig.camera(1) = movedCamera(5.0f);
        const EditorCamera before = rig.camera(0);
        const EditorCamera other = rig.camera(1);

        run(rig.session, "ith.play.simulate");
        CHECK(rig.camera(0) == before); // Simulate doesn't move the view
        CHECK(rig.session.play()->savedCamera() == before);
        // RMB fly on the play viewport moves its own camera, as when editing.
        rig.events({press(MouseButton::Right, 200.0f, 200.0f), moved(210.0f, 200.0f, 40.0f, 0.0f)});
        rig.events({release(MouseButton::Right, 210.0f, 200.0f)});
        CHECK(rig.camera(0) != before);
        CHECK(rig.camera(1) == other); // the other viewports keep their cameras
        CHECK(&rig.session.drawnWorld(1) == &rig.session.document().world());
        // A save while the session runs keeps the pose from before it.
        rig.session.saveViewports();
        const std::string file = ith::fs::joinPath(project.root(), ith::kViewportsFile);
        {
            ith::Viewports saved(file);
            saved.restoreLevel(level);
            CHECK(saved.at(0).camera == before);
        }
        CHECK(rig.camera(0) != before); // and leaves the session's camera alone

        run(rig.session, "ith.play.stop");
        CHECK(rig.camera(0) == before);
        CHECK(rig.camera(1) == other);
        // A level close after Stop writes the pre-Simulate pose.
        rig.camera(1) = movedCamera(6.0f);
        run(rig.session, "ith.file.new_level", R"({"unsaved": "discard"})");
        ith::Viewports reread(file);
        reread.restoreLevel(level);
        CHECK(reread.at(0).camera == before);
        CHECK(reread.at(1).camera == movedCamera(6.0f));
    }

    TEST_CASE("Play: the spectator starts at the camera, turns with captured motion and W flies it (19)") {
        Rig rig;
        rig.camera(0) = movedCamera(2.0f);
        const EditorCamera before = rig.camera(0);
        run(rig.session, "ith.play.play");
        CHECK(rig.camera(0) == before);
        CHECK(rig.input.captureOnClick(kWindow));
        CHECK_FALSE(rig.captured()); // only a click captures
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        CHECK(run(rig.session, "ith.query.play").find("captured")->asBool());

        // Captured motion turns it, through the Game layer.
        CameraInput look;
        look.rmb = true;
        look.mouseDelta = {30.0f, -10.0f};
        const EditorCamera turned = expected(before, look);
        rig.events({moved(220.0f, 200.0f, 30.0f, -10.0f)});
        CHECK(rig.game.saw(EventType::MouseMoved));
        CHECK(rig.camera(0) == turned);
        CHECK(rig.spectator.update(kFrame));

        // W flies it forward at the camera's speed step; the keymap runs nothing.
        rig.events({Event{.type = EventType::KeyDown, .key = Key::W}});
        CHECK(rig.game.sawKey(Key::W));
        CameraInput fly;
        fly.rmb = true;
        fly.forward = true;
        const EditorCamera flown = expected(turned, fly, kFrame);
        CHECK(rig.spectator.update(kFrame));
        CHECK(rig.camera(0) == flown);
        CHECK(rig.session.viewports().tool() == ViewportTool::Rotate);
        rig.events({Event{.type = EventType::KeyUp, .key = Key::W}});
        CHECK_FALSE(rig.spectator.update(kFrame));
        CHECK(rig.camera(0) == flown);
    }

    TEST_CASE("Eject keeps the spectator's pose; Possess resumes there; Simulate possesses at the camera (20)") {
        Rig rig;
        rig.camera(0) = movedCamera(1.0f);
        run(rig.session, "ith.play.play");
        rig.camera(0) = movedCamera(2.0f); // where the spectator flew
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        rig.tap(Key::F8); // Eject
        CHECK_FALSE(run(rig.session, "ith.query.play").find("possessed")->asBool());
        CHECK_FALSE(rig.captured());
        CHECK_FALSE(rig.input.captureOnClick(kWindow));
        CHECK(rig.camera(0) == movedCamera(2.0f));
        REQUIRE(rig.session.play()->spectatorPose().has_value());
        CHECK(*rig.session.play()->spectatorPose() == movedCamera(2.0f));
        rig.camera(0) = movedCamera(3.0f); // the editor camera flies while ejected
        // Ejected, a click on the play viewport doesn't capture.
        rig.click(200.0f, 200.0f);
        CHECK_FALSE(rig.captured());
        run(rig.session, "ith.play.possess_or_eject", R"({"possess": true})");
        CHECK(rig.camera(0) == movedCamera(2.0f));
        CHECK(rig.input.captureOnClick(kWindow));
        run(rig.session, "ith.play.possess_or_eject", R"({"possess": true})"); // no change
        CHECK(rig.camera(0) == movedCamera(2.0f));
        run(rig.session, "ith.play.stop");
        CHECK(rig.camera(0) == movedCamera(1.0f));

        run(rig.session, "ith.play.simulate");
        CHECK_FALSE(rig.input.captureOnClick(kWindow));
        rig.camera(0) = movedCamera(4.0f);
        CHECK_FALSE(rig.session.play()->spectatorPose().has_value());
        run(rig.session, "ith.play.possess_or_eject");
        CHECK(rig.camera(0) == movedCamera(4.0f));
        run(rig.session, "ith.play.stop");
        CHECK(rig.camera(0) == movedCamera(1.0f));
    }

    TEST_CASE("possessed, a press on the play viewport captures, picks nothing and reaches no game consumer (22)") {
        Rig rig;
        createShape(rig.session, 1, 0.0f);
        run(rig.session, "ith.play.play");
        const EditorCamera before = rig.camera(0);
        // A press on a panel and one on another viewport don't capture; the other viewport works.
        rig.click(700.0f, 600.0f);
        CHECK_FALSE(rig.captured());
        rig.click(700.0f, 200.0f);
        CHECK_FALSE(rig.captured());
        CHECK(rig.session.viewports().at(1).click.has_value());
        // The wheel and motion over the play viewport neither move its camera nor reach the game.
        rig.events({Event{.type = EventType::MouseWheel, .x = 200.0f, .y = 200.0f, .deltaY = 1.0f},
                    moved(210.0f, 200.0f, 10.0f, 0.0f)});
        CHECK(rig.camera(0) == before);
        CHECK(rig.game.empty());
        // The press on the play viewport captures; neither it nor its release reaches the game.
        rig.click(200.0f, 200.0f);
        CHECK(rig.captured());
        CHECK_FALSE(rig.session.viewports().at(0).click.has_value());
        CHECK_FALSE(rig.viewports.holdsDrag());
        CHECK(rig.game.empty());
        CHECK(run(rig.session, "ith.query.play").find("captured")->asBool());
        // Captured, presses go to the game only.
        rig.click(200.0f, 200.0f);
        CHECK(rig.game.saw(EventType::MouseButtonDown));
        CHECK_FALSE(rig.session.viewports().at(0).click.has_value());
    }

    TEST_CASE("while captured, keys are the game's except Escape, Shift + F1 and F8 (23, 24)") {
        Rig rig;
        createShape(rig.session, 1, 0.0f);
        run(rig.session, "ith.selection.set", R"({"entities": [")" + idText(1) + R"("]})");
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        const std::vector<Uuid> selection = playSelection(rig.session);
        for (const auto& [key, modifiers] :
             std::vector<std::pair<Key, KeyModifiers>>{{Key::W, KeyModifiers::None},
                                                       {Key::K, KeyModifiers::None},
                                                       {Key::Delete, KeyModifiers::None},
                                                       {Key::Z, KeyModifiers::Control},
                                                       {Key::S, KeyModifiers::Control}}) {
            CAPTURE(static_cast<int>(key));
            rig.game.clear();
            rig.tap(key, modifiers);
            CHECK(rig.game.sawKey(key));
        }
        // No editor command ran: no tool change, no refused Undo or Save, the play selection as it was.
        CHECK(rig.session.viewports().tool() == ViewportTool::Rotate);
        CHECK(rig.shortcuts.lastResult().hasValue());
        CHECK(playSelection(rig.session) == selection);

        // Shift + F1 frees the mouse and keeps the session; keys are the editor's again.
        rig.playKeys.reportUi(true, false); // Shift + F1 and F8 act with a text field active too
        rig.tap(Key::F1, KeyModifiers::Shift);
        CHECK_FALSE(rig.captured());
        CHECK(rig.state() == "running");
        rig.playKeys.reportUi(false, false);
        rig.game.clear();
        rig.tap(Key::W);
        CHECK(rig.session.viewports().tool() == ViewportTool::Translate);
        CHECK_FALSE(rig.game.sawKey(Key::W));
        // The next press on the play viewport captures again.
        rig.click(200.0f, 200.0f);
        CHECK(rig.captured());
        // Escape stops play while captured, even with a text field and a popup reported.
        rig.playKeys.reportUi(true, true);
        rig.tap(Key::Escape);
        CHECK(rig.state() == "stopped");
        CHECK_FALSE(rig.captured());
        CHECK_FALSE(rig.input.captureOnClick(kWindow));
    }

    TEST_CASE("F8 ejects a captured session and possesses an ejected one; the next click captures (23)") {
        Rig rig;
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        rig.tap(Key::F8);
        CHECK_FALSE(rig.session.play()->possessed());
        CHECK_FALSE(rig.captured());
        rig.tap(Key::F8);
        CHECK(rig.session.play()->possessed());
        CHECK_FALSE(rig.captured());
        rig.click(200.0f, 200.0f);
        CHECK(rig.captured());
        // Without a session the Play In Editor keys run nothing: F8 and Shift + F1 reach the game.
        run(rig.session, "ith.play.stop");
        rig.game.clear();
        rig.tap(Key::F8);
        rig.tap(Key::F1, KeyModifiers::Shift);
        CHECK(rig.game.sawKey(Key::F8));
        CHECK(rig.game.sawKey(Key::F1));
        CHECK(rig.playKeys.lastResult().hasValue());
    }

    TEST_CASE("the keymap gate: with the mouse free nothing reaches the game but capture and a possessed gamepad "
              "(24a)") {
        Rig rig;
        rig.events({Event{.type = EventType::GamepadConnected, .device = kGamepad}});
        const Event button{
            .type = EventType::GamepadButtonDown, .device = kGamepad, .gamepadButton = ith::GamepadButton::FaceSouth};
        const Event buttonUp{
            .type = EventType::GamepadButtonUp, .device = kGamepad, .gamepadButton = ith::GamepadButton::FaceSouth};
        // No session: an unbound key and the mouse reach the game.
        rig.game.clear();
        rig.tap(Key::J);
        rig.events({moved(50.0f, 50.0f, 1.0f, 1.0f)});
        CHECK(rig.game.sawKey(Key::J));
        CHECK(rig.game.saw(EventType::MouseMoved));

        for (const bool possessed : {true, false}) {
            CAPTURE(possessed);
            run(rig.session, possessed ? "ith.play.play" : "ith.play.simulate");
            rig.game.clear();
            rig.tap(Key::J);
            rig.events({moved(50.0f, 50.0f, 1.0f, 1.0f),
                        Event{.type = EventType::MouseWheel, .x = 50.0f, .y = 50.0f, .deltaY = 1.0f},
                        press(MouseButton::Middle, 50.0f, 50.0f), release(MouseButton::Middle, 50.0f, 50.0f)});
            CHECK_FALSE(rig.game.sawKey(Key::J));
            CHECK_FALSE(rig.game.saw(EventType::MouseMoved));
            CHECK_FALSE(rig.game.saw(EventType::MouseWheel));
            CHECK_FALSE(rig.game.saw(EventType::MouseButtonDown));
            CHECK_FALSE(rig.captured()); // a press outside the play viewport never captures
            rig.events({button, buttonUp});
            CHECK(rig.game.saw(EventType::GamepadButtonDown) == possessed);
            // Possessed, the press on the play viewport still reaches capture on click.
            rig.click(200.0f, 200.0f);
            CHECK(rig.captured() == possessed);
            run(rig.session, "ith.play.stop");
        }
    }

    TEST_CASE("Escape with the mouse free yields to a text field, a popup and an interaction, then stops (25)") {
        Rig rig;
        run(rig.session, "ith.entity.create",
            R"({"id": ")" + idText(1) + R"(", "components": {"ith::test::editor::Lamp": {"intensity": 2}}})");
        run(rig.session, "ith.selection.set", R"({"entities": [")" + idText(1) + R"("]})");
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        const std::vector<Uuid> selection = playSelection(rig.session);

        // A text field the UI reported: the UI takes Escape (it ends the editing); play runs on.
        rig.playKeys.reportUi(true, false);
        rig.panels.keyboard = true;
        rig.tap(Key::Escape);
        CHECK(rig.state() == "running");
        // An open popup or dialog: the UI consumes Escape and closes it; nothing below sees it.
        rig.playKeys.reportUi(false, true);
        rig.tap(Key::Escape);
        CHECK(rig.state() == "running");
        CHECK(playSelection(rig.session) == selection); // Select None never ran
        rig.playKeys.reportUi(false, false);
        rig.panels.keyboard = false;

        // A Details drag on the game world: the keymap consumer cancels it.
        ith::DetailsModel details(rig.session);
        details.update();
        std::optional<std::size_t> row;
        for (std::size_t i = 0; i < details.rows().size(); ++i) {
            if (details.rows()[i].key == "ith::test::editor::Lamp.intensity") {
                row = i;
            }
        }
        REQUIRE(row.has_value());
        REQUIRE(details.beginEdit(*row).hasValue());
        const float dragged = 5.0f;
        REQUIRE(details.previewValue(&dragged).hasValue());
        rig.tap(Key::Escape);
        CHECK(rig.state() == "running");
        const Entity lamp = rig.session.play()->world().findByStableId(idOf(1));
        CHECK(rig.session.play()->world().get<const ith::test::editor::Lamp>(lamp)->intensity == 2.0f);
        details.commitEdit();
        CHECK(rig.game.empty());

        // With nothing left taking it, Escape stops the session.
        rig.tap(Key::Escape);
        CHECK(rig.state() == "stopped");
        // Without a session Escape is the editor's Select None again.
        rig.tap(Key::Escape);
        CHECK(rig.session.document().selection().empty());
    }

    TEST_CASE("focus loss while captured ends capture, releases held keys and keeps the session running (26)") {
        Rig rig;
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        rig.events({Event{.type = EventType::KeyDown, .key = Key::W}});
        CHECK(rig.spectator.update(kFrame));
        rig.game.clear();
        rig.events({Event{.type = EventType::WindowFocusLost}});
        CHECK_FALSE(rig.captured());
        CHECK(rig.game.saw(EventType::KeyUp));
        CHECK(rig.state() == "running");
        const EditorCamera stopped = rig.camera(0);
        CHECK_FALSE(rig.spectator.update(kFrame));
        CHECK(rig.camera(0) == stopped);
        // Capture on click stays on: after focus returns, a click captures again.
        CHECK(rig.input.captureOnClick(kWindow));
        rig.events({Event{.type = EventType::WindowFocusGained}});
        rig.click(200.0f, 200.0f);
        CHECK(rig.captured());
    }

    TEST_CASE("in Simulate the play viewport works as when editing and nothing reaches the game (27)") {
        Rig rig;
        run(rig.session, "ith.play.simulate");
        const EditorCamera before = rig.camera(0);
        // LMB clicks pick.
        rig.click(200.0f, 200.0f);
        CHECK(rig.session.viewports().at(0).click.has_value());
        CHECK_FALSE(rig.captured());
        // RMB fly with W.
        rig.events({press(MouseButton::Right, 200.0f, 200.0f), Event{.type = EventType::KeyDown, .key = Key::W}});
        CHECK(rig.viewports.flying());
        CHECK(rig.viewports.update(kFrame));
        rig.events({Event{.type = EventType::KeyUp, .key = Key::W}, release(MouseButton::Right, 200.0f, 200.0f)});
        CHECK(rig.camera(0) != before);
        // Alt + LMB orbits, Alt + RMB dollies.
        const EditorCamera flown = rig.camera(0);
        rig.events({press(MouseButton::Left, 200.0f, 200.0f, KeyModifiers::Alt), moved(240.0f, 200.0f, 40.0f, 0.0f),
                    release(MouseButton::Left, 240.0f, 200.0f)});
        CHECK(rig.camera(0) != flown);
        const EditorCamera orbited = rig.camera(0);
        rig.events({press(MouseButton::Right, 200.0f, 200.0f, KeyModifiers::Alt), moved(200.0f, 260.0f, 0.0f, 60.0f),
                    release(MouseButton::Right, 200.0f, 260.0f)});
        CHECK(rig.camera(0) != orbited);
        CHECK(rig.game.empty());
        CHECK(rig.session.viewports().tool() == ViewportTool::Rotate); // W flew, it didn't pick a tool
    }

    TEST_CASE("picks are checked against the world each viewport draws and change the play selection (32, 33)") {
        Rig rig;
        createShape(rig.session, 1, 0.0f);
        createShape(rig.session, 2, 4.0f);
        run(rig.session, "ith.selection.set", R"({"entities": [")" + idText(2) + R"("]})");
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        ith::PlaySession& play = *rig.session.play();
        const ith::World& editor = rig.session.document().world();

        // The play viewport's id target holds game-world handles.
        const Entity gameOne = play.world().findByStableId(idOf(1));
        REQUIRE(ith::applyPick(rig.session, 0, ith::pickTexel(gameOne), PickMode::Replace).hasValue());
        CHECK(playSelection(rig.session) == std::vector<Uuid>{idOf(1)});
        // Another viewport's holds editor-world handles; the pick still changes the play selection.
        const Entity editorTwo = editor.findByStableId(idOf(2));
        REQUIRE(ith::applyPick(rig.session, 1, ith::pickTexel(editorTwo), PickMode::Add).hasValue());
        CHECK(playSelection(rig.session) == (std::vector<Uuid>{idOf(1), idOf(2)}));
        CHECK(rig.session.document().selection().entities() == std::vector<Uuid>{idOf(2)});
        CHECK(run(rig.session, "ith.query.selection", R"({"world": "editor"})").find("primary")->asString() ==
              idText(2));

        // An entity spawned during play has no stable id: a hit on it changes nothing.
        const Entity spawned = rig.session.play()->mutableWorld().spawn(ith::LocalTransform{});
        REQUIRE(ith::applyPick(rig.session, 0, ith::pickTexel(spawned), PickMode::Replace).hasValue());
        CHECK(playSelection(rig.session) == (std::vector<Uuid>{idOf(1), idOf(2)}));
        // A hit in an editor viewport on an entity the game world has despawned changes nothing either.
        CHECK(rig.session.play()->mutableWorld().despawn(play.world().findByStableId(idOf(2))));
        REQUIRE(ith::applyPick(rig.session, 1, ith::pickTexel(editorTwo), PickMode::Replace).hasValue());
        CHECK(playSelection(rig.session) == (std::vector<Uuid>{idOf(1), idOf(2)}));
        // Empty space clears the play selection only.
        REQUIRE(ith::applyPick(rig.session, 1, {}, PickMode::Replace).hasValue());
        CHECK(playSelection(rig.session).empty());
        CHECK(rig.session.document().selection().entities() == std::vector<Uuid>{idOf(2)});

        // No gizmo while playing, possessed or not (33).
        REQUIRE(ith::applyPick(rig.session, 0, ith::pickTexel(gameOne), PickMode::Replace).hasValue());
        rig.session.viewports().setTool(ViewportTool::Translate);
        CHECK_FALSE(ith::viewportGizmo(rig.session, rig.session.viewports().at(0), 1.0f).has_value());
        CHECK_FALSE(ith::viewportGizmo(rig.session, rig.session.viewports().at(1), 1.0f).has_value());
        run(rig.session, "ith.play.possess_or_eject");
        CHECK_FALSE(ith::viewportGizmo(rig.session, rig.session.viewports().at(0), 1.0f).has_value());
        run(rig.session, "ith.play.possess_or_eject");

        // Focus frames the play selection in the world its viewport draws (rule 16).
        run(rig.session, "ith.entity.set_property",
            R"({"ids": [")" + idText(1) +
                R"("], "path": "ith::LocalTransform.value.translation", "value": {"x": 20}})");
        run(rig.session, "ith.viewport.focus_selection", R"({"viewport": "ith.viewport.1"})");
        run(rig.session, "ith.viewport.focus_selection", R"({"viewport": "ith.viewport.2"})");
        CHECK(rig.camera(0).pivot.x == doctest::Approx(20.0f));
        CHECK(rig.camera(1).pivot.x == doctest::Approx(0.0f));
        run(rig.session, "ith.edit.select_none");
        ith::test::editor::fails(rig.session, "ith.viewport.focus_selection", "{}", "editor.command.disabled");
    }

    TEST_CASE("Stop drops the play viewport's click, restores its camera and ends capture and capture on click (34, "
              "35)") {
        Rig rig;
        rig.camera(0) = movedCamera(1.0f);
        rig.camera(1) = movedCamera(2.0f);
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        rig.events({moved(220.0f, 200.0f, 50.0f, 20.0f), Event{.type = EventType::KeyDown, .key = Key::W}});
        CHECK(rig.spectator.update(kFrame));
        rig.tap(Key::F8);
        rig.camera(0).position.x += 3.0f; // ejected, the editor camera flies on
        rig.session.viewports().at(0).click = ith::ViewportClick{};
        run(rig.session, "ith.play.stop");
        CHECK(rig.camera(0) == movedCamera(1.0f));
        CHECK(rig.camera(1) == movedCamera(2.0f));
        CHECK_FALSE(rig.session.viewports().at(0).click.has_value());
        CHECK_FALSE(rig.captured());
        CHECK_FALSE(rig.input.captureOnClick(kWindow));
        // The W held from the session moves nothing now.
        CHECK_FALSE(rig.spectator.update(kFrame));
        CHECK(rig.camera(0) == movedCamera(1.0f));
    }

    TEST_CASE("headless, without a bound Input, nothing is captured and Possess and Free the Mouse still work") {
        EditorSession session(types());
        run(session, "ith.play.play");
        CHECK_FALSE(run(session, "ith.query.play").find("captured")->asBool());
        CHECK_FALSE(run(session, "ith.play.possess_or_eject").find("possessed")->asBool());
        CHECK_FALSE(run(session, "ith.play.free_mouse").find("captured")->asBool());
        CHECK(session.playInput() == nullptr);
        CHECK_FALSE(session.mouseCaptured());
        run(session, "ith.play.stop");
    }

    TEST_CASE("a session destroyed while a possessed session runs stops it through the bound Input") {
        ith::Input input;
        input.processEvents(std::vector<Event>{Event{.type = EventType::WindowFocusGained, .window = kWindow}}, 1);
        auto session = std::make_unique<EditorSession>(types());
        // Bound and unbound while no session runs, as the contract says; then bound for good.
        session->setPlayInput(&input, kWindow);
        CHECK(session->playInput() == &input);
        session->setPlayInput(nullptr, ith::kNoWindow);
        CHECK(session->playInput() == nullptr);
        session->setPlayInput(&input, kWindow);
        run(*session, "ith.play.play");
        REQUIRE(input.captureMouse(kWindow));
        CHECK(session->mouseCaptured());
        CHECK(input.captureOnClick(kWindow));
        session.reset(); // the Input outlives it; under ASan this also shows no use after free
        CHECK_FALSE(input.mouseCaptured(kWindow));
        CHECK_FALSE(input.captureOnClick(kWindow));
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("binding the play input while a session runs asserts and keeps the binding") {
        Rig rig;
        run(rig.session, "ith.play.simulate");
        ith::Input other;
        {
            ith::test::AssertCapture capture;
            rig.session.setPlayInput(&other, 2);
            CHECK(capture.count() == 1);
        }
        CHECK(rig.session.playInput() == &rig.input);
    }
#endif

    TEST_CASE("Stop, Eject and Shift + F1 release only the editor window's capture") {
        constexpr ith::WindowId kOther = 2;
        Rig rig;
        const auto captureOther = [&rig] {
            rig.input.processEvents(std::vector<Event>{Event{.type = EventType::WindowFocusGained, .window = kOther}},
                                    rig.timeNs += 16'000'000);
            REQUIRE(rig.input.captureMouse(kOther));
        };
        run(rig.session, "ith.play.play");
        captureOther();
        run(rig.session, "ith.play.free_mouse");
        CHECK(rig.input.mouseCaptured(kOther));
        CHECK_FALSE(rig.session.mouseCaptured());
        run(rig.session, "ith.play.possess_or_eject"); // Eject
        CHECK(rig.input.mouseCaptured(kOther));
        run(rig.session, "ith.play.stop");
        CHECK(rig.input.mouseCaptured(kOther));
        // The editor window's own capture is still released.
        rig.events({Event{.type = EventType::WindowFocusGained}});
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        run(rig.session, "ith.play.stop");
        CHECK_FALSE(rig.captured());
    }

    TEST_CASE("the spectator moves nothing once the mouse is freed or the session ejected, keys held or not") {
        Rig rig;
        run(rig.session, "ith.play.play");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        rig.events({Event{.type = EventType::KeyDown, .key = Key::W}});
        rig.tap(Key::F1, KeyModifiers::Shift);
        REQUIRE_FALSE(rig.captured());
        const EditorCamera freed = rig.camera(0);
        CHECK_FALSE(rig.spectator.update(0.1f));
        CHECK(rig.camera(0) == freed);
        // Captured again, the W held from before moves nothing until it is pressed again.
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        CHECK_FALSE(rig.spectator.update(0.1f));
        CHECK(rig.camera(0) == freed);
        rig.events({Event{.type = EventType::KeyUp, .key = Key::W}, Event{.type = EventType::KeyDown, .key = Key::W}});
        CHECK(rig.spectator.update(kFrame));
        // Eject: the held W moves nothing.
        rig.tap(Key::F8);
        const EditorCamera ejected = rig.camera(0);
        CHECK_FALSE(rig.spectator.update(0.1f));
        CHECK(rig.camera(0) == ejected);
    }

    TEST_CASE("motion that is not finite moves neither the spectator nor a viewport drag") {
        Rig rig;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        run(rig.session, "ith.play.simulate");
        // A viewport fly drag on the ejected play viewport.
        rig.events({press(MouseButton::Right, 200.0f, 200.0f)});
        const EditorCamera flying = rig.camera(0);
        rig.events({moved(200.0f, 200.0f, nan, 0.0f), moved(200.0f, 200.0f, 0.0f, inf)});
        CHECK(rig.camera(0) == flying);
        rig.events({release(MouseButton::Right, 200.0f, 200.0f)});
        // The spectator, possessed and captured.
        run(rig.session, "ith.play.possess_or_eject");
        rig.click(200.0f, 200.0f);
        REQUIRE(rig.captured());
        const EditorCamera before = rig.camera(0);
        rig.events({moved(200.0f, 200.0f, nan, 3.0f), moved(200.0f, 200.0f, 3.0f, -inf)});
        CHECK(rig.camera(0) == before);
        CHECK(std::isfinite(rig.camera(0).yaw));
        CHECK_FALSE(rig.spectator.update(kFrame));
    }

    TEST_CASE("Simulate, Possess, Eject, then Possess resumes at the Eject pose, not the camera (20)") {
        Rig rig;
        rig.camera(0) = movedCamera(1.0f);
        run(rig.session, "ith.play.simulate");
        rig.camera(0) = movedCamera(2.0f);
        run(rig.session, "ith.play.possess_or_eject", R"({"possess": true})");
        CHECK(rig.camera(0) == movedCamera(2.0f)); // no Eject yet: the camera's pose
        rig.camera(0) = movedCamera(3.0f);         // where the spectator flew
        run(rig.session, "ith.play.possess_or_eject", R"({"possess": false})");
        CHECK(rig.camera(0) == movedCamera(3.0f));
        rig.camera(0) = movedCamera(4.0f); // the editor camera flies while ejected
        run(rig.session, "ith.play.possess_or_eject", R"({"possess": true})");
        CHECK(rig.camera(0) == movedCamera(3.0f));
        run(rig.session, "ith.play.stop");
        CHECK(rig.camera(0) == movedCamera(1.0f));
    }

    TEST_CASE("possessed, the keymap gate passes the press on the play viewport only with capture on click on") {
        Rig rig;
        run(rig.session, "ith.play.play");
        rig.input.setCaptureOnClick(kWindow, false); // as if the application hadn't turned it on
        rig.click(200.0f, 200.0f);
        CHECK_FALSE(rig.captured());
        CHECK(rig.game.empty());
        rig.input.setCaptureOnClick(kWindow, true);
        rig.click(200.0f, 200.0f);
        CHECK(rig.captured());
        CHECK(rig.game.empty());
    }
}
