// The viewport consumer (ADR-0042 rules 6 to 12 and 30) through a real Input with the keymap
// consumer below it: the letter-key rule (RMB + W flies, W alone runs the translate tool, Ctrl + Z
// still undoes during fly), releases to the consumers that saw the press, routing by the reported
// image rectangles, drags held outside them until release, one drag at a time, the wheel with and
// without RMB, pan, orbit and dolly, the click candidate and the clicks it makes (the slop, the
// modifiers, the gestures that never click), and update()'s change report.

#include "ith/editor_core/ViewportInput.h"

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/Viewports.h"
#include "ith/input/Input.h"
#include "ith/math/Math.h"
#include "ith/math/Types.h"
#include "ith/platform_events/Event.h"
#include "ith/serialization/Value.h"

#include "AssertCapture.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

using ith::CameraInput;
using ith::EditorCamera;
using ith::Event;
using ith::EventType;
using ith::InputReply;
using ith::Key;
using ith::KeyModifiers;
using ith::MouseButton;
using ith::PickMode;
using ith::Vec3;
using ith::ViewportClick;
using ith::ViewportGesture;
using ith::ViewportTool;
using ith::test::editor::json;
using ith::test::editor::run;
using ith::test::editor::types;

constexpr ith::WindowId kWindow = 1;
constexpr float kEpsilon = 1e-4f;
// A frame time below ith::kMaxFlyStepSeconds, which fly moves for in full.
constexpr float kFrame = 0.025f;
static_assert(kFrame < ith::kMaxFlyStepSeconds);

bool same(const Vec3& a, const Vec3& b) {
    return glm::all(glm::epsilonEqual(a, b, kEpsilon));
}

bool same(const EditorCamera& a, const EditorCamera& b) {
    return same(a.position, b.position) && same(a.pivot, b.pivot) && std::abs(a.yaw - b.yaw) < kEpsilon &&
           std::abs(a.pitch - b.pitch) < kEpsilon && a.speedStep == b.speedStep;
}

// The camera after one updateCamera call: what the consumer must have fed the controller.
EditorCamera expected(EditorCamera camera, const CameraInput& input, float deltaSeconds = 0.0f) {
    ith::updateCamera(camera, input, deltaSeconds);
    return camera;
}

// An editor window's stack as the editor builds it: the keymap consumer, then the viewport consumer
// above it, with viewport 1's image at (100, 100) to (500, 400) and viewport 2's at (600, 100) to
// (800, 300). The keymap binds W, E, Ctrl + Z and G.
struct Fixture {
    Fixture() {
        std::vector<ith::Error> warnings;
        const ith::serialization::Value layer =
            json(R"({"ith.viewport.translate_tool": ["Keyboard.W"], "ith.viewport.rotate_tool": ["Keyboard.E"],
                     "ith.edit.undo": ["Ctrl+Keyboard.Z"], "ith.viewport.game_view": ["Keyboard.G"]})");
        const ith::serialization::Value* layers[] = {&layer};
        keymap = ith::Keymap::fromLayers(layers, session.commands(), warnings);
        REQUIRE(warnings.empty());
        keymapId = input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
        viewportId =
            input.addConsumer({.window = kWindow, .layer = ith::ConsumerLayer::Editor, .consumer = &viewports});
        ith::ViewportState& first = session.viewports().at(0);
        first.imagePosition = {100.0f, 100.0f};
        first.imageSize = {400.0f, 300.0f};
        first.imageHovered = true;
        ith::ViewportState& second = session.viewports().at(1);
        second.imagePosition = {600.0f, 100.0f};
        second.imageSize = {200.0f, 200.0f};
        second.imageHovered = true;
        // The window has focus; Rotate is the tool, so running translate_tool shows.
        events({Event{.type = EventType::WindowFocusGained}});
        session.viewports().setTool(ViewportTool::Rotate);
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

    EditorCamera& camera(std::size_t index = 0) { return session.viewports().at(index).camera; }

    // What reached the keymap consumer this frame.
    const ith::InputState& keymapState() const { return input.consumerState(keymapId); }

