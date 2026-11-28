#include "ith/input/Input.h"

#include "ith/foundation/Assert.h"

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

namespace ith {

struct Input::Consumer {
    ConsumerId id = kNoConsumer;
    WindowId window = kNoWindow;
    ConsumerLayer layer = ConsumerLayer::Game;
    PlayerIndex player = kNoPlayer;
    InputConsumer* consumer = nullptr;
    // Newer consumers sit above older ones of the same layer.
    std::uint64_t order = 0;
    InputState state;
    bool focused = false;
    bool acceptsText = false;
    // Removed while events were being routed; erased when processEvents ends.
    bool removed = false;
};

namespace {

// The sort key of a consumer: window, then layer top first, then newest first.
auto stackKey(WindowId window, ConsumerLayer layer, std::uint64_t order) {
    return std::make_tuple(window, layer, ~order);
}

} // namespace

Input::Input() {
    m_devices.push_back(InputDevice{
        .id = kKeyboardMouse, .kind = InputDeviceKind::KeyboardMouse, .name = "Keyboard and mouse", .player = 0});
}

Input::~Input() = default;

void Input::processEvents(std::span<const Event> events, std::uint64_t frameTimeNs) {
    ITH_ASSERT_MSG(!m_routing, "Input::processEvents must not be called from a consumer");
    m_routing = true;
    m_events.clear();
    m_state.beginFrame();
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        consumer->state.beginFrame();
    }

    m_frameTimeNs = frameTimeNs;
    resetReassignedAxes(frameTimeNs);
    for (const Event& event : events) {
        m_frameTimeNs = std::max(m_frameTimeNs, event.timestampNs);
        process(event);
    }

    if (m_focusReturned) {
        m_keysHeldAtFocusLoss.reset();
        m_buttonsHeldAtFocusLoss.reset();
        m_focusReturned = false;
    }
    std::erase_if(m_consumers, [](const std::unique_ptr<Consumer>& consumer) { return consumer->removed; });
    m_routing = false;
}

void Input::process(Event event) {
    switch (event.type) {
        case EventType::WindowFocusGained:
            m_focusedWindow = event.window;
            m_focusReturned = true;
            push(event);
            break;
        case EventType::WindowFocusLost:
            releaseAll(event.timestampNs, event.window);
            if (m_focusedWindow == event.window) {
                m_focusedWindow = kNoWindow;
            }
            if (m_captureWindow == event.window) {
                m_captureWindow = kNoWindow;
            }
            push(event);
            break;
        case EventType::KeyDown:
            processKeyDown(event);
            break;
        case EventType::KeyUp:
            if (event.key == Key::None || !m_state.held(event.key)) {
                break;
            }
            event.repeat = false;
            push(event);
            m_state.apply(event);
            routeToHolders(event, false);
            break;
        case EventType::MouseButtonDown: {
            const auto index = static_cast<std::size_t>(event.mouseButton);
            if (event.mouseButton == MouseButton::None || index >= m_buttonsHeldAtFocusLoss.size() ||
                m_state.held(event.mouseButton) || (m_focusReturned && m_buttonsHeldAtFocusLoss[index])) {
                break;
            }
            processMouseDown(event);
            break;
        }
        case EventType::MouseButtonUp:
            if (event.mouseButton == MouseButton::None || !m_state.held(event.mouseButton)) {
                break;
            }
            push(event);
            m_state.apply(event);
            routeToHolders(event, false);
            break;
        case EventType::MouseMoved:
        case EventType::MouseWheel:
            push(event);
            m_state.apply(event);
            routeStack(event, event.window, mouseCaptured(event.window) ? Layers::GameOnly : Layers::All,
                       kKeyboardMouse);
            break;
        case EventType::TextInput:
        case EventType::TextEditing:
            push(event);
            routeText(event);
            break;
        case EventType::GamepadConnected:
        case EventType::GamepadDisconnected:
            processGamepadHotplug(event);
            break;
        case EventType::GamepadButtonDown:
            if (event.gamepadButton == GamepadButton::None || m_state.held(event.device, event.gamepadButton)) {
                break;
            }
            push(event);
            m_state.apply(event);
            if (m_focusedWindow != kNoWindow) {
                routeStack(event, m_focusedWindow, Layers::All, event.device);
            }
            break;
        case EventType::GamepadButtonUp:
            if (event.gamepadButton == GamepadButton::None || !m_state.held(event.device, event.gamepadButton)) {
                break;
            }
            push(event);
            m_state.apply(event);
            routeToHolders(event, false);
            break;
        case EventType::GamepadAxisMoved:
            // Without focus, SDL still sends axes moving toward the centre; focus loss has set
            // every axis to 0 already, so they are ignored entirely.
            if (event.gamepadAxis == GamepadAxis::None || m_focusedWindow == kNoWindow) {
                break;
            }
            push(event);
            m_state.apply(event);
            routeStack(event, m_focusedWindow, Layers::All, event.device);
            break;
        default:
            push(event);
            break;
    }
}

