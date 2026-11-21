#include "ith/input_actions/InputActions.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ith {
namespace {

// The first position of `id` in `path`. A predicate rather than std::ranges::find: the MSVC
// standard library (14.52 and later) sends std::find over 16-byte trivially comparable values such
// as AssetId to a vectorized search that supports only 1-, 2-, 4- and 8-byte elements, which
// fails to compile under clang-cl ("unexpected size").
std::vector<AssetId>::iterator firstOf(std::vector<AssetId>& path, AssetId id) {
    return std::ranges::find_if(path, [id](AssetId entry) { return entry == id; });
}

// Digital keys of a player: keyboard keys, then mouse buttons, then gamepad buttons.
constexpr std::size_t kMouseButtonBase = kKeyTableSize;
constexpr std::size_t kMouseButtonCount = 8;
constexpr std::size_t kGamepadButtonBase = kMouseButtonBase + kMouseButtonCount;
constexpr std::size_t kDigitalCount = kGamepadButtonBase + kGamepadButtonCount;
// The keyboard and mouse keys, whose holders are tracked for the one keyboard and mouse set.
constexpr std::size_t kKeyboardMouseCount = kGamepadButtonBase;
// The edges a digital key may have waiting (section 3.2).
constexpr std::size_t kMaxQueuedEdges = 8;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;

// The control a mapping reads, resolved for evaluation.
struct KeyRef {
    enum class Kind : std::uint8_t {
        None, // unbound
        Digital,
        GamepadAxis,
        MouseAxis,
        Axis2D,
    };
    Kind kind = Kind::None;
    std::uint16_t index = 0; // digital key, GamepadAxis, MouseAxis or Axis2D value

    friend bool operator==(const KeyRef&, const KeyRef&) = default;
};

KeyRef resolveKey(const Control& control) {
    if (const Key* key = std::get_if<Key>(&control)) {
        return {.kind = KeyRef::Kind::Digital, .index = static_cast<std::uint16_t>(*key)};
    }
    if (const MouseButton* button = std::get_if<MouseButton>(&control)) {
        return {.kind = KeyRef::Kind::Digital,
                .index = static_cast<std::uint16_t>(kMouseButtonBase + static_cast<std::size_t>(*button))};
    }
    if (const GamepadButton* button = std::get_if<GamepadButton>(&control)) {
        return {.kind = KeyRef::Kind::Digital,
                .index = static_cast<std::uint16_t>(kGamepadButtonBase + static_cast<std::size_t>(*button))};
    }
    if (const GamepadAxis* axis = std::get_if<GamepadAxis>(&control)) {
        return {.kind = KeyRef::Kind::GamepadAxis, .index = static_cast<std::uint16_t>(*axis)};
    }
    if (const MouseAxis* axis = std::get_if<MouseAxis>(&control)) {
        return {.kind = KeyRef::Kind::MouseAxis, .index = static_cast<std::uint16_t>(*axis)};
    }
    return {.kind = KeyRef::Kind::Axis2D, .index = static_cast<std::uint16_t>(std::get<Axis2D>(control))};
}

std::uint64_t toNanoseconds(float seconds) {
    return seconds <= 0.0f ? 0 : static_cast<std::uint64_t>(std::llround(static_cast<double>(seconds) * 1e9));
}

// A device event copied from the frame's events: the fields the evaluation needs, never the Event
// (its text points into the platform's frame storage).
struct RawEvent {
    enum class Kind : std::uint8_t {
        Press,
        Release,
        MouseMotion,
        MouseWheel,
        GamepadAxis,
        GamepadDisconnected,
    };
    Kind kind = Kind::Press;
    PlayerIndex player = kNoPlayer;
    DeviceId device = 0;
    std::uint16_t key = 0; // the digital key of Press and Release
    GamepadAxis axis = GamepadAxis::None;
    float x = 0.0f;
    float y = 0.0f;
    // A Release that the player didn't make (section 3.10).
    bool forced = false;
    std::uint64_t timestampNs = 0;
};

// One queued edge of a digital key.
struct Edge {
    std::uint16_t key = 0;
    bool press = false;
    // A release the player didn't make: focus loss or capture loss (section 3.10).
    bool forced = false;
    std::uint64_t timestampNs = 0;
};

struct KeySample {
    bool held = false;
    // Held, but not actuated until pressed again (resume after suspension).
    bool blocked = false;
    // The edge applied this frame, if any, and its timestamp.
    bool pressEdge = false;
    bool releaseEdge = false;
    // The release edge applied this frame is a forced one.
    bool releaseForced = false;
    std::uint64_t edgeTimeNs = 0;
    // The timestamp of the next queued release while held, or 0: holds are measured up to it.
    std::uint64_t releasePendingNs = 0;
};

// A connected gamepad's raw values and which player holds each of its buttons.
struct GamepadValues {
    DeviceId device = 0;
    PlayerIndex owner = kNoPlayer; // refreshed each update from Input
    std::size_t order = 0;         // the position in Input's device set
    std::array<float, kGamepadAxisCount> axes{};
    std::array<PlayerIndex, kGamepadButtonCount> holders{};
};

struct CompiledModifier {
    InputModifierType type = InputModifierType::Scale;
    DeadZoneParams deadZone;
    std::array<float, 3> factors{1.0f, 1.0f, 1.0f}; // Scale factors, Curve exponents
    std::array<bool, 3> selected{true, true, true}; // Negate, UserScale, UserNegate
    SwizzleOrder order = SwizzleOrder::YXZ;
    float userFactor = 1.0f; // UserScale: the setting's value, 1 when undeclared
    bool userFlip = false;   // UserNegate: the setting's value, false when undeclared
};

struct ComboStepRuntime {
    std::int32_t action = -1; // index in the player's actions, -1 when not in the table
    ComboStepPhase phase = ComboStepPhase::Triggered;
    std::uint64_t maxNs = 0;
};

struct TriggerRuntime {
    // Configuration.
    InputTriggerType type = InputTriggerType::Down;
    TriggerKind kind = TriggerKind::Explicit;
    float threshold = 0.5f;
    std::uint64_t secondsNs = 0;
    std::uint64_t gapNs = 0;
    bool once = true;
    bool atStart = true;
    std::uint32_t count = 0;  // MultiTap count, Pulse limit
    std::int32_t action = -1; // Chord
    std::vector<ComboStepRuntime> steps;
    std::vector<std::int32_t> cancels;
    bool disabled = false; // part of a dependency cycle
    // The implicit "value is non-zero" trigger of a mapping without explicit triggers: it gates
    // Triggered (rules 1 and 3) but never makes the mapping Ongoing.
    bool gate = false;
    // State.
    bool prevActuated = false;
    bool armed = false; // the actuation started with a real press, not "already held"
    bool fired = false;
    std::uint64_t sinceNs = 0;
    std::uint64_t markNs = 0;
    std::uint32_t counter = 0;
    std::uint32_t step = 0;
};

struct TriggerResult {
    TriggerState state = TriggerState::None;
    std::uint64_t timeNs = 0;
};

struct MappingRuntime {
    // Identity across rebuilds: context, index in the context, and key.
    AssetId context;
    std::uint32_t mappingIndex = 0;
    KeyRef key;
    Control control;
    std::uint32_t rank = 0; // the context's position in the sorted stack
    std::uint32_t action = 0;
    bool chord = false;  // has a Chord trigger that is not a blocker (the chord rule)
    bool claims = false; // its action claims keys
    std::vector<CompiledModifier> modifiers;
    std::vector<TriggerRuntime> triggers;
    TriggerState state = TriggerState::None;
    TriggerState prevState = TriggerState::None;
    std::uint64_t timeNs = 0;
    ActionValue value;
};

struct ActionRuntime {
    AssetId id;
    ActionValueKind kind = ActionValueKind::Digital;
    ActionCombine combine = ActionCombine::Strongest;
    bool clampToUnit = false;
    bool removed = false;                    // left the table: evaluated as None for one frame, then dropped
    std::uint32_t firstMapping = UINT32_MAX; // set when the table is built
    std::uint32_t lastMapping = 0;
    TriggerState state = TriggerState::None;
    TriggerState prevState = TriggerState::None;
    ActionValue value;
    ActionPhase phases = ActionPhase::None;
    std::uint64_t timeNs = 0;
    std::uint64_t startedAtNs = 0;
    std::optional<std::uint64_t> triggeredSinceNs;
    std::array<std::uint64_t, kActionPhaseCount> phaseTimesNs{};
};

struct Claim {
    KeyRef key;
    AssetId context;
    std::uint32_t mappingIndex = 0;
    std::uint32_t rank = 0;
    // An analog key's claim ends when its magnitude falls below this (section 3.7).
    float restBelow = 0.2f;
};

struct StackEntry {
    AssetId context;
    std::int32_t priority = 0;
    std::uint64_t sequence = 0; // when it was first seen; higher is more recent
};

struct PlayerRuntime {
    Entity entity;
    PlayerIndex index = 0;
    std::array<KeySample, kDigitalCount> keys{};
    std::bitset<kDigitalCount> applied;
    std::vector<Edge> pending;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float wheelX = 0.0f;
    float wheelY = 0.0f;
    InputDeviceFamily lastFamily = InputDeviceFamily::KeyboardMouse;
    bool wasSuspended = false;

    // What the mapping table was built from.
    bool built = false;
    std::vector<InputContextEntry> stack;
    std::vector<StackEntry> known;
    std::uint64_t nextSequence = 1;
    std::string profile;
    std::uint64_t libraryVersion = 0;
    std::uint64_t bindingsVersion = 0;

