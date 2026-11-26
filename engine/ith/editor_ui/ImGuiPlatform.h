#pragma once

// The platform backend of the editor UI (ADR-0029 rules 6 and 7): engine events in, clipboard,
// IME, cursor and relative mouse mode out. Part of EditorUi, not of the module's API; no ImGui type
// appears here.
//
// It is the UI's input consumer, in the Editor layer of its window's stack, and sees every event
// that reaches it. It hands key, mouse and text events to ImGui, and consumes
// - key presses only while ImGui wants text input (a text field is active), so W in a text field
//   types W and doesn't reach the keymap below; it is then also the window's focused consumer
//   with acceptsText, which turns the platform's text input on;
// - mouse presses, motion and the wheel while ImGui wants the mouse, except over a viewport image
//   while no ImGui popup is open: those go on to the viewport consumer below (ADR-0042 rule 9).
// While the viewport consumer holds a drag, the mouse presses, motion and wheel are its alone:
// ImGui neither sees nor takes them and is told the mouse is gone, so a drag that leaves the image
// keeps going and touches no panel; releases still reach ImGui. The want-capture flags come from
// the previous ImGui frame, because routing runs before NewFrame; whether a
// text field is active is known at the end of the frame that activates it (ImGui reports it with
// the IME data), so the focus and the key routing follow at once. While the
// mouse is captured (Play In Editor), ImGui is told the mouse is gone. Relative mouse mode is on
// while the mouse is captured or a viewport flies (ADR-0042 rule 11); when fly ends, the cursor is
// put back where its press was before relative mode turns off.

#include "ith/input/Input.h"
#include "ith/math/Types.h"
#include "ith/platform/Window.h"

#include <optional>
#include <string>

namespace ith {

class UiHost;
class ViewportConsumer;
struct UiFrameInput;

class UiPlatformBackend final : public InputConsumer {
public:
    // Installs the clipboard and IME functions in the current ImGui context and adds itself to
    // `window`'s input stack. `viewports` is the window's viewport consumer, already in the stack
    // (null: none). Preconditions: an ImGui context exists; `input` and `viewports` outlive this.
    UiPlatformBackend(Input& input, WindowId window, UiHost* host, const ViewportConsumer* viewports);
    UiPlatformBackend(const UiPlatformBackend&) = delete;
    UiPlatformBackend& operator=(const UiPlatformBackend&) = delete;
    UiPlatformBackend(UiPlatformBackend&&) = delete;
    UiPlatformBackend& operator=(UiPlatformBackend&&) = delete;
    // Removes the consumer and the ImGui functions.
    ~UiPlatformBackend() override;

    InputReply onInputEvent(const Event& event) override;

    // Before ImGui::NewFrame: sizes, time, focus, capture and the cursor.
    void newFrame(const UiFrameInput& frame);

    // After ImGui::Render: takes or gives back the text focus when a text field became active or
    // inactive.
    void endFrame();

    // Whether a text field is active, as of the last frame's end.
    [[nodiscard]] bool textInput() const noexcept { return m_textInput; }

    [[nodiscard]] ConsumerId consumer() const noexcept { return m_consumer; }

    // For the ImGui callbacks.
    [[nodiscard]] UiHost* host() const noexcept { return m_host; }

    // The last clipboard text read, which ImGui reads through a pointer.
    std::string clipboard;

private:
    // Whether the viewport consumer holds a drag (ADR-0042 rule 9): ImGui then gets no mouse
    // presses, motion or wheel, and never takes them.
    [[nodiscard]] bool viewportDragHeld() const;
    // Whether the mouse event is over a viewport image while no ImGui popup is open: ImGui sees it
    // but leaves it to the viewport consumer.
    [[nodiscard]] bool overViewportImage(const Event& event) const;

    Input* m_input;
    WindowId m_window;
    UiHost* m_host;
    const ViewportConsumer* m_viewports;
    ConsumerId m_consumer = kNoConsumer;
    // Window pixels per window coordinate, from the last frame: events carry pixels, ImGui works in
    // window coordinates.
    float m_pixelsPerUnitX = 1.0f;
    float m_pixelsPerUnitY = 1.0f;
    bool m_focused = true;
    // ImGui was told the mouse is gone: it is captured, or a viewport drag is held.
    bool m_mouseHidden = false;
    // The relative mouse mode last handed to the host; windows start without it.
    bool m_relativeMouse = false;
    // Where the current fly's press was, in window pixels, to put the cursor back when it ends.
    std::optional<Vec2> m_flyStart;
    // A text field is active (ImGui's IME data), and the consumer holds the text focus for it.
    bool m_textInput = false;
    bool m_focusedForText = false;
    // The ImGui cursor last handed to the host (ImGuiMouseCursor; -1 hides it), or kNoCursor
    // before the first.
    static constexpr int kNoCursor = -2;
    int m_cursor = kNoCursor;
};

} // namespace ith
