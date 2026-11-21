#include "ith/input_actions/InputAssetLibrary.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ith {

TriggerKind triggerKind(const InputTrigger& trigger) noexcept {
    if (trigger.kind.has_value()) {
        return *trigger.kind;
    }
    return trigger.type == InputTriggerType::Chord ? TriggerKind::Implicit : TriggerKind::Explicit;
}

InputDeviceFamily deviceFamily(const Control& control) noexcept {
    if (std::holds_alternative<GamepadButton>(control) || std::holds_alternative<GamepadAxis>(control)) {
        return InputDeviceFamily::Gamepad;
    }
    if (const Axis2D* axis = std::get_if<Axis2D>(&control)) {
        return *axis == Axis2D::GamepadLeftStick || *axis == Axis2D::GamepadRightStick
                   ? InputDeviceFamily::Gamepad
                   : InputDeviceFamily::KeyboardMouse;
    }
    return InputDeviceFamily::KeyboardMouse;
}

InputRawKind rawKind(const Control& control) noexcept {
    if (std::holds_alternative<GamepadAxis>(control) || std::holds_alternative<MouseAxis>(control)) {
        return InputRawKind::Axis1;
    }
    if (std::holds_alternative<Axis2D>(control)) {
        return InputRawKind::Axis2;
    }
    return InputRawKind::Digital;
}

void InputAssetLibrary::setAction(AssetId id, InputActionAsset action) {
    ITH_ASSERT(!id.isNil());
    m_actions.insert_or_assign(id, std::move(action));
    ++m_version;
}

bool InputAssetLibrary::removeAction(AssetId id) {
    if (m_actions.erase(id) == 0) {
        return false;
    }
    ++m_version;
    return true;
}

const InputActionAsset* InputAssetLibrary::findAction(AssetId id) const {
    const auto found = m_actions.find(id);
    return found != m_actions.end() ? &found->second : nullptr;
}

void InputAssetLibrary::setContext(AssetId id, InputContextAsset context) {
    ITH_ASSERT(!id.isNil());
    m_contexts.insert_or_assign(id, std::move(context));
    ++m_version;
}

bool InputAssetLibrary::removeContext(AssetId id) {
    if (m_contexts.erase(id) == 0) {
        return false;
    }
    ++m_version;
    return true;
}

const InputContextAsset* InputAssetLibrary::findContext(AssetId id) const {
    const auto found = m_contexts.find(id);
    return found != m_contexts.end() ? &found->second : nullptr;
}

void InputAssetLibrary::setSettings(std::vector<InputSettingDecl> settings) {
    m_settings = std::move(settings);
    ++m_version;
}

const InputSettingDecl* InputAssetLibrary::findSetting(std::string_view name) const {
    const auto found =
        std::ranges::find_if(m_settings, [name](const InputSettingDecl& setting) { return setting.name == name; });
    return found != m_settings.end() ? &*found : nullptr;
}

std::optional<InputSlotInfo> InputAssetLibrary::findSlot(std::string_view name) const {
    std::optional<InputSlotInfo> slot;
    for (const auto& [contextId, context] : m_contexts) {
        for (const InputMapping& mapping : context.mappings) {
            if (mapping.slot != name) {
                continue;
            }
            if (!slot.has_value()) {
                const std::optional<Control> key = findControl(mapping.key);
                slot = InputSlotInfo{.name = std::string(name),
                                     .action = mapping.action.assetId(),
                                     .defaultKey = key,
                                     .family = key ? deviceFamily(*key) : InputDeviceFamily::KeyboardMouse,
                                     .rawKind = key ? rawKind(*key) : InputRawKind::Digital};
            }
            if (slot->contexts.empty() || slot->contexts.back() != contextId) {
                slot->contexts.push_back(contextId);
            }
        }
    }
    return slot;
}

