#include "ith/editor_core/ViewportInput.h"

#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Assert.h"
#include "ith/math/Types.h"
#include "ith/platform_events/Event.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace ith {

namespace {

// The gesture a press of `button` on a viewport image starts, by Alt at the press; std::nullopt for
// buttons that start none.
std::optional<ViewportGesture> gestureFor(MouseButton button, bool alt) noexcept {
    switch (button) {
        case MouseButton::Left:
            return alt ? ViewportGesture::Orbit : ViewportGesture::Click;
        case MouseButton::Right:
            return alt ? ViewportGesture::Dolly : ViewportGesture::Fly;
        case MouseButton::Middle:
            return ViewportGesture::Pan;
        default:
            return std::nullopt;
    }
}

// The held buttons and Alt the camera controller reads for `gesture`.
CameraInput inputFor(ViewportGesture gesture) noexcept {
    CameraInput input;
    switch (gesture) {
        case ViewportGesture::Fly:
            input.rmb = true;
            break;
        case ViewportGesture::Dolly:
            input.rmb = true;
            input.alt = true;
            break;
        case ViewportGesture::Orbit:
            input.lmb = true;
            input.alt = true;
            break;
        case ViewportGesture::Pan:
            input.mmb = true;
            break;
        case ViewportGesture::Click:
            break;
    }
    return input;
}

} // namespace

InputReply ViewportConsumer::onInputEvent(const Event& event) {
    switch (event.type) {
        case EventType::MouseButtonDown:
            return onMouseButtonDown(event);
        case EventType::MouseButtonUp:
            if (m_drag.has_value() && event.mouseButton == m_drag->button) {
                // A click candidate ends here too: picking (ADR-0042 rule 22) comes with a later step.
                m_drag.reset();
                m_speedWheel = 0.0f;
            }
            return InputReply::NotConsumed; // Input ignores the reply to a release
        case EventType::MouseMoved:
            if (m_drag.has_value()) {
                if (m_drag->gesture != ViewportGesture::Click) {
                    CameraInput input = inputFor(m_drag->gesture);
                    // Fly's look motion is relative mode's raw motion; other drags move in window
                    // coordinates, the same on every display scale.
                    const float scale = m_drag->gesture == ViewportGesture::Fly ? 1.0f : 1.0f / m_pixelDensity;
                    input.mouseDelta = Vec2{event.deltaX, event.deltaY} * scale;
                    apply(m_drag->viewport, input, 0.0f);
                }
                return InputReply::Consumed;
            }
            return viewportAt(event.x, event.y).has_value() ? InputReply::Consumed : InputReply::NotConsumed;
        case EventType::MouseWheel: {
            if (m_drag.has_value()) {
                // While flying the wheel steps the speed, keeping fractions for the next notch; other
                // drags swallow it.
                if (m_drag->gesture == ViewportGesture::Fly) {
                    m_speedWheel += event.deltaY;
                    const float notches = std::trunc(m_speedWheel);
                    if (notches != 0.0f) {
                        m_speedWheel -= notches;
                        CameraInput input = inputFor(ViewportGesture::Fly);
                        input.wheel = notches;
                        apply(m_drag->viewport, input, 0.0f);
                    }
                }
                return InputReply::Consumed;
            }
            const std::optional<std::size_t> viewport = viewportAt(event.x, event.y);
            if (!viewport.has_value()) {
                return InputReply::NotConsumed;
            }
            if (event.deltaY != 0.0f) {
                CameraInput input;
                input.wheel = event.deltaY;
                apply(*viewport, input, 0.0f);
            }
            return InputReply::Consumed;
        }
        case EventType::KeyDown:
            return onKeyDown(event);
        case EventType::KeyUp:
            if (bool* const held = flyKey(event.key); held != nullptr) {
                *held = false;
            }
            return InputReply::NotConsumed;
        default:
            return InputReply::NotConsumed;
    }
}

