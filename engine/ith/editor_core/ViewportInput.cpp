#include "ith/editor_core/ViewportInput.h"

#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Gizmo.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/ViewportGizmo.h"
#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/math/Types.h"
#include "ith/platform_events/Event.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

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
        case ViewportGesture::Gizmo:
            break;
    }
    return input;
}

// How a click with `modifiers` at its press selects: Ctrl toggles, else Shift adds.
PickMode pickModeFor(KeyModifiers modifiers) noexcept {
    if (hasModifiers(modifiers, KeyModifiers::Control)) {
        return PickMode::Toggle;
    }
    if (hasModifiers(modifiers, KeyModifiers::Shift)) {
        return PickMode::Add;
    }
    return PickMode::Replace;
}

// Whether the pointer at `x`, `y` (window pixels) is at most kClickSlop logical points from `start`,
// at `scale` pixels per point (the display scale).
bool withinSlop(Vec2 start, float x, float y, float scale) noexcept {
    const Vec2 moved = Vec2{x, y} - start;
    const float slop = kClickSlop * scale;
    return moved.x * moved.x + moved.y * moved.y <= slop * slop;
}

} // namespace

ViewportConsumer::ViewportConsumer(EditorSession& session) noexcept
    : m_session(&session), m_viewports(&session.viewports()) {}

