#pragma once

// ith::EditorUi: the editor's immediate-mode UI on Dear ImGui (ADR-0029). It owns the one ImGui
// context, the fonts and theme, the platform backend (engine events in, clipboard, IME and cursor
// out through a UiHost) and the renderer backend (ImGui's draw data as render graph passes on the
// RHI). No ImGui type appears in this header; panels live in editor_ui and draw with ImGui
// directly.
//
//   ith::Input input;
//   ith::KeymapConsumer shortcuts(session, keymap);
//   ith::ViewportConsumer viewportInput(session);
//   input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &shortcuts});
//   input.addConsumer({.window = window.id(), .layer = ith::ConsumerLayer::Editor, .consumer = &viewportInput});
//   ith::WindowUiHost host(platform, window);
//   auto ui = ith::EditorUi::create({.device = &device, .targetFormat = format, .vertexShader = vs,
//                                    .pixelShader = ps, .fontDirectory = fonts, .input = &input,
//                                    .window = window.id(), .host = &host, .viewports = &viewportInput});
//   // added last, so ImGui's consumer sits above the viewport and keymap consumers (ADR-0029 rule 7,
//   // ADR-0042 rule 8)
//   while (running) {
//       input.processEvents(events, platform.pumpTimeNs());
//       if (viewportInput.update(deltaSeconds)) idle.requestFrame();
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
#include <thread>
#include <vector>

namespace ith {

class UiPlatformBackend;
class UiRenderer;
class ContentBrowserModel;
class ContentBrowserPanel;
class ViewportConsumer;

// What the UI needs from the window it runs in: the clipboard, the IME's place, the cursor
// (ADR-0029 rule 6) and the open-file dialog (ADR-0049 rule 26). WindowUiHost implements it over
// ith::Platform and ith::Window; tests implement it to record the calls.
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
    // Relative mouse mode for the window (ADR-0013 rule 5): the cursor hidden and locked, and
    // unaccelerated motion. The UI turns it on while a viewport flies (ADR-0042 rule 11) or the mouse
    // is captured, and off after; it calls this only when the mode changes.
    virtual void setRelativeMouseMode(bool enabled) = 0;
    // Moves the cursor to `x`, `y` in window pixels: where a fly started, just before relative mode
    // turns off.
    virtual void warpMouse(float x, float y) = 0;
    // Shows the operating system's open-file dialog over the window and returns at once, as
    // Platform::showOpenFileDialog does: the chosen paths arrive later as FileDialogFile events and
    // then FileDialogClosed, which the application hands to EditorUi::handleEvent. Fails when the
    // dialog can't be started ("platform.dialog_busy" while one is open). The strings of `desc` need
    // to live only for the call.
    virtual Expected<void> showOpenFileDialog(const OpenFileDialogDesc& desc) = 0;
};

// A UiHost over the application's Platform and one of its windows. Failures are dropped: the
// offscreen driver has no system cursors, and an unreadable clipboard pastes nothing. The first
// failed relative-mode switch is logged as a warning (category "EditorUi") with the platform's
// message; later ones are not, as the UI asks again only when the mode changes. The open-file
// dialog's failure is returned to the caller.
class WindowUiHost final : public UiHost {
public:
    WindowUiHost(Platform& platform, Window& window) noexcept : m_platform(&platform), m_window(&window) {}

