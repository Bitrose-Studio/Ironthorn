#include "ith/editor_ui/EditorUi.h"

#include "ith/editor_ui/ImGuiPlatform.h"
#include "ith/editor_ui/ImGuiRenderer.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"

#include <imgui.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <utility>

namespace ith {
namespace {

// The largest font file the editor reads.
constexpr std::size_t kMaxFontFileSize = std::size_t{16} * 1024 * 1024;

// Whether an EditorUi exists: ImGui has one global context.
std::atomic<bool> g_editorUiExists{false};

// ImGui's allocations. The engine has no general-purpose allocator yet, so they go to the C heap,
// as SDL's, VMA's and D3D12MA's do; this is the one place to change (ADR-0029 rule 5).
void* allocate(std::size_t size, void* /*userData*/) {
    return std::malloc(size);
}

void release(void* pointer, void* /*userData*/) {
    std::free(pointer);
}

} // namespace

// The font files' bytes, which the atlas reads for as long as it lives (ImGui 1.92 loads glyphs on
// demand), and the fonts made from them.
struct EditorUi::Fonts {
    std::array<std::string, kEditorFontFiles.size()> files;
    std::string icons;
    std::array<ImFont*, kEditorFontFiles.size()> fonts{};
};

EditorUi::EditorUi() = default;

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads m_thread when asserts are on
void EditorUi::assertThread() const {
    ITH_ASSERT_MSG(std::this_thread::get_id() == m_thread,
                   "EditorUi user-texture and render calls use its creating thread");
}

Expected<std::unique_ptr<EditorUi>> EditorUi::create(const EditorUiDesc& desc) {
    ITH_ASSERT_MSG(desc.device != nullptr && desc.input != nullptr && desc.window != kNoWindow,
                   "an EditorUi needs a device, the input and a window");
    ITH_ASSERT_MSG(!desc.vertexShader.empty() && !desc.pixelShader.empty(), "an EditorUi needs both shader stages");
    ITH_ASSERT_MSG(desc.displayScale > 0.0f, "the display scale is positive");
    bool expected = false;
    const bool claimed = g_editorUiExists.compare_exchange_strong(expected, true);
    ITH_ASSERT_MSG(claimed, "only one EditorUi may exist at a time: ImGui has one context");
    if (!claimed || desc.device == nullptr || desc.input == nullptr) {
        if (claimed) {
            g_editorUiExists.store(false);
        }
        return makeError("editor_ui.invalid_desc", "an EditorUi needs a device and the input, and only one may exist");
    }

    std::unique_ptr<EditorUi> ui(new EditorUi());
    ui->m_thread = std::this_thread::get_id();
    ui->m_theme = desc.theme;
    ui->m_displayScale = desc.displayScale;
    ui->m_fonts = std::make_unique<Fonts>();

    ImGui::SetAllocatorFunctions(allocate, release, nullptr);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Layouts are the editor's own documents; ImGui writes no file (ADR-0029 rules 5 and 15).
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // ImGui still marks its settings dirty when panels move; DockLayout reads the layout back then,
    // at most every 2 seconds (ADR-0040 rule 11).
    io.IniSavingRate = 2.0f;
    // Floating panels stay inside the main window: no multi-viewports.
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTextCursorBlink = desc.caretBlink;

    // Fonts, read through the engine's file API and handed over from memory; the atlas doesn't own
    // the bytes. The icon font is merged into the text fonts.
    Fonts& fonts = *ui->m_fonts;
    const std::string directory(desc.fontDirectory);
    const auto readFont = [&directory](std::string_view name, std::string& bytes) -> Expected<void> {
        Expected<std::string> read = fs::readFile(fs::joinPath(directory, name), kMaxFontFileSize);
        if (!read) {
            return makeError("editor_ui.font_failed",
                             "the editor font " + std::string(name) + " could not be read: " + read.error().message);
        }
        bytes = std::move(*read);
        return {};
    };
    if (Expected<void> read = readFont(kEditorIconFontFile, fonts.icons); !read) {
        return makeUnexpected(read.error());
    }
    for (std::size_t i = 0; i < kEditorFontFiles.size(); ++i) {
        if (Expected<void> read = readFont(kEditorFontFiles[i], fonts.files[i]); !read) {
            return makeUnexpected(read.error());
        }
        // The text fonts leave the Private Use Area to the icon font merged below: Inter has glyphs of
        // its own there, which would hide icons such as save and plus.
        static constexpr std::array<ImWchar, 3> kPrivateUse = {0xE000, 0xF8FF, 0};
        ImFontConfig config;
        config.GlyphExcludeRanges = kPrivateUse.data();
        config.FontDataOwnedByAtlas = false;
        std::string& bytes = fonts.files[i];
        fonts.fonts[i] =
            io.Fonts->AddFontFromMemoryTTF(bytes.data(), static_cast<int>(bytes.size()), desc.theme.fontSize, &config);
        if (fonts.fonts[i] == nullptr) {
            return makeError("editor_ui.font_failed",
                             "the editor font " + std::string(kEditorFontFiles[i]) + " could not be loaded");
        }
        if (static_cast<UiFont>(i) != UiFont::Mono) {
            ImFontConfig icons;
            icons.FontDataOwnedByAtlas = false;
            icons.MergeMode = true;
            // Icons share one advance, so labels after them line up.
            icons.GlyphMinAdvanceX = desc.theme.fontSize;
            if (io.Fonts->AddFontFromMemoryTTF(fonts.icons.data(), static_cast<int>(fonts.icons.size()),
                                               desc.theme.fontSize, &icons) == nullptr) {
                return makeError("editor_ui.font_failed",
                                 "the icon font " + std::string(kEditorIconFontFile) + " could not be loaded");
            }
        }
    }
    io.FontDefault = fonts.fonts[static_cast<std::size_t>(UiFont::Text)];
    applyTheme(desc.theme, desc.displayScale);

    Expected<std::unique_ptr<UiRenderer>> renderer =
        UiRenderer::create(*desc.device, desc.targetFormat, desc.vertexShader, desc.pixelShader);
    if (!renderer) {
        return makeUnexpected(renderer.error());
    }
    ui->m_renderer = std::move(*renderer);
    ui->m_platform = std::make_unique<UiPlatformBackend>(*desc.input, desc.window, desc.host, desc.viewports);
    return ui;
}

EditorUi::~EditorUi() {
    m_platform.reset();
    m_renderer.reset();
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
        m_fonts.reset();
        g_editorUiExists.store(false);
    }
}

