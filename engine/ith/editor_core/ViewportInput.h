#pragma once

// ith::ViewportConsumer: the viewport consumer of an editor window's input stack (ADR-0042 rules 7
// to 12), without any UI.
//
//   ith::Input input;
//   ith::KeymapConsumer shortcuts(session, keymap);
//   ith::ViewportConsumer viewportInput(session);
//   // top first: the UI's consumer (added last), the viewports, the keymap
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
//   ... EditorUi::create({..., .viewports = &viewportInput}) ...
//   while (running) {
//       if (/* the batch has WindowFocusLost for the window */) { viewportInput.focusLost(); }
//       input.processEvents(events, platform.pumpTimeNs());
//       if (viewportInput.update(deltaSeconds)) { idle.requestFrame(); }
//   }
//
// It routes by the image rectangles the viewport panels reported to the Viewports store in the last
// frame (ViewportState::imagePosition, imageSize, in window pixels, and imageHovered). Mouse presses,
// motion, the wheel and releases are consumed only over a hovered viewport image (nothing drawn on
// top of it in the last frame), or while the consumer holds a drag that started on one, until its
// release. One drag is held at a time; presses of other buttons during it
// are swallowed. What a press on an image starts depends on its button and on Alt at the press:
//
//   RMB          fly: mouse look, W/A/S/D/Q/E move, the wheel changes the speed step (rule 6)
//   Alt + RMB    dolly toward the pivot
//   Alt + LMB    orbit around the pivot
//   MMB          pan
//   LMB          a click candidate: a click picks (ADR-0042 rule 22)
//   LMB on a gizmo handle, with or without Alt: a gizmo drag (rule 25), checked before the above
//
// The translate gizmo (ViewportGizmo.h): an LMB
// press on one of the handles of the gizmo the viewport shows (viewportGizmo(), at setDisplayScale(),
// hit-tested within kGizmoGrabTolerance points) starts a gizmo drag instead of a click candidate or
// an orbit: GizmoDrag::begin opens the "Translate" transaction, and with Alt at the press first
// duplicates the selection inside it. While the drag is held, ViewportState::gizmoDrag names the
// handle; update() previews the pointer's latest position (GizmoDrag::preview), once per frame; the
// release previews its own position and commits, one undo entry. Esc reaches the keymap, which
// cancels the open transaction (Transactions::cancelOpen): the world is restored, the next update()
// clears gizmoDrag, and the drag stays held, moving nothing, until the release. A release without
// any move cancels instead of committing (GizmoDrag::commit), so an Alt press leaves no copies. A
// drag that can't begin (GizmoDrag::begin's errors) is logged as a warning in the "Gizmo" category,
// except a press that grabs no point of the handle (an expected fallback), and the press starts its
// gesture as if it missed the handles. A drag that begins drops every viewport's waiting click, which
// would select inside the drag's transaction. A press that misses the handles starts its gesture as
// below.
//
// A click is an LMB press without Alt on an image and its release, with the pointer never more than
// kClickSlop logical points (kClickSlop * setDisplayScale() in pixels) from the press in between.
// On the release the consumer sets the viewport's ViewportState::click: the press's position from
// the image's top-left corner and the press's modifiers (Ctrl toggles, else Shift adds, else the
// click replaces the selection; Ctrl+Shift toggles; PickMode). A newer click replaces one the panel
// hasn't taken yet. A longer LMB drag does nothing. RMB, MMB and Alt gestures never click.
//
// Focus loss neither picks nor moves anything: when a batch of events holds WindowFocusLost for the
// window, the application calls focusLost() before Input routes the batch. It cancels a held gizmo
// drag (its Alt copies removed) and drops every waiting click and the held press's click candidate,
// so the release Input synthesises for held buttons neither picks nor commits. A real release earlier
// in the same batch is cancelled too: the batch is checked as a whole.
//
// The wheel over an image without a drag dollies that viewport's camera by steps.
//
// Keys (the letter-key rule, rule 10): while fly is active, every key press without Ctrl or Alt is
// consumed: the physical keys W/A/S/D move along the camera's axes and Q/E down and up in world
// space, and other keys are swallowed, so W doesn't switch the tool mid-flight. Chords with Ctrl or
// Alt pass on to the keymap. Without fly no key is consumed. Releases reach this consumer only when
// it saw the press (Input's rule).
//
// Mouse motion and the wheel change the camera as their events arrive; fly movement needs the
// frame's time and is applied by update(). Drag motion reaches the camera in window coordinates
// (event pixels divided by setPixelDensity()), so orbit, pan and dolly feel the same on any display
// scale; fly's look motion is relative mode's raw mouse motion, which no density scales. The editor
// UI asks flying() to put the window in relative mouse mode, dragStart() for where to put the cursor
// back when it ends, and holdsDrag() and viewportAt() to leave the viewport's mouse events to it.
// Main thread only; the session must outlive the consumer.

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/ViewportGizmo.h"
#include "ith/editor_core/Viewports.h"
#include "ith/input/Input.h"
#include "ith/math/Types.h"
#include "ith/platform_events/InputCodes.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ith {

class EditorSession;
struct Event;

// The longest frame time the first update() of a fly movement moves the camera for, in seconds:
// the frame after an idle wait (up to a tenth of a second at the idle rate) moves no farther than a
// 30 Hz frame. Later updates of the same movement use their whole frame time, so the fly speed
// doesn't depend on the frame rate.
inline constexpr float kMaxFlyStepSeconds = 1.0f / 30.0f;

// How far, in logical points (times the display scale in pixels), the pointer may move between an
// LMB press and its release for them to be a click (ADR-0042 rule 12; ADR-0047 item 1): the same
// physical size at any display scale, on every platform.
inline constexpr float kClickSlop = 4.0f;