std::vector<InputSlotInfo> InputAssetLibrary::slots() const {
    std::set<std::string> names;
    for (const auto& [contextId, context] : m_contexts) {
        for (const InputMapping& mapping : context.mappings) {
            if (!mapping.slot.empty()) {
                names.insert(mapping.slot);
            }
        }
    }
    std::vector<InputSlotInfo> result;
    result.reserve(names.size());
    for (const std::string& name : names) {
        if (std::optional<InputSlotInfo> slot = findSlot(name)) {
            result.push_back(std::move(*slot));
        }
    }
    return result;
}

namespace {

// The first position of `id` in `path`. A predicate rather than std::ranges::find: the MSVC
// standard library (14.52 and later) sends std::find over 16-byte trivially comparable values such
// as AssetId to a vectorized search that supports only 1-, 2-, 4- and 8-byte elements, which
// fails to compile under clang-cl ("unexpected size").
std::vector<AssetId>::iterator firstOf(std::vector<AssetId>& path, AssetId id) {
    return std::ranges::find_if(path, [id](AssetId entry) { return entry == id; });
}

using Dependencies = std::map<AssetId, std::set<AssetId>>;

// The actions a list of triggers depends on through Chord and Combo triggers.
void addDependencies(std::span<const InputTrigger> triggers, std::set<AssetId>& out) {
    for (const InputTrigger& trigger : triggers) {
        if (trigger.type == InputTriggerType::Chord) {
            const ChordParams params = trigger.chord.value_or(ChordParams{});
            if (params.action.isSet()) {
                out.insert(params.action.assetId());
            }
        } else if (trigger.type == InputTriggerType::Combo) {
            const ComboParams params = trigger.combo.value_or(ComboParams{});
            for (const ComboStep& step : params.steps) {
                if (step.action.isSet()) {
                    out.insert(step.action.assetId());
                }
            }
            for (const AssetRef& cancel : params.cancelActions) {
                if (cancel.isSet()) {
                    out.insert(cancel.assetId());
                }
            }
        }
    }
}

// The settings a list of modifiers names that the project doesn't declare.
void checkSettings(const InputAssetLibrary& library, std::span<const InputModifier> modifiers, AssetId asset,
                   std::vector<InputAssetProblem>& problems) {
    for (const InputModifier& modifier : modifiers) {
        std::string setting;
        if (modifier.type == InputModifierType::UserScale) {
            setting = modifier.userScale.value_or(UserScaleParams{}).setting;
        } else if (modifier.type == InputModifierType::UserNegate) {
            setting = modifier.userNegate.value_or(UserNegateParams{}).setting;
        } else {
            continue;
        }
        if (library.findSetting(setting) == nullptr) {
            problems.push_back({.asset = asset,
                                .code = "input_actions.unknown_setting",
                                .message = "a modifier names the input setting '" + setting +
                                           "', which the project's input settings don't declare"});
        }
    }
}

// A problem with one modifier or trigger: a reference it lacks or a parameter out of range.
void invalid(std::vector<InputAssetProblem>& problems, AssetId asset, const char* code, std::string message) {
    problems.push_back({.asset = asset, .code = code, .message = std::move(message)});
}

// Parameters out of range in a list of modifiers (section 3.13). Each check is written so that NaN
// fails it too.
void checkModifierParameters(std::span<const InputModifier> modifiers, AssetId asset,
                             std::vector<InputAssetProblem>& problems) {
    for (const InputModifier& modifier : modifiers) {
        if (modifier.type == InputModifierType::DeadZone) {
            const DeadZoneParams params = modifier.deadZone.value_or(DeadZoneParams{});
            const bool valid = params.lower >= 0.0f && params.lower < params.upper; // false for NaN
            if (!valid) {
                invalid(problems, asset, "input_actions.invalid_parameter", "a DeadZone needs 0 <= lower < upper");
            }
        } else if (modifier.type == InputModifierType::Curve) {
            const CurveParams params = modifier.curve.value_or(CurveParams{});
            const bool valid = params.x >= 0.0f && params.y >= 0.0f && params.z >= 0.0f;
            if (!valid) {
                invalid(problems, asset, "input_actions.invalid_parameter", "a Curve exponent is negative");
            }
        }
    }
}

// The times of a trigger, by type, for the range check.
std::vector<float> triggerTimes(const InputTrigger& trigger) {
    switch (trigger.type) {
        case InputTriggerType::Hold:
            return {trigger.hold.value_or(HoldParams{}).seconds};
        case InputTriggerType::HoldRelease:
            return {trigger.holdRelease.value_or(HoldReleaseParams{}).seconds};
        case InputTriggerType::Tap:
            return {trigger.tap.value_or(TapParams{}).seconds};
        case InputTriggerType::MultiTap: {
            const MultiTapParams params = trigger.multiTap.value_or(MultiTapParams{});
            return {params.tapSeconds, params.gapSeconds};
        }
        case InputTriggerType::Pulse:
            return {trigger.pulse.value_or(PulseParams{}).seconds};
        case InputTriggerType::Combo: {
            std::vector<float> times;
            for (const ComboStep& step : trigger.combo.value_or(ComboParams{}).steps) {
                times.push_back(step.maxSeconds);
            }
            return times;
        }
        case InputTriggerType::Down:
        case InputTriggerType::Pressed:
        case InputTriggerType::Released:
        case InputTriggerType::Chord:
            break;
    }
    return {};
}

// Missing references and parameters out of range in a list of triggers (section 3.13).
void checkTriggers(std::span<const InputTrigger> triggers, AssetId asset, std::vector<InputAssetProblem>& problems) {
    for (const InputTrigger& trigger : triggers) {
        if (!(trigger.threshold >= 0.0f)) {
            invalid(problems, asset, "input_actions.invalid_parameter", "a trigger threshold is negative");
        }
        for (const float seconds : triggerTimes(trigger)) {
            if (!(seconds >= 0.0f)) {
                invalid(problems, asset, "input_actions.invalid_parameter", "a trigger time is negative");
            }
        }
        if (trigger.type == InputTriggerType::Chord && !trigger.chord.value_or(ChordParams{}).action.isSet()) {
            invalid(problems, asset, "input_actions.invalid_trigger", "a Chord trigger names no action");
        }
        if (trigger.type == InputTriggerType::Combo) {
            const ComboParams params = trigger.combo.value_or(ComboParams{});
            if (params.steps.empty() ||
                std::ranges::any_of(params.steps, [](const ComboStep& step) { return !step.action.isSet(); })) {
                invalid(problems, asset, "input_actions.invalid_trigger",
                        "a Combo trigger has no steps, or a step that names no action");
            }
        }
        if (trigger.type == InputTriggerType::MultiTap && trigger.multiTap.value_or(MultiTapParams{}).count < 2) {
            invalid(problems, asset, "input_actions.invalid_parameter", "a MultiTap count is below 2");
        }
    }
}

// Missing actions, unknown keys and settings, and slot conflicts in the contexts.
void checkContexts(const InputAssetLibrary& library, std::vector<InputAssetProblem>& problems) {
    struct SlotFirst {
        AssetId action;
        std::string key;
        std::optional<InputDeviceFamily> family;
    };

    std::map<std::string, SlotFirst> slotsSeen;
    for (const auto& [contextId, context] : library.contexts()) {
        for (const InputMapping& mapping : context.mappings) {
            if (library.findAction(mapping.action.assetId()) == nullptr) {
                problems.push_back({.asset = contextId,
                                    .code = "input_actions.missing_action",
                                    .message = "a mapping on '" + mapping.key + "' refers to the action " +
                                               mapping.action.assetId().toString() + ", which doesn't exist"});
            }
            const std::optional<Control> key = findControl(mapping.key);
            if (!key.has_value()) {
                problems.push_back({.asset = contextId,
                                    .code = "input_actions.unknown_key",
                                    .message = "a mapping names the unknown key '" + mapping.key + "'"});
            }
            checkSettings(library, mapping.modifiers, contextId, problems);
            checkModifierParameters(mapping.modifiers, contextId, problems);
            checkTriggers(mapping.triggers, contextId, problems);
            if (mapping.slot.empty()) {
                continue;
            }
            const std::optional<InputDeviceFamily> family =
                key ? std::optional<InputDeviceFamily>(deviceFamily(*key)) : std::nullopt;
            const auto [first, inserted] = slotsSeen.try_emplace(
                mapping.slot, SlotFirst{.action = mapping.action.assetId(), .key = mapping.key, .family = family});
            if (!inserted && (first->second.action != mapping.action.assetId() || first->second.key != mapping.key ||
                              first->second.family != family)) {
                problems.push_back({.asset = contextId,
                                    .code = "input_actions.slot_conflict",
                                    .message = "mappings of the slot '" + mapping.slot +
                                               "' differ in action, device family or default key"});
            }
        }
    }
}

// The actions on a dependency cycle, by depth-first search.
std::set<AssetId> findCycles(const Dependencies& dependencies) {
    enum class Mark : std::uint8_t {
        Active,
        Done,
    };
    std::map<AssetId, Mark> marks;
    std::vector<AssetId> path;
    std::set<AssetId> onCycle;
    const auto visit = [&](const auto& self, AssetId id) -> void {
        marks[id] = Mark::Active;
        path.push_back(id);
        const auto found = dependencies.find(id);
        for (const AssetId next : found != dependencies.end() ? found->second : std::set<AssetId>{}) {
            const auto mark = marks.find(next);
            if (mark == marks.end()) {
                self(self, next);
            } else if (mark->second == Mark::Active) {
                onCycle.insert(firstOf(path, next), path.end());
            }
        }
        path.pop_back();
        marks[id] = Mark::Done;
    };
    for (const auto& [id, uses] : dependencies) {
        if (!marks.contains(id)) {
            visit(visit, id);
        }
    }
    return onCycle;
}

} // namespace