void Input::processKeyDown(Event event) {
    const auto index = static_cast<std::size_t>(event.key);
    if (event.key == Key::None || index >= kKeyTableSize) {
        return;
    }
    if (m_state.held(event.key)) {
        // A press of a held key is a repeat, whatever the event says.
        event.repeat = true;
        push(event);
        m_state.apply(event);
        routeToHolders(event, true);
        return;
    }
    // A repeat of a key that isn't held (it was held when focus left), or a press reported as focus
    // returns for a key held when it left: not a press.
    if (event.repeat || (m_focusReturned && m_keysHeldAtFocusLoss[index])) {
        return;
    }
    push(event);
    m_state.apply(event);
    routeStack(event, event.window, Layers::All, kKeyboardMouse);
}

void Input::processMouseDown(const Event& event) {
    push(event);
    m_state.apply(event);
    const WindowId window = event.window;
    if (mouseCaptured(window)) {
        routeStack(event, window, Layers::GameOnly, kKeyboardMouse);
        return;
    }
    if (window != kNoWindow && window == m_focusedWindow && captureOnClick(window)) {
        // The capturing click: the layers above the game may take it; otherwise the capture
        // consumes it, so it never reaches the game and its release reaches no game consumer. A
        // consumer that turned capture on click off while it saw the press (a Stop or an Eject)
        // wins: nothing is captured, and the press still never reaches the game.
        if (!routeStack(event, window, Layers::AboveGame, kKeyboardMouse) && captureOnClick(window)) {
            m_captureWindow = window;
        }
        return;
    }
    routeStack(event, window, Layers::All, kKeyboardMouse);
}

void Input::resetReassignedAxes(std::uint64_t timestampNs) {
    // Collected first: a consumer may add consumers while it handles a reset.
    std::vector<std::pair<ConsumerId, Event>> resets;
    for (const DeviceId device : m_reassigned) {
        for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
            if (consumer->removed || accepts(*consumer, device)) {
                continue;
            }
            for (std::size_t index = 1; index < kGamepadAxisCount; ++index) {
                const auto axis = static_cast<GamepadAxis>(index);
                if (consumer->state.axis(device, axis) != 0.0f) {
                    resets.emplace_back(consumer->id, Event{.type = EventType::GamepadAxisMoved,
                                                            .timestampNs = timestampNs,
                                                            .device = device,
                                                            .gamepadAxis = axis});
                }
            }
        }
    }
    m_reassigned.clear();
    for (const auto& [id, reset] : resets) {
        deliver(id, reset);
    }
}

void Input::processGamepadHotplug(const Event& event) {
    const bool known = findDevice(event.device) != nullptr;
    if (event.device == kKeyboardMouse || known == (event.type == EventType::GamepadConnected)) {
        return; // a second connect, or the disconnect of an unknown gamepad
    }
    if (event.type == EventType::GamepadConnected) {
        m_devices.push_back(InputDevice{
            .id = event.device,
            .kind = InputDeviceKind::Gamepad,
            .name = std::string(event.text),
            .gamepadType = event.gamepadType,
            .gamepadGuid = event.gamepadGuid,
            .steamHandle = event.steamHandle,
            .player = m_localMultiplayer ? kNoPlayer : PlayerIndex{0},
        });
    } else {
        releaseGamepad(event.device, event.timestampNs);
    }
    push(event);
    m_state.apply(event);
    broadcast(event, event.device);
    if (event.type == EventType::GamepadDisconnected) {
        std::erase_if(m_devices, [&](const InputDevice& device) { return device.id == event.device; });
    }
}