    [[nodiscard]] std::string clipboardText() override;
    void setClipboardText(std::string_view text) override;
    void setTextInputArea(const TextInputArea& area) override;
    void setCursor(std::optional<CursorShape> shape) override;
    // Window::setRelativeMouseMode; logs its first failure.
    void setRelativeMouseMode(bool enabled) override;
    // Window::warpMouse.
    void warpMouse(float x, float y) override;
    // Platform::showOpenFileDialog over the window.
    Expected<void> showOpenFileDialog(const OpenFileDialogDesc& desc) override;

private:
    Platform* m_platform;
    Window* m_window;
    bool m_relativeModeFailed = false;
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
    // Clipboard, IME, cursor and relative mouse mode; null drops them (headless tests). Must outlive
    // the EditorUi.
    UiHost* host = nullptr;
    // The window's viewport consumer (ADR-0042 rules 9 and 11), added to the window's stack before
    // the UI, so it sits below the UI's consumer and above the keymap's. The UI's consumer then
    // leaves to it the mouse events over a viewport image with no ImGui popup open, and every mouse
    // event while it holds a drag; and the UI puts the window in relative mouse mode while it flies.
    // Null: no viewports. Must outlive the EditorUi.
    const ViewportConsumer* viewports = nullptr;
    // The Content Browser's model (session.content()): a mouse press updates its keyboard-focus flag
    // from the browser's rectangles of the last frame before the events after it are routed, so a
    // click into the browser and a key in the same batch of events reach the Content context
    // (ADR-0049 rule 4). Null: no Content Browser. Must outlive the EditorUi.
    ContentBrowserModel* content = nullptr;
    // The Content Browser panel, which handleEvent() hands the operating system's drops and the
    // open-file dialog's results (ADR-0049 rule 26). Null: those events are ignored. Must outlive
    // the EditorUi.
    ContentBrowserPanel* contentBrowser = nullptr;
    ThemeTokens theme = defaultDarkTheme();
    float displayScale = 1.0f;
    // Off draws the text cursor without blinking, for reproducible images.
    bool caretBlink = true;
};

// A rectangle in window coordinates, as a panel drew an item: for tests that click on it.
struct UiRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] float centerX() const noexcept { return x + width * 0.5f; }

    [[nodiscard]] float centerY() const noexcept { return y + height * 0.5f; }
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