    ith::EditorSession session{types()};
    ith::Keymap keymap;
    ith::KeymapConsumer shortcuts{session, keymap};
    ith::ViewportConsumer viewports{session.viewports()};
    ith::Input input;
    ith::ConsumerId keymapId = ith::kNoConsumer;
    ith::ConsumerId viewportId = ith::kNoConsumer;
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

Event wheel(float x, float y, float steps) {
    return {.type = EventType::MouseWheel, .x = x, .y = y, .deltaY = steps};
}

// A key of the physical key `key`, printing `printed` on the user's layout.
Event keyDown(Key key, char32_t printed, KeyModifiers modifiers = KeyModifiers::None) {
    return {.type = EventType::KeyDown, .key = key, .layoutKey = printed, .modifiers = modifiers};
}

Event keyUp(Key key, char32_t printed) {
    return {.type = EventType::KeyUp, .key = key, .layoutKey = printed};
}

} // namespace

TEST_SUITE("editor_core.viewport_input") {
    TEST_CASE("RMB + W flies forward at the step's speed and doesn't run translate_tool") {
        Fixture fixture;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        CHECK(fixture.viewports.flying());
        CHECK_FALSE(fixture.keymapState().held(MouseButton::Right));
        fixture.events({keyDown(Key::W, U'w')});
        CHECK(fixture.session.viewports().tool() == ViewportTool::Rotate);
        CHECK_FALSE(fixture.keymapState().pressed(Key::W));

        const EditorCamera before = fixture.camera();
        CHECK(fixture.viewports.update(kFrame));
        CameraInput fly;
        fly.rmb = true;
        fly.forward = true;
        CHECK(same(fixture.camera(), expected(before, fly, kFrame)));
        // Step 4 is 4 m/s: 0.1 m in a 25 ms frame.
        CHECK(glm::length(fixture.camera().position - before.position) == doctest::Approx(4.0f * kFrame).epsilon(1e-4));

        // Every fly key: S cancels W, D strafes right, E rises in world space.
        fixture.events({keyDown(Key::S, U's'), keyDown(Key::D, U'd'), keyDown(Key::E, U'e')});
        CHECK(fixture.session.viewports().tool() == ViewportTool::Rotate); // E isn't rotate_tool either
        const EditorCamera strafing = fixture.camera();
        CHECK(fixture.viewports.update(kFrame));
        CameraInput all = fly;
        all.back = true;
        all.right = true;
        all.up = true;
        CHECK(same(fixture.camera(), expected(strafing, all, kFrame)));

        // The fly keys are physical: the key that prints "z" on AZERTY (W's place) flies too.
        fixture.events({keyUp(Key::W, U'w'), keyUp(Key::S, U's'), keyUp(Key::D, U'd'), keyUp(Key::E, U'e')});
        CHECK_FALSE(fixture.viewports.update(0.25f));
        fixture.events({keyDown(Key::W, U'z')});
        const EditorCamera azerty = fixture.camera();
        CHECK(fixture.viewports.update(kFrame));
        CHECK(same(fixture.camera(), expected(azerty, fly, kFrame)));
    }

    TEST_CASE("a long first fly frame, as after an idle wait, moves no farther than kMaxFlyStepSeconds allows") {
        Fixture fixture;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), keyDown(Key::W, U'w')});
        const EditorCamera before = fixture.camera();
        CHECK(fixture.viewports.update(0.1f));
        CameraInput fly;
        fly.rmb = true;
        fly.forward = true;
        CHECK(same(fixture.camera(), expected(before, fly, ith::kMaxFlyStepSeconds)));
        // Step 4 is 4 m/s: 4/30 m, not the 0.4 m of the whole tenth of a second.
        CHECK(glm::length(fixture.camera().position - before.position) <= 4.0f * ith::kMaxFlyStepSeconds + kEpsilon);
    }

    TEST_CASE("only the first frame of a fly movement is clamped, so the speed holds at low frame rates") {
        Fixture fixture;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), keyDown(Key::W, U'w')});
        const EditorCamera before = fixture.camera();
        CHECK(fixture.viewports.update(0.05f));
        CHECK(fixture.viewports.update(0.05f));
        // 4 m/s for 1/30 s, then for the whole second 50 ms frame.
        CHECK(glm::length(fixture.camera().position - before.position) ==
              doctest::Approx(4.0f * (ith::kMaxFlyStepSeconds + 0.05f)).epsilon(1e-4));

        // Letting go of the keys ends the movement: the next one starts clamped again.
        fixture.events({keyUp(Key::W, U'w')});
        CHECK_FALSE(fixture.viewports.update(0.05f));
        fixture.events({keyDown(Key::W, U'w')});
        const EditorCamera again = fixture.camera();
        CHECK(fixture.viewports.update(0.05f));
        CHECK(glm::length(fixture.camera().position - again.position) ==
              doctest::Approx(4.0f * ith::kMaxFlyStepSeconds).epsilon(1e-4));
    }

    TEST_CASE("drag motion is in window coordinates: an orbit at pixel density 2 matches density 1") {
        Fixture single;
        Fixture doubled;
        doubled.viewports.setPixelDensity(2.0f);
        // Twice the pixels for the same hand movement on a 200% display.
        single.events({press(MouseButton::Left, 200.0f, 200.0f, KeyModifiers::Alt)});
        single.events({moved(240.0f, 210.0f, 40.0f, 10.0f)});
        doubled.events({press(MouseButton::Left, 200.0f, 200.0f, KeyModifiers::Alt)});
        doubled.events({moved(280.0f, 220.0f, 80.0f, 20.0f)});
        CHECK(single.camera() != EditorCamera{});
        CHECK(same(doubled.camera(), single.camera()));

        // The drag start is kept in window pixels, for the cursor's way back.
        CHECK(doubled.viewports.dragStart() == std::optional<ith::Vec2>(ith::Vec2{200.0f, 200.0f}));
        doubled.events({release(MouseButton::Left, 280.0f, 220.0f)});
        CHECK_FALSE(doubled.viewports.dragStart().has_value());
    }

    TEST_CASE("without fly no key is consumed: W runs translate_tool, also over the image") {
        Fixture fixture;
        fixture.events({moved(200.0f, 200.0f, 1.0f, 1.0f), keyDown(Key::W, U'w')});
        CHECK(fixture.session.viewports().tool() == ViewportTool::Translate);
        CHECK(fixture.keymapState().pressed(Key::W));
        const EditorCamera before = fixture.camera();
        CHECK_FALSE(fixture.viewports.update(0.5f));
        CHECK(fixture.camera() == before);

        // Alt + RMB dollies; it isn't fly, so the keys still go to the keymap.
        fixture.events({keyUp(Key::W, U'w'), press(MouseButton::Right, 200.0f, 200.0f, KeyModifiers::Alt)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Dolly);
        CHECK_FALSE(fixture.viewports.flying());
        fixture.events({keyDown(Key::E, U'e')});
        CHECK(fixture.session.viewports().tool() == ViewportTool::Rotate);
    }

    TEST_CASE("during fly Ctrl and Alt chords reach the keymap; other unmodified keys are swallowed") {
        Fixture fixture;
        run(fixture.session, "ith.entity.create", R"({"label": "One"})");
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        REQUIRE(fixture.viewports.flying());
        fixture.events({keyDown(Key::Z, U'z', KeyModifiers::Control)});
        CHECK(fixture.session.document().world().entityCount() == 0);
        CHECK(fixture.keymapState().pressed(Key::Z));

        // G (game view) is swallowed, with Shift too; it never toggles mid-flight.
        fixture.events({keyUp(Key::Z, U'z'), keyDown(Key::G, U'g'), keyDown(Key::F, U'f', KeyModifiers::Shift)});
        CHECK_FALSE(fixture.session.viewports().at(0).gameView);
        CHECK_FALSE(fixture.keymapState().pressed(Key::G));
        CHECK_FALSE(fixture.keymapState().pressed(Key::F));
    }

    TEST_CASE("releases go to the consumers that saw the press") {
        Fixture fixture;
        // W pressed during fly: only the viewport saw it, so its release doesn't reach the keymap,
        // even after fly ended.
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), keyDown(Key::W, U'w')});
        fixture.events({release(MouseButton::Right, 200.0f, 200.0f)});
        CHECK_FALSE(fixture.viewports.flying());
        CHECK_FALSE(fixture.keymapState().released(MouseButton::Right));
        fixture.events({keyUp(Key::W, U'w')});
        CHECK_FALSE(fixture.keymapState().released(Key::W));
        CHECK(fixture.input.consumerState(fixture.viewportId).released(Key::W));

        // W pressed before fly ran the tool; held into fly, it moves nothing, and its release still
        // reaches the keymap.
        fixture.events({keyDown(Key::W, U'w')});
        CHECK(fixture.session.viewports().tool() == ViewportTool::Translate);
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        const EditorCamera before = fixture.camera();
        CHECK_FALSE(fixture.viewports.update(0.5f));
        CHECK(fixture.camera() == before);
        fixture.events({keyUp(Key::W, U'w')});
        CHECK(fixture.keymapState().released(Key::W));
    }

    TEST_CASE("outside every image the mouse isn't consumed") {
        Fixture fixture;
        const EditorCamera before = fixture.camera();
        fixture.events(
            {moved(50.0f, 50.0f, 5.0f, 5.0f), press(MouseButton::Right, 50.0f, 50.0f), wheel(50.0f, 50.0f, 1.0f)});
        CHECK(fixture.keymapState().pressed(MouseButton::Right));
        CHECK(fixture.keymapState().axis(ith::MouseAxis::DeltaX) == 5.0f);
        CHECK(fixture.keymapState().axis(ith::MouseAxis::WheelY) == 1.0f);
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK_FALSE(fixture.viewports.update(0.1f));
        CHECK(fixture.camera() == before);
        // The right and bottom edges are outside; an image without area contains nothing.
        CHECK_FALSE(fixture.viewports.viewportAt(500.0f, 200.0f).has_value());
        CHECK_FALSE(fixture.viewports.viewportAt(200.0f, 400.0f).has_value());
        CHECK(fixture.viewports.viewportAt(100.0f, 100.0f) == std::optional<std::size_t>(0));
        CHECK(fixture.viewports.viewportAt(700.0f, 200.0f) == std::optional<std::size_t>(1));
        fixture.session.viewports().at(1).imageSize = {0.0f, 200.0f};
        CHECK_FALSE(fixture.viewports.viewportAt(700.0f, 200.0f).has_value());
        // An image with something drawn on top of it (not hovered) takes nothing either.
        fixture.session.viewports().at(0).imageHovered = false;
        CHECK_FALSE(fixture.viewports.viewportAt(200.0f, 200.0f).has_value());
        fixture.events({press(MouseButton::Middle, 200.0f, 200.0f)});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        fixture.events({release(MouseButton::Middle, 200.0f, 200.0f)});
        fixture.session.viewports().at(0).imageHovered = true;

        // Over the image motion is consumed, without a drag too.
        fixture.events({release(MouseButton::Right, 50.0f, 50.0f), moved(200.0f, 200.0f, 3.0f, 0.0f)});
        CHECK(fixture.keymapState().axis(ith::MouseAxis::DeltaX) == 0.0f);
    }

    TEST_CASE("a drag that leaves the image stays held until its release; one drag at a time") {
        Fixture fixture;
        // MMB pans: position and pivot move together.
        const EditorCamera start = fixture.camera();
        fixture.events({press(MouseButton::Middle, 450.0f, 200.0f)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Pan);
        fixture.events({moved(650.0f, 220.0f, 200.0f, 20.0f)});
        CameraInput pan;
        pan.mmb = true;
        pan.mouseDelta = {200.0f, 20.0f};
        CHECK(same(fixture.camera(), expected(start, pan)));
        CHECK(fixture.keymapState().axis(ith::MouseAxis::DeltaX) == 0.0f);
        // Viewport 2's camera, under the pointer now, doesn't move.
        CHECK(fixture.camera(1) == EditorCamera{});
        CHECK(fixture.viewports.update(0.0f));

        // Other presses and the wheel are swallowed during the drag.
        fixture.events({press(MouseButton::Left, 50.0f, 50.0f), wheel(50.0f, 50.0f, 1.0f)});
        CHECK_FALSE(fixture.keymapState().pressed(MouseButton::Left));
        CHECK(fixture.keymapState().axis(ith::MouseAxis::WheelY) == 0.0f);
        CHECK(fixture.viewports.gesture() == ViewportGesture::Pan);
        fixture.events({release(MouseButton::Left, 50.0f, 50.0f)});
        CHECK(fixture.viewports.holdsDrag());

        // The release far outside ends it; then the mouse outside goes on again.
        fixture.events({release(MouseButton::Middle, 50.0f, 50.0f)});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        fixture.events({moved(40.0f, 40.0f, -10.0f, 0.0f)});
        CHECK(fixture.keymapState().axis(ith::MouseAxis::DeltaX) == -10.0f);
    }

    TEST_CASE("the wheel dollies by steps without RMB and changes the speed step with it") {
        Fixture fixture;
        const EditorCamera start = fixture.camera();
        fixture.events({wheel(200.0f, 200.0f, 2.0f)});
        CameraInput dolly;
        dolly.wheel = 2.0f;
        CHECK(same(fixture.camera(), expected(start, dolly)));
        CHECK(glm::length(fixture.camera().position - start.pivot) ==
              doctest::Approx(glm::length(start.position - start.pivot) * 0.81f).epsilon(1e-4));
        CHECK(fixture.keymapState().axis(ith::MouseAxis::WheelY) == 0.0f);
        CHECK(fixture.viewports.update(0.0f));
        // Over viewport 2 the wheel dollies its camera.
        fixture.events({wheel(700.0f, 200.0f, -1.0f)});
        CHECK(fixture.camera(1) != EditorCamera{});

        // With RMB: one step per whole notch, fractions kept, clamped to 1--8.
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        const Vec3 position = fixture.camera().position;
        fixture.events({wheel(200.0f, 200.0f, 1.0f)});
        CHECK(fixture.camera().speedStep == 5);
        CHECK(fixture.camera().position == position);
        fixture.events({wheel(200.0f, 200.0f, 0.5f)});
        CHECK(fixture.camera().speedStep == 5);
        fixture.events({wheel(200.0f, 200.0f, 0.5f)});
        CHECK(fixture.camera().speedStep == 6);
        fixture.events({wheel(200.0f, 200.0f, 10.0f)});
        CHECK(fixture.camera().speedStep == 8);
        fixture.events({wheel(200.0f, 200.0f, -10.0f), wheel(200.0f, 200.0f, -10.0f)});
        CHECK(fixture.camera().speedStep == 1);

        // The new step's speed: 0.5 m/s at step 1.
        fixture.events({keyDown(Key::W, U'w')});
        const EditorCamera slow = fixture.camera();
        CHECK(fixture.viewports.update(kFrame));
        CHECK(glm::length(fixture.camera().position - slow.position) == doctest::Approx(0.5f * kFrame).epsilon(1e-4));
    }

    TEST_CASE("Alt + LMB orbits around the pivot; Alt + RMB dollies toward it; RMB looks") {
        Fixture fixture;
        const EditorCamera start = fixture.camera();
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f, KeyModifiers::Alt)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Orbit);
        // The motion carries no Alt: the gesture was decided at the press.
        fixture.events({moved(240.0f, 210.0f, 40.0f, 10.0f)});
        CameraInput orbit;
        orbit.lmb = true;
        orbit.alt = true;
        orbit.mouseDelta = {40.0f, 10.0f};
        const EditorCamera orbited = expected(start, orbit);
        CHECK(same(fixture.camera(), orbited));
        CHECK(glm::length(fixture.camera().position - fixture.camera().pivot) ==
              doctest::Approx(glm::length(start.position - start.pivot)).epsilon(1e-4));
        fixture.events({release(MouseButton::Left, 240.0f, 210.0f)});

        fixture.events({press(MouseButton::Right, 200.0f, 200.0f, KeyModifiers::Alt)});
        fixture.events({moved(200.0f, 100.0f, 0.0f, -100.0f)});
        CameraInput dolly;
        dolly.rmb = true;
        dolly.alt = true;
        dolly.mouseDelta = {0.0f, -100.0f};
        const EditorCamera dollied = expected(orbited, dolly);
        CHECK(same(fixture.camera(), dollied));
        CHECK(glm::length(fixture.camera().position - fixture.camera().pivot) ==
              doctest::Approx(glm::length(orbited.position - orbited.pivot) * 0.9f).epsilon(1e-4));
        fixture.events({release(MouseButton::Right, 200.0f, 100.0f)});

        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        fixture.events({moved(200.0f, 200.0f, -30.0f, 15.0f)});
        CameraInput look;
        look.rmb = true;
        look.mouseDelta = {-30.0f, 15.0f};
        CHECK(same(fixture.camera(), expected(dollied, look)));
    }

    TEST_CASE("an LMB press without Alt is a held click candidate that changes nothing yet") {
        Fixture fixture;
        const EditorCamera before = fixture.camera();
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f)});
        CHECK(fixture.viewports.gesture() == ViewportGesture::Click);
        CHECK_FALSE(fixture.keymapState().pressed(MouseButton::Left));
        fixture.events({moved(300.0f, 250.0f, 100.0f, 50.0f), wheel(300.0f, 250.0f, 1.0f), keyDown(Key::W, U'w')});
        CHECK(fixture.camera() == before);
        CHECK(fixture.session.viewports().tool() == ViewportTool::Translate); // keys aren't fly's
        CHECK_FALSE(fixture.viewports.update(0.1f));
        fixture.events({release(MouseButton::Left, 300.0f, 250.0f)});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK_FALSE(fixture.session.viewports().at(0).click.has_value()); // a drag, not a click

        // A side button over the image is swallowed and starts nothing.
        fixture.events({press(MouseButton::Back, 200.0f, 200.0f)});
        CHECK_FALSE(fixture.keymapState().pressed(MouseButton::Back));
        CHECK_FALSE(fixture.viewports.holdsDrag());
    }

    TEST_CASE("an LMB click within the slop sets the viewport's click: the press's place and modifiers") {
        Fixture fixture;
        const std::optional<ViewportClick>& first = fixture.session.viewports().at(0).click;
        const std::optional<ViewportClick>& second = fixture.session.viewports().at(1).click;
        // The pointer wanders 3.6 pixels, inside kClickSlop; the click is where the press was.
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), moved(202.0f, 203.0f, 2.0f, 3.0f)});
        CHECK_FALSE(first.has_value()); // not before the release
        fixture.events({release(MouseButton::Left, 202.0f, 203.0f)});
        CHECK(first == ViewportClick{.position = {100.0f, 100.0f}, .mode = PickMode::Replace});
        CHECK_FALSE(fixture.keymapState().pressed(MouseButton::Left));

        // Ctrl toggles, Shift adds, Ctrl wins over Shift; a newer click replaces one not yet taken.
        fixture.events({press(MouseButton::Left, 150.0f, 120.0f, KeyModifiers::Control),
                        release(MouseButton::Left, 150.0f, 120.0f)});
        CHECK(first == ViewportClick{.position = {50.0f, 20.0f}, .mode = PickMode::Toggle});
        fixture.events({press(MouseButton::Left, 150.0f, 120.0f, KeyModifiers::Shift),
                        release(MouseButton::Left, 150.0f, 120.0f)});
        CHECK(first->mode == PickMode::Add);
        fixture.events({press(MouseButton::Left, 150.0f, 120.0f, KeyModifiers::Control | KeyModifiers::Shift),
                        release(MouseButton::Left, 150.0f, 120.0f)});
        CHECK(first->mode == PickMode::Toggle);

        // Each viewport gets its own click, from its own image's corner.
        fixture.events({press(MouseButton::Left, 650.0f, 150.0f), release(MouseButton::Left, 650.0f, 150.0f)});
        CHECK(second == ViewportClick{.position = {50.0f, 50.0f}, .mode = PickMode::Replace});
        CHECK(first->position == ith::Vec2{50.0f, 20.0f});
    }

    TEST_CASE("a drag past the slop doesn't click, even back at the press; the slop follows the display scale") {
        Fixture fixture;
        const std::optional<ViewportClick>& click = fixture.session.viewports().at(0).click;
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), moved(205.0f, 200.0f, 5.0f, 0.0f),
                        moved(200.0f, 200.0f, -5.0f, 0.0f), release(MouseButton::Left, 200.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
        // A release past the slop without motion events in between doesn't click either.
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 200.0f, 210.0f)});
        CHECK_FALSE(click.has_value());
        // At display scale 1 the slop is 4 pixels: 4 clicks, 5 doesn't.
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 204.0f, 200.0f)});
        CHECK(click.has_value());
        fixture.session.viewports().at(0).click.reset();
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 205.0f, 200.0f)});
        CHECK_FALSE(click.has_value());

        // At display scale 2 the slop is 8 pixels: 6 is still a click, 9 isn't. The pixel density
        // (pan and orbit) doesn't change it: on Windows and X11 it stays 1 at 200%.
        fixture.viewports.setDisplayScale(2.0f);
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), moved(206.0f, 200.0f, 6.0f, 0.0f),
                        release(MouseButton::Left, 206.0f, 200.0f)});
        CHECK(click == ViewportClick{.position = {100.0f, 100.0f}, .mode = PickMode::Replace});
        fixture.session.viewports().at(0).click.reset();
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 209.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
        fixture.viewports.setDisplayScale(1.0f);
        fixture.viewports.setPixelDensity(2.0f);
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 206.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
    }

    TEST_CASE("losing focus with LMB held never picks: cancelClicks() drops the synthetic release's click") {
        Fixture fixture;
        const std::optional<ViewportClick>& click = fixture.session.viewports().at(0).click;
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f)});
        REQUIRE(fixture.viewports.gesture() == ViewportGesture::Click);
        // Input releases held buttons on focus loss, at the pointer, before the focus-loss event: a
        // click by itself, which the editor cancels when it sees WindowFocusLost.
        fixture.events({Event{.type = EventType::WindowFocusLost}});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        CHECK(click.has_value());
        fixture.viewports.cancelClicks();
        CHECK_FALSE(click.has_value());

        // Cancelled while still held, the press's release doesn't click either; every viewport's
        // waiting click goes.
        fixture.events({Event{.type = EventType::WindowFocusGained}});
        fixture.events({press(MouseButton::Left, 650.0f, 150.0f), release(MouseButton::Left, 650.0f, 150.0f)});
        REQUIRE(fixture.session.viewports().at(1).click.has_value());
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f)});
        fixture.viewports.cancelClicks();
        CHECK_FALSE(fixture.session.viewports().at(1).click.has_value());
        fixture.events({release(MouseButton::Left, 200.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
        CHECK_FALSE(fixture.viewports.holdsDrag());
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an invalid display scale is a programmer error and keeps the slop") {
        Fixture fixture;
        {
            const ith::test::AssertCapture capture;
            fixture.viewports.setDisplayScale(0.0f);
            fixture.viewports.setDisplayScale(std::numeric_limits<float>::quiet_NaN());
            CHECK(capture.count() == 2);
        }
        fixture.events({press(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Left, 203.0f, 200.0f)});
        CHECK(fixture.session.viewports().at(0).click.has_value());
    }
#endif

    TEST_CASE("RMB, MMB and the Alt gestures never click") {
        Fixture fixture;
        const std::optional<ViewportClick>& click = fixture.session.viewports().at(0).click;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), release(MouseButton::Right, 200.0f, 200.0f)});
        fixture.events({press(MouseButton::Middle, 200.0f, 200.0f), release(MouseButton::Middle, 200.0f, 200.0f)});
        fixture.events(
            {press(MouseButton::Left, 200.0f, 200.0f, KeyModifiers::Alt), release(MouseButton::Left, 200.0f, 200.0f)});
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f, KeyModifiers::Alt),
                        release(MouseButton::Right, 200.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
        // An LMB press during another drag is swallowed and doesn't click on its release.
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), press(MouseButton::Left, 200.0f, 200.0f),
                        release(MouseButton::Left, 200.0f, 200.0f), release(MouseButton::Right, 200.0f, 200.0f)});
        CHECK_FALSE(click.has_value());
        // Outside every image there is nothing to click.
        fixture.events({press(MouseButton::Left, 50.0f, 50.0f), release(MouseButton::Left, 50.0f, 50.0f)});
        CHECK_FALSE(click.has_value());
    }

    TEST_CASE("losing focus ends fly and releases its keys") {
        Fixture fixture;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), keyDown(Key::W, U'w')});
        REQUIRE(fixture.viewports.flying());
        fixture.events({Event{.type = EventType::WindowFocusLost}});
        CHECK_FALSE(fixture.viewports.holdsDrag());
        fixture.events({Event{.type = EventType::WindowFocusGained}});
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f)});
        REQUIRE(fixture.viewports.flying());
        const EditorCamera before = fixture.camera();
        CHECK_FALSE(fixture.viewports.update(0.5f));
        CHECK(fixture.camera() == before);
    }

#ifdef ITH_ENABLE_ASSERTS
    TEST_CASE("an invalid frame time is a programmer error and moves nothing") {
        Fixture fixture;
        fixture.events({press(MouseButton::Right, 200.0f, 200.0f), keyDown(Key::W, U'w')});
        const EditorCamera before = fixture.camera();
        const ith::test::AssertCapture capture;
        CHECK_FALSE(fixture.viewports.update(std::numeric_limits<float>::quiet_NaN()));
        CHECK(capture.count() == 1);
        CHECK(fixture.camera() == before);
    }
#endif
}
