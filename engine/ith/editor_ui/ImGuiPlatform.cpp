#include "ith/editor_ui/ImGuiPlatform.h"

#include "ith/editor_ui/EditorUi.h"
#include "ith/foundation/Assert.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <optional>
#include <string>

// The official SDL3 backend (MIT) was the behavioural reference for the key mapping, the cursor
// and the IME; this backend reads engine events instead of SDL's.

namespace ith {
namespace {

UiPlatformBackend* backendOf(const void* userData) {
    return static_cast<UiPlatformBackend*>(const_cast<void*>(userData));
}

const char* getClipboardText(ImGuiContext* /*context*/) {
    UiPlatformBackend* backend = backendOf(ImGui::GetPlatformIO().Platform_ClipboardUserData);
    if (backend == nullptr || backend->host() == nullptr) {
        return nullptr;
    }
    backend->clipboard = backend->host()->clipboardText();
    return backend->clipboard.c_str();
}

void setClipboardText(ImGuiContext* /*context*/, const char* text) {
    const UiPlatformBackend* backend = backendOf(ImGui::GetPlatformIO().Platform_ClipboardUserData);
    if (backend != nullptr && backend->host() != nullptr && text != nullptr) {
        backend->host()->setClipboardText(text);
    }
}

// The physical keys ImGui names; letters and digits follow the layout (keyFor).
ImGuiKey physicalKey(Key key) {
    switch (key) {
        case Key::Enter:
            return ImGuiKey_Enter;
        case Key::Escape:
            return ImGuiKey_Escape;
        case Key::Backspace:
            return ImGuiKey_Backspace;
        case Key::Tab:
            return ImGuiKey_Tab;
        case Key::Space:
            return ImGuiKey_Space;
        case Key::Minus:
            return ImGuiKey_Minus;
        case Key::Equals:
            return ImGuiKey_Equal;
        case Key::LeftBracket:
            return ImGuiKey_LeftBracket;
        case Key::RightBracket:
            return ImGuiKey_RightBracket;
        case Key::Backslash:
        case Key::NonUsHash:
            return ImGuiKey_Backslash;
        case Key::Semicolon:
            return ImGuiKey_Semicolon;
        case Key::Apostrophe:
            return ImGuiKey_Apostrophe;
        case Key::Grave:
            return ImGuiKey_GraveAccent;
        case Key::Comma:
            return ImGuiKey_Comma;
        case Key::Period:
            return ImGuiKey_Period;
        case Key::Slash:
            return ImGuiKey_Slash;
        case Key::CapsLock:
            return ImGuiKey_CapsLock;
        case Key::PrintScreen:
            return ImGuiKey_PrintScreen;
        case Key::ScrollLock:
            return ImGuiKey_ScrollLock;
        case Key::Pause:
            return ImGuiKey_Pause;
        case Key::Insert:
            return ImGuiKey_Insert;
        case Key::Home:
            return ImGuiKey_Home;
        case Key::PageUp:
            return ImGuiKey_PageUp;
        case Key::Delete:
            return ImGuiKey_Delete;
        case Key::End:
            return ImGuiKey_End;
        case Key::PageDown:
            return ImGuiKey_PageDown;
        case Key::Right:
            return ImGuiKey_RightArrow;
        case Key::Left:
            return ImGuiKey_LeftArrow;
        case Key::Down:
            return ImGuiKey_DownArrow;
        case Key::Up:
            return ImGuiKey_UpArrow;
        case Key::NumLock:
            return ImGuiKey_NumLock;
        case Key::KeypadDivide:
            return ImGuiKey_KeypadDivide;
        case Key::KeypadMultiply:
            return ImGuiKey_KeypadMultiply;
        case Key::KeypadMinus:
            return ImGuiKey_KeypadSubtract;
        case Key::KeypadPlus:
            return ImGuiKey_KeypadAdd;
        case Key::KeypadEnter:
            return ImGuiKey_KeypadEnter;
        case Key::KeypadPeriod:
            return ImGuiKey_KeypadDecimal;
        case Key::KeypadEquals:
            return ImGuiKey_KeypadEqual;
        case Key::NonUsBackslash:
            return ImGuiKey_Oem102;
        case Key::Application:
            return ImGuiKey_Menu;
        case Key::LeftControl:
            return ImGuiKey_LeftCtrl;
        case Key::LeftShift:
            return ImGuiKey_LeftShift;
        case Key::LeftAlt:
            return ImGuiKey_LeftAlt;
        case Key::LeftSuper:
            return ImGuiKey_LeftSuper;
        case Key::RightControl:
            return ImGuiKey_RightCtrl;
        case Key::RightShift:
            return ImGuiKey_RightShift;
        case Key::RightAlt:
            return ImGuiKey_RightAlt;
        case Key::RightSuper:
            return ImGuiKey_RightSuper;
        default:
            break;
    }
    const auto code = static_cast<int>(key);
    if (key >= Key::F1 && key <= Key::F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (code - static_cast<int>(Key::F1)));
    }
    if (key >= Key::F13 && key <= Key::F24) {
        return static_cast<ImGuiKey>(ImGuiKey_F13 + (code - static_cast<int>(Key::F13)));
    }
    if (key >= Key::Keypad1 && key <= Key::Keypad9) {
        return static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (code - static_cast<int>(Key::Keypad1)));
    }
    if (key == Key::Keypad0) {
        return ImGuiKey_Keypad0;
    }
    if (key >= Key::A && key <= Key::Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (code - static_cast<int>(Key::A)));
    }
    if (key >= Key::Digit1 && key <= Key::Digit9) {
        return static_cast<ImGuiKey>(ImGuiKey_1 + (code - static_cast<int>(Key::Digit1)));
    }
    if (key == Key::Digit0) {
        return ImGuiKey_0;
    }
    return ImGuiKey_None;
}

