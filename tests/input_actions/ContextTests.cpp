// Context stacks, claims, routing, focus, capture, suspension, devices and players, and the ECS
// reads (docs/specs/input-actions.md, requirements 24 to 36, 46 and 47).

#include "ith/input_actions/InputActions.h"

#include "ActionTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <ostream> // doctest prints std::string operands through it
#include <string>
#include <vector>

using namespace ith;
using namespace ith::test;

namespace {

// A UI layer that consumes every key press.
class ConsumeKeys final : public InputConsumer {
public:
    InputReply onInputEvent(const Event& event) override {
        return event.type == EventType::KeyDown ? InputReply::Consumed : InputReply::NotConsumed;
    }
};

bool fired(const Rig& rig, AssetId action) {
    return std::ranges::any_of(rig.actions.events(), [action](const ActionEvent& event) {
        return event.action == action && event.phase == ActionPhase::Triggered;
    });
}

} // namespace

TEST_SUITE("input_actions.contexts") {
    TEST_CASE("a higher context takes W; removing it while W is held hands W to the lower one without a press "
              "(requirement 24)") {
        Rig rig;
        const AssetId accelerate = rig.addAction();
        const AssetId move = rig.addAction();
        const AssetId movePressed = rig.addAction();
        const AssetId vehicle = rig.addContext({mapping(accelerate, "Keyboard.W")});
        const AssetId onFoot = rig.addContext(
            {mapping(move, "Keyboard.W"), mapping(movePressed, "Keyboard.W", {trigger(InputTriggerType::Pressed)})});
        const Entity player = rig.addPlayer({{onFoot, 0}, {vehicle, 10}});
        rig.frame(10, {keyDown(Key::W, 5)});
        CHECK(rig.state(player, accelerate) == TriggerState::Triggered);
        CHECK(rig.state(player, move) == TriggerState::None);
        CHECK(rig.state(player, movePressed) == TriggerState::None);

        rig.removeContext(player, vehicle);
        rig.frame(20);
        CHECK(rig.phases(player, accelerate) == ActionPhase::Completed);
        CHECK(rig.phases(player, move) == (ActionPhase::Started | ActionPhase::Triggered));
        CHECK(rig.state(player, movePressed) == TriggerState::None);
        rig.frame(30);
        CHECK(rig.state(player, movePressed) == TriggerState::None);
    }

    TEST_CASE("a Pressed mapping keeps its key claimed while held (requirement 25); consumesKeys false never "
              "hides keys (requirement 26)") {
        Rig rig;
        const AssetId brake = rig.addAction();
        const AssetId horn = rig.addAction(InputActionAsset{.consumesKeys = false});
        const AssetId move = rig.addAction();
        const AssetId wave = rig.addAction();
        const AssetId high = rig.addContext({mapping(brake, "Keyboard.W", {trigger(InputTriggerType::Pressed)}),
                                             mapping(horn, "Keyboard.H", {trigger(InputTriggerType::Pressed)})});
        const AssetId low = rig.addContext({mapping(move, "Keyboard.W"), mapping(wave, "Keyboard.H")});
        const Entity player = rig.addPlayer({{low, 0}, {high, 5}});
        bool moved = false;
        rig.frame(10, {keyDown(Key::W, 5), keyDown(Key::H, 6)});
        CHECK(rig.state(player, brake) == TriggerState::Triggered);
        CHECK(rig.state(player, horn) == TriggerState::Triggered);
        CHECK(rig.state(player, wave) == TriggerState::Triggered);
        for (int frame = 1; frame < 10; ++frame) {
            rig.frame(10 + (frame * 16));
            moved = moved || rig.state(player, move) != TriggerState::None;
            CHECK(rig.state(player, brake) == TriggerState::None);
            CHECK(rig.state(player, wave) == TriggerState::Triggered);
        }
        CHECK_FALSE(moved);
        rig.frame(200, {keyUp(Key::W, 190)});
        rig.frame(220, {keyDown(Key::W, 210)});
        CHECK(rig.state(player, brake) == TriggerState::Triggered);
        CHECK(rig.state(player, move) == TriggerState::None);
    }

    TEST_CASE("an action in two contexts uses only the higher one's mappings; equal priorities both count "
              "(requirement 27)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId high = rig.addContext({mapping(jump, "Keyboard.Space")});
        const AssetId low = rig.addContext({mapping(jump, "Keyboard.J")});
        const Entity player = rig.addPlayer({{low, 0}, {high, 10}});
        rig.frame(10, {keyDown(Key::J, 5)});
        CHECK(rig.state(player, jump) == TriggerState::None);
        rig.frame(20, {keyUp(Key::J, 15), keyDown(Key::Space, 16)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        rig.frame(30, {keyUp(Key::Space, 25)});

        rig.world.get<InputContexts>(player)->contexts[1].priority = 0;
        rig.frame(40, {keyDown(Key::J, 35)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        CHECK(rig.actions.keysFor(player, jump) == std::vector<Control>{Key::Space, Key::J}); // most recent first
    }

    TEST_CASE("equal priorities order the most recently added context first (requirement 28)") {
        Rig rig;
        const AssetId first = rig.addAction();
        const AssetId second = rig.addAction();
        const AssetId older = rig.addContext({mapping(first, "Keyboard.W")});
        const AssetId newer = rig.addContext({mapping(second, "Keyboard.W")});
        const Entity player = rig.addPlayer({{older, 0}});
        rig.frame(10);
        rig.pushContext(player, newer);
        rig.frame(20, {keyDown(Key::W, 15)});
        CHECK(rig.state(player, second) == TriggerState::Triggered);
        CHECK(rig.state(player, first) == TriggerState::None);
        rig.frame(30, {keyUp(Key::W, 25)});

        rig.removeContext(player, older);
        rig.frame(40);
        rig.pushContext(player, older);
        rig.frame(50, {keyDown(Key::W, 45)});
        CHECK(rig.state(player, first) == TriggerState::Triggered);
        CHECK(rig.state(player, second) == TriggerState::None);
    }

    TEST_CASE("a context added while a key is held: Down at once, no Pressed or Tap, Hold from the adding "
              "(requirement 29)") {
        Rig rig;
        const AssetId down = rig.addAction();
        const AssetId pressed = rig.addAction();
        const AssetId tap = rig.addAction();
        const AssetId holding = rig.addAction();
        const AssetId context = rig.addContext(
            {mapping(down, "Keyboard.W", {trigger(InputTriggerType::Down)}),
             mapping(pressed, "Keyboard.W", {trigger(InputTriggerType::Pressed)}),
             mapping(tap, "Keyboard.W", {trigger(InputTriggerType::Tap)}),
             mapping(holding, "Keyboard.W",
                     {InputTrigger{.type = InputTriggerType::Hold, .hold = HoldParams{.seconds = 0.25f}}})});
        const Entity player = rig.addPlayer();
        rig.frame(100, {keyDown(Key::W, 100)});
        rig.pushContext(player, context);
        rig.frame(200);
        CHECK(rig.phases(player, down) == (ActionPhase::Started | ActionPhase::Triggered));
        CHECK(rig.state(player, pressed) == TriggerState::None);
        CHECK(rig.state(player, tap) == TriggerState::None);
        CHECK(rig.state(player, holding) == TriggerState::Ongoing);
        rig.frame(449);
        CHECK(rig.state(player, holding) == TriggerState::Ongoing);
        rig.frame(450);
        CHECK(rig.state(player, holding) == TriggerState::Triggered);
        CHECK(rig.phaseTime(player, holding, ActionPhase::Triggered) == ms(450));
        rig.frame(460, {keyUp(Key::W, 455)});
        CHECK(rig.state(player, tap) == TriggerState::None);
        CHECK(rig.state(player, pressed) == TriggerState::None);
    }

    TEST_CASE("contexts provided by the controlled entity join on possess and leave on eject (requirement 30)") {
        Rig rig;
        const AssetId accelerate = rig.addAction();
        const AssetId vehicleContext = rig.addContext({mapping(accelerate, "Keyboard.W")});
        const Entity vehicle = rig.world.spawn(
            ProvidedInputContexts{.contexts = {{.context = AssetRef::to(vehicleContext), .priority = 5}}});
        const Entity player = rig.addPlayer();
        rig.world.get<InputPlayer>(player)->controlled = vehicle;
        rig.frame(10, {keyDown(Key::W, 5)});
        CHECK(rig.state(player, accelerate) == TriggerState::Triggered);

        rig.world.get<InputPlayer>(player)->controlled = Entity();
        rig.frame(20);
        CHECK(rig.phases(player, accelerate) == ActionPhase::Completed);
        rig.world.get<InputPlayer>(player)->controlled = vehicle;
        rig.frame(30);
        CHECK(rig.phases(player, accelerate) == (ActionPhase::Started | ActionPhase::Triggered));
    }

    TEST_CASE("an action not in the player's table reads as zero and None (requirement 46)") {
        Rig rig;
        const Entity player = rig.addPlayer();
        rig.frame(10);
        const AssetId unknown = AssetId::generate();
        CHECK(rig.states(player).find(unknown) == nullptr);
        CHECK(rig.value(player, unknown) == ActionValue{});
        CHECK(rig.state(player, unknown) == TriggerState::None);
        CHECK_FALSE(rig.states(player).has(unknown, ActionPhase::Triggered));
    }

    TEST_CASE("the key prompt query lists the effective keys in context order, by family (requirement 47)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId high = rig.addContext({mapping(jump, "Gamepad.FaceSouth"), mapping(jump, "Keyboard.Space")});
        const AssetId other = rig.addContext({mapping(jump, "Mouse.Right")});
        const Entity player = rig.addPlayer({{other, 3}, {high, 3}});
        rig.frame(10);
        CHECK(rig.actions.keysFor(player, jump) ==
              std::vector<Control>{GamepadButton::FaceSouth, Key::Space, MouseButton::Right});
        CHECK(rig.actions.keysFor(player, jump, InputDeviceFamily::KeyboardMouse) ==
              std::vector<Control>{Key::Space, MouseButton::Right});
        CHECK(rig.actions.keysFor(player, jump, InputDeviceFamily::Gamepad) ==
              std::vector<Control>{GamepadButton::FaceSouth});
        CHECK(rig.actions.keysFor(player, AssetId::generate()).empty());
    }

    TEST_CASE("missing contexts, actions and unknown keys are reported once and skipped (section 3.12)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId context = rig.addContext({mapping(AssetId::generate(), "Keyboard.A"),
                                                mapping(jump, "Keyboard.Nope"), mapping(jump, "Keyboard.Space")});
        const Entity player = rig.addPlayer({{context, 0}, {AssetId::generate(), 1}});
        rig.frame(10, {keyDown(Key::Space, 5)});
        rig.frame(20);
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        std::vector<Error> messages = rig.actions.takeMessages();
        std::vector<std::string> codes;
        for (const Error& message : messages) {
            codes.push_back(message.code);
        }
        std::ranges::sort(codes);
        CHECK(codes == std::vector<std::string>{"input_actions.missing_action", "input_actions.missing_context",
                                                "input_actions.unknown_key"});
        rig.pushContext(player, AssetId::generate(), 2);
        rig.frame(30);
        CHECK(rig.actions.takeMessages().size() == 1); // only the new missing context
    }
}

TEST_SUITE("input_actions.routing") {
    TEST_CASE("input consumed above the game slot never reaches an action (requirement 31)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const Entity player = rig.addPlayer({{rig.addContext({mapping(jump, "Keyboard.Space")}), 0}});
        ConsumeKeys menu;
        const ConsumerId ui = rig.input.addConsumer({.window = kWindow, .layer = ConsumerLayer::Ui, .consumer = &menu});
        rig.frame(10, {keyDown(Key::Space, 5)});
        rig.frame(20, {keyUp(Key::Space, 15)});
        CHECK(rig.state(player, jump) == TriggerState::None);
        CHECK(rig.actions.events().empty());
        rig.input.removeConsumer(ui);
        rig.frame(30, {keyDown(Key::Space, 25)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
    }

    TEST_CASE("focus loss ends actions at the release times; capture loss ends mouse actions (requirement 32)") {
        Rig rig;
        const AssetId move = rig.addAction();
        const AssetId fire = rig.addAction();
        const AssetId look = rig.addAction(ActionValueKind::Axis2);
        const Entity player = rig.addPlayer(
            {{rig.addContext({mapping(move, "Keyboard.W"), mapping(fire, "Mouse.Left"), mapping(look, "Mouse.Delta")}),
              0}});
        rig.frame(10, {keyDown(Key::W, 5)});
        rig.frame(20, {focusEvent(EventType::WindowFocusLost, 15)});
        CHECK(rig.phases(player, move) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, move, ActionPhase::Completed) == ms(15));
        rig.frame(30, {focusEvent(EventType::WindowFocusGained, 25)});

        REQUIRE(rig.input.captureMouse(kWindow));
        rig.frame(40, {mouseDown(MouseButton::Left, 35)});
        CHECK(rig.state(player, fire) == TriggerState::Triggered);
        rig.input.releaseMouse();
        rig.frame(50);
        CHECK(rig.phases(player, fire) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, fire, ActionPhase::Completed) == ms(50));
        rig.frame(60);
        CHECK(rig.state(player, fire) == TriggerState::None); // still physically held
        rig.frame(70, {mouseUp(MouseButton::Left, 65)});
        rig.frame(80, {mouseDown(MouseButton::Left, 75)});
        CHECK(rig.state(player, fire) == TriggerState::Triggered);

        // Without capture, mouse input no higher consumer takes still drives actions.
        CHECK_FALSE(rig.input.mouseCaptured(kWindow));
        rig.frame(90, {mouseMove(4.0f, 0.0f, 85)});
        CHECK(rig.value(player, look) == ActionValue{.x = 4.0f});
    }

    TEST_CASE("suspension ends every action at once; on resume held keys wait for a press, sticks read at once "
              "(requirement 33)") {
        Rig rig;
        const AssetId move = rig.addAction();
        const AssetId steer = rig.addAction(ActionValueKind::Axis1);
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(move, "Keyboard.W"), mapping(steer, "Gamepad.LeftStickX")}), 0}});
        rig.frame(10, {keyDown(Key::W, 5), padConnected(kPad, 6), padAxis(kPad, GamepadAxis::LeftStickX, 0.7f, 7)});
        CHECK(rig.state(player, move) == TriggerState::Triggered);
        CHECK(rig.state(player, steer) == TriggerState::Triggered);

        rig.world.get<ActionStates>(player)->suspended = true;
        rig.frame(20);
        CHECK(rig.phases(player, move) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, move, ActionPhase::Completed) == ms(20));
        CHECK(rig.phases(player, steer) == ActionPhase::Completed);
        rig.frame(30);
        CHECK(rig.state(player, move) == TriggerState::None);

        rig.world.get<ActionStates>(player)->suspended = false;
        rig.frame(40);
        CHECK(rig.state(player, move) == TriggerState::None);
        CHECK(rig.state(player, steer) == TriggerState::Triggered);
        rig.frame(50, {keyUp(Key::W, 45)});
        rig.frame(60, {keyDown(Key::W, 55)});
        CHECK(rig.state(player, move) == TriggerState::Triggered);
    }

    TEST_CASE("disconnecting a gamepad ends its actions at the disconnect time; the keyboard keeps working "
              "(requirement 34)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId move = rig.addAction();
        const Entity player =
            rig.addPlayer({{rig.addContext({mapping(jump, "Gamepad.FaceSouth"), mapping(move, "Keyboard.W")}), 0}});
        rig.frame(10, {padConnected(kPad, 1), padDown(kPad, GamepadButton::FaceSouth, 2), keyDown(Key::W, 3)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        rig.frame(20, {padDisconnected(kPad, 15)});
        CHECK(rig.phases(player, jump) == ActionPhase::Completed);
        CHECK(rig.phaseTime(player, jump, ActionPhase::Completed) == ms(15));
        CHECK(rig.state(player, move) == TriggerState::Triggered);
        rig.frame(30, {padConnected(kOtherPad, 25)});
        CHECK(rig.state(player, jump) == TriggerState::None);
    }

    TEST_CASE("two local players with the same contexts read only their own devices (requirement 35), and "
              "unassigned devices change nothing (requirement 36)") {
        Rig rig;
        const AssetId brake = rig.addAction();
        const AssetId jump = rig.addAction();
        const AssetId charge = rig.addAction();
        const AssetId high =
            rig.addContext({mapping(brake, "Gamepad.FaceSouth", {trigger(InputTriggerType::Pressed)})});
        const AssetId low = rig.addContext(
            {mapping(jump, "Gamepad.FaceSouth"),
             mapping(charge, "Gamepad.FaceEast",
                     {InputTrigger{.type = InputTriggerType::Hold, .hold = HoldParams{.seconds = 0.25f}}})});
        rig.input.setLocalMultiplayer(true);
        constexpr DeviceId kUnassigned = 23;
        rig.frame(5, {padConnected(kPad, 1), padConnected(kOtherPad, 2), padConnected(kUnassigned, 3)});
        rig.input.assignDevice(kPad, 0);
        rig.input.assignDevice(kOtherPad, 1);
        const Entity one = rig.addPlayer({{low, 0}, {high, 1}}, 0);
        const Entity two = rig.addPlayer({{low, 0}, {high, 1}}, 1);

        rig.frame(10, {padDown(kPad, GamepadButton::FaceSouth, 6), padDown(kPad, GamepadButton::FaceEast, 7)});
        CHECK(rig.state(one, brake) == TriggerState::Triggered);
        CHECK(rig.state(two, brake) == TriggerState::None);
        rig.frame(100,
                  {padDown(kOtherPad, GamepadButton::FaceSouth, 90), padDown(kOtherPad, GamepadButton::FaceEast, 91)});
        CHECK(rig.state(two, brake) == TriggerState::Triggered); // one's claim doesn't hide two's key
        CHECK(rig.state(one, jump) == TriggerState::None);       // one's claim still holds
        rig.frame(260);
        CHECK(rig.state(one, charge) == TriggerState::Triggered); // 7 ms + 250 ms
        CHECK(rig.state(two, charge) == TriggerState::Ongoing);   // 91 ms + 250 ms is later
        rig.frame(341);
        CHECK(rig.state(two, charge) == TriggerState::Triggered);

        rig.frame(350);
        rig.frame(360);
        const std::vector<ActionState> before = rig.states(one).actions;
        rig.frame(400, {padDown(kUnassigned, GamepadButton::FaceWest, 395),
                        padDown(kUnassigned, GamepadButton::FaceSouth, 396),
                        padAxis(kUnassigned, GamepadAxis::LeftStickX, 1.0f, 397)});
        CHECK(rig.actions.events().size() == 0);
        CHECK(fired(rig, brake) == false);
        CHECK(rig.states(one).actions.size() == before.size());
    }
}
