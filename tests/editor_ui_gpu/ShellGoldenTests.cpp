// A golden screenshot of the editor shell in the default layout (ADR-0029 rule 18, ADR-0040 rule 19):
// the main menu bar, the engine's Default layout (an empty central area, as the Viewport panel is
// not registered here, and the Output Log, empty, below it; the Outliner above Details on the right,
// both of the empty level) and the status bar over an untitled level, 1280 x 720 at display scale 1,
// a fixed time step and no caret blink, rendered on the software GPU of each backend (WARP on
// Direct3D 12, lavapipe on Vulkan) with the UI shader compiled from Slang at run time, read back and
// compared with tests/editor_ui_gpu/golden/shell_<backend>.ppm (GoldenImage.h: a pixel differs when a
// channel differs by more than 8 of 255; the shot fails when more than 0.5% of the pixels differ).
//
// ITH_UPDATE_GOLDEN=1 writes the images instead of comparing them. A backend without a software
// device here reports itself skipped. On a failure the rendered image is written next to the test
// executable, as shell_<backend>.actual.ppm.

#include "ith/config/Config.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_ui/DetailsPanel.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/EditorUiShader.h"
#include "ith/editor_ui/OutlinerPanel.h"
#include "ith/editor_ui/OutputLog.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/input/Input.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"
#include "ith/shader_compiler/ShaderCompiler.h"

#include "GoldenImage.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <span>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)

using namespace ith;
using namespace ith::test::golden;

namespace {

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720;
constexpr rhi::Format kFormat = rhi::Format::R8G8B8A8Srgb;
constexpr std::uint32_t kRowPitch = kWidth * 4;

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        return result;
    }();
    return kTypes;
}

// Renders the shell's fourth frame in the default layout on `device` and reads it back.
Image renderShell(rhi::Device& device) {
    Expected<ShaderCompiler> compiler = ShaderCompiler::create();
    REQUIRE(compiler.hasValue());
    const rhi::ShaderFormat format = device.capabilities().shaderFormat;
    const std::vector<std::byte> vertex = compileStage(*compiler, format, kEditorUiShaderName, editorUiShaderSource(),
                                                       kEditorUiVertexEntry, ShaderStage::Vertex);
    const std::vector<std::byte> pixel = compileStage(*compiler, format, kEditorUiShaderName, editorUiShaderSource(),
                                                      kEditorUiPixelEntry, ShaderStage::Pixel);

    const rhi::TextureDesc targetDesc{.width = kWidth,
                                      .height = kHeight,
                                      .format = kFormat,
                                      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                                      .debugName = "Shell golden target"};
    Expected<rhi::TextureHandle, rhi::Error> target = device.createTexture(targetDesc);
    REQUIRE(target.hasValue());
    const rhi::BufferDesc readbackDesc{.size = std::uint64_t{kRowPitch} * kHeight,
                                       .usage = rhi::BufferUsage::CopyDst,
                                       .memory = rhi::MemoryType::Readback,
                                       .debugName = "Shell golden readback"};
    Expected<rhi::BufferHandle, rhi::Error> readback = device.createBuffer(readbackDesc);
    REQUIRE(readback.hasValue());

    Config config({{.name = "engine", .directory = "engine/config"}});
    EditorSession session(types(), {.config = &config});
    std::vector<Error> warnings;
    const Keymap keymap = Keymap::load(config, session.commands(), warnings);
    EditorShellState shell;
    Input input;
    const Expected<std::string> executable = fs::executableDirectory();
    REQUIRE(executable.hasValue());
    const std::string fonts = fs::joinPath(*executable, "fonts");
    Image image{.width = kWidth, .height = kHeight, .rgb = {}};
    {
        Expected<std::unique_ptr<EditorUi>> ui = EditorUi::create({.device = &device,
                                                                   .targetFormat = kFormat,
                                                                   .vertexShader = vertex,
                                                                   .pixelShader = pixel,
                                                                   .fontDirectory = fonts,
                                                                   .input = &input,
                                                                   .window = 1,
                                                                   .caretBlink = false});
        REQUIRE_MESSAGE(ui.hasValue(), (ui ? std::string() : ui.error().message));
        // The P2 panels so far: the Outliner and Details of the empty level, and the Output Log over a
        // buffer no sink writes to, so the image doesn't depend on the log.
        OutlinerPanel outliner(session);
        registerOutliner(session, outliner);
        DetailsPanel details(session);
        registerDetails(session.panels(), details);
        const LogRingBuffer history(16);
        OutputLog log(history, (*ui)->theme());
        registerOutputLog(session.panels(), log);
        DockLayout dock(session);
        REQUIRE(session.layouts().restore().name == "Default");
        rg::RenderGraph graph({.name = "Shell golden"});
        rg::ResourcePool pool(device);
        rg::ImportedState targetState;
        rg::ImportedState readbackState;
        constexpr int kFrames = 4;
        for (int frame = 0; frame < kFrames; ++frame) {
            (*ui)->newFrame({.size = {static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight)},
                             .pixelSize = {static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight)},
                             .deltaSeconds = 1.0f / 60.0f});
            drawEditorShell(session, keymap, shell, dock);
            (*ui)->endFrame();
            Expected<std::optional<rhi::Frame>, rhi::Error> begun = device.beginFrame();
            REQUIRE(begun.hasValue());
            REQUIRE(begun->has_value());
            graph.reset();
            const rg::Texture texture =
                graph.importTexture({.texture = *target, .desc = targetDesc, .state = &targetState, .writable = true});
            (*ui)->addRenderPasses(graph, texture, (*begun)->slot, rhi::LoadOp::Clear,
                                   {0.0103f, 0.0103f, 0.0116f, 1.0f});
            if (frame + 1 == kFrames) {
                const rg::Buffer buffer = graph.importBuffer(
                    {.buffer = *readback, .desc = readbackDesc, .state = &readbackState, .writable = true});
                graph.addPass(
                    "Read back", rg::PassType::Copy,
                    [&](rg::PassBuilder& pass) {
                        pass.read(texture, rg::TextureUse::CopySrc);
                        pass.write(buffer, rg::BufferUse::CopyDst);
                    },
                    [texture, buffer](rg::PassContext& context) {
                        context.list().copyTextureToBuffer({.buffer = context.buffer(buffer),
                                                            .rowPitch = kRowPitch,
                                                            .texture = context.texture(texture)});
                    },
                    rg::PassFlags::NeverCull);
            }
            graph.compile();
            Expected<void, rhi::Error> executed = graph.execute(pool);
            REQUIRE_MESSAGE(executed.hasValue(), (executed ? std::string() : executed.error().toString()));
            REQUIRE(device.endFrame().hasValue());
        }
        REQUIRE(device.waitIdle().hasValue());
        const std::span<const std::byte> pixels = device.mappedMemory(*readback);
        image = imageFromTexels(pixels, kWidth, kHeight);
        graph.reset();
    }
    device.destroy(*readback);
    device.destroy(*target);
    return image;
}

} // namespace

TEST_SUITE("editor_ui_gpu.golden") {
#ifdef ITH_EDITOR_UI_GPU_HAS_D3D12
    TEST_CASE("the shell in the default layout on WARP matches its golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::D3D12);
        if (!device) {
            return;
        }
        checkGolden(renderShell(*device), "shell_d3d12");
    }
#endif

    TEST_CASE("the shell in the default layout on lavapipe matches its golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::Vulkan);
        if (!device || !isLavapipe(*device)) {
            return;
        }
        checkGolden(renderShell(*device), "shell_vulkan");
    }
}