// Letters and digits follow the user's layout, so Ctrl + Z in a text field is the key that prints
// Z, as editor shortcuts are (KeymapConsumer); other keys are physical.
ImGuiKey keyFor(const Event& event) {
    const char32_t printed = event.layoutKey;
    if (printed >= U'a' && printed <= U'z') {
        return static_cast<ImGuiKey>(ImGuiKey_A + static_cast<int>(printed - U'a'));
    }
    const bool keypad = event.key >= Key::KeypadDivide && event.key <= Key::KeypadPeriod;
    if (!keypad && printed >= U'0' && printed <= U'9') {
        return static_cast<ImGuiKey>(ImGuiKey_0 + static_cast<int>(printed - U'0'));
    }
    return physicalKey(event.key);
}

void addModifiers(ImGuiIO& io, KeyModifiers modifiers) {
    io.AddKeyEvent(ImGuiMod_Ctrl, hasModifiers(modifiers, KeyModifiers::Control));
    io.AddKeyEvent(ImGuiMod_Shift, hasModifiers(modifiers, KeyModifiers::Shift));
    io.AddKeyEvent(ImGuiMod_Alt, hasModifiers(modifiers, KeyModifiers::Alt));
    io.AddKeyEvent(ImGuiMod_Super, hasModifiers(modifiers, KeyModifiers::Super));
}

std::optional<ImGuiMouseButton> mouseButtonFor(MouseButton button) {
    switch (button) {
        case MouseButton::Left:
            return ImGuiMouseButton_Left;
        case MouseButton::Right:
            return ImGuiMouseButton_Right;
        case MouseButton::Middle:
            return ImGuiMouseButton_Middle;
        case MouseButton::Back:
            return 3;
        case MouseButton::Forward:
            return 4;
        case MouseButton::None:
            break;
    }
    return std::nullopt;
}

