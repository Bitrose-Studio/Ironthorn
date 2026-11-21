#pragma once

// The data of the input action layer (docs/specs/input-actions.md, section 3.1): input actions,
// input contexts with their mappings, modifiers and triggers, and the input settings players tune.
//
// Both assets are native asset documents (ADR-0024): ".ithasset" files with the formats
// kInputActionFormat and kInputContextFormat, read and written by the text archive with their
// asset id:
//
//   ith::Uuid id;
//   ith::InputContextAsset onFoot;
//   const auto read = ith::serialization::readText(text, ith::kInputContextFormat, onFoot, {.id = &id});
//   const auto text = ith::serialization::writeText(ith::kInputContextFormat, onFoot, &id);
//
// Each modifier and trigger names its type and keeps the parameters of that type in the field
// named after it (`deadZone`, `hold`, ...); a missing block takes the defaults of section 3.5 or
// 3.6, and blocks of other types are ignored. Keys are stable control names (`Keyboard.W`,
// ith/input/ControlNames.h), never numeric ids. A modifier or trigger type the build doesn't know
// fails the read ("serialization.unknown_enumerator"); unknown keys, settings, slot conflicts and
// dependency cycles are reported by InputAssetLibrary::validate.
//
// Plain values: thread-safe like any value.

#include "ith/assets/AssetRef.h"
#include "ith/reflection/Annotations.h"
#include "ith/serialization/Document.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ith {

// What an action's value is. Digital is on or off (1 or 0 in x).
enum class ActionValueKind : std::uint8_t {
    Digital,
    Axis1,
    Axis2,
    Axis3,
};
ITH_ENUM(ActionValueKind)

// How the values of several active mappings of one action combine (section 3.4).
enum class ActionCombine : std::uint8_t {
    Strongest, // per component, the largest magnitude; opposite signs that tie give 0
    Sum,       // per component, the sum, not clamped
};
ITH_ENUM(ActionCombine)

enum class InputModifierType : std::uint8_t {
    DeadZone,
    Scale,
    Negate,
    Swizzle,
    Curve,
    FrameTimeScale,
    UserScale,
    UserNegate,
};
ITH_ENUM(InputModifierType)

enum class DeadZoneShape : std::uint8_t {
    Radial, // on the vector length, keeping the direction
    Axial,  // on each component separately
};
ITH_ENUM(DeadZoneShape)

// The new order of the components: YXZ moves x to y (so W can drive forward on a 2D action).
enum class SwizzleOrder : std::uint8_t {
    YXZ,
    ZYX,
    XZY,
    YZX,
    ZXY,
};
ITH_ENUM(SwizzleOrder)

struct DeadZoneParams {
    ITH_TYPE(Struct)
    // Lengths below `lower` become 0, lengths from `lower` to `upper` are remapped to 0..1, lengths
    // above `upper` become 1.
    ITH_FIELD(Min = 0, Max = 1)
    float lower = 0.2f;
    ITH_FIELD(Min = 0, Max = 1)
    float upper = 1.0f;
    ITH_FIELD()
    DeadZoneShape shape = DeadZoneShape::Radial;
};

struct ScaleParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    float x = 1.0f;
    ITH_FIELD()
    float y = 1.0f;
    ITH_FIELD()
    float z = 1.0f;
};

// The components a Negate modifier flips.
struct NegateParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    bool x = true;
    ITH_FIELD()
    bool y = true;
    ITH_FIELD()
    bool z = true;
};

struct SwizzleParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    SwizzleOrder order = SwizzleOrder::YXZ;
};

// Each component becomes sign * magnitude ^ exponent.
struct CurveParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 0)
    float x = 1.0f;
    ITH_FIELD(Min = 0)
    float y = 1.0f;
    ITH_FIELD(Min = 0)
    float z = 1.0f;
};

// Multiplies the selected components by a float input setting of the player's profile.
struct UserScaleParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::string setting;
    ITH_FIELD()
    bool x = true;
    ITH_FIELD()
    bool y = true;
    ITH_FIELD()
    bool z = true;
};

