#pragma once

// Input routing (ADR-0013): the device registry, per-frame state, consumer stacks with
// consumption, focus, mouse capture and per-player device assignment.
//
//   ith::Input input;
//   const ith::ConsumerId game = input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Game});
//   while (running) {
//       input.processEvents(platform.pumpEvents(), platform.pumpTimeNs());
//       window.setRelativeMouseMode(input.mouseCaptured(window.id()));
//       const ith::InputState& state = input.consumerState(game);
//       if (state.pressed(ith::Key::Space)) { jump(); }
//   }
//
// Input consumes the engine's events (ith/platform_events) and never calls the platform layer:
// the application applies what Input decides to its windows (relative mouse mode for capture,
// text input for the focused text field). That keeps Input testable without a display and lets it
// link into every process kind (ADR-0017); a dedicated server simply has no events to give it.
//
// Threading: an Input is used from one thread, the one that pumps events (not thread-safe).

#include "ith/input/InputState.h"
#include "ith/platform_events/Event.h"

#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ith {

// The device id that stands for every keyboard and mouse together: they count as one device set
// (ADR-0013 rule 12). Gamepad ids are never 0.
inline constexpr DeviceId kKeyboardMouse = 0;

// A local player: 0 is player 1.
using PlayerIndex = std::uint8_t;
// No player: an unassigned device, or a consumer that isn't bound to a player.
inline constexpr PlayerIndex kNoPlayer = 0xFF;

enum class InputDeviceKind : std::uint8_t {
    KeyboardMouse,
    Gamepad,
};

// A connected device.
struct InputDevice {
    DeviceId id = kKeyboardMouse;
    InputDeviceKind kind = InputDeviceKind::KeyboardMouse;
    // UTF-8, from the device; "Keyboard and mouse" for the keyboard and mouse set.
    std::string name;
    GamepadType gamepadType = GamepadType::Unknown;
    GamepadGuid gamepadGuid;
    std::uint64_t steamHandle = 0;
    // The player it is assigned to, or kNoPlayer.
    PlayerIndex player = kNoPlayer;
};

// Identifies a consumer of an Input. Never reused by that Input.
using ConsumerId = std::uint32_t;
inline constexpr ConsumerId kNoConsumer = 0;

// The layers of a window's consumer stack, top first (ADR-0013 rule 10). Within a layer, the
// consumer added last is on top (a modal UI layer is added after the HUD).
enum class ConsumerLayer : std::uint8_t {
    Editor, // the console, debug overlays and editor keys; a game can never take their input
    Ui,     // the focused widget, then UI layers from modal down to HUD
    Game,   // the players' input, e.g. the action and mapping plugin
};

enum class InputReply : std::uint8_t {
    NotConsumed,
    Consumed,
};

// Receives the events routed to it. Implementations decide per event whether lower consumers may
// see it.
class InputConsumer {
public:
    InputConsumer() = default;
    InputConsumer(const InputConsumer&) = default;
    InputConsumer& operator=(const InputConsumer&) = default;
    InputConsumer(InputConsumer&&) = default;
    InputConsumer& operator=(InputConsumer&&) = default;
    virtual ~InputConsumer() = default;

    // Called during Input::processEvents for each event that reaches this consumer, in order,
    // after the consumer's state (Input::consumerState) has applied it. Returning Consumed on a
    // press hides that key or button from every lower consumer until its release; on motion,
    // wheel or an axis it hides that event. The reply to releases, text, and gamepad connects and
    // disconnects is ignored: those go to every consumer they are meant for.
    //
    // It may call any Input function except processEvents, including adding and removing
    // consumers (itself too) and capturing the mouse.
    virtual InputReply onInputEvent(const Event& event) = 0;
};

struct ConsumerDesc {
    // The window whose stack it joins.
    WindowId window = kNoWindow;
    ConsumerLayer layer = ConsumerLayer::Game;
    // Called for its events, or nullptr for a consumer that only reads its state and never
    // consumes. Not owned: it must outlive the registration (removeConsumer).
    InputConsumer* consumer = nullptr;
    // A consumer bound to a player sees only the events of that player's devices; kNoPlayer sees
    // every device's events, including those of unassigned devices (for joining).
    PlayerIndex player = kNoPlayer;
};

