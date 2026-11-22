#pragma once

// ith::EditorUi: the editor's immediate-mode UI on Dear ImGui (ADR-0029). It owns the one ImGui
// context, the fonts and theme, the platform backend (engine events in, clipboard, IME and cursor
// out through a UiHost) and the renderer backend (ImGui's draw data as render graph passes on the
// RHI). No ImGui type appears in this header; panels live in editor_ui and draw with ImGui
// directly.
//
//   ith::Input input;
//   ith::KeymapConsumer shortcuts(session, keymap);
//   input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
//   ith::WindowUiHost host(platform, window);
//   auto ui = ith::EditorUi::create({.device = &device, .targetFormat = format, .vertexShader = vs,
//                                    .pixelShader = ps, .fontDirectory = fonts, .input = &input,
//                                    .window = window.id(), .host = &host});
//   // added after the keymap consumer, so ImGui's consumer sits above it (ADR-0029 rule 7)
//   while (running) {
//       input.processEvents(events, platform.pumpTimeNs());
//       (*ui)->newFrame({.size = window.size(), .pixelSize = window.pixelSize(), .deltaSeconds = dt,
//                        .focused = input.focusedWindow() == window.id()});
//       ith::drawEditorShell(session, keymap, shell);     // panels
//       (*ui)->endFrame();
//       ... device.beginFrame(), graph.reset(), target = graph.importBackbuffer(frame) ...
//       (*ui)->addRenderPasses(graph, target, frame.slot, ith::rhi::LoadOp::Clear);
//       graph.compile(); graph.execute(pool);
//   }
//
// One EditorUi exists at a time (asserted): ImGui has one global context, used on the main thread
// only (ADR-0029 rule 3). Render graph execute callables read only the renderer's own copies of the
// frame's draw data, never ImGui.

#include "ith/editor_ui/Theme.h"
#include "ith/foundation/Expected.h"
#include "ith/input/Input.h"
#include "ith/platform/Platform.h"
#include "ith/platform/Window.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class UiPlatformBackend;
class UiRenderer;

// What the platform backend needs from the window it runs in: the clipboard, the IME's place and
// the cursor (ADR-0029 rule 6). WindowUiHost implements it over ith::Platform and ith::Window; tests
// implement it to record the calls.
class UiHost {
public:
    UiHost() = default;
    UiHost(const UiHost&) = delete;
    UiHost& operator=(const UiHost&) = delete;
    UiHost(UiHost&&) = delete;
    UiHost& operator=(UiHost&&) = delete;
    virtual ~UiHost() = default;

    // The clipboard's UTF-8 text; empty when it holds none.
    [[nodiscard]] virtual std::string clipboardText() = 0;
    virtual void setClipboardText(std::string_view text) = 0;
    // Where the focused text field's cursor is, in window pixels, so the IME's candidate window
    // appears next to it.
    virtual void setTextInputArea(const TextInputArea& area) = 0;
    // The cursor's shape, or std::nullopt to hide it.
    virtual void setCursor(std::optional<CursorShape> shape) = 0;
};

// A UiHost over the application's Platform and one of its windows. Failures are dropped: the
// offscreen driver has no system cursors, and an unreadable clipboard pastes nothing.
class WindowUiHost final : public UiHost {
public:
    WindowUiHost(Platform& platform, Window& window) noexcept : m_platform(&platform), m_window(&window) {}

    [[nodiscard]] std::string clipboardText() override;
    void setClipboardText(std::string_view text) override;
    void setTextInputArea(const TextInputArea& area) override;
    void setCursor(std::optional<CursorShape> shape) override;

private:
    Platform* m_platform;
    Window* m_window;
};

// The fonts of the editor (ADR-0029 rule 14). Text and Bold carry the icon font merged in.
enum class UiFont : std::uint8_t {
    Text,
    Bold,
    Mono,
};

// The font files EditorUi loads from EditorUiDesc::fontDirectory, by UiFont, then the icon font.
inline constexpr std::array<std::string_view, 3> kEditorFontFiles = {"Inter-Regular.ttf", "Inter-SemiBold.ttf",
                                                                     "JetBrainsMono-Regular.ttf"};
inline constexpr std::string_view kEditorIconFontFile = "lucide.ttf";

struct EditorUiDesc {
    // The device the UI renders with; it must outlive the EditorUi. Required.
    rhi::Device* device = nullptr;
    // The format of the textures the UI draws into (the swapchain's): an sRGB format, so blending in
    // linear space is encoded on write (ADR-0029 rule 11).
    rhi::Format targetFormat = rhi::Format::B8G8R8A8Srgb;
    // The compiled stages of editorUiShaderSource() (EditorUiShader.h), in the device's bytecode
    // format; read during create() only. The null device takes any non-empty bytes.
    std::span<const std::byte> vertexShader;
    std::span<const std::byte> pixelShader;
    // The folder holding kEditorFontFiles and kEditorIconFontFile, UTF-8 (the build copies them to
    // "fonts" next to the executables).
    std::string_view fontDirectory;
    // The application's input and the window the UI belongs to. create() adds the UI's consumer to
    // that window's stack in the Editor layer, so it sits above consumers added before it. The
    // Input must outlive the EditorUi. Required.
    Input* input = nullptr;
    WindowId window = kNoWindow;
    // Clipboard, IME and cursor; null drops them (headless tests). Must outlive the EditorUi.
    UiHost* host = nullptr;
    ThemeTokens theme = defaultDarkTheme();
    float displayScale = 1.0f;
    // Off draws the text cursor without blinking, for reproducible images.
    bool caretBlink = true;
};