void EditorUi::newFrame(const UiFrameInput& input) {
    assertThread();
    ITH_ASSERT_MSG(!m_frameOpen, "newFrame() while a UI frame is open");
    m_renderer->cancelGraphImports();
    m_platform->newFrame(input);
    ImGui::NewFrame();
    m_frameOpen = true;
}

void EditorUi::endFrame() {
    assertThread();
    ITH_ASSERT_MSG(m_frameOpen, "endFrame() without newFrame()");
    if (!m_frameOpen) {
        return;
    }
    ImGui::Render();
    m_platform->endFrame();
    m_frameOpen = false;
    m_frameEnded = true;
    m_renderer->beginGraphImports();
}

void EditorUi::abandonFrame() {
    assertThread();
    ITH_ASSERT_MSG(!m_frameOpen, "abandonFrame() while a UI frame is open");
    m_frameEnded = false;
    m_renderer->cancelGraphImports();
}

UiTextureId EditorUi::registerUserTexture(rhi::TextureHandle texture, const rhi::TextureDesc& desc,
                                          rg::ImportedState initialState) {
    assertThread();
    return m_renderer->registerUserTexture(texture, desc, initialState);
}

void EditorUi::unregisterUserTexture(UiTextureId texture) {
    assertThread();
    m_renderer->unregisterUserTexture(texture);
}

rg::Texture EditorUi::importUserTexture(rg::RenderGraph& graph, UiTextureId texture) {
    assertThread();
    ITH_ASSERT_MSG(!m_frameOpen && m_frameEnded,
                   "importUserTexture() follows endFrame() and precedes addRenderPasses()");
    if (m_frameOpen || !m_frameEnded) {
        return {};
    }
    return m_renderer->importUserTexture(graph, texture);
}

void EditorUi::addRenderPasses(rg::RenderGraph& graph, rg::Texture target, std::uint32_t frameSlot, rhi::LoadOp load,
                               const std::array<float, 4>& clearColor) {
    assertThread();
    ITH_ASSERT_MSG(!m_frameOpen && m_frameEnded, "addRenderPasses() follows endFrame()");
    if (m_frameOpen || !m_frameEnded) {
        return;
    }
    const bool serialMatches = m_renderer->graphSerialMatches(target);
    ITH_ASSERT_MSG(serialMatches, "the UI user-texture imports and passes belong to one graph serial");
    if (!serialMatches) {
        return;
    }
    m_frameEnded = false;
    m_renderer->prepare(frameSlot);
    m_renderer->addPasses(graph, target, load, clearColor);
}

void EditorUi::setDisplayScale(float displayScale) {
    m_displayScale = displayScale;
    applyTheme(m_theme, displayScale);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads the context this EditorUi owns
bool EditorUi::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads the context this EditorUi owns
bool EditorUi::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool EditorUi::wantsTextInput() const {
    return m_platform->textInput();
}

ConsumerId EditorUi::consumer() const noexcept {
    return m_platform->consumer();
}

void EditorUi::pushFont(UiFont font) {
    const float size = font == UiFont::Mono ? m_theme.monoFontSize : m_theme.fontSize;
    ImGui::PushFont(m_fonts->fonts[static_cast<std::size_t>(font)], size);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): reads the context this EditorUi owns
void EditorUi::popFont() {
    ImGui::PopFont();
}

const UiRenderStats& EditorUi::renderStats() const noexcept {
    return m_renderer->stats();
}

} // namespace ith