InputReply ViewportConsumer::onInputEvent(const Event& event) {
    switch (event.type) {
        case EventType::MouseButtonDown:
            return onMouseButtonDown(event);
        case EventType::MouseButtonUp:
            if (m_drag.has_value() && event.mouseButton == m_drag->button) {
                const Drag drag = *m_drag;
                if (drag.gesture == ViewportGesture::Gizmo) {
                    endGizmo(drag.viewport, Vec2{event.x, event.y});
                }
                // A click candidate that stayed within the slop picks (ADR-0042 rule 22); the panel
                // takes the click in its next render.
                m_drag.reset();
                m_speedWheel = 0.0f;
                if (drag.click.has_value() && withinSlop(drag.start, event.x, event.y, m_displayScale)) {
                    m_viewports->at(drag.viewport).click = drag.click;
                }
            }
            return InputReply::NotConsumed; // Input ignores the reply to a release
        case EventType::MouseMoved:
            return onMouseMoved(event);
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
            if (!viewport.has_value() || gameHas(*viewport)) {
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

InputReply ViewportConsumer::onMouseMoved(const Event& event) {
    if (m_drag.has_value()) {
        if (m_drag->gesture == ViewportGesture::Gizmo) {
            // Previewed once per frame, in update().
            m_gizmoPointer = Vec2{event.x, event.y};
            m_gizmoMoved = true;
        } else if (m_drag->gesture == ViewportGesture::Click) {
            if (!withinSlop(m_drag->start, event.x, event.y, m_displayScale)) {
                m_drag->click.reset(); // a drag now; it stays one even if the pointer comes back
            }
        } else if (std::isfinite(event.deltaX) && std::isfinite(event.deltaY)) {
            // A camera drag; motion that is not finite moves nothing.
            CameraInput input = inputFor(m_drag->gesture);
            // Fly's look motion is relative mode's raw motion; other drags move in window
            // coordinates, the same on every display scale.
            const float scale = m_drag->gesture == ViewportGesture::Fly ? 1.0f : 1.0f / m_pixelDensity;
            input.mouseDelta = Vec2{event.deltaX, event.deltaY} * scale;
            apply(m_drag->viewport, input, 0.0f);
        }
        return InputReply::Consumed;
    }
    if (const std::optional<std::size_t> viewport = viewportAt(event.x, event.y);
        viewport.has_value() && !gameHas(*viewport)) {
        return InputReply::Consumed;
    }
    return InputReply::NotConsumed;
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
    // The selected viewport is where the next session plays (ADR-0053 decision 5).
    m_viewports->setSelected(*viewport);
    if (gameHas(*viewport)) {
        return InputReply::NotConsumed; // capture on click takes it (ADR-0053 decision 20)
    }
    const bool alt = hasModifiers(event.modifiers, KeyModifiers::Alt);
    // A press on a gizmo handle drags it, before it can be a click candidate or an orbit.
    if (event.mouseButton == MouseButton::Left && beginGizmo(*viewport, Vec2{event.x, event.y}, alt)) {
        return InputReply::Consumed;
    }
    const std::optional<ViewportGesture> gesture = gestureFor(event.mouseButton, alt);
    if (gesture.has_value()) {
        m_drag = Drag{
            .viewport = *viewport, .button = event.mouseButton, .gesture = *gesture, .start = Vec2{event.x, event.y}};
        m_speedWheel = 0.0f;
        if (*gesture == ViewportGesture::Click) {
            // A candidate until its release (rule 22).
            m_drag->click = ViewportClick{.position = m_drag->start - m_viewports->at(*viewport).imagePosition,
                                          .mode = pickModeFor(event.modifiers)};
        }
    }
    return InputReply::Consumed;
}

bool ViewportConsumer::beginGizmo(std::size_t index, Vec2 press, bool alt) {
    ViewportState& state = m_viewports->at(index);
    const std::optional<ViewportGizmo> gizmo = viewportGizmo(*m_session, state, m_displayScale);
    if (!gizmo.has_value()) {
        return false;
    }
    const Vec2 pixel = press - state.imagePosition;
    const GizmoHandle handle = hitGizmo(gizmo->layout, pixel, kGizmoGrabTolerance * m_displayScale);
    if (handle == GizmoHandle::None) {
        return false;
    }
    Expected<GizmoDrag> drag =
        GizmoDrag::begin(*m_session, handle, gizmo->frame, pickRay(state.camera, state.imageSize, pixel), alt);
    if (!drag) {
        // A press that grabs no point of the handle (nearly parallel to it) is an expected fallback.
        if (drag.error().code != "editor.gizmo.no_grab") {
            ITH_LOG_WARNING(Gizmo, "{}", drag.error().message);
        }
        return false;
    }
    m_gizmo.emplace(std::move(*drag));
    m_gizmoMoved = false;
    m_drag = Drag{.viewport = index, .button = MouseButton::Left, .gesture = ViewportGesture::Gizmo, .start = press};
    m_speedWheel = 0.0f;
    state.gizmoDrag = handle;
    // A click waiting to be picked would select during the drag's transaction, which refuses it.
    for (std::size_t viewport = 0; viewport < kMaxViewports; ++viewport) {
        m_viewports->at(viewport).click.reset();
    }
    return true;
}

void ViewportConsumer::endGizmo(std::size_t viewport, Vec2 release) {
    // The release's own position counts, then one undo entry (ADR-0042 rule 25).
    previewGizmo(release);
    if (m_gizmo.has_value()) {
        m_gizmo->commit();
        m_gizmo.reset();
    }
    m_gizmoMoved = false;
    m_viewports->at(viewport).gizmoDrag = GizmoHandle::None;
}

void ViewportConsumer::previewGizmo(Vec2 pointer) {
    if (!m_gizmo.has_value() || !m_drag.has_value()) {
        return;
    }
    const ViewportState& state = m_viewports->at(m_drag->viewport);
    if (!hasImageArea(state.imageSize)) {
        return; // the image is gone (a closed panel): nothing to aim through
    }
    const Ray ray = pickRay(state.camera, state.imageSize, pointer - state.imagePosition);
    if (const Expected<void> previewed = m_gizmo->preview(ray); !previewed) {
        ITH_LOG_WARNING(Gizmo, "{}", previewed.error().message);
    }
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

bool* flyKey(CameraInput& input, Key key) noexcept {
    switch (key) {
        case Key::W:
            return &input.forward;
        case Key::S:
            return &input.back;
        case Key::A:
            return &input.left;
        case Key::D:
            return &input.right;
        case Key::Q:
            return &input.down;
        case Key::E:
            return &input.up;
        default:
            return nullptr;
    }
}

bool* ViewportConsumer::flyKey(Key key) noexcept {
    return ith::flyKey(m_flyKeys, key);
}

bool ViewportConsumer::update(float deltaSeconds) {
    if (m_gizmo.has_value() && m_drag.has_value()) {
        const std::size_t viewport = m_drag->viewport;
        if (m_gizmoMoved) {
            m_gizmoMoved = false;
            previewGizmo(m_gizmoPointer);
        }
        if (m_gizmo.has_value() && !m_gizmo->active()) {
            // Esc cancelled it: nothing is held any more, though the drag waits for its release.
            m_viewports->at(viewport).gizmoDrag = GizmoHandle::None;
        }
    }
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

void ViewportConsumer::setDisplayScale(float scale) noexcept {
    const bool valid = std::isfinite(scale) && scale > 0.0f;
    ITH_ASSERT_MSG(valid, "the display scale must be finite and positive");
    if (valid) {
        m_displayScale = scale;
    }
}

void ViewportConsumer::focusLost() {
    // The held gizmo drag is cancelled (its Alt copies removed); the release Input then synthesises
    // finds its transaction ended and records nothing.
    if (m_gizmo.has_value()) {
        m_gizmo->cancel();
        if (m_drag.has_value()) {
            m_viewports->at(m_drag->viewport).gizmoDrag = GizmoHandle::None;
        }
    }
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        m_viewports->at(index).click.reset();
    }
    if (m_drag.has_value()) {
        m_drag->click.reset();
    }
}

std::optional<std::size_t> ViewportConsumer::viewportAt(float x, float y) const noexcept {
    return m_viewports->imageAt(x, y);
}

bool ViewportConsumer::gameHas(std::size_t index) const noexcept {
    const PlaySession* play = m_session->play();
    return play != nullptr && play->possessed() && play->viewport() == index;
}

void ViewportConsumer::apply(std::size_t index, const CameraInput& input, float deltaSeconds) noexcept {
    EditorCamera& camera = m_viewports->at(index).camera;
    const EditorCamera before = camera;
    updateCamera(camera, input, deltaSeconds);
    m_changed = m_changed || camera != before;
}

} // namespace ith