std::optional<CursorShape> cursorShapeFor(ImGuiMouseCursor cursor) {
    switch (cursor) {
        case ImGuiMouseCursor_Arrow:
            return CursorShape::Arrow;
        case ImGuiMouseCursor_TextInput:
            return CursorShape::TextInput;
        case ImGuiMouseCursor_ResizeAll:
            return CursorShape::ResizeAll;
        case ImGuiMouseCursor_ResizeNS:
            return CursorShape::ResizeNorthSouth;
        case ImGuiMouseCursor_ResizeEW:
            return CursorShape::ResizeEastWest;
        case ImGuiMouseCursor_ResizeNESW:
            return CursorShape::ResizeNorthEastSouthWest;
        case ImGuiMouseCursor_ResizeNWSE:
            return CursorShape::ResizeNorthWestSouthEast;
        case ImGuiMouseCursor_Hand:
            return CursorShape::Hand;
        case ImGuiMouseCursor_Wait:
            return CursorShape::Wait;
        case ImGuiMouseCursor_Progress:
            return CursorShape::Progress;
        case ImGuiMouseCursor_NotAllowed:
            return CursorShape::NotAllowed;
        default:
            return std::nullopt;
    }
}

} // namespace

UiPlatformBackend::UiPlatformBackend(Input& input, WindowId window, UiHost* host)
    : m_input(&input), m_window(window), m_host(host) {
    ITH_ASSERT_MSG(ImGui::GetCurrentContext() != nullptr, "the platform backend needs the UI's ImGui context");
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "ironthorn_platform";
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_GetClipboardTextFn = getClipboardText;
    platformIo.Platform_SetClipboardTextFn = setClipboardText;
    platformIo.Platform_ClipboardUserData = this;
    platformIo.Platform_ImeUserData = this;
    platformIo.Platform_SetImeDataFn = [](ImGuiContext* /*context*/, ImGuiViewport* /*viewport*/,
                                          ImGuiPlatformImeData* data) {
        UiPlatformBackend* backend = backendOf(ImGui::GetPlatformIO().Platform_ImeUserData);
        if (backend == nullptr || data == nullptr) {
            return;
        }
        // ImGui calls this at the end of a frame whose IME data changed; WantTextInput is the value
        // io.WantTextInput takes in the next frame.
        backend->m_textInput = data->WantTextInput;
        if (backend->host() == nullptr || !data->WantVisible) {
            return;
        }
        // ImGui's position is in window coordinates; the IME's area is in window pixels.
        backend->host()->setTextInputArea(
            {.x = static_cast<std::int32_t>(data->InputPos.x * backend->m_pixelsPerUnitX),
             .y = static_cast<std::int32_t>(data->InputPos.y * backend->m_pixelsPerUnitY),
             .width = 0,
             .height = static_cast<std::int32_t>(std::max(0.0f, data->InputLineHeight) * backend->m_pixelsPerUnitY),
             .cursor = 0});
    };
    m_consumer = input.addConsumer({.window = window, .layer = ConsumerLayer::Editor, .consumer = this});
}

UiPlatformBackend::~UiPlatformBackend() {
    m_input->removeConsumer(m_consumer);
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
        platformIo.Platform_GetClipboardTextFn = nullptr;
        platformIo.Platform_SetClipboardTextFn = nullptr;
        platformIo.Platform_ClipboardUserData = nullptr;
        platformIo.Platform_SetImeDataFn = nullptr;
        platformIo.Platform_ImeUserData = nullptr;
        ImGuiIO& io = ImGui::GetIO();
        io.BackendPlatformName = nullptr;
        // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
        io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
    }
}