// Owns the input of an application. Created by the application and passed to its users.
//
// Each frame, processEvents takes the platform's events and:
// - keeps the device registry and the all-device state();
// - releases every held key and button, and sets every non-zero gamepad axis to 0, with real
//   events, when a window loses focus; a key or mouse button held or released in the frame of the
//   focus loss that reports a press in the frame focus returns (SDL does that for held modifiers
//   and mouse buttons) counts only once pressed again (ADR-0027 rule 4);
// - ignores gamepad axis events while no window has focus;
// - routes each event through a consumer stack:
//   - keyboard events to the stack of their window, mouse events to the stack of the window under
//     the mouse (only its Game layer while the mouse is captured), gamepad events to the stack of
//     the window with focus;
//   - top first, until a consumer consumes the event;
//   - a release goes to exactly the consumers that saw its press, and a key repeat to those
//     consumers only; so a consumed press hides the key from lower consumers until its release;
//   - text events go only to the focused consumer of their window, while it accepts text;
//   - gamepad connects go to every consumer that sees the device; a disconnect, after the
//     gamepad's releases and axis resets, also to consumers that still have its values.
// Consumers bound to a player see only events from that player's devices.
class Input {
public:
    Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&) = delete;
    Input& operator=(Input&&) = delete;
    ~Input();

    // Processes one frame's events, in order: starts a new frame in every state, resets the axes
    // of consumers that lost a gamepad through assignDevice since the last call, then applies and
    // routes each event. `frameTimeNs` is the frame's input time on the events' clock
    // (Platform::pumpTimeNs()); frameTimeNs() reports it, raised to the latest event timestamp if
    // that is later.
    //
    // Precondition (asserted): not called from a consumer.
    void processEvents(std::span<const Event> events, std::uint64_t frameTimeNs);

    // The frame's input time: when its events were gathered, never earlier than their timestamps.
    [[nodiscard]] std::uint64_t frameTimeNs() const { return m_frameTimeNs; }

    // The frame's events as Input saw them, in order: presses of held controls, releases of
    // controls that aren't held, stale presses after focus returns and axes without focus are
    // left out, and the releases and resets made on focus loss and gamepad disconnect are in.
    // Read it after processEvents, not from a consumer during it (it is still growing). Valid
    // until the next processEvents; the events' `text` views point into the platform's storage
    // and are valid only until its next pump, so an Event is never kept across frames.
    [[nodiscard]] std::span<const Event> events() const { return m_events; }

    // The state of every device, before routing: what the hardware did.
    [[nodiscard]] const InputState& state() const { return m_state; }

    // The connected devices: the keyboard and mouse set (always first), then gamepads in the
    // order they connected.
    [[nodiscard]] std::span<const InputDevice> devices() const { return m_devices; }

    // The device with `id`, or nullptr when it isn't connected.
    [[nodiscard]] const InputDevice* findDevice(DeviceId id) const;

    // Single-player (the default): player 0 gets the keyboard, the mouse and every gamepad,
    // including gamepads that connect later. Local multiplayer: every device becomes unassigned,
    // new gamepads stay unassigned, and players join by assignDevice, typically when an
    // unbound consumer sees a button press on an unassigned device. Switching back assigns every
    // device to player 0.
    void setLocalMultiplayer(bool enabled);

    [[nodiscard]] bool localMultiplayer() const { return m_localMultiplayer; }

    // Assigns `device` to `player`, or unassigns it with kNoPlayer. Held keys and buttons still
    // release to the consumers that saw their press. Consumers that stop seeing a gamepad get
    // each of its non-zero axes set to 0 with an axis event at the start of the next
    // processEvents (not at once: this may be called from a consumer).
    //
    // Precondition (asserted): `device` is connected.
    void assignDevice(DeviceId device, PlayerIndex player);
    // The player `device` is assigned to; kNoPlayer when unassigned or not connected.
    [[nodiscard]] PlayerIndex playerOf(DeviceId device) const;

    // Adds a consumer to a window's stack. Its state starts empty: input already held doesn't
    // reach it until pressed again.
    //
    // Precondition (asserted): desc.window is not kNoWindow.
    ConsumerId addConsumer(const ConsumerDesc& desc);
    // Removes a consumer; unknown ids are ignored. Its InputConsumer is not called again.
    void removeConsumer(ConsumerId consumer);
    // The state built from the events that reached `consumer`, so it never shows input taken
    // above it. Valid until the consumer is removed.
    //
    // Precondition (asserted): `consumer` exists.
    [[nodiscard]] const InputState& consumerState(ConsumerId consumer) const;

    // The window with keyboard focus, from the focus events; kNoWindow when none has it.
    [[nodiscard]] WindowId focusedWindow() const { return m_focusedWindow; }

    // Gives `consumer` the focus within its window (a widget's layer or the game viewport); with
    // `acceptsText` it receives the window's text events, and textInputWanted() turns true.
    //
    // Precondition (asserted): `consumer` exists.
    void setFocusedConsumer(ConsumerId consumer, bool acceptsText = false);
    // Takes the focus from whichever consumer of `window` has it.
    void clearFocusedConsumer(WindowId window);
    // The consumer with the focus in `window`, or kNoConsumer.
    [[nodiscard]] ConsumerId focusedConsumer(WindowId window) const;
    // Whether text input should be on for `window`: its focused consumer accepts text. The
    // application applies it: Window::startTextInput / stopTextInput.
    [[nodiscard]] bool textInputWanted(WindowId window) const;

    // Capture on click (ADR-0027 rule 3), off by default; the application turns it on for a game
    // window or the Play In Editor viewport. While it is on, `window` has focus and the mouse
    // isn't captured, a mouse press that the Editor and Ui layers leave unconsumed captures the
    // mouse and is consumed by the capture: no Game consumer sees the press or its release.
    void setCaptureOnClick(WindowId window, bool enabled);
    [[nodiscard]] bool captureOnClick(WindowId window) const;

    // Captures the mouse for `window` (ADR-0013 rule 11): its mouse input goes only to its Game
    // layer and the application puts the window in relative mode
    // (Window::setRelativeMouseMode(mouseCaptured(id))). Fails, returning false, when `window`
    // doesn't have focus. Capture ends on focus loss and on releaseMouse(), which a UI layer
    // calls when it needs the cursor and the editor calls on Shift + F1.
    bool captureMouse(WindowId window);
    void releaseMouse();
    [[nodiscard]] bool mouseCaptured(WindowId window) const;

    [[nodiscard]] WindowId mouseCaptureWindow() const { return m_captureWindow; }

