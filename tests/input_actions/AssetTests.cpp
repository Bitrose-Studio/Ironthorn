// Input action and context assets: round trips through the text archive, defaults, load errors and
// validation (docs/specs/input-actions.md, requirements 1 to 4).

#include "ith/input_actions/InputActionAssets.h"
#include "ith/input_actions/InputAssetLibrary.h"
#include "ith/serialization/TextArchive.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream> // doctest prints std::string operands through it
#include <string>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using ith::AssetId;
using ith::AssetRef;
using ith::InputActionAsset;
using ith::InputContextAsset;
using ith::InputMapping;
using ith::InputModifier;
using ith::InputModifierType;
using ith::InputTrigger;
using ith::InputTriggerType;

std::vector<std::string> codes(const std::vector<ith::InputAssetProblem>& problems) {
    std::vector<std::string> result;
    for (const ith::InputAssetProblem& problem : problems) {
        result.push_back(problem.code);
    }
    return result;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::ranges::find(list, value) != list.end();
}

InputMapping mappingOf(AssetId action, std::string key) {
    return InputMapping{.action = AssetRef::to(action), .key = std::move(key)};
}

} // namespace

TEST_SUITE("input_actions.assets") {
    TEST_CASE("an input action round-trips with every field and its asset id (requirement 1)") {
        const AssetId other = AssetId::generate();
        InputActionAsset action{.displayName = "Jump",
                                .category = "Movement",
                                .description = "Leave the ground",
                                .valueKind = ith::ActionValueKind::Axis2,
                                .combine = ith::ActionCombine::Sum,
                                .clampToUnit = true,
                                .consumesKeys = false};
        action.modifiers = {
            InputModifier{.type = InputModifierType::DeadZone,
                          .deadZone =
                              ith::DeadZoneParams{.lower = 0.1f, .upper = 0.9f, .shape = ith::DeadZoneShape::Axial}},
            InputModifier{.type = InputModifierType::Scale, .scale = ith::ScaleParams{.x = 2.0f, .y = 3.0f, .z = 4.0f}},
            InputModifier{.type = InputModifierType::Negate, .negate = ith::NegateParams{.x = false}},
            InputModifier{.type = InputModifierType::Swizzle,
                          .swizzle = ith::SwizzleParams{.order = ith::SwizzleOrder::ZXY}},
            InputModifier{.type = InputModifierType::Curve, .curve = ith::CurveParams{.x = 2.0f}},
            InputModifier{.type = InputModifierType::FrameTimeScale},
            InputModifier{.type = InputModifierType::UserScale, .userScale = ith::UserScaleParams{.setting = "look"}},
            InputModifier{.type = InputModifierType::UserNegate,
                          .userNegate = ith::UserNegateParams{.setting = "invert"}},
        };
        action.triggers = {
            InputTrigger{.type = InputTriggerType::Hold,
                         .kind = ith::TriggerKind::Blocker,
                         .threshold = 0.25f,
                         .hold = ith::HoldParams{.seconds = 1.5f, .once = false}},
            InputTrigger{.type = InputTriggerType::HoldRelease, .holdRelease = ith::HoldReleaseParams{.seconds = 2.0f}},
            InputTrigger{.type = InputTriggerType::Tap, .tap = ith::TapParams{.seconds = 0.1f}},
            InputTrigger{.type = InputTriggerType::MultiTap,
                         .multiTap = ith::MultiTapParams{.count = 3, .tapSeconds = 0.1f, .gapSeconds = 0.2f}},
            InputTrigger{.type = InputTriggerType::Pulse,
                         .pulse = ith::PulseParams{.seconds = 0.25f, .atStart = false, .limit = 4}},
            InputTrigger{.type = InputTriggerType::Chord, .chord = ith::ChordParams{.action = AssetRef::to(other)}},
            InputTrigger{.type = InputTriggerType::Combo,
                         .combo = ith::ComboParams{.steps = {{.action = AssetRef::to(other),
                                                              .phase = ith::ComboStepPhase::Completed,
                                                              .maxSeconds = 0.75f}},
                                                   .cancelActions = {AssetRef::to(other)}}},
        };
        const ith::Uuid id = ith::Uuid::generate();
        const auto text = serialization::writeText(ith::kInputActionFormat, action, &id);
        REQUIRE_MESSAGE(text.hasValue(), text.error().toString());
        CHECK(text->find("\"format\": \"ith.input_action\"") != std::string::npos);

        ith::Uuid readId;
        InputActionAsset read;
        const auto result = serialization::readText(
            *text, ith::kInputActionFormat, read, {.unknownFields = serialization::UnknownFields::Fail, .id = &readId});
        REQUIRE_MESSAGE(result.hasValue(), result.error().toString());
        CHECK(readId == id);
        CHECK(read.displayName == "Jump");
        CHECK(read.valueKind == ith::ActionValueKind::Axis2);
        CHECK(read.consumesKeys == false);
        REQUIRE(read.modifiers.size() == 8);
        CHECK(read.modifiers[0].deadZone->shape == ith::DeadZoneShape::Axial);
        CHECK(read.modifiers[6].userScale->setting == "look");
        REQUIRE(read.triggers.size() == 7);
        CHECK(read.triggers[0].kind == ith::TriggerKind::Blocker);
        CHECK(read.triggers[6].combo->steps[0].maxSeconds == 0.75f);
        // Written again, the document is identical: every field survived.
        CHECK(serialization::writeText(ith::kInputActionFormat, read, &id).value() == *text);
    }

    TEST_CASE("an input context round-trips with every mapping field (requirement 1)") {
        const AssetId jump = AssetId::generate();
        InputContextAsset context{.displayName = "On foot", .description = "Walking around"};
        InputMapping mapping = mappingOf(jump, "Keyboard.Space");
        mapping.modifiers = {InputModifier{.type = InputModifierType::Negate}};
        mapping.triggers = {InputTrigger{.type = InputTriggerType::Pressed}};
        mapping.slot = "jump.keyboard";
        mapping.slotLabel = "Primary";
        context.mappings = {mapping, mappingOf(jump, "Gamepad.FaceSouth")};

        const ith::Uuid id = ith::Uuid::generate();
        const auto text = serialization::writeText(ith::kInputContextFormat, context, &id);
        REQUIRE(text.hasValue());
        InputContextAsset read;
        ith::Uuid readId;
        REQUIRE(serialization::readText(*text, ith::kInputContextFormat, read, {.id = &readId}));
        CHECK(readId == id);
        REQUIRE(read.mappings.size() == 2);
        CHECK(read.mappings[0].action.assetId() == jump);
        CHECK(read.mappings[0].slot == "jump.keyboard");
        CHECK(read.mappings[0].slotLabel == "Primary");
        CHECK(serialization::writeText(ith::kInputContextFormat, read, &id).value() == *text);
    }

    TEST_CASE("missing fields take the defaults of section 3.1") {
        const std::string text =
            R"({"format": "ith.input_action", "version": 1, )"
            R"("id": "0b8e7f6a-1c2d-4e3f-8a9b-0c1d2e3f4a5b", "data": {"triggers": [{"type": "Hold"}]}})";
        InputActionAsset read;
        ith::Uuid id;
        REQUIRE(serialization::readText(text, ith::kInputActionFormat, read, {.id = &id}));
        CHECK(read.valueKind == ith::ActionValueKind::Digital);
        CHECK(read.combine == ith::ActionCombine::Strongest);
        CHECK_FALSE(read.clampToUnit);
        CHECK_FALSE(read.consumesKeys.has_value()); // true when absent
        REQUIRE(read.triggers.size() == 1);
        CHECK(read.triggers[0].threshold == 0.5f);
        CHECK_FALSE(read.triggers[0].hold.has_value()); // the evaluation uses HoldParams{}: 0.5 s, once
        CHECK(ith::HoldParams{}.seconds == 0.5f);
        CHECK(ith::triggerKind(read.triggers[0]) == ith::TriggerKind::Explicit);
        CHECK(ith::triggerKind(InputTrigger{.type = InputTriggerType::Chord}) == ith::TriggerKind::Implicit);
    }

    TEST_CASE("an asset document needs its id; a modifier or trigger type the build doesn't know fails the load "
              "(requirement 2)") {
        InputActionAsset read;
        CHECK(serialization::readText(R"({"format": "ith.input_action", "version": 1, "data": {}})",
                                      ith::kInputActionFormat, read, {.id = nullptr})
                  .hasValue());
        ith::Uuid id;
        CHECK(serialization::readText(R"({"format": "ith.input_action", "version": 1, "data": {}})",
                                      ith::kInputActionFormat, read, {.id = &id})
                  .error()
                  .code == "serialization.not_a_document");
        const auto unknown = serialization::readText(
            R"({"format": "ith.input_action", "version": 1, )"
            R"("id": "0b8e7f6a-1c2d-4e3f-8a9b-0c1d2e3f4a5b", "data": {"modifiers": [{"type": "Smooth"}]}})",
            ith::kInputActionFormat, read, {.id = &id});
        REQUIRE_FALSE(unknown.hasValue());
        CHECK(unknown.error().code == "serialization.unknown_enumerator");
        const auto trigger = serialization::readText(
            R"({"format": "ith.input_action", "version": 1, )"
            R"("id": "0b8e7f6a-1c2d-4e3f-8a9b-0c1d2e3f4a5b", "data": {"triggers": [{"type": "Wiggle"}]}})",
            ith::kInputActionFormat, read, {.id = &id});
        CHECK(trigger.error().code == "serialization.unknown_enumerator");
    }

    TEST_CASE("unknown keys, unknown settings and missing actions are validation errors (requirement 2)") {
        ith::InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        library.setAction(jump, InputActionAsset{});
        library.setSettings({ith::InputSettingDecl{.name = "look"}});
        InputMapping bad = mappingOf(jump, "Keyboard.NoSuchKey");
        bad.modifiers = {
            InputModifier{.type = InputModifierType::UserScale, .userScale = ith::UserScaleParams{.setting = "nope"}},
            InputModifier{.type = InputModifierType::UserScale, .userScale = ith::UserScaleParams{.setting = "look"}}};
        const AssetId context = AssetId::generate();
        library.setContext(context, InputContextAsset{.mappings = {bad, mappingOf(AssetId::generate(), "Keyboard.A")}});

        const std::vector<ith::InputAssetProblem> problems = library.validate();
        const std::vector<std::string> found = codes(problems);
        CHECK(std::ranges::count(found, std::string("input_actions.unknown_key")) == 1);
        CHECK(std::ranges::count(found, std::string("input_actions.unknown_setting")) == 1);
        CHECK(std::ranges::count(found, std::string("input_actions.missing_action")) == 1);
        CHECK(std::ranges::all_of(problems,
                                  [&](const ith::InputAssetProblem& p) { return p.error && p.asset == context; }));
    }

    TEST_CASE("mappings sharing a slot with another action, family or default key are a validation error "
              "(requirement 3)") {
        ith::InputAssetLibrary library;
        const AssetId jump = AssetId::generate();
        const AssetId crouch = AssetId::generate();
        library.setAction(jump, {});
        library.setAction(crouch, {});
        const auto slotted = [](AssetId action, std::string key) {
            InputMapping mapping = mappingOf(action, std::move(key));
            mapping.slot = "jump.keyboard";
            return mapping;
        };
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space")}});
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {slotted(jump, "Keyboard.Space")}});
        CHECK(library.validate().empty());

        for (const InputMapping& clash :
             {slotted(crouch, "Keyboard.Space"), slotted(jump, "Keyboard.C"), slotted(jump, "Gamepad.FaceSouth")}) {
            CAPTURE(clash.key);
            const AssetId id = AssetId::generate();
            library.setContext(id, InputContextAsset{.mappings = {clash}});
            CHECK(contains(codes(library.validate()), "input_actions.slot_conflict"));
            library.removeContext(id);
        }
        const std::optional<ith::InputSlotInfo> slot = library.findSlot("jump.keyboard");
        REQUIRE(slot.has_value());
        CHECK(slot->contexts.size() == 2);
        CHECK(slot->family == ith::InputDeviceFamily::KeyboardMouse);
        CHECK(slot->rawKind == ith::InputRawKind::Digital);
    }

    TEST_CASE("chord and combo dependency cycles are validation errors; related warnings (requirement 4)") {
        ith::InputAssetLibrary library;
        const AssetId a = AssetId::generate();
        const AssetId b = AssetId::generate();
        const AssetId c = AssetId::generate();
        library.setAction(a, InputActionAsset{.triggers = {InputTrigger{.type = InputTriggerType::Chord,
                                                                        .chord = ith::ChordParams{AssetRef::to(b)}}}});
        library.setAction(b, InputActionAsset{.consumesKeys = true});
        library.setAction(c, {});
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {mappingOf(a, "Keyboard.A")}});
        std::vector<std::string> found = codes(library.validate());
        CHECK_FALSE(contains(found, "input_actions.dependency_cycle"));
        CHECK(contains(found, "input_actions.consumes_keys_on_dependency"));
        CHECK(contains(found, "input_actions.unmapped_chord_action"));

        // b depends on c through a combo, c on a through a chord on its mapping: a -> b -> c -> a.
        library.setAction(b, InputActionAsset{.triggers = {InputTrigger{
                                                  .type = InputTriggerType::Combo,
                                                  .combo = ith::ComboParams{.steps = {{.action = AssetRef::to(c)}}}}}});
        InputMapping chord = mappingOf(c, "Keyboard.C");
        chord.triggers = {InputTrigger{.type = InputTriggerType::Chord, .chord = ith::ChordParams{AssetRef::to(a)}}};
        library.setContext(AssetId::generate(), InputContextAsset{.mappings = {chord, mappingOf(b, "Keyboard.B")}});
        const std::vector<ith::InputAssetProblem> problems = library.validate();
        CHECK(std::ranges::count_if(problems, [](const ith::InputAssetProblem& p) {
                  return p.code == "input_actions.dependency_cycle" && p.error;
              }) == 3);
    }

    TEST_CASE("controls have a device family and a raw kind") {
        CHECK(ith::deviceFamily(ith::Key::W) == ith::InputDeviceFamily::KeyboardMouse);
        CHECK(ith::deviceFamily(ith::Axis2D::MouseDelta) == ith::InputDeviceFamily::KeyboardMouse);
        CHECK(ith::deviceFamily(ith::Axis2D::GamepadLeftStick) == ith::InputDeviceFamily::Gamepad);
        CHECK(ith::deviceFamily(ith::GamepadAxis::LeftTrigger) == ith::InputDeviceFamily::Gamepad);
        CHECK(ith::rawKind(ith::MouseButton::Left) == ith::InputRawKind::Digital);
        CHECK(ith::rawKind(ith::MouseAxis::WheelY) == ith::InputRawKind::Axis1);
        CHECK(ith::rawKind(ith::Axis2D::GamepadRightStick) == ith::InputRawKind::Axis2);
    }
}