// An opaque user-texture registration. Zero is invalid. A panel in editor_ui converts this value to
// ImTextureID for ImGui::Image; no ImGui type crosses the module boundary.
using UiTextureId = std::uint64_t;

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
    // ImGui context. Execute or reset every graph that holds the UI's passes or user-texture imports
    // first: such a graph points at state the EditorUi owns.
    ~EditorUi();

    // Starts a UI frame: hands the frame's window sizes, time and focus to ImGui, applies the
    // cursor the last frame asked for, and begins ImGui's frame, after which panels draw. The
    // events Input routed to the UI since the last frame are applied here, so the want-capture
    // flags they were routed by come from the frame before (ImGui's normal contract). Main thread
    // (asserted). Precondition (asserted): no frame is open.
    void newFrame(const UiFrameInput& input);

    // Ends the UI frame and turns it into draw data, and opens the graph-import phase of
    // importUserTexture(). Main thread (asserted). Precondition (asserted): a frame is open.
    void endFrame();

    // Gives up a frame that won't be rendered: after endFrame() when addRenderPasses() won't follow
    // (the device had no frame, the target couldn't be imported, rendering failed, or the editor
    // exits). Closes the graph-import phase, so textures may be registered and unregistered again
    // (teardown included); the frame's texture requests stay for the next frame. Does nothing when
    // no frame awaits its passes. Main thread (asserted). Precondition (asserted): no frame is open.
    void abandonFrame();

    // Registers a borrowed, single-layer 2D texture with Sampled usage for an editor_ui panel to
    // draw. Its format must be linear filterable (rhi::isLinearFilterable: no integer, 32-bit float
    // or depth formats), since the UI's sampler filters linearly. `desc` is the description the
    // texture was created with and needs a non-empty debugName; the description and name are copied.
    // The renderer owns the texture's ImportedState from here on, starting at a copy of
    // `initialState`, and never destroys the texture. Pass the texture's current state when the
    // caller knows it (as its last graph wrote it back), so the first barrier waits for that use; the
    // default (Undefined) lets the first use discard the contents. After an unregistration the
    // caller doesn't know the state, so registering the same texture again must pass the default.
    // The texture must belong to this device; that is the caller's responsibility, not checked. Main
    // thread (asserted). Preconditions (asserted): no graph-import phase is open, the texture is
    // live, the description follows these rules, and the texture is not already registered. Returns
    // zero when a precondition fails without assertions.
    [[nodiscard]] UiTextureId registerUserTexture(rhi::TextureHandle texture, const rhi::TextureDesc& desc,
                                                  rg::ImportedState initialState = {});

    // Stops drawing a registered user texture. Draws that still use its id, including after its table
    // slot is reused, are skipped. Does not destroy the texture: normally unregister after every
    // graph that imported it has executed and before Device::destroy(). Destroying first is tolerated
    // once every graph that imported the texture has executed: later draws are skipped, but
    // unregistration is still required. A graph built before the call still samples the texture
    // when it executes, and the freed slot is not reused before the next endFrame(). The texture's
    // graph state stays with the renderer, so a later registration or direct graph import of the
    // same RHI texture must start from the default ImportedState (Undefined), which discards its
    // contents. Main thread (asserted). Preconditions (asserted): no graph-import phase is open and
    // the id is currently registered.
    void unregisterUserTexture(UiTextureId texture);

    // Returns the registered texture's import in `graph`, importing it once for reuse by the viewport
    // scene passes and the EditorUi pass. Add the scene passes with this handle before
    // addRenderPasses(); the graph then orders their writes before the UI's sampled read. A texture
    // enters the graph only when requested here or referenced by this frame's draws; an unused
    // registration is not imported. Use one graph per frame, never import the same RHI texture
    // directly with graph.importTexture(), and execute the graph before the next frame's imports.
    // Returns a null handle for an unknown, unregistered or destroyed id. Main thread (asserted).
    // Preconditions (asserted): after endFrame() and before addRenderPasses(), with every such call
    // and the UI passes using the same graph serial; do not reset the graph between these calls and
    // addRenderPasses().
    [[nodiscard]] rg::Texture importUserTexture(rg::RenderGraph& graph, UiTextureId texture);

    // Adds the UI's passes to `graph` after copying the last endFrame()'s geometry and texture
    // changes into the upload buffer of `frameSlot` (rhi::Frame::slot): "EditorUiUpload" (Copy), when
    // textures change, and "EditorUi" (Raster), which draws into `target` with `load` (and
    // `clearColor` for LoadOp::Clear). Imports the upload buffer and each texture used by an upload,
    // a draw or an explicit importUserTexture() call. Call it once per frame after the device's
    // beginFrame() and before graph.compile(). The passes read only the renderer's copies, valid
    // until the next endFrame(): execute (or reset) the graph before then. Nothing checks that it
    // ran; a debug check could not tell a pending graph from an abandoned one. Main thread
    // (asserted). Preconditions (asserted): no UI frame is open, endFrame() ran since the last call,
    // and `target` belongs to the graph of this frame's importUserTexture() calls (a mismatch adds
    // nothing and keeps this frame's texture changes for the next call).
    void addRenderPasses(rg::RenderGraph& graph, rg::Texture target, std::uint32_t frameSlot,
                         rhi::LoadOp load = rhi::LoadOp::Load,
                         const std::array<float, 4>& clearColor = {0.0f, 0.0f, 0.0f, 1.0f});

    // Hands the UI an event Input doesn't route (ADR-0049 rule 26): the operating system's drops
    // (DropBegin, DropPosition, DropFile, DropComplete) and the open-file dialog's results
    // (FileDialogFile, FileDialogClosed) of the UI's window go to the Content Browser
    // (EditorUiDesc::contentBrowser), with positions turned from window pixels into UI coordinates by
    // the last frame's pixel ratio (pixels per window coordinate, from the UiFrameInput of the last
    // newFrame; a display-scale change applies from the next frame). The browser hit-tests a drop
    // against where it was drawn in the last frame and imports into the folder under it; a drop
    // elsewhere is ignored. Every other event,
    // events of other windows (a DropBegin without a window included), and every event when there is
    // no Content Browser, are ignored. Call it for each pumped event, outside a UI frame. Main thread
    // (asserted).
    void handleEvent(const Event& event);

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
    void assertThread() const;

    std::unique_ptr<Fonts> m_fonts;
    std::unique_ptr<UiPlatformBackend> m_platform;
    std::unique_ptr<UiRenderer> m_renderer;
    ContentBrowserPanel* m_contentBrowser = nullptr;
    WindowId m_window = kNoWindow;
    ThemeTokens m_theme;
    float m_displayScale = 1.0f;
    std::thread::id m_thread;
    bool m_frameOpen = false;
    bool m_frameEnded = false;
};

} // namespace ith
