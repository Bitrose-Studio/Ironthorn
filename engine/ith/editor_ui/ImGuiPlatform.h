#pragma once

// The platform backend of the editor UI (ADR-0029 rules 6 and 7): engine events in, clipboard,
// IME and cursor out. Part of EditorUi, not of the module's API; no ImGui type appears here.
//
// It is the UI's input consumer, in the Editor layer of its window's stack, and sees every event
// that reaches it. It hands every key, mouse and text event to ImGui, and consumes
// - key presses only while ImGui wants text input (a text field is active), so W in a text field
//   types W and doesn't reach the keymap below; it is then also the window's focused consumer
//   with acceptsText, which turns the platform's text input on;
// - mouse presses, motion and the wheel while ImGui wants the mouse.
// The flags come from the previous ImGui frame, because routing runs before NewFrame; whether a
// text field is active is known at the end of the frame that activates it (ImGui reports it with
// the IME data), so the focus and the key routing follow at once. While the
// mouse is captured (Play In Editor), ImGui is told the mouse is gone.

#include "ith/input/Input.h"
#include "ith/platform/Window.h"

#include <string>

namespace ith {

class UiHost;
struct UiFrameInput;

class UiPlatformBackend final : public InputConsumer {
public:
    // Installs the clipboard and IME functions in the current ImGui context and adds itself to
    // `window`'s input stack. Preconditions: an ImGui context exists; `input` outlives this.
    UiPlatformBackend(Input& input, WindowId window, UiHost* host);
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
    Input* m_input;
    WindowId m_window;
    UiHost* m_host;
    ConsumerId m_consumer = kNoConsumer;
    // Window pixels per window coordinate, from the last frame: events carry pixels, ImGui works in
    // window coordinates.
    float m_pixelsPerUnitX = 1.0f;
    float m_pixelsPerUnitY = 1.0f;
    bool m_focused = true;
    bool m_mouseCaptured = false;
    // A text field is active (ImGui's IME data), and the consumer holds the text focus for it.
    bool m_textInput = false;
    bool m_focusedForText = false;
    // The ImGui cursor last handed to the host (ImGuiMouseCursor; -1 hides it), or kNoCursor
    // before the first.
    static constexpr int kNoCursor = -2;
    int m_cursor = kNoCursor;
};

} // namespace ith
