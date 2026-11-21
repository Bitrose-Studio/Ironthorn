// The evaluation of values, modifiers, triggers, phases and events, with injected device events and
// explicit frame times (docs/specs/input-actions.md, requirements 5 to 23).

#include "ith/input_actions/InputActions.h"

#include "ActionTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ostream> // doctest prints std::string operands through it
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace ith;
using namespace ith::test;

namespace {

InputTrigger hold(float seconds, bool once = true) {
    return InputTrigger{.type = InputTriggerType::Hold, .hold = HoldParams{.seconds = seconds, .once = once}};
}

InputTrigger chord(AssetId action, TriggerKind kind = TriggerKind::Implicit) {
    return InputTrigger{
        .type = InputTriggerType::Chord, .kind = kind, .chord = ChordParams{.action = AssetRef::to(action)}};
}

InputModifier scale(float x) {
    return InputModifier{.type = InputModifierType::Scale, .scale = ScaleParams{.x = x}};
}

InputModifier curve(float x) {
    return InputModifier{.type = InputModifierType::Curve, .curve = CurveParams{.x = x}};
}

std::size_t count(const std::vector<ActionEvent>& events, ActionPhase phase) {
    return static_cast<std::size_t>(std::ranges::count(events, phase, &ActionEvent::phase));
}

} // namespace

