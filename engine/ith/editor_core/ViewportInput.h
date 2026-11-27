#pragma once

// ith::ViewportConsumer: the viewport consumer of an editor window's input stack (ADR-0042 rules 7
// to 12), without any UI.
//
//   ith::Input input;
//   ith::KeymapConsumer shortcuts(session, keymap);
//   ith::ViewportConsumer viewportInput(session.viewports());
//   // top first: the UI's consumer (added last), the viewports, the keymap
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
//   input.addConsumer({.window = id, .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
//   ... EditorUi::create({..., .viewports = &viewportInput}) ...
//   while (running) {
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
//
// A click is an LMB press without Alt on an image and its release, with the pointer never more than
// kClickSlop logical points (kClickSlop * setDisplayScale() in pixels) from the press in between.
// On the release the consumer sets the viewport's ViewportState::click: the press's position from
// the image's top-left corner and the press's modifiers (Ctrl toggles, else Shift adds, else the
// click replaces the selection; Ctrl+Shift toggles; PickMode). A newer click replaces one the panel
// hasn't taken yet. A longer LMB drag does nothing. RMB, MMB and Alt gestures never click. Losing
// focus must not pick: the application calls cancelClicks() on WindowFocusLost, after Input's
// synthetic release. The translate gizmo (rule 25) will take an LMB press on one of its handles
// before it becomes a click candidate.
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
// Main thread only; the Viewports store must outlive the consumer.

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/Viewports.h"
#include "ith/input/Input.h"
#include "ith/math/Types.h"
#include "ith/platform_events/InputCodes.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ith {

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
};

class ViewportConsumer final : public InputConsumer {
public:
    explicit ViewportConsumer(Viewports& viewports) noexcept : m_viewports(&viewports) {}

    InputReply onInputEvent(const Event& event) override;

    // Moves the flying camera by the held fly keys at its speed step for `deltaSeconds`; the first
    // update with fly keys held after one without moves for at most kMaxFlyStepSeconds. Returns
    // whether any camera changed since the last call, by these moves or by the mouse events routed
    // in between, so the editor keeps drawing frames while a camera moves. Precondition (asserted):
    // `deltaSeconds` is finite and non-negative; otherwise nothing moves.
    [[nodiscard]] bool update(float deltaSeconds) noexcept;

    // The window's pixels per window coordinate (the platform's pixel density), which drag motion is
    // divided by; 1 at first. Set it before routing the frame's events. Precondition (asserted):
    // finite and positive; otherwise it stays.
    void setPixelDensity(float density) noexcept;

    // The window's display scale (Window::displayScale(): the platform's content scale, which
    // includes the pixel density on every platform), which kClickSlop is multiplied by to give the
    // click slop in pixels; 1 at first. Set it before routing the frame's events. Precondition
    // (asserted): finite and positive; otherwise it stays.
    void setDisplayScale(float scale) noexcept;

    // Drops every click not yet taken (ViewportState::click of every viewport) and the held LMB
    // press's click candidate, so its release doesn't click. Call it on WindowFocusLost, which Input
    // delivers after the release it synthesises for held buttons: a focus loss never picks.
    void cancelClicks() noexcept;

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
    // The field of m_flyKeys that the physical key `key` holds, or null.
    bool* flyKey(Key key) noexcept;
    // Applies `input` to viewport `index`'s camera, and notes whether it changed.
    void apply(std::size_t index, const CameraInput& input, float deltaSeconds) noexcept;

    Viewports* m_viewports;
    std::optional<Drag> m_drag;
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
