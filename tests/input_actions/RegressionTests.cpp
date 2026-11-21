// Regression tests for the cases found in review (probes P1 to P11) and the rules added with them:
// forced ends, analog claims ending on rest, overlapping controls, axis values of unassigned
// gamepads, combo steps on the first Triggered frame, bounded edge-queue warnings, checked binding
// layers and the added validation (docs/specs/input-actions.md 3.6, 3.7, 3.9, 3.10, 3.11, 3.13).

#include "ith/input_actions/InputActions.h"

#include "ActionTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream> // doctest prints std::string operands through it
#include <string>
#include <vector>

using namespace ith;
using namespace ith::test;

namespace {

InputTrigger pressedTrigger() {
    return InputTrigger{.type = InputTriggerType::Pressed};
}

std::size_t countCode(const std::vector<InputAssetProblem>& problems, const std::string& code) {
    return static_cast<std::size_t>(
        std::ranges::count_if(problems, [&code](const InputAssetProblem& problem) { return problem.code == code; }));
}

} // namespace

TEST_SUITE("input_actions.regressions") {
    TEST_CASE("P1: a combo step on an action held with Down counts in its first Triggered frame only") {
        Rig rig;
        const AssetId a = rig.addAction();
        const AssetId b = rig.addAction();
        const AssetId c = rig.addAction();
        const AssetId combo = rig.addAction();
        const InputTrigger comboTrigger{
            .type = InputTriggerType::Combo,
            .combo = ComboParams{
                .steps = {{.action = AssetRef::to(a)}, {.action = AssetRef::to(b)}, {.action = AssetRef::to(c)}}}};
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(a, "Keyboard.A", {pressedTrigger()}),
                                            mapping(b, "Keyboard.B", {trigger(InputTriggerType::Down)}),
                                            mapping(c, "Keyboard.C", {pressedTrigger()}),
                                            mapping(combo, "Keyboard.C", {comboTrigger})}),
                            0}});
        rig.frame(100, {keyDown(Key::A, 100)});
        rig.frame(110, {keyUp(Key::A, 110)});
        rig.frame(200, {keyDown(Key::B, 200)});
        CHECK(rig.state(player, combo) == TriggerState::Ongoing);
        rig.frame(216); // B still Triggered: neither a step again nor out of order
        CHECK(rig.state(player, combo) == TriggerState::Ongoing);
        rig.frame(232, {keyUp(Key::B, 225)});
        rig.frame(300, {keyDown(Key::C, 300)});
        CHECK(rig.state(player, combo) == TriggerState::Triggered);
    }

    TEST_CASE("P2: a setting beyond the range of a float is refused before the cast") {
        const auto read = readInputBindings(R"({"format": "ith.input_bindings", "version": 1, "data": )"
                                            R"({"profiles": {"default": {"settings": {"look": 1e300}}}}})");
        REQUIRE_FALSE(read.hasValue());
        CHECK(read.error().code == "serialization.out_of_range");
        CHECK(read.error().path == "$.profiles.default.settings.look");
    }

    TEST_CASE("P3: an analog claim ends when the key rests below the claiming mapping's threshold") {
        Rig rig;
        const AssetId high = rig.addAction(ActionValueKind::Axis1);
        const AssetId low = rig.addAction(ActionValueKind::Axis1);
        const AssetId hi = rig.addContext({mapping(high, "Gamepad.LeftStickX", {pressedTrigger()})});
        const AssetId lo = rig.addContext({mapping(low, "Gamepad.LeftStickX")});
        const Entity player = rig.addPlayer({{lo, 0}, {hi, 10}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, 1.0f, 2)});
        CHECK(rig.state(player, low) == TriggerState::None);
        rig.frame(20, {padAxis(kPad, GamepadAxis::LeftStickX, 0.01f, 12)}); // drift, never exactly 0
        rig.frame(30, {padAxis(kPad, GamepadAxis::LeftStickX, 0.3f, 22)});
        CHECK(rig.value(player, low).x == doctest::Approx(0.3f));
    }

    TEST_CASE("analog claims end below the lowest threshold of the claiming triggers, or 0.2 without triggers") {
        Rig rig;
        const AssetId several = rig.addAction(ActionValueKind::Axis1);
        const AssetId plain = rig.addAction(ActionValueKind::Axis1);
        const AssetId lowLeft = rig.addAction(ActionValueKind::Axis1);
        const AssetId lowRight = rig.addAction(ActionValueKind::Axis1);
        const AssetId hi = rig.addContext({mapping(several, "Gamepad.LeftTrigger",
                                                   {InputTrigger{.type = InputTriggerType::Down, .threshold = 0.6f},
                                                    InputTrigger{.type = InputTriggerType::Down, .threshold = 0.4f}}),
                                           mapping(plain, "Gamepad.RightTrigger")});
        const AssetId lo =
            rig.addContext({mapping(lowLeft, "Gamepad.LeftTrigger"), mapping(lowRight, "Gamepad.RightTrigger")});
        const Entity player = rig.addPlayer({{lo, 0}, {hi, 10}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftTrigger, 1.0f, 2),
                       padAxis(kPad, GamepadAxis::RightTrigger, 1.0f, 3)});
        rig.frame(20, {padAxis(kPad, GamepadAxis::LeftTrigger, 0.45f, 12),
                       padAxis(kPad, GamepadAxis::RightTrigger, 0.25f, 13)});
        rig.frame(30);
        CHECK(rig.state(player, lowLeft) == TriggerState::None);  // 0.45 is not below 0.4: still claimed
        CHECK(rig.state(player, lowRight) == TriggerState::None); // 0.25 is not below 0.2: still claimed
        rig.frame(40, {padAxis(kPad, GamepadAxis::LeftTrigger, 0.35f, 32),
                       padAxis(kPad, GamepadAxis::RightTrigger, 0.15f, 33)});
        rig.frame(50);
        CHECK(rig.value(player, lowLeft).x == doctest::Approx(0.35f));
        CHECK(rig.value(player, lowRight).x == doctest::Approx(0.15f));
    }

    TEST_CASE("P4: a claim on a 2D control hides its 1D axes, and the other way round") {
        Rig rig;
        const AssetId drive = rig.addAction(ActionValueKind::Axis2);
        const AssetId walk = rig.addAction(ActionValueKind::Axis1);
        const AssetId aim = rig.addAction(ActionValueKind::Axis1);
        const AssetId look = rig.addAction(ActionValueKind::Axis2);
        const AssetId hi = rig.addContext({mapping(drive, "Gamepad.LeftStick"), mapping(aim, "Mouse.DeltaY")});
        const AssetId lo = rig.addContext({mapping(walk, "Gamepad.LeftStickX"), mapping(look, "Mouse.Delta")});
        const Entity player = rig.addPlayer({{lo, 0}, {hi, 10}});
        rig.frame(10,
                  {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, 1.0f, 2), mouseMove(0.0f, 5.0f, 3)});
        CHECK(rig.state(player, drive) == TriggerState::Triggered);
        CHECK(rig.state(player, walk) == TriggerState::None);
        CHECK(rig.state(player, aim) == TriggerState::Triggered);
        CHECK(rig.state(player, look) == TriggerState::None);
    }

    TEST_CASE("P5: a binding layer can't bind a key rebind() would refuse") {
        Rig rig;
        const AssetId jump = rig.addAction();
        InputMapping slotted = mapping(jump, "Keyboard.Space");
        slotted.slot = "jump.keyboard";
        rig.addContext({slotted});
        const auto read = readInputBindings(R"({"format": "ith.input_bindings", "version": 1, "data": {"profiles": )"
                                            R"({"default": {"slots": {"jump.keyboard": "Gamepad.LeftStick"}}}}})");
        REQUIRE(read.hasValue());
        std::vector<serialization::Error> warnings;
        rig.bindings.setLayer(InputBindingLayer::User, *read, "user", &warnings);
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].code == "input_actions.wrong_device_family");
        CHECK(warnings[0].source == "user");
        CHECK(rig.bindings.key("default", "jump.keyboard") == Control{Key::Space});
    }

    TEST_CASE("S7: binding layers clamp settings and sort what code built") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, {});
        InputMapping a = mapping(jump, "Keyboard.Space");
        a.slot = "b.slot";
        InputMapping b = mapping(jump, "Keyboard.J");
        b.slot = "a.slot";
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {a, b}});
        library.setSettings({InputSettingDecl{.name = "look", .min = 0.5f, .max = 2.0f}});
        InputBindingConfig config(library);
        config.setLayer(
            InputBindingLayer::Project,
            InputBindings{.profiles = {{.name = "z", .slots = {{.slot = "b.slot", .key = Key::F}}},
                                       {.name = "default",
                                        .slots = {{.slot = "b.slot", .key = Key::G}, {.slot = "a.slot", .key = Key::H}},
                                        .settings = {{.name = "look", .value = 9.0f}}}}});
        CHECK(config.key("default", "a.slot") == Control{Key::H});
        CHECK(config.key("default", "b.slot") == Control{Key::G});
        CHECK(config.key("z", "b.slot") == Control{Key::F});
        CHECK(std::get<float>(*config.setting("default", "look")) == 2.0f);
        CHECK(config.layer(InputBindingLayer::Project).profiles[0].name == "default");
    }

    TEST_CASE("P6: Ctrl alone gives the chorded action Started, Ongoing, then Canceled; it never triggers") {
        Rig rig;
        const AssetId ctrl = rig.addAction();
        const AssetId save = rig.addAction();
        const Entity player = rig.addPlayer(
            {{rig.addContext({mapping(ctrl, "Keyboard.LeftControl"),
                              mapping(save, "Keyboard.S",
                                      {InputTrigger{.type = InputTriggerType::Chord,
                                                    .chord = ChordParams{.action = AssetRef::to(ctrl)}}})}),
              0}});
        rig.frame(10, {keyDown(Key::LeftControl, 5)});
        CHECK(rig.phases(player, save) == (ActionPhase::Started | ActionPhase::Ongoing));
        rig.frame(20, {keyUp(Key::LeftControl, 15)});
        CHECK(rig.phases(player, save) == ActionPhase::Canceled);
    }

    TEST_CASE("P7: a pulse shorter than a frame reports its last crossing, not the first missed one") {
        Rig rig;
        const AssetId fire = rig.addAction();
        const Entity player = rig.addPlayer(
            {{rig.addContext(
                  {mapping(fire, "Keyboard.F",
                           {InputTrigger{.type = InputTriggerType::Pulse, .pulse = PulseParams{.seconds = 0.001f}}})}),
              0}});
        rig.frame(10, {keyDown(Key::F, 10)});
        for (int i = 1; i <= 60; ++i) {
            rig.frame(10 + (i * 16.0));
            CHECK(rig.state(player, fire) == TriggerState::Triggered);
        }
        CHECK(rig.phaseTime(player, fire, ActionPhase::Triggered) == ms(970));

        Rig limited;
        const AssetId burst = limited.addAction();
        const Entity other = limited.addPlayer(
            {{limited.addContext({mapping(burst, "Keyboard.F",
                                          {InputTrigger{.type = InputTriggerType::Pulse,
                                                        .pulse = PulseParams{.seconds = 0.001f, .limit = 5}}})}),
              0}});
        limited.frame(10, {keyDown(Key::F, 10)});
        limited.frame(26); // the remaining four pulses at once: the last at 14 ms
        CHECK(limited.phaseTime(other, burst, ActionPhase::Triggered) == ms(14));
        limited.frame(42);
        CHECK(limited.phases(other, burst) == ActionPhase::Completed);
    }

    TEST_CASE("P8: an overflowing edge queue is reported once per player and key") {
        Rig rig;
        const AssetId jump = rig.addAction();
        rig.addPlayer({{rig.addContext({mapping(jump, "Keyboard.Space")}), 0}});
        for (int f = 0; f < 50; ++f) {
            std::vector<Event> events;
            const double t = 100 + (f * 16.0);
            for (int i = 0; i < 6; ++i) {
                events.push_back(keyDown(Key::Space, t + i));
                events.push_back(keyUp(Key::Space, t + i + 0.5));
            }
            rig.frame(t + 10, events);
        }
        const std::vector<Error> messages = rig.actions.takeMessages();
#ifndef ITH_SHIPPING
        REQUIRE(messages.size() == 1);
        CHECK(messages[0].code == "input_actions.edge_queue_full");
#else
        CHECK(messages.empty());
#endif
    }

    TEST_CASE("P9: a stick released while its gamepad was unassigned reads 0 when it is assigned again") {
        Rig rig;
        const AssetId move = rig.addAction(ActionValueKind::Axis1);
        const Entity player = rig.addPlayer({{rig.addContext({mapping(move, "Gamepad.LeftStickX")}), 0}});
        rig.frame(10, {padConnected(kPad, 1), padAxis(kPad, GamepadAxis::LeftStickX, 0.8f, 2)});
        CHECK(rig.value(player, move).x == doctest::Approx(0.8f));
        rig.input.assignDevice(kPad, kNoPlayer);
        rig.frame(20, {padAxis(kPad, GamepadAxis::LeftStickX, 0.0f, 15)});
        CHECK(rig.value(player, move).x == 0.0f);
        rig.frame(30);
        rig.input.assignDevice(kPad, 0);
        rig.frame(40);
        CHECK(rig.value(player, move).x == 0.0f);
        CHECK(rig.state(player, move) == TriggerState::None);
    }

    TEST_CASE("P10: a frame time earlier than the last frame's events doesn't wrap timing") {
        Rig rig;
        const AssetId tap = rig.addAction();
        const Entity player = rig.addPlayer(
            {{rig.addContext({mapping(tap, "Keyboard.H", {InputTrigger{.type = InputTriggerType::MultiTap}})}), 0}});
        rig.frame(10, {keyDown(Key::H, 50)});
        rig.frame(20, {keyUp(Key::H, 60)});
        rig.frame(30); // earlier than the release at 60 ms
        CHECK(rig.state(player, tap) == TriggerState::Ongoing);
        rig.frame(70, {keyDown(Key::H, 70)});
        rig.frame(80, {keyUp(Key::H, 80)});
        CHECK(rig.state(player, tap) == TriggerState::Triggered);
    }

    TEST_CASE("P11: forced ends never fire Released, HoldRelease or Tap") {
        SUBCASE("suspension") {
            Rig rig;
            const AssetId fire = rig.addAction();
            const AssetId charge = rig.addAction();
            const AssetId tap = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext({mapping(fire, "Keyboard.F", {trigger(InputTriggerType::Released)}),
                                  mapping(charge, "Keyboard.G",
                                          {InputTrigger{.type = InputTriggerType::HoldRelease,
                                                        .holdRelease = HoldReleaseParams{.seconds = 0.1f}}}),
                                  mapping(tap, "Keyboard.T", {trigger(InputTriggerType::Tap)})}),
                  0}});
            rig.frame(10, {keyDown(Key::F, 5), keyDown(Key::G, 5)});
            rig.frame(300, {keyDown(Key::T, 295)});
            rig.world.get<ActionStates>(player)->suspended = true;
            rig.frame(316);
            CHECK(rig.phases(player, fire) == ActionPhase::Canceled);
            CHECK(rig.phases(player, charge) == ActionPhase::Canceled);
            CHECK(rig.phases(player, tap) == ActionPhase::Canceled);
        }
        SUBCASE("focus loss") {
            Rig rig;
            const AssetId fire = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext({mapping(fire, "Keyboard.F", {trigger(InputTriggerType::Released)})}), 0}});
            rig.frame(10, {keyDown(Key::F, 5)});
            rig.frame(20, {focusEvent(EventType::WindowFocusLost, 15)});
            CHECK(rig.phases(player, fire) == ActionPhase::Canceled);
            CHECK(rig.phaseTime(player, fire, ActionPhase::Canceled) == ms(15));
        }
        SUBCASE("capture loss") {
            Rig rig;
            const AssetId fire = rig.addAction();
            const Entity player = rig.addPlayer(
                {{rig.addContext({mapping(fire, "Mouse.Left", {trigger(InputTriggerType::Released)})}), 0}});
            REQUIRE(rig.input.captureMouse(kWindow));
            rig.frame(10, {mouseDown(MouseButton::Left, 5)});
            rig.input.releaseMouse();
            rig.frame(20);
            CHECK(rig.phases(player, fire) == ActionPhase::Canceled);
        }
        SUBCASE("a claim from a higher context and the chord rule") {
            Rig rig;
            const AssetId brake = rig.addAction();
            // Without consumesKeys, so fire's own claim doesn't keep F from the higher context.
            const AssetId fire = rig.addAction(InputActionAsset{.consumesKeys = false});
            const AssetId ctrl = rig.addAction();
            const AssetId save = rig.addAction();
            const AssetId back = rig.addAction();
            const AssetId high = rig.addContext({mapping(brake, "Keyboard.F")});
            const AssetId low =
                rig.addContext({mapping(fire, "Keyboard.F", {trigger(InputTriggerType::Released)}),
                                mapping(ctrl, "Keyboard.LeftControl"),
                                mapping(save, "Keyboard.S",
                                        {InputTrigger{.type = InputTriggerType::Chord,
                                                      .chord = ChordParams{.action = AssetRef::to(ctrl)}}}),
                                mapping(back, "Keyboard.S", {trigger(InputTriggerType::Released)})});
            const Entity player = rig.addPlayer({{low, 0}});
            rig.frame(10, {keyDown(Key::F, 5), keyDown(Key::S, 6)});
            CHECK(rig.state(player, fire) == TriggerState::Ongoing);
            CHECK(rig.state(player, back) == TriggerState::Ongoing);
            rig.pushContext(player, high, 10); // added while F is held: Down fires at once and claims F
            rig.frame(20, {keyDown(Key::LeftControl, 15)});
            CHECK(rig.state(player, brake) == TriggerState::Triggered);
            CHECK(rig.phases(player, fire) == ActionPhase::Canceled); // the claim hides F
            CHECK(rig.phases(player, back) == ActionPhase::Canceled); // the chord rule hides S
            rig.frame(30, {keyUp(Key::F, 25), keyUp(Key::S, 26)});
            CHECK(rig.phases(player, fire) == ActionPhase::None);
            CHECK(rig.phases(player, back) == ActionPhase::None);
        }
    }

    TEST_CASE("duplicate player indices are reported once; only the first entity is evaluated") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId context = rig.addContext({mapping(jump, "Keyboard.Space")});
        rig.addPlayer({{context, 0}}, 0);
        rig.addPlayer({{context, 0}}, 0);
        rig.frame(10, {keyDown(Key::Space, 5)});
        rig.frame(20);
        const std::vector<Error> messages = rig.actions.takeMessages();
        REQUIRE(messages.size() == 1);
        CHECK(messages[0].code == "input_actions.duplicate_player");
        CHECK(rig.actions.events().size() == 1); // one player's Triggered
    }

    TEST_CASE("phaseIndex gives no array index for None or a set of phases") {
        CHECK(phaseIndex(ActionPhase::Started) == 0);
        CHECK(phaseIndex(ActionPhase::Canceled) == 4);
        CHECK(phaseIndex(ActionPhase::None) == kActionPhaseCount);
        CHECK(phaseIndex(ActionPhase::Started | ActionPhase::Ongoing) == kActionPhaseCount);
    }

    TEST_CASE("validation reports chords without an action, combos without steps and parameters out of range") {
        InputAssetLibrary library;
        const AssetId action = AssetId::generate();
        library.setAction(
            action,
            InputActionAsset{
                .modifiers = {InputModifier{.type = InputModifierType::DeadZone,
                                            .deadZone = DeadZoneParams{.lower = 0.5f, .upper = 0.5f}},
                              InputModifier{.type = InputModifierType::Curve, .curve = CurveParams{.y = -1.0f}}},
                .triggers = {
                    InputTrigger{.type = InputTriggerType::Chord}, InputTrigger{.type = InputTriggerType::Combo},
                    InputTrigger{.type = InputTriggerType::Hold, .hold = HoldParams{.seconds = -1.0f}},
                    InputTrigger{.type = InputTriggerType::Down, .threshold = -0.5f},
                    InputTrigger{.type = InputTriggerType::MultiTap, .multiTap = MultiTapParams{.count = 1}}}});
        InputMapping mapped = mapping(action, "Keyboard.A");
        mapped.triggers = {
            InputTrigger{.type = InputTriggerType::Combo, .combo = ComboParams{.steps = {{.action = AssetRef{}}}}},
            InputTrigger{.type = InputTriggerType::Tap, .tap = TapParams{.seconds = -0.1f}}};
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {mapped}});
        const std::vector<InputAssetProblem> problems = library.validate();
        CHECK(countCode(problems, "input_actions.invalid_trigger") == 3);
        // Dead zone, curve, hold time, threshold, MultiTap count, tap time.
        CHECK(countCode(problems, "input_actions.invalid_parameter") == 6);
        CHECK(std::ranges::all_of(problems, [](const InputAssetProblem& problem) { return problem.error; }));

        InputAssetLibrary valid;
        const AssetId other = AssetId::generate();
        valid.setAction(other, {});
        valid.setAction(
            action, InputActionAsset{.triggers = {InputTrigger{.type = InputTriggerType::Chord,
                                                               .chord = ChordParams{.action = AssetRef::to(other)}}}});
        valid.setContext(AssetId::generate(),
                         InputContextAsset{.mappings = {mapping(action, "Keyboard.A"), mapping(other, "Keyboard.B")}});
        CHECK(valid.validate().empty());
    }
}