TEST_SUITE("input_actions.values") {
    TEST_CASE("a digital key on an Axis2 action with Swizzle(YXZ) gives (0, 1), Negate after it (0, -1) "
              "(requirement 5)") {
        Rig rig;
        const AssetId forward = rig.addAction(ActionValueKind::Axis2);
        const AssetId back = rig.addAction(ActionValueKind::Axis2);
        const AssetId context =
            rig.addContext({mapping(forward, "Keyboard.W", {}, {modifier(InputModifierType::Swizzle)}),
                            mapping(back, "Keyboard.W", {},
                                    {modifier(InputModifierType::Swizzle), modifier(InputModifierType::Negate)})});
        const Entity player = rig.addPlayer({{context, 0}});
        rig.frame(10, {keyDown(Key::W, 5)});
        CHECK(rig.value(player, forward) == ActionValue{.x = 0.0f, .y = 1.0f});
        CHECK(rig.value(player, back) == ActionValue{.x = 0.0f, .y = -1.0f});
    }

    TEST_CASE("DeadZone remaps radially and axially (requirement 6)") {
        Rig rig;
        const AssetId radial = rig.addAction(ActionValueKind::Axis2);
        const AssetId axial = rig.addAction(ActionValueKind::Axis2);
        const AssetId context =
            rig.addContext({mapping(radial, "Gamepad.LeftStick", {}, {modifier(InputModifierType::DeadZone)}),
                            mapping(axial, "Gamepad.LeftStick", {},
                                    {InputModifier{.type = InputModifierType::DeadZone,
                                                   .deadZone = DeadZoneParams{.shape = DeadZoneShape::Axial}}})});
        const Entity player = rig.addPlayer({{context, 0}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, 0.1f, 2)});
        CHECK(rig.value(player, radial) == ActionValue{});
        CHECK(rig.state(player, radial) == TriggerState::None);

        rig.frame(20, {padAxis(kPad, GamepadAxis::LeftStickX, 0.6f, 12)});
        CHECK(rig.value(player, radial).x == doctest::Approx(0.5f));
        CHECK(rig.value(player, radial).y == 0.0f);

        rig.frame(30,
                  {padAxis(kPad, GamepadAxis::LeftStickX, 0.8f, 22), padAxis(kPad, GamepadAxis::LeftStickY, 0.8f, 23)});
        const ActionValue diagonal = rig.value(player, radial);
        CHECK(std::hypot(diagonal.x, diagonal.y) == doctest::Approx(1.0f));
        CHECK(diagonal.x == doctest::Approx(diagonal.y));

        rig.frame(40,
                  {padAxis(kPad, GamepadAxis::LeftStickX, 0.6f, 32), padAxis(kPad, GamepadAxis::LeftStickY, 0.1f, 33)});
        CHECK(rig.value(player, axial).x == doctest::Approx(0.5f));
        CHECK(rig.value(player, axial).y == 0.0f);
    }

    TEST_CASE("each modifier gives its documented result, in list order (requirement 7)") {
        Rig rig;
        rig.library.setSettings(
            {InputSettingDecl{.name = "look", .defaultFloat = 3.0f},
             InputSettingDecl{.name = "invert", .type = InputSettingType::Bool, .defaultBool = true}});
        const auto axis = [&rig] { return rig.addAction(ActionValueKind::Axis1); };
        const AssetId scaled = axis();
        const AssetId negated = axis();
        const AssetId curved = axis();
        const AssetId scaleThenCurve = axis();
        const AssetId curveThenScale = axis();
        const AssetId perSecond = axis();
        const AssetId user = axis();
        const AssetId unknownSetting = axis();
        const AssetId inverted = axis();
        const std::string stick = "Gamepad.LeftStickX";
        const AssetId context = rig.addContext({
            mapping(scaled, stick, {}, {scale(3.0f)}),
            mapping(negated, stick, {}, {modifier(InputModifierType::Negate)}),
            mapping(curved, stick, {}, {curve(2.0f)}),
            mapping(scaleThenCurve, stick, {}, {scale(2.0f), curve(2.0f)}),
            mapping(curveThenScale, stick, {}, {curve(2.0f), scale(2.0f)}),
            mapping(perSecond, stick, {}, {modifier(InputModifierType::FrameTimeScale)}),
            mapping(
                user, stick, {},
                {InputModifier{.type = InputModifierType::UserScale, .userScale = UserScaleParams{.setting = "look"}}}),
            mapping(
                unknownSetting, stick, {},
                {InputModifier{.type = InputModifierType::UserScale, .userScale = UserScaleParams{.setting = "none"}}}),
            mapping(inverted, stick, {},
                    {InputModifier{.type = InputModifierType::UserNegate,
                                   .userNegate = UserNegateParams{.setting = "invert", .x = true}}}),
        });
        const Entity player = rig.addPlayer({{context, 0}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, -0.5f, 2)});
        rig.frame(30); // 20 ms after the last frame
        CHECK(rig.value(player, scaled).x == doctest::Approx(-1.5f));
        CHECK(rig.value(player, negated).x == doctest::Approx(0.5f));
        CHECK(rig.value(player, curved).x == doctest::Approx(-0.25f));
        CHECK(rig.value(player, scaleThenCurve).x == doctest::Approx(-1.0f));
        CHECK(rig.value(player, curveThenScale).x == doctest::Approx(-0.5f));
        CHECK(rig.value(player, perSecond).x == doctest::Approx(-0.01f));
        CHECK(rig.value(player, user).x == doctest::Approx(-1.5f));
        CHECK(rig.value(player, unknownSetting).x == doctest::Approx(-0.5f));
        CHECK(rig.value(player, inverted).x == doctest::Approx(0.5f));

        // A profile's value replaces the declared default.
        REQUIRE(rig.bindings.setSetting("default", "look", 2.0f));
        REQUIRE(rig.bindings.setSetting("default", "invert", false));
        rig.frame(50);
        CHECK(rig.value(player, user).x == doctest::Approx(-1.0f));
        CHECK(rig.value(player, inverted).x == doctest::Approx(-0.5f));
    }

    TEST_CASE("action-level modifiers and triggers run after each mapping's own (requirement 8)") {
        Rig rig;
        const AssetId action = rig.addAction(InputActionAsset{.valueKind = ActionValueKind::Axis1,
                                                              .modifiers = {curve(2.0f)},
                                                              .triggers = {trigger(InputTriggerType::Pressed)}});
        const AssetId context = rig.addContext({mapping(action, "Gamepad.LeftStickX", {}, {scale(2.0f)})});
        const Entity player = rig.addPlayer({{context, 0}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, -0.5f, 2)});
        // Scale(2) then Curve(2): -1; the other order would give -0.5.
        CHECK(rig.value(player, action).x == doctest::Approx(-1.0f));
        CHECK(rig.state(player, action) == TriggerState::Triggered);
        rig.frame(20);
        CHECK(rig.state(player, action) == TriggerState::None); // Pressed fires once
    }

    TEST_CASE("Strongest cancels opposite keys and clamps on request; Sum adds (requirement 9)") {
        Rig rig;
        const auto moveMappings = [](AssetId action) {
            const InputModifier swizzle = modifier(InputModifierType::Swizzle);
            return std::vector<InputMapping>{
                mapping(action, "Keyboard.W", {}, {swizzle}),
                mapping(action, "Keyboard.S", {}, {swizzle, modifier(InputModifierType::Negate)}),
                mapping(action, "Keyboard.D")};
        };
        const AssetId move = rig.addAction(ActionValueKind::Axis2);
        const AssetId clamped =
            rig.addAction(InputActionAsset{.valueKind = ActionValueKind::Axis2, .clampToUnit = true});
        const AssetId look =
            rig.addAction(InputActionAsset{.valueKind = ActionValueKind::Axis2, .combine = ActionCombine::Sum});
        std::vector<InputMapping> mappings = moveMappings(move);
        for (InputMapping& entry : moveMappings(clamped)) {
            mappings.push_back(entry);
        }
        mappings.push_back(mapping(look, "Mouse.Delta"));
        mappings.push_back(mapping(look, "Mouse.Wheel"));
        const Entity player = rig.addPlayer({{rig.addContext(mappings), 0}});

        rig.frame(10, {keyDown(Key::W, 1), keyDown(Key::S, 2)});
        CHECK(rig.value(player, move) == ActionValue{});
        rig.frame(20, {keyUp(Key::S, 11), keyDown(Key::D, 12), mouseMove(3.0f, 4.0f, 13), mouseWheel(1.0f, 1.0f, 14)});
        CHECK(rig.value(player, move) == ActionValue{.x = 1.0f, .y = 1.0f});
        const ActionValue unit = rig.value(player, clamped);
        CHECK(std::hypot(unit.x, unit.y) == doctest::Approx(1.0f));
        CHECK(rig.value(player, look) == ActionValue{.x = 4.0f, .y = 5.0f});
    }

    TEST_CASE("mouse motion and wheel are the frame's sums, without a dead zone (requirement 10)") {
        Rig rig;
        const AssetId look = rig.addAction(ActionValueKind::Axis2);
        const AssetId wheel = rig.addAction(ActionValueKind::Axis1);
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(look, "Mouse.Delta"), mapping(wheel, "Mouse.WheelY")}), 0}});
        rig.frame(10, {mouseMove(2.0f, 0.0f, 1), mouseMove(3.0f, 0.01f, 2), mouseWheel(0.0f, 0.25f, 3),
                       mouseWheel(0.0f, 0.5f, 4)});
        CHECK(rig.value(player, look) == ActionValue{.x = 5.0f, .y = 0.01f});
        CHECK(rig.value(player, wheel).x == 0.75f);
        rig.frame(20);
        CHECK(rig.value(player, look) == ActionValue{});
        CHECK(rig.state(player, look) == TriggerState::None);
    }
}