// What a held viewport drag does (ADR-0042 rule 12).
enum class ViewportGesture : std::uint8_t {
    Fly,   // RMB
    Dolly, // Alt + RMB
    Orbit, // Alt + LMB
    Pan,   // MMB
    Click, // LMB: a click candidate
    Gizmo, // LMB on a gizmo handle, with or without Alt
};

class ViewportConsumer final : public InputConsumer {
public:
    // Camera gestures, clicks and the translate gizmo on the viewports of `session.viewports()`.
    explicit ViewportConsumer(EditorSession& session) noexcept;

    InputReply onInputEvent(const Event& event) override;

    // Moves the flying camera by the held fly keys at its speed step for `deltaSeconds`; the first
    // update with fly keys held after one without moves for at most kMaxFlyStepSeconds. Previews a
    // held gizmo drag at the pointer's latest position when it moved since the last call. Returns
    // whether any camera changed since the last call, by these moves or by the mouse events routed
    // in between, so the editor keeps drawing frames while a camera moves (a gizmo preview changes
    // the document, which EditorChanges reports). Precondition (asserted): `deltaSeconds` is finite
    // and non-negative; otherwise no camera moves.
    [[nodiscard]] bool update(float deltaSeconds);

    // The window's pixels per window coordinate (the platform's pixel density), which drag motion is
    // divided by; 1 at first. Set it before routing the frame's events. Precondition (asserted):
    // finite and positive; otherwise it stays.
    void setPixelDensity(float density) noexcept;

    // The window's display scale (Window::displayScale(): the platform's content scale, which
    // includes the pixel density on every platform), which kClickSlop is multiplied by to give the
    // click slop in pixels; 1 at first. Set it before routing the frame's events. Precondition
    // (asserted): finite and positive; otherwise it stays.
    void setDisplayScale(float scale) noexcept;

    // Cancels a held gizmo drag (its Alt copies removed), and drops every click not yet taken
    // (ViewportState::click of every viewport) and the held LMB press's click candidate, so the
    // release Input synthesises for held buttons neither commits nor clicks. Call it before Input
    // routes a batch of events that holds WindowFocusLost for the window: a focus loss never picks or
    // moves anything, and a real release earlier in that batch is cancelled too. A held drag stays
    // held until its release.
    void focusLost();

    // Whether a drag that started on a viewport image is held, until its button's release.
    [[nodiscard]] bool holdsDrag() const noexcept { return m_drag.has_value(); }

    // Whether fly is active: RMB held from a press without Alt on a viewport image.
    [[nodiscard]] bool flying() const noexcept { return m_drag.has_value() && m_drag->gesture == ViewportGesture::Fly; }

    // The held drag's gesture; std::nullopt without a drag.
    [[nodiscard]] std::optional<ViewportGesture> gesture() const noexcept {
        return m_drag.has_value() ? std::optional<ViewportGesture>(m_drag->gesture) : std::nullopt;
    }

    // Where the held drag's press was, in window pixels; std::nullopt without a drag.
    [[nodiscard]] std::optional<Vec2> dragStart() const noexcept {
        return m_drag.has_value() ? std::optional<Vec2>(m_drag->start) : std::nullopt;
    }

    // The index (Viewports::at) of the viewport whose last reported image contains the point at `x`,
    // `y` in window pixels and was hovered in the last frame (ViewportState::imageHovered), or
    // std::nullopt. An image without area contains no point.
    [[nodiscard]] std::optional<std::size_t> viewportAt(float x, float y) const noexcept;

private:
    struct Drag {
        std::size_t viewport = 0;
        MouseButton button = MouseButton::None;
        ViewportGesture gesture = ViewportGesture::Fly;
        Vec2 start{0.0f};
        // A Click candidate's click, set at the press; reset once the pointer moves past kClickSlop.
        std::optional<ViewportClick> click;
    };

    InputReply onMouseButtonDown(const Event& event);
    InputReply onKeyDown(const Event& event);
    // Starts a gizmo drag for an LMB press at window pixel `press` on viewport `index` when it hits a
    // handle of the gizmo the viewport shows; returns whether it did.
    bool beginGizmo(std::size_t index, Vec2 press, bool alt);
    // Previews the held gizmo drag at window pixel `pointer`, logging a refused change.
    void previewGizmo(Vec2 pointer);
    // Ends the held gizmo drag of viewport `viewport` on its release at window pixel `release`:
    // previews there and commits.
    void endGizmo(std::size_t viewport, Vec2 release);
    // The field of m_flyKeys that the physical key `key` holds, or null.
    bool* flyKey(Key key) noexcept;
    // Applies `input` to viewport `index`'s camera, and notes whether it changed.
    void apply(std::size_t index, const CameraInput& input, float deltaSeconds) noexcept;

    EditorSession* m_session;
    Viewports* m_viewports;
    std::optional<Drag> m_drag;
    // The held gizmo drag (a Gizmo gesture), the pointer's latest window pixel, and whether it moved
    // since the last preview.
    std::optional<GizmoDrag> m_gizmo;
    Vec2 m_gizmoPointer{0.0f};
    bool m_gizmoMoved = false;
    // The fly keys held from presses this consumer took; only the key fields are used.
    CameraInput m_flyKeys;
    // Wheel notches of the current fly not yet turned into speed steps.
    float m_speedWheel = 0.0f;
    float m_pixelDensity = 1.0f;
    float m_displayScale = 1.0f;
    // The last update moved the flying camera.
    bool m_wasMoving = false;
    bool m_changed = false;
};

} // namespace ith