    std::vector<MappingRuntime> mappings;
    std::vector<ActionRuntime> actions;
    std::vector<Claim> claims;
    std::vector<std::pair<std::uint32_t, KeyRef>> chordBlocked; // per frame: context rank and key
    std::vector<InputContextEntry> scratchStack;
    // Keys whose edge queue overflowed and was reported once.
    std::bitset<kDigitalCount> overflowReported;
};

// The 1D axes a 2D control is made of, as indices of their own KeyRef kinds.
struct AxisPair {
    KeyRef::Kind kind = KeyRef::Kind::None;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};

AxisPair axesOf(Axis2D axis) noexcept {
    switch (axis) {
        case Axis2D::MouseDelta:
            return {.kind = KeyRef::Kind::MouseAxis,
                    .x = static_cast<std::uint16_t>(MouseAxis::DeltaX),
                    .y = static_cast<std::uint16_t>(MouseAxis::DeltaY)};
        case Axis2D::MouseWheel:
            return {.kind = KeyRef::Kind::MouseAxis,
                    .x = static_cast<std::uint16_t>(MouseAxis::WheelX),
                    .y = static_cast<std::uint16_t>(MouseAxis::WheelY)};
        case Axis2D::GamepadLeftStick:
            return {.kind = KeyRef::Kind::GamepadAxis,
                    .x = static_cast<std::uint16_t>(GamepadAxis::LeftStickX),
                    .y = static_cast<std::uint16_t>(GamepadAxis::LeftStickY)};
        case Axis2D::GamepadRightStick:
            return {.kind = KeyRef::Kind::GamepadAxis,
                    .x = static_cast<std::uint16_t>(GamepadAxis::RightStickX),
                    .y = static_cast<std::uint16_t>(GamepadAxis::RightStickY)};
        case Axis2D::None:
            break;
    }
    return {};
}

// Whether a 2D control contains a 1D axis.
bool contains(const KeyRef& stick, const KeyRef& axis) noexcept {
    if (stick.kind != KeyRef::Kind::Axis2D) {
        return false;
    }
    const AxisPair pair = axesOf(static_cast<Axis2D>(stick.index));
    return pair.kind == axis.kind && pair.kind != KeyRef::Kind::None && (axis.index == pair.x || axis.index == pair.y);
}

// Whether two keys are one control for claims: the same key, or a 2D control and one of its axes
// (section 3.7).
[[nodiscard]] bool overlaps(const KeyRef& a, const KeyRef& b) noexcept {
    return a == b || contains(a, b) || contains(b, a);
}

[[nodiscard]] bool isZero(const ActionValue& value) noexcept {
    return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
}

[[nodiscard]] float magnitude(const ActionValue& value, ActionValueKind kind) noexcept {
    switch (kind) {
        case ActionValueKind::Digital:
        case ActionValueKind::Axis1:
            return std::abs(value.x);
        case ActionValueKind::Axis2:
            return std::hypot(value.x, value.y);
        case ActionValueKind::Axis3:
            break;
    }
    return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
}

void truncate(ActionValue& value, ActionValueKind kind) noexcept {
    if (kind == ActionValueKind::Digital || kind == ActionValueKind::Axis1) {
        value.y = 0.0f;
    }
    if (kind != ActionValueKind::Axis3) {
        value.z = 0.0f;
    }
}

float deadZone(float length, const DeadZoneParams& params) noexcept {
    if (length < params.lower) {
        return 0.0f;
    }
    if (length >= params.upper || params.upper <= params.lower) {
        return 1.0f;
    }
    return (length - params.lower) / (params.upper - params.lower);
}

void applyDeadZone(std::array<float, 3>& v, const DeadZoneParams& params) noexcept {
    if (params.shape == DeadZoneShape::Radial) {
        const float length = std::sqrt((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
        const float scale = length > 0.0f ? deadZone(length, params) / length : 0.0f;
        for (float& c : v) {
            c *= scale;
        }
        return;
    }
    for (float& c : v) {
        c = c == 0.0f ? 0.0f : std::copysign(deadZone(std::abs(c), params), c);
    }
}

std::array<float, 3> swizzle(const std::array<float, 3>& in, SwizzleOrder order) noexcept {
    switch (order) {
        case SwizzleOrder::YXZ:
            return {in[1], in[0], in[2]};
        case SwizzleOrder::ZYX:
            return {in[2], in[1], in[0]};
        case SwizzleOrder::XZY:
            return {in[0], in[2], in[1]};
        case SwizzleOrder::YZX:
            return {in[1], in[2], in[0]};
        case SwizzleOrder::ZXY:
            return {in[2], in[0], in[1]};
    }
    return in;
}

ActionValue applyModifier(const CompiledModifier& modifier, ActionValue value, float frameSeconds) noexcept {
    std::array<float, 3> v{value.x, value.y, value.z};
    switch (modifier.type) {
        case InputModifierType::DeadZone:
            applyDeadZone(v, modifier.deadZone);
            break;
        case InputModifierType::Scale:
            for (std::size_t i = 0; i < 3; ++i) {
                v[i] *= modifier.factors[i];
            }
            break;
        case InputModifierType::Negate:
        case InputModifierType::UserNegate:
            // Negate flips the selected components; UserNegate when its setting is true.
            for (std::size_t i = 0; i < 3; ++i) {
                const bool flip =
                    modifier.selected[i] && (modifier.type == InputModifierType::Negate || modifier.userFlip);
                v[i] = flip ? -v[i] : v[i];
            }
            break;
        case InputModifierType::Swizzle:
            v = swizzle(v, modifier.order);
            break;
        case InputModifierType::Curve:
            for (std::size_t i = 0; i < 3; ++i) {
                v[i] = v[i] == 0.0f ? 0.0f : std::copysign(std::pow(std::abs(v[i]), modifier.factors[i]), v[i]);
            }
            break;
        case InputModifierType::FrameTimeScale:
            for (float& c : v) {
                c *= frameSeconds;
            }
            break;
        case InputModifierType::UserScale:
            for (std::size_t i = 0; i < 3; ++i) {
                v[i] = modifier.selected[i] ? v[i] * modifier.userFactor : v[i];
            }
            break;
    }
    return ActionValue{.x = v[0], .y = v[1], .z = v[2]};
}

CompiledModifier compileModifier(const InputModifier& modifier, const InputBindingConfig& bindings,
                                 std::string_view profile) {
    CompiledModifier compiled{.type = modifier.type};
    switch (modifier.type) {
        case InputModifierType::DeadZone:
            compiled.deadZone = modifier.deadZone.value_or(DeadZoneParams{});
            break;
        case InputModifierType::Scale: {
            const ScaleParams params = modifier.scale.value_or(ScaleParams{});
            compiled.factors = {params.x, params.y, params.z};
            break;
        }
        case InputModifierType::Negate: {
            const NegateParams params = modifier.negate.value_or(NegateParams{});
            compiled.selected = {params.x, params.y, params.z};
            break;
        }
        case InputModifierType::Swizzle:
            compiled.order = modifier.swizzle.value_or(SwizzleParams{}).order;
            break;
        case InputModifierType::Curve: {
            const CurveParams params = modifier.curve.value_or(CurveParams{});
            compiled.factors = {params.x, params.y, params.z};
            break;
        }
        case InputModifierType::FrameTimeScale:
            break;
        case InputModifierType::UserScale: {
            const UserScaleParams params = modifier.userScale.value_or(UserScaleParams{});
            compiled.selected = {params.x, params.y, params.z};
            const auto value = bindings.setting(profile, params.setting);
            compiled.userFactor = value && std::holds_alternative<float>(*value) ? std::get<float>(*value) : 1.0f;
            break;
        }
        case InputModifierType::UserNegate: {
            const UserNegateParams params = modifier.userNegate.value_or(UserNegateParams{});
            compiled.selected = {params.x, params.y, params.z};
            const auto value = bindings.setting(profile, params.setting);
            compiled.userFlip = value && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
            break;
        }
    }
    return compiled;
}

// The input of every trigger of one mapping in one frame.
struct TriggerFrame {
    float magnitude = 0.0f;
    // A rise of the value now is a real press: an analog key, or a digital press edge seen by this
    // mapping. Otherwise the key was already held (section 3.6).
    bool realPress = false;
    // A fall of the value now is a release the player made: a real release edge of a key the mapping
    // can see, or an analog value that went down while visible. Forced ends are not (section 3.10).
    bool realRelease = false;
    // When the value changed this frame: the digital edge's timestamp, or now.
    std::uint64_t transitionNs = 0;
    // How far a hold is measured: now, or the timestamp of the key's queued release.
    std::uint64_t heldUntilNs = 0;
    std::uint64_t nowNs = 0;
};

// The time from `from` to `to`, 0 when `to` is earlier: the frame's time can be earlier than the
// timestamps of the previous frame's events.
[[nodiscard]] std::uint64_t timeBetween(std::uint64_t from, std::uint64_t to) noexcept {
    return to > from ? to - from : 0;
}

// Whether a combo step's action shows the step's phase this frame. A Triggered step counts only in
// the first frame of a run of Triggered frames, so actions that stay Triggered while held work
// as steps (section 3.6).
[[nodiscard]] bool showsPhase(const ActionRuntime& action, ComboStepPhase phase, std::uint64_t& at) noexcept {
    const ActionPhase wanted = phase == ComboStepPhase::Triggered ? ActionPhase::Triggered : ActionPhase::Completed;
    if ((action.phases & wanted) == ActionPhase::None) {
        return false;
    }
    if (phase == ComboStepPhase::Triggered && action.prevState == TriggerState::Triggered) {
        return false;
    }
    at = action.phaseTimesNs[phaseIndex(wanted)];
    return true;
}

TriggerResult evaluateCombo(TriggerRuntime& t, std::span<const ActionRuntime> actions, std::uint64_t now) {
    if (t.disabled || t.steps.empty()) {
        return {.state = TriggerState::None, .timeNs = now};
    }
    for (const std::int32_t cancel : t.cancels) {
        if (cancel >= 0 &&
            (actions[static_cast<std::size_t>(cancel)].phases & ActionPhase::Triggered) != ActionPhase::None) {
            t.step = 0;
            return {.state = TriggerState::None, .timeNs = now};
        }
    }
    if (t.step > 0 && timeBetween(t.markNs, now) > t.steps[t.step].maxNs) {
        const std::uint64_t at = t.markNs + t.steps[t.step].maxNs;
        t.step = 0;
        return {.state = TriggerState::None, .timeNs = at};
    }
    const auto shows = [&](const ComboStepRuntime& step, std::uint64_t& at) {
        return step.action >= 0 && showsPhase(actions[static_cast<std::size_t>(step.action)], step.phase, at);
    };
    std::uint64_t at = now;
    const ComboStepRuntime& current = t.steps[t.step];
    if (shows(current, at)) {
        t.markNs = at;
        if (++t.step == t.steps.size()) {
            t.step = 0;
            return {.state = TriggerState::Triggered, .timeNs = at};
        }
        return {.state = TriggerState::Ongoing, .timeNs = at};
    }
    for (std::size_t j = 0; j < t.steps.size(); ++j) {
        if (j != t.step && t.steps[j].action != current.action && shows(t.steps[j], at)) {
            // Out of order: start again, from this step if it is the first.
            t.step = 0;
            if (t.steps.size() > 1 && shows(t.steps[0], at)) {
                t.step = 1;
                t.markNs = at;
                return {.state = TriggerState::Ongoing, .timeNs = at};
            }
            return {.state = TriggerState::None, .timeNs = now};
        }
    }
    return {.state = t.step > 0 ? TriggerState::Ongoing : TriggerState::None, .timeNs = now};
}

[[nodiscard]] TriggerResult resultOf(TriggerState state, std::uint64_t timeNs) noexcept {
    return {.state = state, .timeNs = timeNs};
}

// What one trigger sees of its mapping's value in one frame, after the actuation edges are applied.
struct TriggerStep {
    const TriggerFrame& frame;
    bool actuated = false;
    bool rising = false;
    bool falling = false;
    // A fall that is a release the player made, not a forced end (section 3.10).
    bool released = false;
    // When the actuation changed this frame (the rise, the fall), or now.
    std::uint64_t changeNs = 0;

    [[nodiscard]] TriggerResult none() const { return {.state = TriggerState::None, .timeNs = changeNs}; }
};

[[nodiscard]] std::uint64_t elapsedSince(const TriggerRuntime& t, std::uint64_t at) noexcept {
    return at > t.sinceNs ? at - t.sinceNs : 0;
}

TriggerResult evaluateHold(TriggerRuntime& t, const TriggerStep& s) {
    if (!s.actuated) {
        return s.none();
    }
    if (elapsedSince(t, s.frame.heldUntilNs) < t.secondsNs) {
        return resultOf(TriggerState::Ongoing, s.changeNs);
    }
    if (!t.fired) {
        t.fired = true;
        return resultOf(TriggerState::Triggered, t.sinceNs + t.secondsNs);
    }
    return resultOf(t.once ? TriggerState::None : TriggerState::Triggered, s.frame.nowNs);
}

TriggerResult evaluateTap(TriggerRuntime& t, const TriggerStep& s) {
    if (s.actuated) {
        if (t.armed && elapsedSince(t, s.frame.heldUntilNs) <= t.secondsNs) {
            return resultOf(TriggerState::Ongoing, s.changeNs);
        }
        const bool expiredNow = t.armed;
        t.armed = false;
        return resultOf(TriggerState::None, expiredNow ? t.sinceNs + t.secondsNs : s.frame.nowNs);
    }
    if (s.released && t.armed && elapsedSince(t, s.frame.transitionNs) <= t.secondsNs) {
        return resultOf(TriggerState::Triggered, s.frame.transitionNs);
    }
    return s.none();
}

TriggerResult evaluateMultiTapRelease(TriggerRuntime& t, const TriggerStep& s) {
    if (!s.released || !t.armed || elapsedSince(t, s.frame.transitionNs) > t.secondsNs) {
        t.counter = 0;
        return s.none();
    }
    t.markNs = s.frame.transitionNs;
    if (++t.counter >= t.count) {
        t.counter = 0;
        return resultOf(TriggerState::Triggered, s.frame.transitionNs);
    }
    return resultOf(TriggerState::Ongoing, s.frame.transitionNs);
}

TriggerResult evaluateMultiTap(TriggerRuntime& t, const TriggerStep& s) {
    if (s.rising) {
        if (!t.armed) {
            t.counter = 0;
            return s.none();
        }
        if (t.counter > 0 && timeBetween(t.markNs, t.sinceNs) > t.gapNs) {
            t.counter = 0; // too late for the previous taps: this press starts again
        }
        return resultOf(TriggerState::Ongoing, t.sinceNs);
    }
    if (s.actuated) {
        if (!t.armed) {
            return s.none();
        }
        if (elapsedSince(t, s.frame.heldUntilNs) > t.secondsNs) {
            t.armed = false;
            t.counter = 0;
            return resultOf(TriggerState::None, t.sinceNs + t.secondsNs);
        }
        return resultOf(TriggerState::Ongoing, s.frame.nowNs);
    }
    if (s.falling) {
        return evaluateMultiTapRelease(t, s);
    }
    if (t.counter == 0) {
        return s.none();
    }
    if (timeBetween(t.markNs, s.frame.nowNs) > t.gapNs) {
        t.counter = 0;
        return resultOf(TriggerState::None, t.markNs + t.gapNs);
    }
    return resultOf(TriggerState::Ongoing, s.frame.nowNs);
}

TriggerResult evaluatePulse(TriggerRuntime& t, const TriggerStep& s) {
    if (!s.actuated) {
        return s.none();
    }
    if (s.rising) {
        t.counter = t.atStart ? 1 : 0;
        t.markNs = t.sinceNs + t.secondsNs;
        return resultOf(t.atStart ? TriggerState::Triggered : TriggerState::Ongoing, t.sinceNs);
    }
    if (t.count != 0 && t.counter >= t.count) {
        return resultOf(TriggerState::None, s.frame.nowNs);
    }
    if (s.frame.heldUntilNs >= t.markNs) {
        // Every interval that ended since the last frame counts; the phase carries the last
        // crossing, so a pulse shorter than a frame doesn't fall behind.
        const std::uint64_t interval = std::max<std::uint64_t>(t.secondsNs, 1);
        std::uint64_t crossings = ((s.frame.heldUntilNs - t.markNs) / interval) + 1;
        if (t.count != 0) {
            crossings = std::min<std::uint64_t>(crossings, t.count - t.counter);
        }
        const std::uint64_t at = t.markNs + ((crossings - 1) * interval);
        t.counter += static_cast<std::uint32_t>(crossings);
        t.markNs = at + interval;
        return resultOf(TriggerState::Triggered, at);
    }
    return resultOf(TriggerState::Ongoing, s.frame.nowNs);
}

TriggerResult evaluateTrigger(TriggerRuntime& t, const TriggerFrame& f, std::span<const ActionRuntime> actions) {
    if (t.type == InputTriggerType::Chord) {
        const bool on = !t.disabled && t.action >= 0 &&
                        actions[static_cast<std::size_t>(t.action)].state == TriggerState::Triggered;
        return {.state = on ? TriggerState::Triggered : TriggerState::None, .timeNs = f.nowNs};
    }
    if (t.type == InputTriggerType::Combo) {
        return evaluateCombo(t, actions, f.nowNs);
    }

    TriggerStep s{.frame = f, .changeNs = f.nowNs};
    s.actuated = t.threshold > 0.0f ? f.magnitude >= t.threshold : f.magnitude > 0.0f;
    s.rising = s.actuated && !t.prevActuated;
    s.falling = !s.actuated && t.prevActuated;
    s.released = s.falling && f.realRelease;
    t.prevActuated = s.actuated;
    if (s.rising) {
        t.sinceNs = f.realPress ? f.transitionNs : f.nowNs;
        t.armed = f.realPress;
        t.fired = false;
        s.changeNs = t.sinceNs;
    } else if (s.falling) {
        s.changeNs = f.transitionNs;
    }

    switch (t.type) {
        case InputTriggerType::Down:
            return s.actuated ? resultOf(TriggerState::Triggered, s.changeNs) : s.none();
        case InputTriggerType::Pressed:
            return s.rising && t.armed ? resultOf(TriggerState::Triggered, t.sinceNs) : s.none();
        case InputTriggerType::Released:
            if (s.actuated) {
                return resultOf(TriggerState::Ongoing, s.changeNs);
            }
            return s.released ? resultOf(TriggerState::Triggered, f.transitionNs) : s.none();
        case InputTriggerType::Hold:
            return evaluateHold(t, s);
        case InputTriggerType::HoldRelease:
            if (s.actuated) {
                return resultOf(TriggerState::Ongoing, s.changeNs);
            }
            return s.released && elapsedSince(t, f.transitionNs) >= t.secondsNs
                       ? resultOf(TriggerState::Triggered, f.transitionNs)
                       : s.none();
        case InputTriggerType::Tap:
            return evaluateTap(t, s);
        case InputTriggerType::MultiTap:
            return evaluateMultiTap(t, s);
        case InputTriggerType::Pulse:
            return evaluatePulse(t, s);
        case InputTriggerType::Chord:
        case InputTriggerType::Combo:
            break;
    }
    return s.none();
}

TriggerRuntime compileTrigger(const InputTrigger& trigger) {
    TriggerRuntime t{.type = trigger.type, .kind = triggerKind(trigger), .threshold = trigger.threshold};
    switch (trigger.type) {
        case InputTriggerType::Hold: {
            const HoldParams params = trigger.hold.value_or(HoldParams{});
            t.secondsNs = toNanoseconds(params.seconds);
            t.once = params.once;
            break;
        }
        case InputTriggerType::HoldRelease:
            t.secondsNs = toNanoseconds(trigger.holdRelease.value_or(HoldReleaseParams{}).seconds);
            break;
        case InputTriggerType::Tap:
            t.secondsNs = toNanoseconds(trigger.tap.value_or(TapParams{}).seconds);
            break;
        case InputTriggerType::MultiTap: {
            const MultiTapParams params = trigger.multiTap.value_or(MultiTapParams{});
            t.count = std::max<std::uint32_t>(params.count, 1);
            t.secondsNs = toNanoseconds(params.tapSeconds);
            t.gapNs = toNanoseconds(params.gapSeconds);
            break;
        }
        case InputTriggerType::Pulse: {
            const PulseParams params = trigger.pulse.value_or(PulseParams{});
            t.secondsNs = toNanoseconds(params.seconds);
            t.atStart = params.atStart;
            t.count = params.limit;
            break;
        }
        case InputTriggerType::Down:
        case InputTriggerType::Pressed:
        case InputTriggerType::Released:
        case InputTriggerType::Chord:
        case InputTriggerType::Combo:
            break;
    }
    return t;
}

// The phases of an action from its previous and current state (section 3.8).
ActionPhase phasesFor(TriggerState previous, TriggerState current) noexcept {
    switch (current) {
        case TriggerState::None:
            if (previous == TriggerState::Ongoing) {
                return ActionPhase::Canceled;
            }
            return previous == TriggerState::Triggered ? ActionPhase::Completed : ActionPhase::None;
        case TriggerState::Ongoing:
            return previous == TriggerState::None ? ActionPhase::Started | ActionPhase::Ongoing : ActionPhase::Ongoing;
        case TriggerState::Triggered:
            return previous == TriggerState::None ? ActionPhase::Started | ActionPhase::Triggered
                                                  : ActionPhase::Triggered;
    }
    return ActionPhase::None;
}

constexpr std::array<ActionPhase, kActionPhaseCount> kPhaseOrder{
    ActionPhase::Started, ActionPhase::Ongoing, ActionPhase::Triggered, ActionPhase::Completed, ActionPhase::Canceled};

// A context of a player's stack, in table order.
struct SortedContext {
    AssetId id;
    std::int32_t priority = 0;
    std::uint64_t sequence = 0;
    const InputContextAsset* asset = nullptr;
};

// One mapping chosen for the table: its context, the context's rank, and its index there.
struct Pick {
    const SortedContext* context = nullptr;
    std::uint32_t rank = 0;
    std::uint32_t index = 0;
};

// What a mapping table is built from (section 3.7).
struct TablePlan {
    std::vector<SortedContext> contexts;
    // The actions in order of their first mapping in context order, and the priority of the
    // context their mappings come from.
    std::vector<AssetId> appearance;
    std::map<AssetId, std::int32_t> chosen;
    // Chord and Combo dependencies between actions of the table.
    std::map<AssetId, std::set<AssetId>> dependencies;
    // The actions others depend on, each after its own dependencies.
    std::vector<AssetId> dependencyOrder;
    std::set<AssetId> isDependency;
    std::set<AssetId> onCycle;
    std::vector<Pick> picks;

    // Whether `mapping` of `context` is in the table: its action's mappings come from contexts of
    // this priority.
    [[nodiscard]] bool included(const SortedContext& context, const InputMapping& mapping) const {
        const auto found = chosen.find(mapping.action.assetId());
        return found != chosen.end() && found->second == context.priority;
    }

    [[nodiscard]] std::uint32_t rankOf(const SortedContext& context) const {
        return static_cast<std::uint32_t>(&context - contexts.data());
    }
};

// The actions a list of triggers refers to through Chord and Combo triggers.
std::vector<AssetId> referencedActions(const InputTrigger& trigger) {
    std::vector<AssetId> refs;
    if (trigger.type == InputTriggerType::Chord) {
        refs.push_back(trigger.chord.value_or(ChordParams{}).action.assetId());
    } else if (trigger.type == InputTriggerType::Combo) {
        const ComboParams params = trigger.combo.value_or(ComboParams{});
        for (const ComboStep& step : params.steps) {
            refs.push_back(step.action.assetId());
        }
        for (const AssetRef& cancel : params.cancelActions) {
            refs.push_back(cancel.assetId());
        }
    }
    return refs;
}

void addDependencies(TablePlan& plan, AssetId owner, const std::vector<InputTrigger>& triggers) {
    for (const InputTrigger& trigger : triggers) {
        for (const AssetId ref : referencedActions(trigger)) {
            if (plan.chosen.contains(ref)) {
                plan.dependencies[owner].insert(ref);
            }
        }
    }
}

// Orders the actions others depend on, each after its own dependencies, and finds the actions on
// a cycle, by depth-first search.
void orderDependencies(TablePlan& plan) {
    enum class Mark : std::uint8_t {
        Active,
        Done,
    };
    std::map<AssetId, Mark> marks;
    std::vector<AssetId> path;
    const auto visit = [&](const auto& self, AssetId id) -> void {
        marks[id] = Mark::Active;
        path.push_back(id);
        const auto found = plan.dependencies.find(id);
        for (const AssetId next : found != plan.dependencies.end() ? found->second : std::set<AssetId>{}) {
            const auto mark = marks.find(next);
            if (mark == marks.end()) {
                self(self, next);
            } else if (mark->second == Mark::Active) {
                plan.onCycle.insert(firstOf(path, next), path.end());
            }
        }
        path.pop_back();
        marks[id] = Mark::Done;
        plan.dependencyOrder.push_back(id);
    };
    for (const AssetId action : plan.appearance) {
        const auto found = plan.dependencies.find(action);
        for (const AssetId used : found != plan.dependencies.end() ? found->second : std::set<AssetId>{}) {
            if (!marks.contains(used)) {
                visit(visit, used);
            }
        }
    }
    plan.isDependency.insert(plan.dependencyOrder.begin(), plan.dependencyOrder.end());
}

// Whether a mapping has a Chord trigger that is not a blocker, its action's included (the chord
// rule of section 3.7).
bool hasChord(const InputAssetLibrary& library, const InputMapping& mapping) {
    const auto isChord = [](const InputTrigger& trigger) {
        return trigger.type == InputTriggerType::Chord && triggerKind(trigger) != TriggerKind::Blocker;
    };
    const InputActionAsset* action = library.findAction(mapping.action.assetId());
    return std::ranges::any_of(mapping.triggers, isChord) ||
           (action != nullptr && std::ranges::any_of(action->triggers, isChord));
}

// The table's mappings: dependencies first, then context order, mappings with a chord first within
// a context, otherwise declaration order (section 3.7 steps 3 and 4).
void pickMappings(TablePlan& plan, const InputAssetLibrary& library) {
    for (const AssetId dependency : plan.dependencyOrder) {
        for (const SortedContext& context : plan.contexts) {
            for (std::uint32_t i = 0; i < context.asset->mappings.size(); ++i) {
                const InputMapping& mapping = context.asset->mappings[i];
                if (mapping.action.assetId() == dependency && plan.included(context, mapping)) {
                    plan.picks.push_back({.context = &context, .rank = plan.rankOf(context), .index = i});
                }
            }
        }
    }
    for (const SortedContext& context : plan.contexts) {
        for (const bool chordPass : {true, false}) {
            for (std::uint32_t i = 0; i < context.asset->mappings.size(); ++i) {
                const InputMapping& mapping = context.asset->mappings[i];
                if (!plan.isDependency.contains(mapping.action.assetId()) && plan.included(context, mapping) &&
                    hasChord(library, mapping) == chordPass) {
                    plan.picks.push_back({.context = &context, .rank = plan.rankOf(context), .index = i});
                }
            }
        }
    }
}

// The index of an action in the player's table, or -1.
std::int32_t actionIndex(const PlayerRuntime& player, AssetId id) {
    for (std::size_t i = 0; i < player.actions.size(); ++i) {
        if (player.actions[i].id == id) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

// Resolves the actions a Chord or Combo trigger refers to; a trigger whose owner and referenced
// action are on one cycle (`onCycle` is the cycle set when the owner is on it) is disabled.
void linkTrigger(TriggerRuntime& compiled, const InputTrigger& trigger, const PlayerRuntime& player,
                 const std::set<AssetId>* onCycle) {
    const auto cyclic = [onCycle](AssetId ref) { return onCycle != nullptr && onCycle->contains(ref); };
    if (trigger.type == InputTriggerType::Chord) {
        const AssetId ref = trigger.chord.value_or(ChordParams{}).action.assetId();
        compiled.action = actionIndex(player, ref);
        compiled.disabled = cyclic(ref);
    } else if (trigger.type == InputTriggerType::Combo) {
        const ComboParams params = trigger.combo.value_or(ComboParams{});
        for (const ComboStep& step : params.steps) {
            compiled.steps.push_back(ComboStepRuntime{.action = actionIndex(player, step.action.assetId()),
                                                      .phase = step.phase,
                                                      .maxNs = toNanoseconds(step.maxSeconds)});
            compiled.disabled = compiled.disabled || cyclic(step.action.assetId());
        }
        for (const AssetRef& cancel : params.cancelActions) {
            compiled.cancels.push_back(actionIndex(player, cancel.assetId()));
        }
    }
}

// Keeps the state of a mapping that stays in the table: same context, index and key.
void carryOver(MappingRuntime& runtime, const std::vector<MappingRuntime>& oldMappings) {
    const auto old = std::ranges::find_if(oldMappings, [&runtime](const MappingRuntime& other) {
        return other.context == runtime.context && other.mappingIndex == runtime.mappingIndex &&
               other.key == runtime.key;
    });
    if (old == oldMappings.end()) {
        return; // new: its triggers start unactuated, so a held key counts as already held
    }
    runtime.state = old->state;
    runtime.value = old->value;
    if (old->triggers.size() != runtime.triggers.size()) {
        return;
    }
    for (std::size_t i = 0; i < runtime.triggers.size(); ++i) {
        TriggerRuntime& trigger = runtime.triggers[i];
        const TriggerRuntime& before = old->triggers[i];
        if (before.type == trigger.type) {
            trigger.prevActuated = before.prevActuated;
            trigger.armed = before.armed;
            trigger.fired = before.fired;
            trigger.sinceNs = before.sinceNs;
            trigger.markNs = before.markNs;
            trigger.counter = before.counter;
            trigger.step = before.step < trigger.steps.size() ? before.step : 0;
        }
    }
}

// Claims end early when their mapping left the table (section 3.7); the others take its new rank.
void remapClaims(PlayerRuntime& player) {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < player.claims.size(); ++i) {
        const Claim claim = player.claims[i];
        const auto mapping = std::ranges::find_if(player.mappings, [&claim](const MappingRuntime& m) {
            return m.context == claim.context && m.mappingIndex == claim.mappingIndex && m.key == claim.key;
        });
        if (mapping != player.mappings.end()) {
            player.claims[kept] = claim;
            player.claims[kept].rank = mapping->rank;
            ++kept;
        }
    }
    player.claims.resize(kept);
}

// Combines the triggers of one mapping (section 3.6 rules 2 to 5) into its state, with the time of
// the trigger that decided it.
TriggerResult combineTriggers(std::vector<TriggerRuntime>& triggers, const TriggerFrame& frame,
                              std::span<const ActionRuntime> actions) {
    bool blocked = false;
    bool allImplicit = true;
    std::optional<std::uint64_t> triggeredTime;
    std::optional<std::uint64_t> activeTime;
    std::uint64_t noneTime = 0;
    for (TriggerRuntime& trigger : triggers) {
        const TriggerResult result = evaluateTrigger(trigger, frame, actions);
        if (trigger.kind == TriggerKind::Blocker) {
            blocked = blocked || result.state == TriggerState::Triggered;
            continue;
        }
        allImplicit = allImplicit && (trigger.kind != TriggerKind::Implicit || result.state == TriggerState::Triggered);
        if (trigger.kind == TriggerKind::Explicit) {
            if (result.state == TriggerState::Triggered && !triggeredTime.has_value()) {
                triggeredTime = result.timeNs;
            }
            noneTime = std::max(noneTime, result.timeNs);
        }
        if (result.state != TriggerState::None && !activeTime.has_value() && !trigger.gate) {
            activeTime = result.timeNs;
        }
    }
    if (blocked) {
        return {.state = TriggerState::None, .timeNs = frame.nowNs};
    }
    if (allImplicit && triggeredTime.has_value()) {
        return {.state = TriggerState::Triggered, .timeNs = *triggeredTime};
    }
    if (activeTime.has_value()) {
        return {.state = TriggerState::Ongoing, .timeNs = *activeTime};
    }
    return {.state = TriggerState::None, .timeNs = noneTime != 0 ? noneTime : frame.nowNs};
}

// Where an analog claim of `mapping` ends: below the lowest actuation threshold of its triggers, or
// the default dead zone of 0.2 when it has none (section 3.7).
float restThreshold(const MappingRuntime& mapping) noexcept {
    float lowest = -1.0f;
    for (const TriggerRuntime& trigger : mapping.triggers) {
        if (!trigger.gate) {
            const float threshold = std::max(trigger.threshold, 0.0f);
            lowest = lowest < 0.0f ? threshold : std::min(lowest, threshold);
        }
    }
    return lowest < 0.0f ? 0.2f : lowest;
}

// The value of an action from its mappings that are not None (section 3.4).
ActionValue combineValues(const PlayerRuntime& player, const ActionRuntime& action, std::uint32_t index) {
    std::array<float, 3> combined{};
    std::array<float, 3> largest{};
    for (std::uint32_t m = action.firstMapping; m <= action.lastMapping; ++m) {
        const MappingRuntime& mapping = player.mappings[m];
        if (mapping.action != index || mapping.state == TriggerState::None) {
            continue;
        }
        const std::array<float, 3> v{mapping.value.x, mapping.value.y, mapping.value.z};
        for (std::size_t c = 0; c < 3; ++c) {
            if (action.combine == ActionCombine::Sum) {
                combined[c] += v[c];
            } else if (std::abs(v[c]) > largest[c]) {
                largest[c] = std::abs(v[c]);
                combined[c] = v[c];
            } else if (std::abs(v[c]) == largest[c] && v[c] != combined[c]) {
                combined[c] = 0.0f; // opposite values that tie cancel out
            }
        }
    }
    ActionValue value{.x = combined[0], .y = combined[1], .z = combined[2]};
    truncate(value, action.kind);
    if (const float length = magnitude(value, action.kind); action.clampToUnit && length > 1.0f) {
        value.x /= length;
        value.y /= length;
        value.z /= length;
    }
    if (action.kind == ActionValueKind::Digital) {
        value = ActionValue{.x = isZero(value) ? 0.0f : 1.0f};
    }
    return value;
}

} // namespace

struct InputActions::State final : InputConsumer {
    Input* input = nullptr;
    const InputBindingConfig* bindings = nullptr;
    std::vector<std::pair<WindowId, ConsumerId>> windows;

    std::vector<RawEvent> rawEvents;
    std::array<PlayerIndex, kKeyboardMouseCount> keyboardHolders{};
    std::vector<GamepadValues> gamepads;
    std::vector<std::unique_ptr<PlayerRuntime>> players;
    std::vector<std::pair<PlayerIndex, Entity>> scratchPlayers;
    std::vector<ActionEvent> events;
    std::vector<Error> messages;
    std::set<std::string> reported;
    std::uint64_t lastNowNs = 0;
    bool firstUpdate = true;
    bool wasCaptured = false;
    // Player indices with more than one entity, reported once.
    std::bitset<256> duplicateReported;

    State() {
        keyboardHolders.fill(kNoPlayer);
        rawEvents.reserve(256);
        events.reserve(256);
        scratchPlayers.reserve(8);
    }

    // Records a problem once per `cause`.
    void report(const std::string& cause, std::string code, std::string message) {
        if (reported.insert(cause).second) {
            messages.push_back(Error{.code = std::move(code), .message = std::move(message)});
        }
    }

    InputReply onInputEvent(const Event& event) override {
        RawEvent copy{.timestampNs = event.timestampNs};
        switch (event.type) {
            case EventType::KeyDown:
            case EventType::KeyUp:
                if (event.repeat || event.key == Key::None || static_cast<std::size_t>(event.key) >= kKeyTableSize) {
                    return InputReply::NotConsumed;
                }
                copy.kind = event.type == EventType::KeyDown ? RawEvent::Kind::Press : RawEvent::Kind::Release;
                copy.key = static_cast<std::uint16_t>(event.key);
                copy.device = kKeyboardMouse;
                break;
            case EventType::MouseButtonDown:
            case EventType::MouseButtonUp:
                if (event.mouseButton == MouseButton::None ||
                    static_cast<std::size_t>(event.mouseButton) >= kMouseButtonCount) {
                    return InputReply::NotConsumed;
                }
                copy.kind = event.type == EventType::MouseButtonDown ? RawEvent::Kind::Press : RawEvent::Kind::Release;
                copy.key = static_cast<std::uint16_t>(kMouseButtonBase + static_cast<std::size_t>(event.mouseButton));
                copy.device = kKeyboardMouse;
                break;
            case EventType::MouseMoved:
            case EventType::MouseWheel:
                copy.kind =
                    event.type == EventType::MouseMoved ? RawEvent::Kind::MouseMotion : RawEvent::Kind::MouseWheel;
                copy.device = kKeyboardMouse;
                copy.x = event.deltaX;
                copy.y = event.deltaY;
                break;
            case EventType::GamepadButtonDown:
            case EventType::GamepadButtonUp:
                if (event.gamepadButton == GamepadButton::None ||
                    static_cast<std::size_t>(event.gamepadButton) >= kGamepadButtonCount) {
                    return InputReply::NotConsumed;
                }
                copy.kind =
                    event.type == EventType::GamepadButtonDown ? RawEvent::Kind::Press : RawEvent::Kind::Release;
                copy.key =
                    static_cast<std::uint16_t>(kGamepadButtonBase + static_cast<std::size_t>(event.gamepadButton));
                copy.device = event.device;
                break;
            case EventType::GamepadAxisMoved:
                if (event.gamepadAxis == GamepadAxis::None ||
                    static_cast<std::size_t>(event.gamepadAxis) >= kGamepadAxisCount) {
                    return InputReply::NotConsumed;
                }
                copy.kind = RawEvent::Kind::GamepadAxis;
                copy.device = event.device;
                copy.axis = event.gamepadAxis;
                copy.x = event.axisValue;
                break;
            case EventType::GamepadDisconnected:
                copy.kind = RawEvent::Kind::GamepadDisconnected;
                copy.device = event.device;
                rawEvents.push_back(copy);
                return InputReply::NotConsumed;
            default:
                return InputReply::NotConsumed; // text, hotplug connects and window events are not read
        }
        copy.player = input->playerOf(copy.device);
        // Releases go to their holder even after the device was unassigned, and axis values are
        // kept for every gamepad, so a gamepad assigned again doesn't bring back a stale value;
        // reading filters them by owner.
        if (copy.player == kNoPlayer && copy.kind != RawEvent::Kind::Release) {
            if (copy.kind == RawEvent::Kind::GamepadAxis) {
                rawEvents.push_back(copy);
            }
            return InputReply::NotConsumed;
        }
        rawEvents.push_back(copy);
        return InputReply::Consumed;
    }

    PlayerRuntime* findPlayer(PlayerIndex index) {
        for (const std::unique_ptr<PlayerRuntime>& player : players) {
            if (player->index == index) {
                return player.get();
            }
        }
        return nullptr;
    }

    GamepadValues& gamepad(DeviceId device) {
        for (GamepadValues& values : gamepads) {
            if (values.device == device) {
                return values;
            }
        }
        GamepadValues& added = gamepads.emplace_back();
        added.device = device;
        added.holders.fill(kNoPlayer);
        return added;
    }

    void pushEdge(PlayerIndex player, std::uint16_t key, bool press, std::uint64_t timestampNs, bool forced = false) {
        PlayerRuntime* runtime = findPlayer(player);
        if (runtime == nullptr) {
            return;
        }
        std::vector<Edge>& pending = runtime->pending;
        pending.push_back(Edge{.key = key, .press = press, .forced = forced, .timestampNs = timestampNs});
        const auto count = std::ranges::count_if(pending, [key](const Edge& edge) { return edge.key == key; });
        if (std::cmp_less_equal(count, kMaxQueuedEdges)) {
            return;
        }
        // Drop the oldest complete press-release pair of this key.
        const auto firstPress =
            std::ranges::find_if(pending, [key](const Edge& edge) { return edge.key == key && edge.press; });
        const auto release =
            std::find_if(firstPress, pending.end(), [key](const Edge& edge) { return edge.key == key && !edge.press; });
        if (firstPress != pending.end() && release != pending.end()) {
            pending.erase(release);
            pending.erase(firstPress);
        }
#ifndef ITH_SHIPPING
        // Once per player and key: a flooded key must not grow the messages every frame.
        if (!runtime->overflowReported.test(key)) {
            runtime->overflowReported.set(key);
            messages.push_back(Error{.code = "input_actions.edge_queue_full",
                                     .message = "more than 8 edges of key " + std::to_string(key) +
                                                " are queued for player " + std::to_string(player) +
                                                "; the oldest press and release are dropped"});
        }
#endif
    }

    // A digital key went down or up on a device of `player`.
    void digitalEdge(const RawEvent& event) {
        const bool press = event.kind == RawEvent::Kind::Press;
        if (event.key < kKeyboardMouseCount) {
            PlayerIndex& holder = keyboardHolders[event.key];
            if (press) {
                if (holder == kNoPlayer && event.player != kNoPlayer) {
                    holder = event.player;
                    pushEdge(holder, event.key, true, event.timestampNs);
                }
            } else if (holder != kNoPlayer) {
                pushEdge(holder, event.key, false, event.timestampNs, event.forced);
                holder = kNoPlayer;
            }
            return;
        }
        const std::size_t button = event.key - kGamepadButtonBase;
        GamepadValues& values = gamepad(event.device);
        const auto heldElsewhere = [&](PlayerIndex player) {
            return std::ranges::any_of(gamepads, [&](const GamepadValues& other) {
                return other.device != event.device && other.holders[button] == player;
            });
        };
        PlayerIndex& holder = values.holders[button];
        if (press) {
            if (holder == kNoPlayer && event.player != kNoPlayer) {
                holder = event.player;
                if (!heldElsewhere(holder)) {
                    pushEdge(holder, event.key, true, event.timestampNs);
                }
            }
        } else if (holder != kNoPlayer) {
            const PlayerIndex player = holder;
            holder = kNoPlayer;
            if (!heldElsewhere(player)) {
                pushEdge(player, event.key, false, event.timestampNs, event.forced);
            }
        }
    }

    // Whether a release is one of those Input makes when a window loses focus: they carry the
    // focus event's timestamp, and the focus event follows them in the frame's events.
    [[nodiscard]] bool isFocusLossRelease(const RawEvent& release) const {
        return std::ranges::any_of(input->events(), [&release](const Event& event) {
            return event.type == EventType::WindowFocusLost && event.timestampNs == release.timestampNs;
        });
    }

    void dispatchRawEvents() {
        for (const RawEvent& event : rawEvents) {
            PlayerRuntime* player = event.player != kNoPlayer ? findPlayer(event.player) : nullptr;
            switch (event.kind) {
                case RawEvent::Kind::Press:
                    digitalEdge(event);
                    break;
                case RawEvent::Kind::Release: {
                    RawEvent release = event;
                    release.forced = isFocusLossRelease(event);
                    digitalEdge(release);
                    break;
                }
                case RawEvent::Kind::MouseMotion:
                case RawEvent::Kind::MouseWheel:
                    if (player != nullptr) {
                        float& x = event.kind == RawEvent::Kind::MouseMotion ? player->mouseDeltaX : player->wheelX;
                        float& y = event.kind == RawEvent::Kind::MouseMotion ? player->mouseDeltaY : player->wheelY;
                        x += event.x;
                        y += event.y;
                        if (std::abs(x) > 0.5f || std::abs(y) > 0.5f) {
                            player->lastFamily = InputDeviceFamily::KeyboardMouse;
                        }
                    }
                    break;
                case RawEvent::Kind::GamepadAxis:
                    gamepad(event.device).axes[static_cast<std::size_t>(event.axis)] = event.x;
                    if (player != nullptr && std::abs(event.x) > 0.5f) {
                        player->lastFamily = InputDeviceFamily::Gamepad;
                    }
                    break;
                case RawEvent::Kind::GamepadDisconnected: {
                    // Input releases the buttons and resets the axes first; this covers what is left.
                    GamepadValues& values = gamepad(event.device);
                    for (std::size_t button = 0; button < kGamepadButtonCount; ++button) {
                        if (values.holders[button] != kNoPlayer) {
                            digitalEdge(RawEvent{.kind = RawEvent::Kind::Release,
                                                 .device = event.device,
                                                 .key = static_cast<std::uint16_t>(kGamepadButtonBase + button),
                                                 .timestampNs = event.timestampNs});
                        }
                    }
                    std::erase_if(gamepads, [&](const GamepadValues& other) { return other.device == event.device; });
                    break;
                }
            }
        }
        rawEvents.clear();
    }

    // Applies at most one queued edge per key, in order; later edges wait for the next frames.
    static void applyEdges(PlayerRuntime& player) {
        player.applied.reset();
        for (KeySample& key : player.keys) {
            key.pressEdge = false;
            key.releaseEdge = false;
            key.releaseForced = false;
            key.releasePendingNs = 0;
        }
        std::size_t kept = 0;
        for (const Edge& edge : player.pending) {
            if (player.applied.test(edge.key)) {
                player.pending[kept++] = edge;
                continue;
            }
            KeySample& key = player.keys[edge.key];
            if (edge.press == key.held) {
                continue; // a press of a held key or a release of a free one changes nothing
            }
            player.applied.set(edge.key);
            key.held = edge.press;
            key.blocked = false;
            key.edgeTimeNs = edge.timestampNs;
            (edge.press ? key.pressEdge : key.releaseEdge) = true;
            key.releaseForced = !edge.press && edge.forced;
            if (edge.press) {
                player.lastFamily =
                    edge.key >= kGamepadButtonBase ? InputDeviceFamily::Gamepad : InputDeviceFamily::KeyboardMouse;
            }
        }
        player.pending.resize(kept);
        for (const Edge& edge : player.pending) {
            KeySample& key = player.keys[edge.key];
            if (key.held && !edge.press && key.releasePendingNs == 0) {
                key.releasePendingNs = edge.timestampNs;
            }
        }
    }

    // The raw value of a key for `player`, as a vector (section 3.2).
    [[nodiscard]] ActionValue sample(const PlayerRuntime& player, const KeyRef& key) const {
        switch (key.kind) {
            case KeyRef::Kind::None:
                return {};
            case KeyRef::Kind::Digital: {
                const KeySample& sample = player.keys[key.index];
                return {.x = sample.held && !sample.blocked ? 1.0f : 0.0f};
            }
            case KeyRef::Kind::GamepadAxis:
                return {.x = gamepadAxis(player, static_cast<std::size_t>(key.index))};
            case KeyRef::Kind::MouseAxis:
                switch (static_cast<MouseAxis>(key.index)) {
                    case MouseAxis::DeltaX:
                        return {.x = player.mouseDeltaX};
                    case MouseAxis::DeltaY:
                        return {.x = player.mouseDeltaY};
                    case MouseAxis::WheelX:
                        return {.x = player.wheelX};
                    case MouseAxis::WheelY:
                        return {.x = player.wheelY};
                    case MouseAxis::None:
                        break;
                }
                return {};
            case KeyRef::Kind::Axis2D:
                switch (static_cast<Axis2D>(key.index)) {
                    case Axis2D::MouseDelta:
                        return {.x = player.mouseDeltaX, .y = player.mouseDeltaY};
                    case Axis2D::MouseWheel:
                        return {.x = player.wheelX, .y = player.wheelY};
                    case Axis2D::GamepadLeftStick:
                        return stick(player, GamepadAxis::LeftStickX, GamepadAxis::LeftStickY);
                    case Axis2D::GamepadRightStick:
                        return stick(player, GamepadAxis::RightStickX, GamepadAxis::RightStickY);
                    case Axis2D::None:
                        break;
                }
                return {};
        }
        return {};
    }

    // The value with the largest magnitude over the player's gamepads.
    [[nodiscard]] float gamepadAxis(const PlayerRuntime& player, std::size_t axis) const {
        float best = 0.0f;
        for (const GamepadValues& values : gamepads) {
            if (values.owner == player.index && std::abs(values.axes[axis]) > std::abs(best)) {
                best = values.axes[axis];
            }
        }
        return best;
    }

    // The longest stick over the player's gamepads.
    [[nodiscard]] ActionValue stick(const PlayerRuntime& player, GamepadAxis x, GamepadAxis y) const {
        ActionValue best;
        float bestLength = 0.0f;
        for (const GamepadValues& values : gamepads) {
            const float vx = values.axes[static_cast<std::size_t>(x)];
            const float vy = values.axes[static_cast<std::size_t>(y)];
            const float length = std::hypot(vx, vy);
            if (values.owner == player.index && length > bestLength) {
                bestLength = length;
                best = {.x = vx, .y = vy};
            }
        }
        return best;
    }

    // Collects the player's stack: its own contexts, then those of the entity it controls; a context
    // listed twice keeps one entry with the later priority. Returns whether it changed.
    static bool gatherStack(World& world, PlayerRuntime& player, const InputPlayer& component) {
        std::vector<InputContextEntry>& stack = player.scratchStack;
        stack.clear();
        const auto add = [&stack](const std::vector<InputContextEntry>& entries) {
            for (const InputContextEntry& entry : entries) {
                const auto found = std::ranges::find_if(
                    stack, [&entry](const InputContextEntry& other) { return other.context == entry.context; });
                if (found != stack.end()) {
                    found->priority = entry.priority;
                } else if (entry.context.isSet()) {
                    stack.push_back(entry);
                }
            }
        };
        if (const InputContexts* own = world.get<InputContexts>(player.entity)) {
            add(own->contexts);
        }
        if (const ProvidedInputContexts* provided =
                component.controlled != Entity() && world.isAlive(component.controlled)
                    ? world.get<ProvidedInputContexts>(component.controlled)
                    : nullptr) {
            add(provided->contexts);
        }
        if (stack == player.stack) {
            return false;
        }
        player.stack.swap(stack);
        return true;
    }

    void sortStack(PlayerRuntime& player, TablePlan& plan);
    void chooseContexts(TablePlan& plan);
    std::optional<Control> mappingKey(const PlayerRuntime& player, const SortedContext& context, std::uint32_t index);
    MappingRuntime compileMapping(const PlayerRuntime& player, const TablePlan& plan, const Pick& pick);
    void rebuild(PlayerRuntime& player);
    void evaluatePlayer(World& world, PlayerRuntime& player, std::uint64_t now, float frameSeconds);
    void evaluateMapping(PlayerRuntime& player, MappingRuntime& mapping, bool suspended, std::uint64_t now,
                         float frameSeconds) const;
    static void combineAction(PlayerRuntime& player, ActionRuntime& action, std::uint64_t now);
};

void InputActions::State::sortStack(PlayerRuntime& player, TablePlan& plan) {
    // Forget the contexts that left the stack, so one added again counts as added now.
    std::erase_if(player.known, [&player](const StackEntry& known) {
        return std::ranges::none_of(player.stack, [&known](const InputContextEntry& entry) {
            return entry.context.assetId() == known.context;
        });
    });
    for (const InputContextEntry& entry : player.stack) {
        const AssetId id = entry.context.assetId();
        auto known = std::ranges::find_if(player.known, [id](const StackEntry& other) { return other.context == id; });
        if (known == player.known.end()) {
            player.known.push_back(StackEntry{.context = id, .sequence = player.nextSequence++});
            known = player.known.end() - 1;
        }
        const InputContextAsset* asset = bindings->library().findContext(id);
        if (asset == nullptr) {
            report("context:" + id.toString(), "input_actions.missing_context",
                   "the input context " + id.toString() + " is not loaded; it is left out of the stack");
            continue;
        }
        plan.contexts.push_back({.id = id, .priority = entry.priority, .sequence = known->sequence, .asset = asset});
    }
    // By priority, the most recently added first among equals.
    std::ranges::sort(plan.contexts, [](const SortedContext& a, const SortedContext& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.sequence > b.sequence;
    });
}

void InputActions::State::chooseContexts(TablePlan& plan) {
    // One context per action: the priority of the highest context that maps it (section 3.7 step 2).
    const InputAssetLibrary& library = bindings->library();
    for (const SortedContext& context : plan.contexts) {
        for (std::size_t i = 0; i < context.asset->mappings.size(); ++i) {
            const AssetId action = context.asset->mappings[i].action.assetId();
            if (library.findAction(action) == nullptr) {
                report("action:" + context.id.toString() + ":" + std::to_string(i), "input_actions.missing_action",
                       "a mapping of the context " + context.id.toString() + " refers to the action " +
                           action.toString() + ", which is not loaded; the mapping is dropped");
            } else if (plan.chosen.try_emplace(action, context.priority).second) {
                plan.appearance.push_back(action);
            }
        }
    }
    for (const AssetId action : plan.appearance) {
        addDependencies(plan, action, library.findAction(action)->triggers);
    }
    for (const SortedContext& context : plan.contexts) {
        for (const InputMapping& mapping : context.asset->mappings) {
            if (plan.included(context, mapping)) {
                addDependencies(plan, mapping.action.assetId(), mapping.triggers);
            }
        }
    }
    orderDependencies(plan);
    if (!plan.onCycle.empty()) {
        std::string list;
        for (const AssetId id : plan.onCycle) {
            list += (list.empty() ? "" : ", ") + id.toString();
        }
        report("cycle:" + list, "input_actions.dependency_cycle",
               "the chords or combos of the actions " + list +
                   " depend on each other through the player's contexts; those triggers report None");
    }
}

std::optional<Control> InputActions::State::mappingKey(const PlayerRuntime& player, const SortedContext& context,
                                                       std::uint32_t index) {
    const InputMapping& mapping = context.asset->mappings[index];
    if (!mapping.slot.empty()) {
        return bindings->key(player.profile, mapping.slot);
    }
    std::optional<Control> control = findControl(mapping.key);
    if (!control.has_value()) {
        report("key:" + context.id.toString() + ":" + std::to_string(index), "input_actions.unknown_key",
               "a mapping of the context " + context.id.toString() + " names the unknown key '" + mapping.key +
                   "'; it reads as not actuated");
    }
    return control;
}

MappingRuntime InputActions::State::compileMapping(const PlayerRuntime& player, const TablePlan& plan,
                                                   const Pick& pick) {
    const InputMapping& mapping = pick.context->asset->mappings[pick.index];
    const AssetId actionId = mapping.action.assetId();
    const InputActionAsset& asset = *bindings->library().findAction(actionId);
    const std::optional<Control> control = mappingKey(player, *pick.context, pick.index);
    MappingRuntime runtime{.context = pick.context->id,
                           .mappingIndex = pick.index,
                           .key = control ? resolveKey(*control) : KeyRef{},
                           .control = control.value_or(Control{}),
                           .rank = pick.rank,
                           .action = static_cast<std::uint32_t>(actionIndex(player, actionId)),
                           .chord = hasChord(bindings->library(), mapping),
                           .claims = asset.consumesKeys.value_or(true) && !plan.isDependency.contains(actionId)};
    for (const std::vector<InputModifier>* list : {&mapping.modifiers, &asset.modifiers}) {
        for (const InputModifier& modifier : *list) {
            runtime.modifiers.push_back(compileModifier(modifier, *bindings, player.profile));
        }
    }
    const bool ownerOnCycle = plan.onCycle.contains(actionId);
    for (const std::vector<InputTrigger>* list : {&mapping.triggers, &asset.triggers}) {
        for (const InputTrigger& trigger : *list) {
            runtime.triggers.push_back(compileTrigger(trigger));
            linkTrigger(runtime.triggers.back(), trigger, player, ownerOnCycle ? &plan.onCycle : nullptr);
        }
    }
    if (std::ranges::none_of(runtime.triggers,
                             [](const TriggerRuntime& t) { return t.kind == TriggerKind::Explicit; })) {
        // Without explicit triggers the mapping is Triggered only while its value is non-zero
        // (section 3.6 rule 1), also when implicit triggers are Triggered, so a chord needs its own key.
        runtime.triggers.push_back(TriggerRuntime{.type = InputTriggerType::Down, .threshold = 0.0f, .gate = true});
    }
    return runtime;
}

void InputActions::State::rebuild(PlayerRuntime& player) {
    const InputAssetLibrary& library = bindings->library();
    std::vector<MappingRuntime> oldMappings;
    std::vector<ActionRuntime> oldActions;
    oldMappings.swap(player.mappings);
    oldActions.swap(player.actions);
    std::erase_if(oldActions, [](const ActionRuntime& action) { return action.removed; });

    TablePlan plan;
    sortStack(player, plan);
    chooseContexts(plan);
    pickMappings(plan, library);

    // Actions in order of their first mapping, keeping the state of those that stay.
    for (const Pick& pick : plan.picks) {
        const AssetId id = pick.context->asset->mappings[pick.index].action.assetId();
        if (actionIndex(player, id) < 0) {
            const InputActionAsset& asset = *library.findAction(id);
            ActionRuntime action{
                .id = id, .kind = asset.valueKind, .combine = asset.combine, .clampToUnit = asset.clampToUnit};
            if (const auto old = std::ranges::find_if(oldActions, [id](const ActionRuntime& a) { return a.id == id; });
                old != oldActions.end()) {
                action.state = old->state;
                action.startedAtNs = old->startedAtNs;
                action.triggeredSinceNs = old->triggeredSinceNs;
            }
            player.actions.push_back(action);
        }
    }

    // The mappings, keeping the state of those that stay.
    for (const Pick& pick : plan.picks) {
        MappingRuntime runtime = compileMapping(player, plan, pick);
        carryOver(runtime, oldMappings);
        ActionRuntime& owner = player.actions[runtime.action];
        const auto position = static_cast<std::uint32_t>(player.mappings.size());
        owner.firstMapping = std::min(owner.firstMapping, position);
        owner.lastMapping = position;
        player.mappings.push_back(std::move(runtime));
    }
    remapClaims(player);

    // Actions that left the table end this frame (section 3.8).
    for (const ActionRuntime& old : oldActions) {
        if (actionIndex(player, old.id) < 0) {
            ActionRuntime removed = old;
            removed.removed = true;
            player.actions.push_back(removed);
        }
    }
    player.built = true;
}

void InputActions::State::evaluateMapping(PlayerRuntime& player, MappingRuntime& mapping, bool suspended,
                                          std::uint64_t now, float frameSeconds) const {
    const ActionRuntime& action = player.actions[mapping.action];
    const ActionValue raw = sample(player, mapping.key);
    const bool claimed = std::ranges::any_of(player.claims, [&mapping](const Claim& claim) {
        return overlaps(claim.key, mapping.key) && claim.rank < mapping.rank;
    });
    const bool chordBlocked =
        !mapping.chord && std::ranges::any_of(player.chordBlocked, [&mapping](const auto& blocked) {
            return blocked.first == mapping.rank && overlaps(blocked.second, mapping.key);
        });
    const bool visible = !suspended && !claimed && !chordBlocked && mapping.key.kind != KeyRef::Kind::None;

    ActionValue value = visible ? raw : ActionValue{};
    for (const CompiledModifier& modifier : mapping.modifiers) {
        value = applyModifier(modifier, value, frameSeconds);
    }
    truncate(value, action.kind);

    TriggerFrame frame{.magnitude = magnitude(value, action.kind),
                       .realPress = true,
                       .realRelease = visible,
                       .transitionNs = now,
                       .heldUntilNs = now,
                       .nowNs = now};
    if (mapping.key.kind == KeyRef::Kind::Digital) {
        const KeySample& key = player.keys[mapping.key.index];
        frame.realPress = visible && key.pressEdge;
        frame.realRelease = visible && key.releaseEdge && !key.releaseForced;
        if (key.pressEdge || key.releaseEdge) {
            frame.transitionNs = key.edgeTimeNs;
        }
        if (key.releasePendingNs != 0) {
            frame.heldUntilNs = std::min(now, key.releasePendingNs);
        }
    }

    const TriggerResult result = combineTriggers(mapping.triggers, frame, player.actions);
    mapping.prevState = mapping.state;
    mapping.state = result.state;
    mapping.timeNs = result.timeNs;
    mapping.value = value;

    // Claims and the chord rule (section 3.7).
    if (mapping.state == TriggerState::None) {
        return;
    }
    // An analog key is claimed only while it is at or above where its claim would end, so a claim
    // that just ended on rest isn't taken again by the same small value.
    const float restBelow = restThreshold(mapping);
    const bool claimable = mapping.key.kind == KeyRef::Kind::Digital
                               ? !isZero(raw)
                               : std::sqrt((raw.x * raw.x) + (raw.y * raw.y)) >= std::max(restBelow, 1e-6f);
    if (mapping.claims && claimable && std::ranges::none_of(player.claims, [&mapping](const Claim& claim) {
            return overlaps(claim.key, mapping.key);
        })) {
        player.claims.push_back(Claim{.key = mapping.key,
                                      .context = mapping.context,
                                      .mappingIndex = mapping.mappingIndex,
                                      .rank = mapping.rank,
                                      .restBelow = restBelow});
    }
    if (mapping.chord) {
        player.chordBlocked.emplace_back(mapping.rank, mapping.key);
    }
}

void InputActions::State::combineAction(PlayerRuntime& player, ActionRuntime& action, std::uint64_t now) {
    const auto index = static_cast<std::uint32_t>(&action - player.actions.data());
    action.prevState = action.state;
    action.state = TriggerState::None;
    std::uint64_t endTime = 0;
    bool stateFound = false;
    if (!action.removed) {
        for (std::uint32_t m = action.firstMapping; m <= action.lastMapping; ++m) {
            const MappingRuntime& mapping = player.mappings[m];
            if (mapping.action != index) {
                continue;
            }
            action.state = std::max(action.state, mapping.state);
            if (mapping.prevState != TriggerState::None && mapping.state == TriggerState::None) {
                endTime = std::max(endTime, mapping.timeNs);
            }
        }
        // The phase time is that of the first mapping in the action's state.
        for (std::uint32_t m = action.firstMapping; m <= action.lastMapping && !stateFound; ++m) {
            const MappingRuntime& mapping = player.mappings[m];
            if (action.state != TriggerState::None && mapping.action == index && mapping.state == action.state) {
                stateFound = true;
                action.timeNs = mapping.timeNs;
            }
        }
    }
    if (!stateFound) {
        action.timeNs = endTime != 0 ? endTime : now;
    }
    action.value = action.removed ? ActionValue{} : combineValues(player, action, index);
    action.phases = phasesFor(action.prevState, action.state);
    action.phaseTimesNs = {};
    for (const ActionPhase phase : kPhaseOrder) {
        if ((action.phases & phase) != ActionPhase::None) {
            action.phaseTimesNs[phaseIndex(phase)] = action.timeNs;
        }
    }
    if ((action.phases & ActionPhase::Started) != ActionPhase::None) {
        action.startedAtNs = action.timeNs;
    }
    if (action.state != TriggerState::Triggered) {
        action.triggeredSinceNs.reset();
    } else if (action.prevState != TriggerState::Triggered) {
        action.triggeredSinceNs = action.timeNs;
    }
}

void InputActions::State::evaluatePlayer(World& world, PlayerRuntime& player, std::uint64_t now, float frameSeconds) {
    auto* out = world.get<ActionStates>(player.entity);
    const auto* component = world.get<InputPlayer>(player.entity);
    if (out == nullptr || component == nullptr) {
        return;
    }

    // Actions that left the table last frame are gone now.
    std::erase_if(player.actions, [](const ActionRuntime& action) { return action.removed; });

    // 1. Samples.
    applyEdges(player);
    std::erase_if(player.claims, [this, &player](const Claim& claim) {
        // A claim lasts until its key is released; an analog key's until it rests below the
        // claiming mapping's threshold (section 3.7).
        if (claim.key.kind == KeyRef::Kind::Digital) {
            return !player.keys[claim.key.index].held;
        }
        const ActionValue value = sample(player, claim.key);
        const float length = std::sqrt((value.x * value.x) + (value.y * value.y));
        return length == 0.0f || length < claim.restBelow;
    });

    // 2. Changes.
    const InputProfile* profile = world.get<InputProfile>(player.entity);
    const std::string_view profileName = profile != nullptr ? std::string_view(profile->name) : kDefaultBindingProfile;
    bool changed = gatherStack(world, player, *component);
    if (!player.built || player.profile != profileName || player.libraryVersion != bindings->library().version() ||
        player.bindingsVersion != bindings->version()) {
        player.profile = profileName;
        player.libraryVersion = bindings->library().version();
        player.bindingsVersion = bindings->version();
        changed = true;
    }
    if (changed) {
        rebuild(player);
    }

    // Suspension: on resume, digital keys still held count only once pressed again.
    const bool suspended = out->suspended;
    if (player.wasSuspended && !suspended) {
        for (KeySample& key : player.keys) {
            key.blocked = key.held && !key.pressEdge;
        }
    }
    player.wasSuspended = suspended;

    // 3 and 4. Mappings in table order; each action combines after its last mapping.
    player.chordBlocked.clear();
    for (std::uint32_t m = 0; m < player.mappings.size(); ++m) {
        evaluateMapping(player, player.mappings[m], suspended, now, frameSeconds);
        ActionRuntime& action = player.actions[player.mappings[m].action];
        if (action.lastMapping == m) {
            combineAction(player, action, now);
        }
    }
    for (ActionRuntime& action : player.actions) {
        if (action.removed) {
            combineAction(player, action, now);
        }
    }

    // Output: the action states and the events.
    out->lastFamily = player.lastFamily;
    out->actions.resize(player.actions.size());
    for (std::size_t i = 0; i < player.actions.size(); ++i) {
        const ActionRuntime& action = player.actions[i];
        ActionState& state = out->actions[i];
        state.action = action.id.uuid();
        state.value = action.value;
        state.state = action.state;
        state.phases = action.phases;
        state.startedAtNs = action.startedAtNs;
        state.triggeredSinceNs = action.triggeredSinceNs;
        state.phaseTimesNs = action.phaseTimesNs;
        for (const ActionPhase phase : kPhaseOrder) {
            if ((action.phases & phase) != ActionPhase::None) {
                events.push_back(ActionEvent{.player = player.entity,
                                             .action = action.id,
                                             .phase = phase,
                                             .value = action.value,
                                             .timestampNs = action.timeNs});
            }
        }
    }

    // Per-frame sums start again.
    player.mouseDeltaX = 0.0f;
    player.mouseDeltaY = 0.0f;
    player.wheelX = 0.0f;
    player.wheelY = 0.0f;
}

InputActions::InputActions(Input& input, const InputBindingConfig& bindings) : m_state(std::make_unique<State>()) {
    m_state->input = &input;
    m_state->bindings = &bindings;
}

InputActions::~InputActions() {
    for (const auto& [window, consumer] : m_state->windows) {
        m_state->input->removeConsumer(consumer);
    }
}

void InputActions::attachWindow(WindowId window) {
    ITH_ASSERT(window != kNoWindow);
    if (std::ranges::any_of(m_state->windows, [window](const auto& entry) { return entry.first == window; })) {
        return;
    }
    const ConsumerId consumer =
        m_state->input->addConsumer({.window = window, .layer = ConsumerLayer::Game, .consumer = m_state.get()});
    m_state->windows.emplace_back(window, consumer);
}

void InputActions::detachWindow(WindowId window) {
    const auto found =
        std::ranges::find_if(m_state->windows, [window](const auto& entry) { return entry.first == window; });
    if (found != m_state->windows.end()) {
        m_state->input->removeConsumer(found->second);
        m_state->windows.erase(found);
    }
}

void InputActions::update(World& world) {
    State& state = *m_state;
    const std::uint64_t now = state.input->frameTimeNs();
    const float frameSeconds = state.firstUpdate || now <= state.lastNowNs
                                   ? 0.0f
                                   : static_cast<float>(static_cast<double>(now - state.lastNowNs) /
                                                        static_cast<double>(kNanosecondsPerSecond));
    state.firstUpdate = false;
    state.lastNowNs = now;
    state.events.clear();

    // Local players, by index.
    state.scratchPlayers.clear();
    world.query<const InputPlayer>().forEach([&state](Entity entity, const InputPlayer& player) {
        state.scratchPlayers.emplace_back(player.index, entity);
    });
    std::ranges::sort(state.scratchPlayers, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.bits() < b.second.bits();
    });
    // One player per index: a second entity with the same index is reported once and not evaluated.
    for (std::size_t i = 1; i < state.scratchPlayers.size(); ++i) {
        const PlayerIndex index = state.scratchPlayers[i].first;
        if (index == state.scratchPlayers[i - 1].first && !state.duplicateReported.test(index)) {
            state.duplicateReported.set(index);
            state.messages.push_back(Error{.code = "input_actions.duplicate_player",
                                           .message = "more than one entity has InputPlayer index " +
                                                      std::to_string(index) + "; only the first is evaluated"});
        }
    }
    const auto duplicates = std::ranges::unique(state.scratchPlayers, {}, &std::pair<PlayerIndex, Entity>::first);
    state.scratchPlayers.erase(duplicates.begin(), duplicates.end());
    for (const auto& [index, entity] : state.scratchPlayers) {
        if (!world.has<ActionStates>(entity)) {
            world.add(entity, ActionStates{});
        }
    }
    std::vector<std::unique_ptr<PlayerRuntime>>& players = state.players;
    for (std::size_t i = 0; i < state.scratchPlayers.size(); ++i) {
        const auto [index, entity] = state.scratchPlayers[i];
        const auto found = std::find_if(players.begin() + static_cast<std::ptrdiff_t>(i), players.end(),
                                        [entity](const auto& player) { return player->entity == entity; });
        if (found != players.end()) {
            std::iter_swap(players.begin() + static_cast<std::ptrdiff_t>(i), found);
        } else {
            auto created = std::make_unique<PlayerRuntime>();
            created->entity = entity;
            created->pending.reserve(64);
            created->claims.reserve(16);
            created->chordBlocked.reserve(16);
            players.insert(players.begin() + static_cast<std::ptrdiff_t>(i), std::move(created));
        }
        players[i]->index = index;
    }
    players.resize(state.scratchPlayers.size());

    // Device events, then the end of the mouse capture.
    for (GamepadValues& values : state.gamepads) {
        values.owner = state.input->playerOf(values.device);
    }
    state.dispatchRawEvents();
    const bool captured = state.input->mouseCaptureWindow() != kNoWindow;
    if (state.wasCaptured && !captured) {
        for (std::size_t button = 0; button < kMouseButtonCount; ++button) {
            const auto key = static_cast<std::uint16_t>(kMouseButtonBase + button);
            if (state.keyboardHolders[key] != kNoPlayer) {
                state.pushEdge(state.keyboardHolders[key], key, false, now, true);
                state.keyboardHolders[key] = kNoPlayer;
            }
        }
    }
    state.wasCaptured = captured;
    // Owners, and the order of the device set, so ties between a player's gamepads go to the
    // first one (section 3.9).
    const std::span<const InputDevice> devices = state.input->devices();
    for (GamepadValues& values : state.gamepads) {
        values.owner = state.input->playerOf(values.device);
        const auto position = std::ranges::find(devices, values.device, &InputDevice::id);
        values.order = static_cast<std::size_t>(position - devices.begin());
    }
    std::ranges::sort(state.gamepads, {}, &GamepadValues::order);

    for (const std::unique_ptr<PlayerRuntime>& player : players) {
        state.evaluatePlayer(world, *player, now, frameSeconds);
    }
}

std::span<const ActionEvent> InputActions::events() const {
    return m_state->events;
}

std::vector<Error> InputActions::takeMessages() {
    std::vector<Error> messages;
    messages.swap(m_state->messages);
    return messages;
}

std::vector<Control> InputActions::keysFor(Entity player, AssetId action,
                                           std::optional<InputDeviceFamily> family) const {
    std::vector<Control> keys;
    for (const std::unique_ptr<PlayerRuntime>& runtime : m_state->players) {
        if (runtime->entity != player) {
            continue;
        }
        // Context order: by rank, then declaration order.
        std::vector<const MappingRuntime*> mappings;
        for (const MappingRuntime& mapping : runtime->mappings) {
            if (runtime->actions[mapping.action].id == action && mapping.key.kind != KeyRef::Kind::None &&
                (!family.has_value() || deviceFamily(mapping.control) == *family)) {
                mappings.push_back(&mapping);
            }
        }
        std::ranges::stable_sort(mappings, [](const MappingRuntime* a, const MappingRuntime* b) {
            return a->rank != b->rank ? a->rank < b->rank : a->mappingIndex < b->mappingIndex;
        });
        for (const MappingRuntime* mapping : mappings) {
            if (std::ranges::find(keys, mapping->control) == keys.end()) {
                keys.push_back(mapping->control);
            }
        }
    }
    return keys;
}

} // namespace ith