TEST_SUITE("input_actions.triggers") {
    TEST_CASE("a mapping without triggers is Triggered exactly while its value is non-zero (requirement 11)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const Entity player = rig.addPlayer({{rig.addContext({mapping(jump, "Keyboard.Space")}), 0}});
        rig.frame(10);
        CHECK(rig.state(player, jump) == TriggerState::None);
        rig.frame(20, {keyDown(Key::Space, 15)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        CHECK(rig.value(player, jump).x == 1.0f);
        rig.frame(30);
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        rig.frame(40, {keyUp(Key::Space, 35)});
        CHECK(rig.state(player, jump) == TriggerState::None);
        CHECK(rig.value(player, jump).x == 0.0f);
    }

    TEST_CASE("Pressed fires once per press, Released once per release, Down every frame (requirement 12)") {
        Rig rig;
        const AssetId pressed = rig.addAction();
        const AssetId released = rig.addAction();
        const AssetId down = rig.addAction();
        static_cast<void>(
            rig.addPlayer({{rig.addContext({mapping(pressed, "Keyboard.E", {trigger(InputTriggerType::Pressed)}),
                                            mapping(released, "Keyboard.E", {trigger(InputTriggerType::Released)}),
                                            mapping(down, "Keyboard.E", {trigger(InputTriggerType::Down)})}),
                            0}}));
        std::size_t pressedCount = 0;
        std::size_t releasedCount = 0;
        std::size_t downCount = 0;
        const auto run = [&](double atMs, std::vector<Event> events) {
            rig.frame(atMs, std::move(events));
            pressedCount += count(rig.eventsOf(pressed), ActionPhase::Triggered);
            releasedCount += count(rig.eventsOf(released), ActionPhase::Triggered);
            downCount += count(rig.eventsOf(down), ActionPhase::Triggered);
        };
        run(10, {keyDown(Key::E, 5)});
        run(20, {});
        run(30, {});
        run(40, {keyUp(Key::E, 35)});
        run(50, {keyDown(Key::E, 45)});
        run(60, {keyUp(Key::E, 55)});
        CHECK(pressedCount == 2);
        CHECK(releasedCount == 2);
        CHECK(downCount == 4); // frames 10, 20, 30 and 50
    }

    TEST_CASE("Hold reaches its threshold at the exact instant; early release cancels (requirement 13)") {
        Rig rig;
        const AssetId once = rig.addAction();
        const AssetId repeat = rig.addAction();
        const Entity player = rig.addPlayer({{rig.addContext({mapping(once, "Keyboard.F", {hold(0.5f)}),
                                                              mapping(repeat, "Keyboard.F", {hold(0.5f, false)})}),
                                              0}});
        rig.frame(100, {keyDown(Key::F, 100)});
        CHECK(rig.phases(player, once) == (ActionPhase::Started | ActionPhase::Ongoing));
        int firstTriggered = -1;
        for (int k = 1; k <= 31; ++k) {
            rig.frame(100 + (k * 16.6));
            if (rig.state(player, once) == TriggerState::Triggered && firstTriggered < 0) {
                firstTriggered = k;
                CHECK(rig.phaseTime(player, once, ActionPhase::Triggered) == ms(600));
                CHECK(rig.phaseTime(player, repeat, ActionPhase::Triggered) == ms(600));
                CHECK(rig.states(player).find(once)->triggeredSinceNs == ms(600));
            }
        }
        CHECK(firstTriggered == 31); // 100 + 31 * 16.6 = 614.6 is the first frame at or after 600
        rig.frame(631.2);
        CHECK(rig.phases(player, once) == ActionPhase::Completed);
        CHECK(rig.state(player, repeat) == TriggerState::Triggered);
        rig.frame(647.8);
        CHECK(rig.state(player, repeat) == TriggerState::Triggered);

        rig.frame(700, {keyUp(Key::F, 690)});
        rig.frame(800, {keyDown(Key::F, 800)});
        rig.frame(1000, {keyUp(Key::F, 950)});
        CHECK(rig.phases(player, once) == ActionPhase::Canceled);
        CHECK(rig.phaseTime(player, once, ActionPhase::Canceled) == ms(950));
    }

    TEST_CASE("HoldRelease, Tap, MultiTap and Pulse follow section 3.6 at their boundaries (requirement 14)") {
        SUBCASE("HoldRelease: exactly the time counts") {
            Rig rig;
            const AssetId action = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext({mapping(action, "Keyboard.G",
                                          {InputTrigger{.type = InputTriggerType::HoldRelease,
                                                        .holdRelease = HoldReleaseParams{.seconds = 0.25f}}})}),
                  0}});
            rig.frame(100, {keyDown(Key::G, 100)});
            CHECK(rig.state(player, action) == TriggerState::Ongoing);
            rig.frame(360, {keyUp(Key::G, 350)});
            CHECK(rig.state(player, action) == TriggerState::Triggered);
            CHECK(rig.phaseTime(player, action, ActionPhase::Triggered) == ms(350));
            rig.frame(400, {keyDown(Key::G, 400)});
            rig.frame(700, {keyUp(Key::G, 649)});
            CHECK(rig.phases(player, action) == ActionPhase::Canceled);
        }
        SUBCASE("Tap: a release exactly at the limit triggers; a longer hold gives None") {
            Rig rig;
            const AssetId action = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext(
                      {mapping(action, "Keyboard.G",
                               {InputTrigger{.type = InputTriggerType::Tap, .tap = TapParams{.seconds = 0.25f}}})}),
                  0}});
            rig.frame(100, {keyDown(Key::G, 100)});
            CHECK(rig.state(player, action) == TriggerState::Ongoing);
            rig.frame(350);
            CHECK(rig.state(player, action) == TriggerState::Ongoing);
            rig.frame(360, {keyUp(Key::G, 350)});
            CHECK(rig.state(player, action) == TriggerState::Triggered);
            rig.frame(400, {keyDown(Key::G, 400)});
            rig.frame(651);
            CHECK(rig.phases(player, action) == ActionPhase::Canceled);
            CHECK(rig.phaseTime(player, action, ActionPhase::Canceled) == ms(650));
            rig.frame(700, {keyUp(Key::G, 690)});
            CHECK(rig.phases(player, action) == ActionPhase::None);
        }
        SUBCASE("MultiTap: two taps with a gap exactly at the limit trigger; a longer gap resets") {
            Rig rig;
            const AssetId action = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext({mapping(
                      action, "Keyboard.G",
                      {InputTrigger{.type = InputTriggerType::MultiTap,
                                    .multiTap =
                                        MultiTapParams{.count = 2, .tapSeconds = 0.25f, .gapSeconds = 0.25f}}})}),
                  0}});
            rig.frame(100, {keyDown(Key::G, 100)});
            CHECK(rig.phases(player, action) == (ActionPhase::Started | ActionPhase::Ongoing));
            rig.frame(150, {keyUp(Key::G, 150)});
            CHECK(rig.state(player, action) == TriggerState::Ongoing);
            rig.frame(400, {keyDown(Key::G, 400)});
            CHECK(rig.state(player, action) == TriggerState::Ongoing);
            rig.frame(450, {keyUp(Key::G, 450)});
            CHECK(rig.state(player, action) == TriggerState::Triggered);

            rig.frame(500);
            rig.frame(600, {keyDown(Key::G, 600)});
            rig.frame(650, {keyUp(Key::G, 650)});
            rig.frame(901);
            CHECK(rig.phases(player, action) == ActionPhase::Canceled);
            CHECK(rig.phaseTime(player, action, ActionPhase::Canceled) == ms(900));

            rig.frame(1000, {keyDown(Key::G, 1000)});
            rig.frame(1251);
            CHECK(rig.state(player, action) == TriggerState::None); // held longer than tapSeconds
        }
        SUBCASE("Pulse: at the start, then every interval, up to the limit") {
            Rig rig;
            const AssetId repeating = rig.addAction();
            const AssetId late = rig.addAction();
            const AssetId limited = rig.addAction();
            const auto pulse = [](bool atStart, std::uint32_t limit) {
                return InputTrigger{.type = InputTriggerType::Pulse,
                                    .pulse = PulseParams{.seconds = 0.25f, .atStart = atStart, .limit = limit}};
            };
            const Entity player = rig.addPlayer({{rig.addContext({mapping(repeating, "Keyboard.G", {pulse(true, 0)}),
                                                                  mapping(late, "Keyboard.G", {pulse(false, 0)}),
                                                                  mapping(limited, "Keyboard.G", {pulse(true, 2)})}),
                                                  0}});
            rig.frame(100, {keyDown(Key::G, 100)});
            CHECK(rig.state(player, repeating) == TriggerState::Triggered);
            CHECK(rig.state(player, late) == TriggerState::Ongoing);
            CHECK(rig.state(player, limited) == TriggerState::Triggered);
            rig.frame(200);
            CHECK(rig.phases(player, repeating) == ActionPhase::Ongoing); // Triggered -> Ongoing
            rig.frame(350);
            CHECK(rig.state(player, repeating) == TriggerState::Triggered);
            CHECK(rig.phaseTime(player, repeating, ActionPhase::Triggered) == ms(350));
            CHECK(rig.state(player, late) == TriggerState::Triggered);
            CHECK(rig.state(player, limited) == TriggerState::Triggered);
            rig.frame(600);
            CHECK(rig.state(player, repeating) == TriggerState::Triggered);
            CHECK(rig.phases(player, limited) == ActionPhase::Completed); // two pulses, then None
            rig.frame(900);
            CHECK(rig.state(player, limited) == TriggerState::None);
        }
    }

    TEST_CASE("a press and release inside one frame spread over two frames with real durations (requirement 15)") {
        Rig rig;
        const AssetId down = rig.addAction();
        const AssetId tap = rig.addAction();
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(down, "Keyboard.Q", {trigger(InputTriggerType::Down)}),
                                            mapping(tap, "Keyboard.Q", {trigger(InputTriggerType::Tap)})}),
                            0}});
        // A long frame: the hold is measured up to the queued release, not to the frame's time.
        rig.frame(400, {keyDown(Key::Q, 5), keyUp(Key::Q, 15)});
        CHECK(rig.phases(player, down) == (ActionPhase::Started | ActionPhase::Triggered));
        CHECK(rig.phaseTime(player, down, ActionPhase::Started) == ms(5));
        CHECK(rig.state(player, tap) == TriggerState::Ongoing);
        rig.frame(416);
        CHECK(rig.phases(player, down) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, down, ActionPhase::Completed) == ms(15));
        CHECK(rig.state(player, tap) == TriggerState::Triggered);
        CHECK(rig.phaseTime(player, tap, ActionPhase::Triggered) == ms(15));
    }

    TEST_CASE("presses inside one frame are spread over frames; a ninth queued edge drops the oldest pair "
              "(requirement 16)") {
        Rig rig;
        const AssetId pressed = rig.addAction();
        const Entity player = rig.addPlayer(
            {{rig.addContext({mapping(pressed, "Keyboard.R", {trigger(InputTriggerType::Pressed)})}), 0}});
        const auto presses = [&](int edges, double startMs) {
            std::vector<Event> events;
            for (int i = 0; i < edges; ++i) {
                events.push_back(i % 2 == 0 ? keyDown(Key::R, startMs + i) : keyUp(Key::R, startMs + i));
            }
            std::size_t fired = 0;
            rig.frame(startMs + 20, events);
            fired += count(rig.eventsOf(pressed), ActionPhase::Triggered);
            for (int f = 1; f < 12; ++f) {
                rig.frame(startMs + 20 + (f * 16));
                fired += count(rig.eventsOf(pressed), ActionPhase::Triggered);
            }
            return fired;
        };
        CHECK(presses(6, 100) == 3);
        static_cast<void>(rig.actions.takeMessages());
        CHECK(presses(9, 1000) == 4); // 5 presses, the oldest press and release dropped
        const std::vector<Error> messages = rig.actions.takeMessages();
#ifndef ITH_SHIPPING
        REQUIRE(messages.size() == 1);
        CHECK(messages[0].code == "input_actions.edge_queue_full");
#else
        CHECK(messages.empty());
#endif
        CHECK(rig.state(player, pressed) == TriggerState::None);
    }

    TEST_CASE("trigger kinds combine as section 3.6 rules 1 to 5 say (requirement 17)") {
        // One action per combination of an explicit Hold, an implicit chord on I and a chord blocker on
        // B, each on its own key so the chord rule doesn't interfere.
        struct Case {
            const char* name;
            Key key;
            bool explicitHold;
            bool implicitChord;
            bool blocker;
        };

        const std::vector<Case> cases{
            {"E", Key::Digit1, true, false, false}, {"I", Key::Digit2, false, true, false},
            {"B", Key::Digit3, false, false, true}, {"EI", Key::Digit4, true, true, false},
            {"EB", Key::Digit5, true, false, true}, {"IB", Key::Digit6, false, true, true},
            {"EIB", Key::Digit7, true, true, true},
        };

        // Expected [early, late] for (I held, B held).
        struct Run {
            bool implicitHeld;
            bool blockerHeld;
            std::vector<std::pair<TriggerState, TriggerState>> expected;
        };

        constexpr TriggerState kNone = TriggerState::None;
        constexpr TriggerState kOngoing = TriggerState::Ongoing;
        constexpr TriggerState kTriggered = TriggerState::Triggered;
        const std::vector<Run> runs{
            {true,
             false,
             {{kOngoing, kTriggered},
              {kTriggered, kTriggered},
              {kTriggered, kTriggered},
              {kOngoing, kTriggered},
              {kOngoing, kTriggered},
              {kTriggered, kTriggered},
              {kOngoing, kTriggered}}},
            {true,
             true,
             {{kOngoing, kTriggered},
              {kTriggered, kTriggered},
              {kNone, kNone},
              {kOngoing, kTriggered},
              {kNone, kNone},
              {kNone, kNone},
              {kNone, kNone}}},
            {false,
             false,
             {{kOngoing, kTriggered},
              {kNone, kNone},
              {kTriggered, kTriggered},
              {kOngoing, kOngoing},
              {kOngoing, kTriggered},
              {kNone, kNone},
              {kOngoing, kOngoing}}},
        };
        for (const Run& run : runs) {
            CAPTURE(run.implicitHeld);
            CAPTURE(run.blockerHeld);
            Rig rig;
            const AssetId implicitAction = rig.addAction();
            const AssetId blockerAction = rig.addAction();
            std::vector<InputMapping> mappings{mapping(implicitAction, "Keyboard.I"),
                                               mapping(blockerAction, "Keyboard.B")};
            std::vector<AssetId> ids;
            std::vector<Event> presses;
            for (const Case& entry : cases) {
                std::vector<InputTrigger> triggers;
                if (entry.explicitHold) {
                    triggers.push_back(hold(0.25f));
                }
                if (entry.implicitChord) {
                    triggers.push_back(chord(implicitAction));
                }
                if (entry.blocker) {
                    triggers.push_back(chord(blockerAction, TriggerKind::Blocker));
                }
                ids.push_back(rig.addAction());
                mappings.push_back(mapping(ids.back(), std::string(controlName(entry.key)), triggers));
                presses.push_back(keyDown(entry.key, 100));
            }
            if (run.implicitHeld) {
                presses.push_back(keyDown(Key::I, 100));
            }
            if (run.blockerHeld) {
                presses.push_back(keyDown(Key::B, 100));
            }
            const Entity player = rig.addPlayer({{rig.addContext(mappings), 0}});
            rig.frame(100, presses);
            rig.frame(200);
            std::vector<TriggerState> early;
            for (const AssetId id : ids) {
                early.push_back(rig.state(player, id));
            }
            rig.frame(400);
            for (std::size_t i = 0; i < cases.size(); ++i) {
                CAPTURE(cases[i].name);
                CHECK(early[i] == run.expected[i].first);
                CHECK(rig.state(player, ids[i]) == run.expected[i].second);
            }
        }

        // An implicit chord whose action is Triggered while the mapping's own key is not actuated:
        // Ongoing, never Triggered.
        Rig rig;
        const AssetId modifierAction = rig.addAction();
        const AssetId chorded = rig.addAction();
        const Entity player = rig.addPlayer({{rig.addContext({mapping(modifierAction, "Keyboard.I"),
                                                              mapping(chorded, "Keyboard.K", {chord(modifierAction)})}),
                                              0}});
        rig.frame(100, {keyDown(Key::I, 100)});
        CHECK(rig.state(player, chorded) == TriggerState::Ongoing);
    }

    TEST_CASE("a chord on a Ctrl action: Ctrl + S saves and doesn't go back; S alone goes back (requirement 18)") {
        Rig rig;
        const AssetId ctrl = rig.addAction();
        const AssetId save = rig.addAction();
        const AssetId back = rig.addAction();
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(back, "Keyboard.S"), mapping(save, "Keyboard.S", {chord(ctrl)}),
                                            mapping(ctrl, "Keyboard.LeftControl")}),
                            0}});
        rig.frame(10, {keyDown(Key::LeftControl, 1), keyDown(Key::S, 2)});
        CHECK(rig.state(player, save) == TriggerState::Triggered);
        CHECK(rig.state(player, back) == TriggerState::None);
        rig.frame(20, {keyUp(Key::S, 11), keyUp(Key::LeftControl, 12)});
        rig.frame(30, {keyDown(Key::S, 21)});
        CHECK(rig.state(player, back) == TriggerState::Triggered);
        CHECK(rig.state(player, save) == TriggerState::None);

        // Save in a higher context than the Ctrl action still sees Ctrl in the same frame.
        Rig split;
        const AssetId ctrl2 = split.addAction();
        const AssetId save2 = split.addAction();
        const AssetId high = split.addContext({mapping(save2, "Keyboard.S", {chord(ctrl2)})});
        const AssetId low = split.addContext({mapping(ctrl2, "Keyboard.LeftControl")});
        const Entity other = split.addPlayer({{low, 0}, {high, 10}});
        split.frame(10, {keyDown(Key::LeftControl, 1), keyDown(Key::S, 2)});
        CHECK(split.state(other, save2) == TriggerState::Triggered);
    }

    TEST_CASE("a combo of three steps triggers in order and resets when late, canceled or out of order "
              "(requirement 19)") {
        Rig rig;
        const AssetId a = rig.addAction();
        const AssetId b = rig.addAction();
        const AssetId c = rig.addAction();
        const AssetId cancel = rig.addAction();
        const AssetId combo = rig.addAction();
        const InputTrigger comboTrigger{
            .type = InputTriggerType::Combo,
            .combo = ComboParams{
                .steps = {{.action = AssetRef::to(a)}, {.action = AssetRef::to(b)}, {.action = AssetRef::to(c)}},
                .cancelActions = {AssetRef::to(cancel)}}};
        const InputTrigger pressed = trigger(InputTriggerType::Pressed);
        const Entity player = rig.addPlayer(
            {{rig.addContext({mapping(a, "Keyboard.A", {pressed}), mapping(b, "Keyboard.B", {pressed}),
                              mapping(c, "Keyboard.C", {pressed}), mapping(cancel, "Keyboard.Z", {pressed}),
                              mapping(combo, "Keyboard.C", {comboTrigger})}),
              0}});
        const auto tap = [&rig](Key key, double atMs) {
            rig.frame(atMs, {keyDown(key, atMs)});
            rig.frame(atMs + 10, {keyUp(key, atMs + 10)});
        };
        rig.frame(100, {keyDown(Key::A, 100)});
        CHECK(rig.phases(player, combo) == (ActionPhase::Started | ActionPhase::Ongoing));
        rig.frame(110, {keyUp(Key::A, 110)});
        tap(Key::B, 300);
        CHECK(rig.state(player, combo) == TriggerState::Ongoing);
        rig.frame(500, {keyDown(Key::C, 500)});
        CHECK(rig.state(player, combo) == TriggerState::Triggered);
        CHECK(rig.phaseTime(player, combo, ActionPhase::Triggered) == ms(500));
        rig.frame(510, {keyUp(Key::C, 510)});
        CHECK(rig.phases(player, combo) == ActionPhase::Completed);

        // Late: B more than 0.5 s after A.
        tap(Key::A, 1000);
        rig.frame(1600, {keyDown(Key::B, 1600)});
        CHECK(rig.phases(player, combo) == ActionPhase::Canceled);
        CHECK(rig.phaseTime(player, combo, ActionPhase::Canceled) == ms(1500));
        rig.frame(1610, {keyUp(Key::B, 1610)});

        // Canceled by the cancel action.
        tap(Key::A, 2000);
        tap(Key::Z, 2100);
        CHECK(rig.state(player, combo) == TriggerState::None);
        tap(Key::B, 2200);
        tap(Key::C, 2300);
        CHECK(rig.state(player, combo) == TriggerState::None);

        // Out of order: C right after A.
        tap(Key::A, 3000);
        rig.frame(3100, {keyDown(Key::C, 3100)});
        CHECK(rig.phases(player, combo) == ActionPhase::Canceled);
    }
}

