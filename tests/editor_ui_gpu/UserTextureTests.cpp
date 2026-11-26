// A user texture written by a scene pass and drawn by ImGui::Image on WARP and lavapipe, checked by
// readback without a golden image (ADR-0042 rule 14). The UNORM source and target keep the known
// colour independent of sRGB conversion; the shell goldens cover the editor's sRGB path.

#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/EditorUiShader.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/input/Input.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"
#include "ith/shader_compiler/ShaderCompiler.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ith;

namespace {

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kRowPitch = kSize * 4;
constexpr std::array<float, 4> kColour{0.2f, 0.4f, 0.6f, 1.0f};

std::vector<std::byte> compileStage(ShaderCompiler& compiler, rhi::ShaderFormat format, std::string_view entry,
                                    ShaderStage stage) {
    Expected<std::vector<std::byte>> code =
        compiler.compile({.name = kEditorUiShaderName,
                          .source = editorUiShaderSource(),
                          .entryPoint = entry,
                          .stage = stage,
                          .target = format == rhi::ShaderFormat::Dxil ? ShaderTarget::Dxil : ShaderTarget::Spirv});
    REQUIRE_MESSAGE(code.hasValue(), (code ? std::string() : code.error().message));
    return std::move(*code);
}

void drawImage(UiTextureId texture) {
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({static_cast<float>(kSize), static_cast<float>(kSize)});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ImageBorderSize, 0.0f);
    ImGui::Begin("User texture", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Image(static_cast<ImTextureID>(texture), {static_cast<float>(kSize), static_cast<float>(kSize)});
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void checkUserTexture(rhi::Device& device) {
    Expected<ShaderCompiler> compiler = ShaderCompiler::create();
    REQUIRE(compiler.hasValue());
    const rhi::ShaderFormat shaderFormat = device.capabilities().shaderFormat;
    const std::vector<std::byte> vertex =
        compileStage(*compiler, shaderFormat, kEditorUiVertexEntry, ShaderStage::Vertex);
    const std::vector<std::byte> pixel = compileStage(*compiler, shaderFormat, kEditorUiPixelEntry, ShaderStage::Pixel);
    const Expected<std::string> executable = fs::executableDirectory();
    REQUIRE(executable.hasValue());
    const std::string fonts = fs::joinPath(*executable, "fonts");
    Input input;
    Expected<std::unique_ptr<EditorUi>> made = EditorUi::create({.device = &device,
                                                                 .targetFormat = rhi::Format::R8G8B8A8Unorm,
                                                                 .vertexShader = vertex,
                                                                 .pixelShader = pixel,
                                                                 .fontDirectory = fonts,
                                                                 .input = &input,
                                                                 .window = 1,
                                                                 .caretBlink = false});
    REQUIRE_MESSAGE(made.hasValue(), (made ? std::string() : made.error().message));
    std::unique_ptr<EditorUi> ui = std::move(*made);

    const rhi::TextureDesc sourceDesc{.width = kSize,
                                      .height = kSize,
                                      .format = rhi::Format::R8G8B8A8Unorm,
                                      .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::RenderTarget,
                                      .debugName = "User texture colour"};
    const rhi::TextureDesc targetDesc{.width = kSize,
                                      .height = kSize,
                                      .format = rhi::Format::R8G8B8A8Unorm,
                                      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                                      .debugName = "User texture target"};
    Expected<rhi::TextureHandle, rhi::Error> source = device.createTexture(sourceDesc);
    Expected<rhi::TextureHandle, rhi::Error> target = device.createTexture(targetDesc);
    REQUIRE(source.hasValue());
    REQUIRE(target.hasValue());
    const rhi::BufferDesc readbackDesc{.size = std::uint64_t{kRowPitch} * kSize,
                                       .usage = rhi::BufferUsage::CopyDst,
                                       .memory = rhi::MemoryType::Readback,
                                       .debugName = "User texture readback"};
    Expected<rhi::BufferHandle, rhi::Error> readback = device.createBuffer(readbackDesc);
    REQUIRE(readback.hasValue());

    rg::ImportedState targetState;
    rg::ImportedState readbackState;
    const UiTextureId user = ui->registerUserTexture(*source, sourceDesc);
    REQUIRE(user != 0);
    rg::RenderGraph graph({.name = "User texture"});
    rg::ResourcePool pool(device);
    constexpr int kFrames = 3;
    for (int frame = 0; frame < kFrames; ++frame) {
        ui->newFrame({.size = {static_cast<std::int32_t>(kSize), static_cast<std::int32_t>(kSize)},
                      .pixelSize = {static_cast<std::int32_t>(kSize), static_cast<std::int32_t>(kSize)},
                      .deltaSeconds = 1.0f / 60.0f});
        drawImage(user);
        ui->endFrame();

        Expected<std::optional<rhi::Frame>, rhi::Error> begun = device.beginFrame();
        REQUIRE(begun.hasValue());
        REQUIRE(begun->has_value());
        graph.reset();
        const rg::Texture output =
            graph.importTexture({.texture = *target, .desc = targetDesc, .state = &targetState, .writable = true});
        const rg::Texture colour = ui->importUserTexture(graph, user);
        REQUIRE_FALSE(colour.isNull());
        graph.addPass(
            "Known colour", rg::PassType::Raster,
            [&](rg::PassBuilder& pass) { pass.renderTarget(colour, rhi::LoadOp::Clear, kColour); },
            [](rg::PassContext&) {});
        ui->addRenderPasses(graph, output, (*begun)->slot, rhi::LoadOp::Clear);
        if (frame + 1 == kFrames) {
            const rg::Buffer buffer = graph.importBuffer(
                {.buffer = *readback, .desc = readbackDesc, .state = &readbackState, .writable = true});
            graph.addPass(
                "Read back", rg::PassType::Copy,
                [&](rg::PassBuilder& pass) {
                    pass.read(output, rg::TextureUse::CopySrc);
                    pass.write(buffer, rg::BufferUse::CopyDst);
                },
                [output, buffer](rg::PassContext& context) {
                    context.list().copyTextureToBuffer(
                        {.buffer = context.buffer(buffer), .rowPitch = kRowPitch, .texture = context.texture(output)});
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
    REQUIRE(pixels.size() == readbackDesc.size);
    constexpr std::array<std::uint8_t, 3> kExpected{51, 102, 153};
    std::uint32_t wrongPixels = 0;
    for (std::size_t pixelIndex = 0; pixelIndex < std::size_t{kSize} * kSize; ++pixelIndex) {
        bool wrong = false;
        for (std::size_t channel = 0; channel < kExpected.size(); ++channel) {
            wrong = wrong || std::to_integer<std::uint8_t>(pixels[pixelIndex * 4 + channel]) != kExpected[channel];
        }
        wrongPixels += wrong ? 1 : 0;
    }
    CHECK(wrongPixels == 0);

    graph.reset();
    ui->unregisterUserTexture(user);
    ui.reset();
    device.destroy(*readback);
    device.destroy(*target);
    device.destroy(*source);
}

} // namespace

TEST_SUITE("editor_ui_gpu.user_texture") {
#ifdef ITH_EDITOR_UI_GPU_HAS_D3D12
    TEST_CASE("ImGui Image samples the colour written by the scene pass on WARP") {
        Expected<rhi::Device, rhi::Error> device = rhi::createDevice(
            {.adapter = rhi::AdapterPreference::Software, .applicationName = "editor_ui_gpu_user_texture"},
            rhi::Backend::D3D12);
        if (!device) {
            MESSAGE("skipped: no WARP device: " << device.error().toString());
            return;
        }
        REQUIRE(device->capabilities().adapter.software);
        checkUserTexture(*device);
    }
#endif

    TEST_CASE("ImGui Image samples the colour written by the scene pass on lavapipe") {
        Expected<rhi::Device, rhi::Error> device = rhi::createDevice(
            {.adapter = rhi::AdapterPreference::Software, .applicationName = "editor_ui_gpu_user_texture"},
            rhi::Backend::Vulkan);
        if (!device) {
            MESSAGE("skipped: no software Vulkan device: " << device.error().toString());
            return;
        }
        if (device->capabilities().adapter.name.find("llvmpipe") == std::string::npos) {
            MESSAGE("skipped: the software Vulkan device is not lavapipe: " << device->capabilities().adapter.name);
            return;
        }
        checkUserTexture(*device);
    }
}