std::vector<InputAssetProblem> InputAssetLibrary::validate() const {
    std::vector<InputAssetProblem> problems;
    checkContexts(*this, problems);

    // Dependencies of each action, from its own triggers and those of its mappings.
    Dependencies dependencies;
    std::set<AssetId> mapped;
    for (const auto& [id, action] : m_actions) {
        addDependencies(action.triggers, dependencies[id]);
    }
    for (const auto& [contextId, context] : m_contexts) {
        for (const InputMapping& mapping : context.mappings) {
            mapped.insert(mapping.action.assetId());
            addDependencies(mapping.triggers, dependencies[mapping.action.assetId()]);
        }
    }
    std::set<AssetId> dependedOn;
    for (const auto& [id, uses] : dependencies) {
        dependedOn.insert(uses.begin(), uses.end());
    }

    for (const auto& [id, action] : m_actions) {
        checkSettings(*this, action.modifiers, id, problems);
        checkModifierParameters(action.modifiers, id, problems);
        checkTriggers(action.triggers, id, problems);
        if (dependedOn.contains(id) && action.consumesKeys.has_value()) {
            problems.push_back({.asset = id,
                                .error = false,
                                .code = "input_actions.consumes_keys_on_dependency",
                                .message = "a chord or combo depends on this action, so it never claims keys; "
                                           "its consumesKeys setting has no effect"});
        }
    }
    for (const auto& [id, uses] : dependencies) {
        for (const AssetId used : uses) {
            if (!mapped.contains(used)) {
                problems.push_back({.asset = id,
                                    .error = false,
                                    .code = "input_actions.unmapped_chord_action",
                                    .message = "a chord or combo of this action depends on the action " +
                                               used.toString() + ", which no context maps"});
            }
        }
    }
    for (const AssetId id : findCycles(dependencies)) {
        problems.push_back({.asset = id,
                            .code = "input_actions.dependency_cycle",
                            .message = "the action's chords or combos depend on themselves through other actions"});
    }
    return problems;
}

} // namespace ith