// Flips the selected components while a boolean input setting of the player's profile is true.
struct UserNegateParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::string setting;
    ITH_FIELD()
    bool x = false;
    ITH_FIELD()
    bool y = true;
    ITH_FIELD()
    bool z = false;
};

// One step that reshapes a mapping's value (section 3.5). Only the block of `type` is read;
// FrameTimeScale has no parameters.
struct InputModifier {
    ITH_TYPE(Struct)
    ITH_FIELD()
    InputModifierType type = InputModifierType::DeadZone;
    ITH_FIELD()
    std::optional<DeadZoneParams> deadZone;
    ITH_FIELD()
    std::optional<ScaleParams> scale;
    ITH_FIELD()
    std::optional<NegateParams> negate;
    ITH_FIELD()
    std::optional<SwizzleParams> swizzle;
    ITH_FIELD()
    std::optional<CurveParams> curve;
    ITH_FIELD()
    std::optional<UserScaleParams> userScale;
    ITH_FIELD()
    std::optional<UserNegateParams> userNegate;
};

enum class InputTriggerType : std::uint8_t {
    Down,
    Pressed,
    Released,
    Hold,
    HoldRelease,
    Tap,
    MultiTap,
    Pulse,
    Chord,
    Combo,
};
ITH_ENUM(InputTriggerType)

// How a trigger combines with the others on one mapping (section 3.6).
enum class TriggerKind : std::uint8_t {
    Explicit, // at least one explicit trigger must be Triggered
    Implicit, // every implicit trigger must be Triggered
    Blocker,  // a Triggered blocker makes the mapping None
};
ITH_ENUM(TriggerKind)

struct HoldParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 0, Unit = "s")
    float seconds = 0.5f;
    // Triggered in one frame, then None until released; otherwise every frame while held.
    ITH_FIELD()
    bool once = true;
};

struct HoldReleaseParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 0, Unit = "s")
    float seconds = 0.5f;
};

struct TapParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 0, Unit = "s")
    float seconds = 0.2f;
};

struct MultiTapParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 1)
    std::uint32_t count = 2;
    ITH_FIELD(Min = 0, Unit = "s")
    float tapSeconds = 0.2f;
    ITH_FIELD(Min = 0, Unit = "s")
    float gapSeconds = 0.3f;
};

struct PulseParams {
    ITH_TYPE(Struct)
    ITH_FIELD(Min = 0, Unit = "s")
    float seconds = 1.0f;
    ITH_FIELD()
    bool atStart = true;
    // The number of pulses before the trigger is None until released; 0 for no limit.
    ITH_FIELD()
    std::uint32_t limit = 0;
};

struct ChordParams {
    ITH_TYPE(Struct)
    // Triggered while this action of the same player is Triggered this frame.
    ITH_FIELD()
    AssetRef action;
};

// Which phase of a combo step's action advances the combo.
enum class ComboStepPhase : std::uint8_t {
    Triggered,
    Completed,
};
ITH_ENUM(ComboStepPhase)

struct ComboStep {
    ITH_TYPE(Struct)
    ITH_FIELD()
    AssetRef action;
    ITH_FIELD()
    ComboStepPhase phase = ComboStepPhase::Triggered;
    // The longest time since the previous step; the first step has no limit.
    ITH_FIELD(Min = 0, Unit = "s")
    float maxSeconds = 0.5f;
};

struct ComboParams {
    ITH_TYPE(Struct)
    ITH_FIELD()
    std::vector<ComboStep> steps;
    // A Triggered phase of any of these actions resets the combo.
    ITH_FIELD()
    std::vector<AssetRef> cancelActions;
};