void Input::releaseAll(std::uint64_t timestampNs, WindowId window) {
    m_keysHeldAtFocusLoss.reset();
    m_buttonsHeldAtFocusLoss.reset();
    for (std::size_t index = 0; index < kKeyTableSize; ++index) {
        const auto key = static_cast<Key>(index);
        // SDL releases held keys itself just before it reports the focus loss, in the same pump
        // (SDL_SetKeyboardFocus); those keys were held at focus loss too.
        if (m_state.released(key)) {
            m_keysHeldAtFocusLoss[index] = true;
        }
        if (m_state.held(key)) {
            m_keysHeldAtFocusLoss[index] = true;
            process(Event{.type = EventType::KeyUp, .timestampNs = timestampNs, .window = window, .key = key});
        }
    }
    for (std::size_t index = 0; index < m_buttonsHeldAtFocusLoss.size(); ++index) {
        const auto button = static_cast<MouseButton>(index);
        if (button != MouseButton::None && m_state.released(button)) {
            m_buttonsHeldAtFocusLoss[index] = true;
        }
        if (button != MouseButton::None && m_state.held(button)) {
            m_buttonsHeldAtFocusLoss[index] = true;
            const InputVector2 position = m_state.mousePosition();
            process(Event{.type = EventType::MouseButtonUp,
                          .timestampNs = timestampNs,
                          .window = m_state.mouseWindow(),
                          .mouseButton = button,
                          .x = position.x,
                          .y = position.y});
        }
    }
    // Gamepad events stop while no window has focus, so their values would go stale.
    for (const DeviceId gamepad : m_state.gamepads()) {
        releaseGamepad(gamepad, timestampNs);
    }
}

void Input::releaseGamepad(DeviceId gamepad, std::uint64_t timestampNs) {
    for (std::size_t index = 1; index < kGamepadButtonCount; ++index) {
        const auto button = static_cast<GamepadButton>(index);
        if (m_state.held(gamepad, button)) {
            process(Event{.type = EventType::GamepadButtonUp,
                          .timestampNs = timestampNs,
                          .device = gamepad,
                          .gamepadButton = button});
        }
    }
    for (std::size_t index = 1; index < kGamepadAxisCount; ++index) {
        const auto axis = static_cast<GamepadAxis>(index);
        if (m_state.axis(gamepad, axis) != 0.0f) {
            const Event reset{.type = EventType::GamepadAxisMoved,
                              .timestampNs = timestampNs,
                              .device = gamepad,
                              .gamepadAxis = axis};
            push(reset);
            m_state.apply(reset);
            routeAxisReset(reset);
        }
    }
}

void Input::push(const Event& event) {
    m_events.push_back(event);
}

bool Input::deliver(ConsumerId id, const Event& event) {
    Consumer* consumer = find(id);
    if (consumer == nullptr) {
        return false;
    }
    consumer->state.apply(event);
    // Copied first: the call may remove the consumer.
    InputConsumer* callback = consumer->consumer;
    return callback != nullptr && callback->onInputEvent(event) == InputReply::Consumed;
}

