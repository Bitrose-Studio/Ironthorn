// The input actions benchmark (docs/specs/input-actions.md, requirement 49): four local players with
// 100 mappings each, evaluated once per frame. The target is 0.1 ms per frame on the Low tier
// reference machine (ADR-0005). Run on a Release build, by hand or as the CTest
// `input_actions_benchmark` (label "benchmark", excluded from normal test runs). It only prints
// numbers and always exits with 0.
//
//   cmake --build build/windows --config Release --target ith_input_actions_benchmark
//   build/windows/bin/Release/ith_input_actions_benchmark
//
// Each player has four contexts of equal priority with 25 mappings each, over 40 actions: key,
// mouse and stick mappings with dead zones, swizzles and scales; Down, Pressed, Hold, Tap and Pulse
// triggers, and chords. Every frame injects key presses and releases, mouse motion and stick motion
// on every player's devices. The time is the median of the frames after a warm-up.

#include "ith/foundation/Stopwatch.h"
#include "ith/input_actions/InputActions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace ith;

constexpr WindowId kWindow = 1;
constexpr int kPlayers = 4;
constexpr int kContexts = 4;
constexpr int kMappingsPerContext = 25;
constexpr int kActions = 40;
constexpr int kFrames = 2000;
constexpr double kTargetMs = 0.1;

std::uint64_t ms(double milliseconds) {
    return static_cast<std::uint64_t>(milliseconds * 1'000'000.0);
}

InputMapping makeMapping(int index, const std::vector<AssetId>& actions) {
    static const std::vector<std::string> kKeys{
        "Keyboard.W",        "Keyboard.A",       "Keyboard.S",           "Keyboard.D",          "Keyboard.Space",
        "Keyboard.E",        "Keyboard.Q",       "Keyboard.R",           "Mouse.Left",          "Mouse.Right",
        "Gamepad.FaceSouth", "Gamepad.FaceEast", "Gamepad.LeftShoulder", "Gamepad.RightTrigger"};
    InputMapping mapping{.action = AssetRef::to(actions[static_cast<std::size_t>(index % kActions)])};
    switch (index % 5) {
        case 0:
            mapping.key = "Gamepad.LeftStick";
            mapping.modifiers = {InputModifier{.type = InputModifierType::DeadZone}};
            break;
        case 1:
            mapping.key = "Mouse.Delta";
            mapping.modifiers = {InputModifier{.type = InputModifierType::Scale, .scale = ScaleParams{.x = 0.5f}}};
            break;
        default:
            mapping.key = kKeys[static_cast<std::size_t>(index) % kKeys.size()];
            mapping.modifiers = {InputModifier{.type = InputModifierType::Swizzle}};
            break;
    }
    static const std::vector<InputTriggerType> kTriggers{InputTriggerType::Down, InputTriggerType::Pressed,
                                                         InputTriggerType::Hold, InputTriggerType::Tap,
                                                         InputTriggerType::Pulse};
    mapping.triggers = {InputTrigger{.type = kTriggers[static_cast<std::size_t>(index / 3) % kTriggers.size()]}};
    if (index % 17 == 0) {
        mapping.triggers.push_back(
            InputTrigger{.type = InputTriggerType::Chord, .chord = ChordParams{.action = AssetRef::to(actions[0])}});
    }
    return mapping;
}

} // namespace

int main() {
    Input input;
    InputAssetLibrary library;
    InputBindingConfig bindings(library);
    InputActions actions(input, bindings);
    World world;
    actions.attachWindow(kWindow);
    input.setLocalMultiplayer(true);

    std::vector<AssetId> actionIds;
    for (int i = 0; i < kActions; ++i) {
        actionIds.push_back(AssetId::generate());
        library.setAction(actionIds.back(), InputActionAsset{.valueKind = ActionValueKind::Axis2});
    }
    InputContexts stack;
    for (int c = 0; c < kContexts; ++c) {
        InputContextAsset context;
        for (int m = 0; m < kMappingsPerContext; ++m) {
            context.mappings.push_back(makeMapping((c * kMappingsPerContext) + m, actionIds));
        }
        const AssetId id = AssetId::generate();
        library.setContext(id, std::move(context));
        // Equal priorities, so every mapping is in the table (section 3.7 step 2).
        stack.contexts.push_back({.context = AssetRef::to(id), .priority = 0});
    }

    // Player 0 has the keyboard and mouse and a gamepad; players 1 to 3 one gamepad each.
    std::vector<Event> setup{Event{.type = EventType::WindowFocusGained, .timestampNs = ms(1), .window = kWindow}};
    for (int p = 0; p < kPlayers; ++p) {
        setup.push_back(Event{.type = EventType::GamepadConnected,
                              .timestampNs = ms(1),
                              .device = static_cast<DeviceId>(100 + p),
                              .text = "Pad"});
    }
    input.processEvents(setup, ms(1));
    input.assignDevice(kKeyboardMouse, 0);
    for (int p = 0; p < kPlayers; ++p) {
        input.assignDevice(static_cast<DeviceId>(100 + p), static_cast<PlayerIndex>(p));
        world.spawn(InputPlayer{.index = static_cast<std::uint8_t>(p)}, stack);
    }

    std::vector<double> samples;
    samples.reserve(kFrames);
    std::size_t events = 0;
    std::vector<Event> frame;
    for (int f = 0; f < kFrames; ++f) {
        const double at = 10.0 + (f * 16.6);
        frame.clear();
        const Key keys[] = {Key::W, Key::A, Key::S, Key::D, Key::Space, Key::E};
        frame.push_back(Event{.type = (f / 6) % 2 == 0 ? EventType::KeyDown : EventType::KeyUp,
                              .timestampNs = ms(at - 2),
                              .window = kWindow,
                              .key = keys[f % 6]});
        frame.push_back(Event{.type = EventType::MouseMoved,
                              .timestampNs = ms(at - 2),
                              .window = kWindow,
                              .deltaX = 2.0f,
                              .deltaY = -1.0f});
        for (int p = 0; p < kPlayers; ++p) {
            const auto pad = static_cast<DeviceId>(100 + p);
            frame.push_back(Event{.type = EventType::GamepadAxisMoved,
                                  .timestampNs = ms(at - 1),
                                  .device = pad,
                                  .gamepadAxis = GamepadAxis::LeftStickX,
                                  .axisValue = static_cast<float>((f + p) % 20) / 20.0f});
            frame.push_back(Event{.type = (f / 4) % 2 == 0 ? EventType::GamepadButtonDown : EventType::GamepadButtonUp,
                                  .timestampNs = ms(at - 1),
                                  .device = pad,
                                  .gamepadButton = (f % 2) == 0 ? GamepadButton::FaceSouth : GamepadButton::FaceEast});
        }
        input.processEvents(frame, ms(at));
        const Stopwatch stopwatch;
        actions.update(world);
        if (f >= 100) { // after the warm-up
            samples.push_back(stopwatch.elapsedMilliseconds());
        }
        events += actions.events().size();
    }
    std::ranges::sort(samples);
    const double median = samples[samples.size() / 2];
    const double p99 = samples[(samples.size() * 99) / 100];
    std::printf("input actions: %d players x %d mappings, %zu events over %d frames\n", kPlayers,
                kContexts * kMappingsPerContext, events, kFrames);
    std::printf("%-40s %9.4f ms   target %6.2f ms   %s\n", "update, median", median, kTargetMs,
                median <= kTargetMs ? "within target" : "ABOVE TARGET");
    std::printf("%-40s %9.4f ms\n", "update, 99th percentile", p99);
    return 0;
}