// A rule that decides when a mapping counts (section 3.6). Only the block of `type` is read;
// Down, Pressed and Released have no parameters.
struct InputTrigger {
    ITH_TYPE(Struct)
    ITH_FIELD()
    InputTriggerType type = InputTriggerType::Down;
    // Empty: the type's default kind (Implicit for Chord, Explicit for the others).
    ITH_FIELD()
    std::optional<TriggerKind> kind;
    // The magnitude at which the value counts as actuated. 0 or less: any non-zero value.
    ITH_FIELD(Min = 0)
    float threshold = 0.5f;
    ITH_FIELD()
    std::optional<HoldParams> hold;
    ITH_FIELD()
    std::optional<HoldReleaseParams> holdRelease;
    ITH_FIELD()
    std::optional<TapParams> tap;
    ITH_FIELD()
    std::optional<MultiTapParams> multiTap;
    ITH_FIELD()
    std::optional<PulseParams> pulse;
    ITH_FIELD()
    std::optional<ChordParams> chord;
    ITH_FIELD()
    std::optional<ComboParams> combo;
};

// The kind a trigger has: its own, or its type's default.
[[nodiscard]] TriggerKind triggerKind(const InputTrigger& trigger) noexcept;

// An input action asset (format "ith.input_action"): one thing a player can do.
struct InputActionAsset {
    ITH_TYPE(Asset)
    // Shown in settings menus and the editor; empty means the asset's name.
    ITH_FIELD()
    std::string displayName;
    ITH_FIELD()
    std::string category;
    ITH_FIELD()
    std::string description;
    ITH_FIELD()
    ActionValueKind valueKind = ActionValueKind::Digital;
    ITH_FIELD()
    ActionCombine combine = ActionCombine::Strongest;
    // After combining, a value longer than 1 is scaled down to length 1.
    ITH_FIELD()
    bool clampToUnit = false;
    // Mappings of this action claim their keys from lower contexts (section 3.7). Empty means
    // true; actions that chords or combos depend on never claim, and setting it on one is a
    // validation warning.
    ITH_FIELD()
    std::optional<bool> consumesKeys;
    // Appended to every mapping's modifiers and triggers.
    ITH_FIELD()
    std::vector<InputModifier> modifiers;
    ITH_FIELD()
    std::vector<InputTrigger> triggers;
};

// One key bound to one action inside a context.
struct InputMapping {
    ITH_TYPE(Struct)
    // The input action; required.
    ITH_FIELD()
    AssetRef action;
    // A stable control name (`Keyboard.Space`, `Gamepad.LeftStick`); required.
    ITH_FIELD()
    std::string key;
    ITH_FIELD()
    std::vector<InputModifier> modifiers;
    ITH_FIELD()
    std::vector<InputTrigger> triggers;
    // The binding slot (`jump.keyboard`); not empty means the mapping is rebindable.
    ITH_FIELD()
    std::string slot;
    // Shown next to the action's name in a settings menu (`Primary`, `Alternate`).
    ITH_FIELD()
    std::string slotLabel;
};

// An input context asset (format "ith.input_context"): the mappings of one situation.
struct InputContextAsset {
    ITH_TYPE(Asset)
    ITH_FIELD()
    std::string displayName;
    ITH_FIELD()
    std::string description;
    ITH_FIELD()
    std::vector<InputMapping> mappings;
};

enum class InputSettingType : std::uint8_t {
    Float,
    Bool,
};
ITH_ENUM(InputSettingType)

// A value players can tune per profile (look sensitivity, invert look), read by the UserScale
// and UserNegate modifiers. Declared in the project's input settings.
struct InputSettingDecl {
    ITH_TYPE(Struct)
    // Stable, project-unique (`look_sensitivity`).
    ITH_FIELD()
    std::string name;
    ITH_FIELD()
    InputSettingType type = InputSettingType::Float;
    ITH_FIELD()
    float defaultFloat = 1.0f;
    ITH_FIELD()
    bool defaultBool = false;
    // The range of a float setting; values set by players are clamped to it.
    ITH_FIELD()
    float min = 0.0f;
    ITH_FIELD()
    float max = 10.0f;
    ITH_FIELD()
    std::string displayName;
};

inline constexpr serialization::DocumentFormat kInputActionFormat{.name = "ith.input_action", .version = 1};
inline constexpr serialization::DocumentFormat kInputContextFormat{.name = "ith.input_context", .version = 1};

} // namespace ith