bool Input::routeStack(const Event& event, WindowId window, Layers layers, DeviceId device) {
    m_route.clear();
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        const bool layerMatches =
            layers == Layers::All || (layers == Layers::GameOnly) == (consumer->layer == ConsumerLayer::Game);
        if (!consumer->removed && consumer->window == window && layerMatches && accepts(*consumer, device)) {
            m_route.push_back(consumer->id);
        }
    }
    const std::vector<ConsumerId>& route = m_route; // consumers never route events themselves
    for (std::size_t i = 0; i < route.size(); ++i) {
        if (!deliver(route[i], event)) {
            continue;
        }
        // Consumed. An axis below that still shows a value would keep it forever: reset it.
        if (event.type == EventType::GamepadAxisMoved) {
            Event reset = event;
            reset.axisValue = 0.0f;
            for (std::size_t j = i + 1; j < route.size(); ++j) {
                const Consumer* lower = find(route[j]);
                if (lower != nullptr && lower->state.axis(event.device, event.gamepadAxis) != 0.0f) {
                    deliver(route[j], reset);
                }
            }
        }
        return true;
    }
    return false;
}

void Input::routeToHolders(const Event& event, bool stopWhenConsumed) {
    m_route.clear();
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (consumer->removed) {
            continue;
        }
        const InputState& state = consumer->state;
        bool holds = false;
        switch (event.type) {
            case EventType::KeyDown:
            case EventType::KeyUp:
                holds = state.held(event.key);
                break;
            case EventType::MouseButtonUp:
                holds = state.held(event.mouseButton);
                break;
            case EventType::GamepadButtonUp:
                holds = state.held(event.device, event.gamepadButton);
                break;
            default:
                break;
        }
        if (holds) {
            m_route.push_back(consumer->id);
        }
    }
    const std::vector<ConsumerId>& route = m_route; // consumers never route events themselves
    for (const ConsumerId id : route) {
        if (deliver(id, event) && stopWhenConsumed) {
            return;
        }
    }
}

void Input::routeAxisReset(const Event& event) {
    m_route.clear();
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (!consumer->removed && consumer->state.axis(event.device, event.gamepadAxis) != 0.0f) {
            m_route.push_back(consumer->id);
        }
    }
    const std::vector<ConsumerId>& route = m_route; // consumers never route events themselves
    for (const ConsumerId id : route) {
        deliver(id, event);
    }
}

void Input::broadcast(const Event& event, DeviceId device) {
    m_route.clear();
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        // A disconnect also reaches consumers that no longer see the device but still have its values.
        const bool knows = event.type == EventType::GamepadDisconnected && consumer->state.hasGamepad(device);
        if (!consumer->removed && (accepts(*consumer, device) || knows)) {
            m_route.push_back(consumer->id);
        }
    }
    const std::vector<ConsumerId>& route = m_route; // consumers never route events themselves
    for (const ConsumerId id : route) {
        deliver(id, event);
    }
}

void Input::routeText(const Event& event) {
    // The id is found first and the loop left before the call, which may add consumers.
    ConsumerId focused = kNoConsumer;
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (!consumer->removed && consumer->window == event.window && consumer->focused && consumer->acceptsText) {
            focused = consumer->id;
            break;
        }
    }
    if (focused != kNoConsumer) {
        deliver(focused, event);
    }
}

bool Input::accepts(const Consumer& consumer, DeviceId device) const {
    return consumer.player == kNoPlayer || consumer.player == playerOf(device);
}

Input::Consumer* Input::find(ConsumerId id) {
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (consumer->id == id && !consumer->removed) {
            return consumer.get();
        }
    }
    return nullptr;
}

const Input::Consumer* Input::find(ConsumerId id) const {
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (consumer->id == id && !consumer->removed) {
            return consumer.get();
        }
    }
    return nullptr;
}

const InputDevice* Input::findDevice(DeviceId id) const {
    const auto found = std::ranges::find(m_devices, id, &InputDevice::id);
    return found != m_devices.end() ? &*found : nullptr;
}

InputDevice* Input::findDeviceMutable(DeviceId id) {
    const auto found = std::ranges::find(m_devices, id, &InputDevice::id);
    return found != m_devices.end() ? &*found : nullptr;
}

void Input::setLocalMultiplayer(bool enabled) {
    m_localMultiplayer = enabled;
    for (InputDevice& device : m_devices) {
        assign(device, enabled ? kNoPlayer : PlayerIndex{0});
    }
}