// One frame's view of the window.
struct UiFrameInput {
    // The window's size in window coordinates and in pixels (Window::size(), Window::pixelSize()).
    WindowSize size;
    WindowSize pixelSize;
    // Seconds since the last frame; values below a microsecond count as one.
    float deltaSeconds = 1.0f / 60.0f;
    // Whether the window has keyboard focus.
    bool focused = true;
};

// What the renderer prepared for the last frame, for tests and statistics.
struct UiRenderStats {
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t indices = 0;
    // Texture copies recorded this frame: whole textures created, and changed rectangles.
    std::uint32_t textureUploads = 0;
    // Textures the renderer holds for ImGui.
    std::uint32_t textures = 0;
    // The size of this frame's upload buffer, which grows when a frame needs more.
    std::uint64_t uploadBufferSize = 0;
    // The smallest rectangle holding every draw's scissor, in target pixels; scissors are clamped
    // to the target, so it never reaches outside.
    rhi::Rect scissorBounds;
};

class EditorUi {
public:
    // Creates the ImGui context, loads the fonts, applies the theme, adds the UI's input consumer,
    // and creates the renderer's pipeline and sampler. Fails with "editor_ui.font_failed" (a font
    // file can't be read or parsed) or "editor_ui.renderer_failed" (the pipeline or sampler can't be
    // created), the RHI's message inside. Preconditions (asserted): no other EditorUi exists; the
    // device, input and window are set; both shaders are not empty.
    static Expected<std::unique_ptr<EditorUi>> create(const EditorUiDesc& desc);

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;
    EditorUi(EditorUi&&) = delete;
    EditorUi& operator=(EditorUi&&) = delete;
    // Removes the input consumer, destroys the UI's textures (deferred by the device) and the
    // ImGui context.
    ~EditorUi();

    // Starts a UI frame: hands the frame's window sizes, time and focus to ImGui, applies the
    // cursor the last frame asked for, and begins ImGui's frame, after which panels draw. The
    // events Input routed to the UI since the last frame are applied here, so the want-capture
    // flags they were routed by come from the frame before (ImGui's normal contract). Precondition
    // (asserted): no frame is open.
    void newFrame(const UiFrameInput& input);

    // Ends the UI frame and turns it into draw data. Precondition (asserted): a frame is open.
    void endFrame();

    // Adds the UI's passes to `graph` after copying the last endFrame()'s geometry and texture
    // changes into the upload buffer of `frameSlot` (rhi::Frame::slot): "EditorUiUpload" (Copy), when
    // textures change, and "EditorUi" (Raster), which draws into `target` with `load` (and
    // `clearColor` for LoadOp::Clear). Imports the upload buffer and every UI texture. Call it once
    // per frame after the device's beginFrame() and before graph.compile(); the passes read only
    // the renderer's copies, valid until the next addRenderPasses(). Precondition (asserted): no UI
    // frame is open, and endFrame() ran since the last call.
    void addRenderPasses(rg::RenderGraph& graph, rg::Texture target, std::uint32_t frameSlot,
                         rhi::LoadOp load = rhi::LoadOp::Load,
                         const std::array<float, 4>& clearColor = {0.0f, 0.0f, 0.0f, 1.0f});

    // Applies the theme again at a new display scale (WindowDisplayScaleChanged). Fonts follow
    // without rebuilding the atlas.
    void setDisplayScale(float displayScale);

    [[nodiscard]] float displayScale() const noexcept { return m_displayScale; }

    [[nodiscard]] const ThemeTokens& theme() const noexcept { return m_theme; }

    // The want-capture flags of the last frame: ImGui wants the mouse, the keyboard, text input.
    [[nodiscard]] bool wantsMouse() const;
    [[nodiscard]] bool wantsKeyboard() const;
    [[nodiscard]] bool wantsTextInput() const;

    // The UI's consumer in its window's input stack.
    [[nodiscard]] ConsumerId consumer() const noexcept;

    // Pushes one of the editor's fonts for the panel code that follows, at the theme's size for it,
    // and pops it. Main thread, inside a frame.
    void pushFont(UiFont font);
    void popFont();

    [[nodiscard]] const UiRenderStats& renderStats() const noexcept;

private:
    struct Fonts;

    EditorUi();

    std::unique_ptr<Fonts> m_fonts;
    std::unique_ptr<UiPlatformBackend> m_platform;
    std::unique_ptr<UiRenderer> m_renderer;
    ThemeTokens m_theme;
    float m_displayScale = 1.0f;
    bool m_frameOpen = false;
    bool m_frameEnded = false;
};

} // namespace ith