TEST_SUITE("input_actions.phases") {
    TEST_CASE("every row of the phase table (requirement 20) with its timestamps (requirement 21)") {
        Rig rig;
        const AssetId holding = rig.addAction();
        const AssetId down = rig.addAction();
        const AssetId pulse = rig.addAction();
        const Entity player = rig.addPlayer(
            {{rig.addContext(
                  {mapping(holding, "Keyboard.H", {hold(0.25f)}), mapping(down, "Keyboard.J"),
                   mapping(pulse, "Keyboard.K",
                           {InputTrigger{.type = InputTriggerType::Pulse, .pulse = PulseParams{.seconds = 1.0f}}})}),
              0}});
        rig.frame(10);
        CHECK(rig.phases(player, holding) == ActionPhase::None); // None -> None
        rig.frame(100, {keyDown(Key::H, 95), keyDown(Key::J, 96), keyDown(Key::K, 97)});
        CHECK(rig.phases(player, holding) == (ActionPhase::Started | ActionPhase::Ongoing)); // None -> Ongoing
        CHECK(rig.phaseTime(player, holding, ActionPhase::Started) == ms(95));               // a device edge
        CHECK(rig.states(player).find(holding)->startedAtNs == ms(95));
        CHECK(rig.phases(player, down) == (ActionPhase::Started | ActionPhase::Triggered)); // None -> Triggered
        CHECK(rig.phaseTime(player, down, ActionPhase::Triggered) == ms(96));
        rig.frame(200);
        CHECK(rig.phases(player, holding) == ActionPhase::Ongoing); // Ongoing -> Ongoing
        CHECK(rig.phases(player, down) == ActionPhase::Triggered);  // Triggered -> Triggered
        CHECK(rig.phases(player, pulse) == ActionPhase::Ongoing);   // Triggered -> Ongoing
        rig.frame(400);
        CHECK(rig.phases(player, holding) == ActionPhase::Triggered);             // Ongoing -> Triggered
        CHECK(rig.phaseTime(player, holding, ActionPhase::Triggered) == ms(345)); // the computed instant
        rig.frame(500, {keyUp(Key::J, 450)});
        CHECK(rig.phases(player, holding) == ActionPhase::Completed); // Triggered -> None (once)
        CHECK(rig.phases(player, down) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, down, ActionPhase::Completed) == ms(450));
        rig.frame(600, {keyUp(Key::H, 550)});
        rig.frame(620, {keyDown(Key::H, 590)});
        rig.frame(700, {keyUp(Key::H, 650)});
        CHECK(rig.phases(player, holding) == ActionPhase::Canceled); // Ongoing -> None
        CHECK(rig.phaseTime(player, holding, ActionPhase::Canceled) == ms(650));
        CHECK_FALSE(rig.states(player).find(holding)->triggeredSinceNs.has_value());
    }

    TEST_CASE("a phase with no device edge or threshold carries the frame's time (requirement 21)") {
        Rig rig;
        const AssetId move = rig.addAction();
        const AssetId context = rig.addContext({mapping(move, "Keyboard.W")});
        const Entity player = rig.addPlayer({{context, 0}});
        rig.frame(100, {keyDown(Key::W, 90)});
        rig.removeContext(player, context);
        rig.frame(200);
        CHECK(rig.phases(player, move) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, move, ActionPhase::Completed) == ms(200));
        rig.frame(300);
        CHECK(rig.states(player).find(move) == nullptr); // gone from the table
    }

    TEST_CASE("events come by player index, then table order, then phase order, once each (requirement 22)") {
        Rig rig;
        const AssetId first = rig.addAction();
        const AssetId second = rig.addAction();
        const AssetId context = rig.addContext({mapping(first, "Keyboard.W"), mapping(second, "Gamepad.FaceSouth")});
        rig.input.setLocalMultiplayer(true);
        rig.frame(5, {padConnected(kPad, 1)});
        rig.input.assignDevice(kKeyboardMouse, 1);
        rig.input.assignDevice(kPad, 0);
        const Entity playerTwo = rig.addPlayer({{context, 0}}, 1); // spawned first, index 1
        const Entity playerOne = rig.addPlayer({{context, 0}}, 0);
        rig.frame(10, {keyDown(Key::W, 6), padDown(kPad, GamepadButton::FaceSouth, 7)});
        const std::span<const ActionEvent> events = rig.actions.events();
        REQUIRE(events.size() == 4);
        CHECK(events[0].player == playerOne);
        CHECK(events[0].action == second);
        CHECK(events[0].phase == ActionPhase::Started);
        CHECK(events[1].phase == ActionPhase::Triggered);
        CHECK(events[2].player == playerTwo);
        CHECK(events[2].action == first);
        CHECK(events[2].phase == ActionPhase::Started);
        CHECK(events[3].phase == ActionPhase::Triggered);
        CHECK(events[3].timestampNs == ms(6));
        CHECK(events[3].value.x == 1.0f);

        rig.frame(20);
        CHECK(rig.actions.events().size() == 2); // Triggered again for each; the old events are gone
        rig.frame(30, {keyUp(Key::W, 25)});
        REQUIRE(rig.actions.events().size() == 2);
        CHECK(rig.actions.events()[1].phase == ActionPhase::Completed);
    }

    TEST_CASE("the same device events and frame times give identical states and events (requirement 23)") {
        struct Recorded {
            std::vector<std::tuple<AssetId, std::uint64_t, ActionPhase, float, float>> events;
            std::vector<ActionState> finalStates;
        };

        const auto run = [] {
            Rig rig;
            const auto fixed = [](std::uint64_t n) { return AssetId(Uuid(0x1234, n)); };
            rig.library.setAction(fixed(1), InputActionAsset{.valueKind = ActionValueKind::Axis2});
            rig.library.setAction(fixed(2), InputActionAsset{});
            rig.library.setContext(fixed(10), InputContextAsset{.mappings = {
                                                                    mapping(fixed(1), "Gamepad.LeftStick", {},
                                                                            {modifier(InputModifierType::DeadZone)}),
                                                                    mapping(fixed(1), "Mouse.Delta"),
                                                                    mapping(fixed(2), "Keyboard.Space", {hold(0.1f)}),
                                                                }});
            const Entity player = rig.addPlayer({{fixed(10), 0}});
            Recorded recorded;
            for (int frame = 0; frame < 60; ++frame) {
                const double at = 10 + (frame * 16.6);
                std::vector<Event> events;
                if (frame == 0) {
                    events.push_back(padConnected(kPad, at - 5));
                }
                events.push_back(padAxis(kPad, GamepadAxis::LeftStickX, std::sin(frame * 0.3f), at - 4));
                events.push_back(mouseMove(static_cast<float>(frame % 7), -1.5f, at - 3));
                if (frame % 9 == 0) {
                    events.push_back(keyDown(Key::Space, at - 2));
                }
                if (frame % 9 == 4) {
                    events.push_back(keyUp(Key::Space, at - 1));
                }
                rig.frame(at, events);
                for (const ActionEvent& event : rig.actions.events()) {
                    recorded.events.emplace_back(event.action, event.timestampNs, event.phase, event.value.x,
                                                 event.value.y);
                }
            }
            recorded.finalStates = rig.states(player).actions;
            return recorded;
        };
        const Recorded one = run();
        const Recorded two = run();
        CHECK(one.events.size() > 100);
        CHECK(one.events == two.events);
        REQUIRE(one.finalStates.size() == two.finalStates.size());
        for (std::size_t i = 0; i < one.finalStates.size(); ++i) {
            CHECK(one.finalStates[i].value == two.finalStates[i].value);
            CHECK(one.finalStates[i].state == two.finalStates[i].state);
            CHECK(one.finalStates[i].phaseTimesNs == two.finalStates[i].phaseTimesNs);
        }
    }
}
