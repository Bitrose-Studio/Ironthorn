#pragma once

// Play In Editor's consumers of an editor window's input stack (ADR-0053 decisions 17 to 22,
// docs/specs/play-in-editor.md 3.6 and 3.7), without any UI.
//
//   session.setPlayInput(&input, window.id());
//   ith::KeymapConsumer shortcuts(session, keymap);
//   ith::ViewportConsumer viewportInput(session);
//   ith::SpectatorConsumer spectator(session);
//   ith::PlayInputConsumer playKeys(session, keymap);
//   // top first: playKeys (added last), the UI's consumer, the viewports, the keymap; then the game
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Game, .consumer = &spectator});
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
//   ... the UI's consumer ...
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &playKeys});
//   while (running) {
//       input.processEvents(events, platform.pumpTimeNs());
//       if (spectator.update(deltaSeconds)) { idle.requestFrame(); }
//       ... the UI's frame, which ends with playKeys.reportUi(textFieldActive, popupOpen) ...
//   }
//
// The Play In Editor consumer handles the Play In Editor keymap context (kPlayInEditorCommands:
// Escape stops, Shift + F1 frees the mouse, F8 possesses or ejects) while a session runs, and passes
// everything else on. The spectator is P2's view for Play while no game provides one: it flies the
// play viewport's camera from the Game layer, so it gets input exactly when a game would. The keymap
// consumer's capture and gate rules (Keymap.h) decide what reaches it. Main thread only; the session
// and the keymap must outlive the consumers.

#include "ith/editor_core/EditorCamera.h"
#include "ith/foundation/Expected.h"
#include "ith/input/Input.h"

namespace ith {

class EditorSession;
class Keymap;
struct Event;

// The Play In Editor consumer: on top of the editor window's stack (Editor layer, added last). While
// a session runs, a key press (not a repeat) bound in the Play In Editor context runs its command,
// invoked as a user gesture, and is consumed, also when the command refuses (lastResult). The one
// exception is the chord of ith.play.stop (Escape) while the mouse is free (ADR-0053 decision 19): it
// passes on, unconsumed, when something else uses Escape, so that takes it:
// - an interaction holds a transaction open in the editor world or the game world (the keymap
//   consumer below cancels it); or
// - the UI reported in its last frame an active text field or an open popup (reportUi).
// While the mouse is captured Escape always stops. Without a session it consumes nothing.
class PlayInputConsumer final : public InputConsumer {
public:
    PlayInputConsumer(EditorSession& session, const Keymap& keymap) noexcept : m_session(&session), m_keymap(&keymap) {}

    InputReply onInputEvent(const Event& event) override;

    // Points the consumer at a reloaded keymap.
    void setKeymap(const Keymap& keymap) noexcept { m_keymap = &keymap; }

    // The UI's report at the end of each frame: whether a text field has the keyboard (ImGui's
    // WantTextInput) and whether a popup or dialog is open. Escape uses the last report, the same
    // one-frame lag as the UI's want-capture flags (ADR-0029 rule 7). Both false at first.
    void reportUi(bool textFieldActive, bool popupOpen) noexcept {
        m_textFieldActive = textFieldActive;
        m_popupOpen = popupOpen;
    }

    // The result of the last command a Play In Editor key ran: its error when it refused, success
    // when it ran and before any key ran a command.
    [[nodiscard]] const Expected<void>& lastResult() const noexcept { return m_lastResult; }

private:
    // Whether something besides Play In Editor uses Escape now.
    [[nodiscard]] bool escapeTaken() const;

    EditorSession* m_session;
    const Keymap* m_keymap;
    bool m_textFieldActive = false;
    bool m_popupOpen = false;
    Expected<void> m_lastResult;
};

// The spectator view (ADR-0053 decision 17): a Game-layer consumer that, while a session is
// possessed and the bound window has the mouse captured (EditorSession::mouseCaptured), flies the
// play viewport's camera through the camera controller (updateCamera, as RMB fly does): mouse motion
// turns it, with no button held, and the physical keys W/A/S/D move it along its axes and Q/E down
// and up, at the camera's speed step. Motion that is not finite is ignored. It consumes the motion
// and the fly keys it uses and passes everything else on. With the mouse free (Shift + F1, focus
// loss), ejected, or without a session it moves nothing, and update() forgets the keys it held, so
// they move nothing after a recapture until pressed again. With the editor's stack it gets input
// only while the mouse is captured anyway: with the mouse free the keymap consumer's gate keeps
// everything from the game.
class SpectatorConsumer final : public InputConsumer {
public:
    explicit SpectatorConsumer(EditorSession& session) noexcept : m_session(&session) {}

    InputReply onInputEvent(const Event& event) override;

    // Moves the camera by the held fly keys for `deltaSeconds` while possessed and captured; the
    // first update of a movement moves for at most kMaxFlyStepSeconds, as viewport fly does.
    // Otherwise it moves nothing and forgets the held keys. Returns whether the camera changed since
    // the last call, by these moves or by the motion routed in between, so the editor keeps drawing
    // frames. Precondition (asserted): `deltaSeconds` is finite and non-negative; otherwise nothing
    // moves.
    [[nodiscard]] bool update(float deltaSeconds);

private:
    // The play viewport's camera while a session is possessed and the mouse captured, else null.
    [[nodiscard]] EditorCamera* camera() const noexcept;

    EditorSession* m_session;
    // The fly keys held from presses this consumer saw; only the key fields are used.
    CameraInput m_keys;
    bool m_wasMoving = false;
    bool m_changed = false;
};

} // namespace ith