void Input::assignDevice(DeviceId device, PlayerIndex player) {
    InputDevice* found = findDeviceMutable(device);
    ITH_ASSERT_MSG(found != nullptr, "only a connected device can be assigned to a player");
    if (found != nullptr) {
        assign(*found, player);
    }
}

void Input::assign(InputDevice& device, PlayerIndex player) {
    if (device.player != player && device.kind == InputDeviceKind::Gamepad) {
        // Consumers that stop seeing the gamepad get its axes reset at the next processEvents;
        // routing now could happen inside a consumer's call.
        m_reassigned.push_back(device.id);
    }
    device.player = player;
}

PlayerIndex Input::playerOf(DeviceId device) const {
    const InputDevice* found = findDevice(device);
    return found != nullptr ? found->player : kNoPlayer;
}

ConsumerId Input::addConsumer(const ConsumerDesc& desc) {
    ITH_ASSERT_MSG(desc.window != kNoWindow, "a consumer belongs to a window");
    auto consumer = std::make_unique<Consumer>();
    consumer->id = m_nextConsumer++;
    consumer->window = desc.window;
    consumer->layer = desc.layer;
    consumer->player = desc.player;
    consumer->consumer = desc.consumer;
    consumer->order = m_nextOrder++;
    const auto key = stackKey(consumer->window, consumer->layer, consumer->order);
    const auto position = std::ranges::upper_bound(m_consumers, key, {}, [](const std::unique_ptr<Consumer>& entry) {
        return stackKey(entry->window, entry->layer, entry->order);
    });
    const ConsumerId id = consumer->id;
    m_consumers.insert(position, std::move(consumer));
    return id;
}

void Input::removeConsumer(ConsumerId consumer) {
    Consumer* found = find(consumer);
    if (found == nullptr) {
        return;
    }
    if (m_routing) {
        // Erased when processEvents ends: routing may still hold its id.
        found->removed = true;
        found->consumer = nullptr;
        return;
    }
    std::erase_if(m_consumers, [&](const std::unique_ptr<Consumer>& entry) { return entry->id == consumer; });
}

const InputState& Input::consumerState(ConsumerId consumer) const {
    const Consumer* found = find(consumer);
    ITH_ASSERT_MSG(found != nullptr, "consumerState of a consumer that doesn't exist");
    static const InputState kEmpty;
    return found != nullptr ? found->state : kEmpty;
}

void Input::setFocusedConsumer(ConsumerId consumer, bool acceptsText) {
    Consumer* found = find(consumer);
    ITH_ASSERT_MSG(found != nullptr, "only an existing consumer can have the focus");
    if (found == nullptr) {
        return;
    }
    clearFocusedConsumer(found->window);
    found->focused = true;
    found->acceptsText = acceptsText;
}

void Input::clearFocusedConsumer(WindowId window) {
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (consumer->window == window) {
            consumer->focused = false;
            consumer->acceptsText = false;
        }
    }
}

ConsumerId Input::focusedConsumer(WindowId window) const {
    for (const std::unique_ptr<Consumer>& consumer : m_consumers) {
        if (!consumer->removed && consumer->window == window && consumer->focused) {
            return consumer->id;
        }
    }
    return kNoConsumer;
}

bool Input::textInputWanted(WindowId window) const {
    const Consumer* focused = find(focusedConsumer(window));
    return focused != nullptr && focused->acceptsText;
}

bool Input::captureMouse(WindowId window) {
    if (window == kNoWindow || window != m_focusedWindow) {
        return false;
    }
    m_captureWindow = window;
    return true;
}

void Input::setCaptureOnClick(WindowId window, bool enabled) {
    std::erase(m_captureOnClick, window);
    if (enabled) {
        m_captureOnClick.push_back(window);
    }
}

bool Input::captureOnClick(WindowId window) const {
    return std::ranges::find(m_captureOnClick, window) != m_captureOnClick.end();
}

void Input::releaseMouse() {
    m_captureWindow = kNoWindow;
}

bool Input::mouseCaptured(WindowId window) const {
    return window != kNoWindow && window == m_captureWindow;
}

} // namespace ith
