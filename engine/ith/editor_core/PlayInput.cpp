#include "ith/editor_core/PlayInput.h"

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Assert.h"
#include "ith/platform_events/Event.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ith {

InputReply PlayInputConsumer::onInputEvent(const Event& event) {
    if (event.type != EventType::KeyDown || event.repeat || m_session->play() == nullptr) {
        return InputReply::NotConsumed;
    }
    const std::string* command = m_keymap->commandFor(chordOf(event), KeymapContext::PlayInEditor);
    if (command == nullptr) {
        return InputReply::NotConsumed;
    }
    const Input* input = m_session->playInput();
    const bool captured = input != nullptr && input->mouseCaptured(event.window);
    if (*command == "ith.play.stop" && !captured && escapeTaken()) {
        return InputReply::NotConsumed; // a text field, a popup or an interaction takes it
    }
    Expected<serialization::Value> result = m_session->commands().invoke(*m_session, *command);
    m_lastResult = result ? Expected<void>() : Expected<void>(makeUnexpected(std::move(result).error()));
    return InputReply::Consumed;
}

bool PlayInputConsumer::escapeTaken() const {
    return m_textFieldActive || m_popupOpen || m_session->document().transactions().isOpen() ||
           m_session->active().transactions.isOpen();
}

InputReply SpectatorConsumer::onInputEvent(const Event& event) {
    if (event.type == EventType::KeyUp) {
        if (bool* const held = flyKey(m_keys, event.key); held != nullptr) {
            *held = false;
        }
        return InputReply::NotConsumed;
    }
    EditorCamera* const view = camera();
    if (view == nullptr) {
        return InputReply::NotConsumed;
    }
    switch (event.type) {
        case EventType::MouseMoved: {
            // Look as RMB fly does; captured motion is relative mode's raw motion. Motion that is not
            // finite is ignored.
            if (!std::isfinite(event.deltaX) || !std::isfinite(event.deltaY)) {
                return InputReply::NotConsumed;
            }
            CameraInput input;
            input.rmb = true;
            input.mouseDelta = Vec2{event.deltaX, event.deltaY};
            const EditorCamera before = *view;
            updateCamera(*view, input, 0.0f);
            m_changed = m_changed || *view != before;
            return InputReply::Consumed;
        }
        case EventType::KeyDown: {
            bool* const held = flyKey(m_keys, event.key);
            if (held == nullptr) {
                return InputReply::NotConsumed;
            }
            if (!event.repeat) {
                *held = true;
            }
            return InputReply::Consumed;
        }
        default:
            return InputReply::NotConsumed;
    }
}

bool SpectatorConsumer::update(float deltaSeconds) {
    const bool valid = std::isfinite(deltaSeconds) && deltaSeconds >= 0.0f;
    ITH_ASSERT_MSG(valid, "the spectator's frame time must be finite and non-negative");
    EditorCamera* const view = camera();
    if (view == nullptr) {
        // Freed, ejected or stopped: a key held from before moves nothing, now or after a recapture.
        m_keys = CameraInput{};
        m_wasMoving = false;
        return std::exchange(m_changed, false);
    }
    const bool moving =
        valid && (m_keys.forward || m_keys.back || m_keys.left || m_keys.right || m_keys.down || m_keys.up);
    if (moving) {
        CameraInput input = m_keys;
        input.rmb = true;
        input.mouseDelta = Vec2{0.0f};
        input.wheel = 0.0f;
        const EditorCamera before = *view;
        // The first frame of a movement may follow an idle wait; later frames move for all of theirs.
        updateCamera(*view, input, m_wasMoving ? deltaSeconds : std::min(deltaSeconds, kMaxFlyStepSeconds));
        m_changed = m_changed || *view != before;
    }
    m_wasMoving = moving;
    return std::exchange(m_changed, false);
}

EditorCamera* SpectatorConsumer::camera() const noexcept {
    const PlaySession* play = m_session->play();
    if (play == nullptr || !play->possessed() || !m_session->mouseCaptured()) {
        return nullptr;
    }
    return &m_session->viewports().at(play->viewport()).camera;
}

} // namespace ith