private:
    struct Consumer;

    // Which layers of a stack an event may reach.
    enum class Layers : std::uint8_t {
        All,
        GameOnly,
        AboveGame,
    };

    void process(Event event);
    void processKeyDown(Event event);
    void processMouseDown(const Event& event);
    void processGamepadHotplug(const Event& event);
    void resetReassignedAxes(std::uint64_t timestampNs);
    void releaseAll(std::uint64_t timestampNs, WindowId window);
    void releaseGamepad(DeviceId gamepad, std::uint64_t timestampNs);
    void assign(InputDevice& device, PlayerIndex player);
    void push(const Event& event);
    bool deliver(ConsumerId id, const Event& event);
    // Returns whether a consumer consumed the event.
    bool routeStack(const Event& event, WindowId window, Layers layers, DeviceId device);
    void routeToHolders(const Event& event, bool stopWhenConsumed);
    void routeAxisReset(const Event& event);
    void broadcast(const Event& event, DeviceId device);
    void routeText(const Event& event);
    bool accepts(const Consumer& consumer, DeviceId device) const;
    Consumer* find(ConsumerId id);
    const Consumer* find(ConsumerId id) const;
    InputDevice* findDeviceMutable(DeviceId id);

    InputState m_state;
    std::vector<Event> m_events;
    std::vector<InputDevice> m_devices;
    // Sorted by window, then layer (top first), then newest first.
    std::vector<std::unique_ptr<Consumer>> m_consumers;
    // The ids an event is being routed to; reused to avoid allocating per event. Nothing a
    // consumer may call routes events, so it is never in use twice.
    std::vector<ConsumerId> m_route;
    // Gamepads reassigned since the last processEvents.
    std::vector<DeviceId> m_reassigned;
    std::vector<WindowId> m_captureOnClick;
    std::uint64_t m_frameTimeNs = 0;
    ConsumerId m_nextConsumer = 1;
    std::uint64_t m_nextOrder = 0;
    WindowId m_focusedWindow = kNoWindow;
    WindowId m_captureWindow = kNoWindow;
    bool m_localMultiplayer = false;
    bool m_routing = false;
    // Keys and mouse buttons held or released in the frame focus was last lost, and whether focus
    // returned this frame.
    std::bitset<kKeyTableSize> m_keysHeldAtFocusLoss;
    std::bitset<8> m_buttonsHeldAtFocusLoss;
    bool m_focusReturned = false;
};

} // namespace ith