InputReply ViewportConsumer::onMouseButtonDown(const Event& event) {
    // One drag at a time: presses of other buttons during it are swallowed.
    if (m_drag.has_value()) {
        return InputReply::Consumed;
    }
    const std::optional<std::size_t> viewport = viewportAt(event.x, event.y);
    if (!viewport.has_value()) {
        return InputReply::NotConsumed;
    }
    const std::optional<ViewportGesture> gesture =
        gestureFor(event.mouseButton, hasModifiers(event.modifiers, KeyModifiers::Alt));
    if (gesture.has_value()) {
        // A Click is only a candidate so far: gizmo drags (ADR-0042 rule 25) and picking
        // (rule 22) start from it in later steps.
        m_drag = Drag{
            .viewport = *viewport, .button = event.mouseButton, .gesture = *gesture, .start = Vec2{event.x, event.y}};
        m_speedWheel = 0.0f;
    }
    return InputReply::Consumed;
}

InputReply ViewportConsumer::onKeyDown(const Event& event) {
    // The letter-key rule (ADR-0042 rule 10): without fly no key is consumed, and chords with Ctrl or
    // Alt always go on to the keymap.
    if (!flying() || hasModifiers(event.modifiers, KeyModifiers::Control) ||
        hasModifiers(event.modifiers, KeyModifiers::Alt)) {
        return InputReply::NotConsumed;
    }
    if (bool* const held = flyKey(event.key); held != nullptr && !event.repeat) {
        *held = true;
    }
    return InputReply::Consumed;
}

bool* ViewportConsumer::flyKey(Key key) noexcept {
    switch (key) {
        case Key::W:
            return &m_flyKeys.forward;
        case Key::S:
            return &m_flyKeys.back;
        case Key::A:
            return &m_flyKeys.left;
        case Key::D:
            return &m_flyKeys.right;
        case Key::Q:
            return &m_flyKeys.down;
        case Key::E:
            return &m_flyKeys.up;
        default:
            return nullptr;
    }
}

bool ViewportConsumer::update(float deltaSeconds) noexcept {
    const bool valid = std::isfinite(deltaSeconds) && deltaSeconds >= 0.0f;
    ITH_ASSERT_MSG(valid, "the viewport frame time must be finite and non-negative");
    const bool moving =
        m_flyKeys.forward || m_flyKeys.back || m_flyKeys.left || m_flyKeys.right || m_flyKeys.down || m_flyKeys.up;
    const bool flyMoving = valid && moving && m_drag.has_value() && m_drag->gesture == ViewportGesture::Fly;
    if (flyMoving && m_drag.has_value()) { // has_value() again for the optional-access check
        CameraInput input = inputFor(ViewportGesture::Fly);
        input.forward = m_flyKeys.forward;
        input.back = m_flyKeys.back;
        input.left = m_flyKeys.left;
        input.right = m_flyKeys.right;
        input.down = m_flyKeys.down;
        input.up = m_flyKeys.up;
        // The first frame of a movement may follow an idle wait; later frames move for all of theirs.
        apply(m_drag->viewport, input, m_wasMoving ? deltaSeconds : std::min(deltaSeconds, kMaxFlyStepSeconds));
    }
    m_wasMoving = flyMoving;
    const bool changed = m_changed;
    m_changed = false;
    return changed;
}

void ViewportConsumer::setPixelDensity(float density) noexcept {
    const bool valid = std::isfinite(density) && density > 0.0f;
    ITH_ASSERT_MSG(valid, "the pixel density must be finite and positive");
    if (valid) {
        m_pixelDensity = density;
    }
}

std::optional<std::size_t> ViewportConsumer::viewportAt(float x, float y) const noexcept {
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        const ViewportState& state = m_viewports->at(index);
        const Vec2 position = state.imagePosition;
        const Vec2 size = state.imageSize;
        if (state.imageHovered && size.x > 0.0f && size.y > 0.0f && x >= position.x && x < position.x + size.x &&
            y >= position.y && y < position.y + size.y) {
            return index;
        }
    }
    return std::nullopt;
}

void ViewportConsumer::apply(std::size_t index, const CameraInput& input, float deltaSeconds) noexcept {
    EditorCamera& camera = m_viewports->at(index).camera;
    const EditorCamera before = camera;
    updateCamera(camera, input, deltaSeconds);
    m_changed = m_changed || camera != before;
}

} // namespace ith