InputReply UiPlatformBackend::onInputEvent(const Event& event) {
    ImGuiIO& io = ImGui::GetIO();
    const auto consumedIf = [](bool condition) { return condition ? InputReply::Consumed : InputReply::NotConsumed; };
    switch (event.type) {
        case EventType::KeyDown:
        case EventType::KeyUp: {
            const bool down = event.type == EventType::KeyDown;
            if (!event.repeat) {
                addModifiers(io, event.modifiers);
                const ImGuiKey key = keyFor(event);
                if (key != ImGuiKey_None) {
                    io.AddKeyEvent(key, down);
                }
            }
            return consumedIf(down && m_textInput);
        }
        case EventType::TextInput: {
            const std::string text(event.text);
            io.AddInputCharactersUTF8(text.c_str());
            return InputReply::Consumed;
        }
        case EventType::MouseMoved:
            io.AddMousePosEvent(event.x / m_pixelsPerUnitX, event.y / m_pixelsPerUnitY);
            return consumedIf(io.WantCaptureMouse);
        case EventType::MouseButtonDown:
        case EventType::MouseButtonUp: {
            const std::optional<ImGuiMouseButton> button = mouseButtonFor(event.mouseButton);
            if (!button) {
                return InputReply::NotConsumed;
            }
            addModifiers(io, event.modifiers);
            io.AddMousePosEvent(event.x / m_pixelsPerUnitX, event.y / m_pixelsPerUnitY);
            io.AddMouseButtonEvent(*button, event.type == EventType::MouseButtonDown);
            return consumedIf(io.WantCaptureMouse);
        }
        case EventType::MouseWheel:
            addModifiers(io, event.modifiers);
            // ImGui scrolls left for a positive horizontal step; engine steps are positive to the right.
            io.AddMouseWheelEvent(-event.deltaX, event.deltaY);
            return consumedIf(io.WantCaptureMouse);
        default:
            return InputReply::NotConsumed;
    }
}

void UiPlatformBackend::newFrame(const UiFrameInput& frame) {
    ImGuiIO& io = ImGui::GetIO();
    const float width = static_cast<float>(std::max(frame.size.width, 1));
    const float height = static_cast<float>(std::max(frame.size.height, 1));
    io.DisplaySize = {width, height};
    m_pixelsPerUnitX = frame.pixelSize.width > 0 ? static_cast<float>(frame.pixelSize.width) / width : 1.0f;
    m_pixelsPerUnitY = frame.pixelSize.height > 0 ? static_cast<float>(frame.pixelSize.height) / height : 1.0f;
    io.DisplayFramebufferScale = {m_pixelsPerUnitX, m_pixelsPerUnitY};
    io.DeltaTime = std::max(frame.deltaSeconds, 1.0e-6f);

    if (frame.focused != m_focused) {
        io.AddFocusEvent(frame.focused);
        m_focused = frame.focused;
    }
    // While Play In Editor has the mouse, Input sends it only to the game; the UI sees no mouse.
    const bool captured = m_input->mouseCaptured(m_window);
    if (captured && !m_mouseCaptured) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    m_mouseCaptured = captured;

    // The cursor the last frame asked for.
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    if (m_host != nullptr && (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0 && !captured) {
        const ImGuiMouseCursor cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
        if (cursor != m_cursor) {
            m_host->setCursor(cursorShapeFor(cursor));
            m_cursor = cursor;
        }
    }
}

void UiPlatformBackend::endFrame() {
    // A text field became active or inactive in this frame: it takes or gives back the window's
    // text input.
    if (m_textInput != m_focusedForText) {
        if (m_textInput) {
            m_input->setFocusedConsumer(m_consumer, true);
        } else if (m_input->focusedConsumer(m_window) == m_consumer) {
            m_input->clearFocusedConsumer(m_window);
        }
        m_focusedForText = m_textInput;
    }
}

std::string WindowUiHost::clipboardText() {
    return m_platform->clipboardText();
}

void WindowUiHost::setClipboardText(std::string_view text) {
    // Text with a NUL character can't go on the clipboard; ImGui passes C strings, so it has none.
    static_cast<void>(m_platform->setClipboardText(text));
}

void WindowUiHost::setTextInputArea(const TextInputArea& area) {
    static_cast<void>(m_window->setTextInputArea(area));
}

void WindowUiHost::setCursor(std::optional<CursorShape> shape) {
    // The offscreen driver has no system cursors; the shape then stays as it was.
    if (shape) {
        static_cast<void>(m_platform->setCursor(*shape));
    }
    static_cast<void>(m_platform->setCursorVisible(shape.has_value()));
}

} // namespace ith
