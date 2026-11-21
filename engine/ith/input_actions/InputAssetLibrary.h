#pragma once

// The input actions, input contexts and input settings an application uses, by asset id
// (docs/specs/input-actions.md, sections 3.1, 3.5 and 3.11), with their validation.
//
//   ith::InputAssetLibrary library;
//   library.setAction(jumpId, jump);
//   library.setContext(onFootId, onFoot);
//   for (const ith::InputAssetProblem& problem : library.validate()) { report(problem); }
//
// The application loads the assets (until the runtime asset manager exists, from the documents
// of InputActionAssets.h) and registers them here; InputActions and InputBindingConfig read them.
// A change takes effect at the next InputActions::update.
//
// Not thread-safe: one thread at a time.

#include "ith/assets/AssetId.h"
#include "ith/input/ControlNames.h"
#include "ith/input_actions/InputActionAssets.h"
#include "ith/reflection/Annotations.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

// Keyboard and mouse are one family, as they are one device set (ADR-0013 rule 12).
enum class InputDeviceFamily : std::uint8_t {
    KeyboardMouse,
    Gamepad,
};
ITH_ENUM(InputDeviceFamily)

// What a control reports: digital (keys and buttons), 1D (a trigger, the wheel, one stick or mouse
// axis) or 2D (a stick, the mouse delta, the wheel).
enum class InputRawKind : std::uint8_t {
    Digital,
    Axis1,
    Axis2,
};

// The family and raw kind of a control. Thread-safe.
[[nodiscard]] InputDeviceFamily deviceFamily(const Control& control) noexcept;
[[nodiscard]] InputRawKind rawKind(const Control& control) noexcept;

// One problem found by InputAssetLibrary::validate. Errors fail cooking; warnings don't.
struct InputAssetProblem {
    // The asset with the problem.
    AssetId asset;
    bool error = true;
    // Stable: "input_actions.missing_action", "input_actions.unknown_key",
    // "input_actions.unknown_setting", "input_actions.slot_conflict",
    // "input_actions.dependency_cycle", "input_actions.invalid_trigger" (a Chord without an action,
    // a Combo without steps), "input_actions.invalid_parameter" (a negative time or threshold, a
    // DeadZone whose lower bound is not below its upper bound, a negative Curve exponent, a MultiTap
    // count below 2) (errors); "input_actions.consumes_keys_on_dependency",
    // "input_actions.unmapped_chord_action" (warnings).
    std::string code;
    std::string message;
};

// A binding slot: one rebindable binding, shared by every mapping that names it.
struct InputSlotInfo {
    std::string name;
    AssetId action;
    // The authored key; nullopt when the authored name is unknown.
    std::optional<Control> defaultKey;
    InputDeviceFamily family = InputDeviceFamily::KeyboardMouse;
    InputRawKind rawKind = InputRawKind::Digital;
    // The contexts with a mapping in this slot, in id order.
    std::vector<AssetId> contexts;
};

class InputAssetLibrary {
public:
    // Adds or replaces an action. Precondition (asserted): `id` is not nil.
    void setAction(AssetId id, InputActionAsset action);
    // Removes an action; false when there is none with `id`.
    bool removeAction(AssetId id);
    // The action with `id`, or null. Valid until the action is replaced or removed.
    [[nodiscard]] const InputActionAsset* findAction(AssetId id) const;

    // Adds or replaces a context. Precondition (asserted): `id` is not nil.
    void setContext(AssetId id, InputContextAsset context);
    bool removeContext(AssetId id);
    [[nodiscard]] const InputContextAsset* findContext(AssetId id) const;

    // The contexts by id.
    [[nodiscard]] const std::map<AssetId, InputContextAsset>& contexts() const { return m_contexts; }

    // Replaces the project's input settings.
    void setSettings(std::vector<InputSettingDecl> settings);

    [[nodiscard]] std::span<const InputSettingDecl> settings() const { return m_settings; }

    // The setting called `name`, or null.
    [[nodiscard]] const InputSettingDecl* findSetting(std::string_view name) const;

    // The slot called `name`, from its first mapping (contexts in id order, then declaration
    // order); nullopt when no mapping names it.
    [[nodiscard]] std::optional<InputSlotInfo> findSlot(std::string_view name) const;
    // Every slot, by name.
    [[nodiscard]] std::vector<InputSlotInfo> slots() const;

    // Checks every registered asset (section 3.13): mappings whose action is missing, unknown key
    // names, modifiers naming undeclared settings, mappings sharing a slot with another action,
    // device family or default key, Chord and Combo dependency cycles, `consumesKeys` set on an
    // action a chord or combo depends on, chords whose action no context maps, chords without an
    // action, combos without steps and parameters out of range. Ordered by
    // context, then action, by id.
    [[nodiscard]] std::vector<InputAssetProblem> validate() const;

    // Changes on every set and remove, so users can tell that what they derived is stale.
    [[nodiscard]] std::uint64_t version() const { return m_version; }

private:
    std::map<AssetId, InputActionAsset> m_actions;
    std::map<AssetId, InputContextAsset> m_contexts;
    std::vector<InputSettingDecl> m_settings;
    std::uint64_t m_version = 1;
};

} // namespace ith
