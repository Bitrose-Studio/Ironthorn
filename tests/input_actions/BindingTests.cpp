// Rebinding: slots, profiles, the layered configuration and the "ith.input_bindings" document
// (docs/specs/input-actions.md, requirements 37 to 42 and 44).

#include "ith/input_actions/InputBindings.h"

#include "ActionTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <ostream> // doctest prints std::string operands through it
#include <string>
#include <vector>

using namespace ith;
using namespace ith::test;

namespace {

InputMapping slotted(AssetId action, std::string key, std::string slot, std::vector<InputModifier> modifiers = {}) {
    InputMapping result = mapping(action, std::move(key), {}, std::move(modifiers));
    result.slot = std::move(slot);
    return result;
}

std::vector<std::string> codes(const std::vector<serialization::Error>& warnings) {
    std::vector<std::string> result;
    for (const serialization::Error& warning : warnings) {
        result.push_back(warning.code);
    }
    return result;
}

} // namespace

TEST_SUITE("input_actions.bindings") {
    TEST_CASE("the bindings document reads the documented layout and writes it back") {
        const std::string text = R"({
  // the user's rebinds
  "format": "ith.input_bindings",
  "version": 1,
  "data": {
    "profiles": {
      "default": {
        "slots": {"jump.keyboard": "Keyboard.Space", "crouch.gamepad": null, "fire": "Keyboard.Nope"},
        "settings": {"look_sensitivity": 1.4, "invert_look_y": true},
        "colour": "red"
      }
    }
  }
})";
        std::vector<serialization::Error> warnings;
        const auto read = readInputBindings(text, {.source = "user/input.json", .warnings = &warnings});
        REQUIRE_MESSAGE(read.hasValue(), read.error().toString());
        REQUIRE(read->profiles.size() == 1);
        const InputBindingProfile& profile = read->profiles[0];
        CHECK(profile.name == "default");
        REQUIRE(profile.slots.size() == 2);
        CHECK(profile.slots[0] == InputSlotBinding{.slot = "crouch.gamepad"});
        CHECK(profile.slots[1] == InputSlotBinding{.slot = "jump.keyboard", .key = Key::Space});
        REQUIRE(profile.settings.size() == 2);
        CHECK(profile.settings[0] == InputSettingValue{.name = "invert_look_y", .value = true});
        CHECK(profile.settings[1] == InputSettingValue{.name = "look_sensitivity", .value = 1.4f});
        CHECK(codes(warnings) == std::vector<std::string>{"input_actions.unknown_key", "serialization.unknown_field"});
        CHECK(warnings[0].source == "user/input.json");
        CHECK(warnings[0].path == "$.profiles.default.slots.fire");

        const auto written = writeInputBindings(*read);
        REQUIRE(written.hasValue());
        const auto again = readInputBindings(*written);
        REQUIRE(again.hasValue());
        CHECK(*again == *read);

        CHECK(readInputBindings(R"({"format": "ith.input_bindings", "version": 1, "data": {"profiles": []}})")
                  .error()
                  .code == "serialization.wrong_type");
        CHECK(readInputBindings(R"({"format": "ith.input_bindings", "version": 1, "data": {"profiles": )"
                                R"({"p": {"settings": {"x": "loud"}}}}})")
                  .error()
                  .code == "serialization.wrong_type");
        CHECK(readInputBindings(R"({"format": "other", "version": 1, "data": {}})").error().code ==
              "serialization.wrong_format");
    }

    TEST_CASE("a rebind changes every mapping of the slot from the next frame, keeping modifiers and triggers "
              "(requirement 37)") {
        Rig rig;
        const AssetId jump = rig.addAction(ActionValueKind::Axis1);
        const AssetId onFoot = rig.addContext(
            {slotted(jump, "Keyboard.Space", "jump.keyboard",
                     {InputModifier{.type = InputModifierType::Scale, .scale = ScaleParams{.x = 2.0f}}})});
        const AssetId swimming = rig.addContext({slotted(jump, "Keyboard.Space", "jump.keyboard")});
        const Entity player = rig.addPlayer({{swimming, 0}, {onFoot, 0}}); // on foot is the most recent
        rig.frame(10);
        REQUIRE(rig.bindings.rebind("default", "jump.keyboard", Key::F));
        CHECK(rig.bindings.key("default", "jump.keyboard") == Control{Key::F});
        rig.frame(20, {keyDown(Key::Space, 15)});
        CHECK(rig.state(player, jump) == TriggerState::None);
        rig.frame(30, {keyUp(Key::Space, 25), keyDown(Key::F, 26)});
        CHECK(rig.state(player, jump) == TriggerState::Triggered);
        CHECK(rig.value(player, jump).x == 2.0f); // the scaled on-foot mapping, which claims F first
        CHECK(rig.actions.keysFor(player, jump) == std::vector<Control>{Key::F});
    }

    TEST_CASE("layers: a user override replaces the platform's; removing it restores the platform key "
              "(requirement 38)") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, {});
        library.setContext(AssetId::generate(),
                           InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space", "jump")}});
        InputBindingConfig config(library);
        const InputBindings platform{.profiles = {{.name = "default", .slots = {{.slot = "jump", .key = Key::J}}}}};
        config.setLayer(InputBindingLayer::Platform, platform);
        CHECK(config.key("default", "jump") == Control{Key::J});
        REQUIRE(config.rebind("default", "jump", Key::K));
        CHECK(config.key("default", "jump") == Control{Key::K});
        CHECK(config.key("other", "jump") == Control{Key::Space}); // profiles don't inherit
        config.resetProfile("default");
        CHECK(config.key("default", "jump") == Control{Key::J});
        REQUIRE(config.rebind("default", "jump", std::nullopt));
        CHECK_FALSE(config.key("default", "jump").has_value()); // unbound
    }

    TEST_CASE("only differences from the lower layers are written; resetting removes them (requirement 39)") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, {});
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space", "jump"),
                                                                               slotted(jump, "Keyboard.C", "crouch")}});
        library.setSettings({InputSettingDecl{.name = "look", .defaultFloat = 1.0f, .min = 0.1f, .max = 5.0f}});
        InputBindingConfig config(library);
        REQUIRE(config.rebind("default", "jump", Key::Space)); // the authored key: nothing to write
        REQUIRE(config.setSetting("default", "look", 1.0f));
        CHECK(config.layer(InputBindingLayer::User).profiles.empty());

        REQUIRE(config.rebind("default", "jump", Key::G));
        REQUIRE(config.setSetting("default", "look", 9.0f)); // clamped to 5
        CHECK(std::get<float>(*config.setting("default", "look")) == 5.0f);
        const InputBindings& user = config.layer(InputBindingLayer::User);
        REQUIRE(user.profiles.size() == 1);
        CHECK(user.profiles[0].slots == std::vector<InputSlotBinding>{{.slot = "jump", .key = Key::G}});
        REQUIRE(config.rebind("default", "jump", Key::Space)); // back to the authored key
        CHECK(config.layer(InputBindingLayer::User).profiles[0].slots.empty());

        config.copyProfile("default", "left_handed");
        CHECK(std::get<float>(*config.setting("left_handed", "look")) == 5.0f);
        CHECK(config.layer(InputBindingLayer::User).profiles.size() == 2);
        config.resetProfile("default");
        config.deleteProfile("left_handed");
        CHECK(config.layer(InputBindingLayer::User).profiles.empty());
        CHECK(std::get<float>(*config.setting("default", "look")) == 1.0f);
    }

    TEST_CASE("unknown slots and settings in a layer are warnings and are ignored (requirement 40)") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, {});
        library.setContext(AssetId::generate(),
                           InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space", "jump")}});
        library.setSettings({InputSettingDecl{.name = "invert", .type = InputSettingType::Bool}});
        InputBindingConfig config(library);
        const InputBindings layer{
            .profiles = {{.name = "default",
                          .slots = {{.slot = "jump", .key = Key::J}, {.slot = "dance", .key = Key::D}},
                          .settings = {{.name = "invert", .value = true},
                                       {.name = "volume", .value = 0.5f},
                                       {.name = "invert2", .value = 1.0f}}}}};
        std::vector<serialization::Error> warnings;
        config.setLayer(InputBindingLayer::Project, layer, "project/input.json", &warnings);
        CHECK(codes(warnings) == std::vector<std::string>{"input_actions.unknown_slot", "input_actions.unknown_setting",
                                                          "input_actions.unknown_setting"});
        CHECK(warnings[0].source == "project/input.json");
        CHECK(warnings[0].message.find("dance") != std::string::npos);
        CHECK(config.key("default", "jump") == Control{Key::J});
        CHECK(std::get<bool>(*config.setting("default", "invert")));
        CHECK(config.layer(InputBindingLayer::Project).profiles[0].slots.size() == 1);
    }

    TEST_CASE("a rebind to another device family or an incompatible raw kind is refused (requirement 41)") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, {});
        library.setContext(AssetId::generate(),
                           InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space", "jump"),
                                                          slotted(jump, "Gamepad.LeftTrigger", "throttle"),
                                                          slotted(jump, "Gamepad.LeftStick", "move")}});
        InputBindingConfig config(library);
        CHECK(config.rebind("default", "jump", GamepadButton::FaceSouth).error().code ==
              "input_actions.wrong_device_family");
        CHECK(config.rebind("default", "jump", Axis2D::MouseDelta).error().code == "input_actions.incompatible_key");
        CHECK(config.rebind("default", "jump", MouseAxis::WheelY)); // a digital slot takes 1D keys
        CHECK(config.rebind("default", "throttle", GamepadButton::FaceSouth).error().code ==
              "input_actions.incompatible_key");
        CHECK(config.rebind("default", "throttle", GamepadAxis::RightTrigger));
        CHECK(config.rebind("default", "move", GamepadAxis::LeftStickX).error().code ==
              "input_actions.incompatible_key");
        CHECK(config.rebind("default", "move", Axis2D::GamepadRightStick));
        const auto unknown = config.rebind("default", "nope", Key::A);
        CHECK(unknown.error().code == "input_actions.unknown_slot");
        CHECK(unknown.error().message.find("nope") != std::string::npos);
    }

    TEST_CASE("the conflict query lists every slot of the family and profile using the key (requirement 42)") {
        InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        const AssetId use = AssetId::generate();
        library.setAction(jump, {});
        library.setAction(use, {});
        const AssetId context = AssetId::generate();
        library.setContext(context, InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space", "jump.keyboard"),
                                                                   slotted(use, "Keyboard.E", "use.keyboard"),
                                                                   slotted(use, "Keyboard.F", "use.alternate"),
                                                                   slotted(use, "Gamepad.FaceSouth", "use.gamepad")}});
        InputBindingConfig config(library);
        CHECK(config.conflicts("default", "jump.keyboard", Key::Q).empty());
        REQUIRE(config.rebind("default", "use.alternate", Key::E));
        const std::vector<InputBindingConflict> conflicts = config.conflicts("default", "jump.keyboard", Key::E);
        REQUIRE(conflicts.size() == 2);
        CHECK(conflicts[0].slot == "use.alternate");
        CHECK(conflicts[1].slot == "use.keyboard");
        CHECK(conflicts[1].action == use);
        CHECK(conflicts[1].contexts == std::vector<AssetId>{context});
        CHECK(config.conflicts("other", "jump.keyboard", Key::E).size() == 1); // another profile
        CHECK(config.conflicts("default", "jump.keyboard", Key::F).empty());   // rebound away
    }

    TEST_CASE("two players with different profiles use different keys for the same slot (requirement 44)") {
        Rig rig;
        const AssetId jump = rig.addAction();
        const AssetId context = rig.addContext({slotted(jump, "Keyboard.Space", "jump.keyboard")});
        REQUIRE(rig.bindings.rebind("left_handed", "jump.keyboard", Key::Enter));
        rig.input.setLocalMultiplayer(true);
        rig.input.assignDevice(kKeyboardMouse, 0);
        const Entity one = rig.addPlayer({{context, 0}}, 0);
        const Entity two = rig.addPlayer({{context, 0}}, 1);
        rig.world.add(two, InputProfile{.name = "left_handed"});
        rig.frame(10);
        CHECK(rig.actions.keysFor(one, jump) == std::vector<Control>{Key::Space});
        CHECK(rig.actions.keysFor(two, jump) == std::vector<Control>{Key::Enter});
        rig.frame(20, {keyDown(Key::Space, 15)});
        CHECK(rig.state(one, jump) == TriggerState::Triggered);

        // The keyboard moves to player two: Space does nothing there, Enter jumps.
        rig.frame(30, {keyUp(Key::Space, 25)});
        rig.input.assignDevice(kKeyboardMouse, 1);
        rig.frame(40, {keyDown(Key::Space, 35)});
        CHECK(rig.state(two, jump) == TriggerState::None);
        rig.frame(50, {keyDown(Key::Enter, 45)});
        CHECK(rig.state(two, jump) == TriggerState::Triggered);
    }
}
