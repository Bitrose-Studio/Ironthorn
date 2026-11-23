// A golden screenshot of the editor shell in the default layout (ADR-0029 rule 18, ADR-0040 rule 19):
// the main menu bar, the engine's Default layout (an empty central area and the Output Log, empty,
// below it) and the status bar over an untitled level, 1280 x 720 at display scale 1, a fixed time
// step and no caret blink, rendered on the software GPU of each backend (WARP on Direct3D 12,
// lavapipe on Vulkan) with the UI shader compiled from Slang at run time, read back and compared
// with tests/editor_ui_gpu/golden/shell_<backend>.ppm. A pixel differs when a channel differs by
// more than 8 of 255; the shot fails when more than 0.5% of the pixels differ.
//
// ITH_UPDATE_GOLDEN=1 writes the images instead of comparing them. A backend without a software
// device here reports itself skipped. On a failure the rendered image is written next to the test
// executable, as shell_<backend>.actual.ppm.

#include "ith/config/Config.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/EditorUiShader.h"
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

namespace {

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720;
constexpr rhi::Format kFormat = rhi::Format::R8G8B8A8Srgb;
constexpr std::uint32_t kRowPitch = kWidth * 4;
constexpr int kChannelTolerance = 8;
constexpr double kDifferingPixelsAllowed = 0.005;

// Whether the environment variable `name` is set to `value`.
bool environmentIs(const char* name, std::string_view value) {
#ifdef _WIN32
    // getenv_s: the C runtime deprecates getenv on Windows.
    std::array<char, 32> buffer{};
    std::size_t length = 0;
    if (getenv_s(&length, buffer.data(), buffer.size(), name) != 0 || length == 0) {
        return false;
    }
    return value == buffer.data();
#else
    const char* set = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    return set != nullptr && value == set;
#endif
}

// An RGB image in binary PPM (P6), 8 bits per channel.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

std::string toPpm(const Image& image) {
    std::string text = "P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    text.append(reinterpret_cast<const char*>(image.rgb.data()), image.rgb.size());
    return text;
}

// The image in `bytes`, or nothing when they are not a binary PPM of 8-bit channels (an LFS
// pointer file of a checkout without LFS, for one).
std::optional<Image> fromPpm(std::string_view bytes) {
    std::size_t at = 0;
    const auto token = [&]() -> std::string_view {
        while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\n' || bytes[at] == '\r' || bytes[at] == '\t')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < bytes.size() && bytes[at] != ' ' && bytes[at] != '\n' && bytes[at] != '\r' && bytes[at] != '\t') {
            ++at;
        }
        return bytes.substr(start, at - start);
    };
    if (token() != "P6") {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(std::strtoul(std::string(token()).c_str(), nullptr, 10));
    image.height = static_cast<std::uint32_t>(std::strtoul(std::string(token()).c_str(), nullptr, 10));
    if (token() != "255" || at >= bytes.size()) {
        return std::nullopt;
    }
    ++at; // the one whitespace character after the maximum
    const std::size_t size = std::size_t{image.width} * image.height * 3;
    if (bytes.size() - at != size) {
        return std::nullopt;
    }
    image.rgb.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return image;
}

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        return result;
    }();
    return kTypes;
}

std::optional<rhi::Device> softwareDevice(rhi::Backend backend) {
    rhi::DeviceDesc desc{.adapter = rhi::AdapterPreference::Software, .applicationName = "editor_ui_gpu"};
    if (environmentIs("ITH_RHI_TEST_VALIDATION", "off")) {
        desc.validation = false;
    }
    Expected<rhi::Device, rhi::Error> device = rhi::createDevice(desc, backend);
    if (!device) {
        MESSAGE("skipped: no software device: " << device.error().toString());
        return std::nullopt;
    }
    return std::move(*device);
}

std::vector<std::byte> compile(ShaderCompiler& compiler, rhi::ShaderFormat format, std::string_view entry,
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

// Renders the shell's fourth frame in the default layout on `device` and reads it back.
Image renderShell(rhi::Device& device) {
    Expected<ShaderCompiler> compiler = ShaderCompiler::create();
    REQUIRE(compiler.hasValue());
    const rhi::ShaderFormat format = device.capabilities().shaderFormat;
    const std::vector<std::byte> vertex = compile(*compiler, format, kEditorUiVertexEntry, ShaderStage::Vertex);
    const std::vector<std::byte> pixel = compile(*compiler, format, kEditorUiPixelEntry, ShaderStage::Pixel);

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
        // The Output Log over a buffer no sink writes to: empty, so the image doesn't depend on the log.
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
        image.rgb.resize(std::size_t{kWidth} * kHeight * 3);
        for (std::size_t i = 0; i < std::size_t{kWidth} * kHeight; ++i) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                image.rgb[i * 3 + channel] = std::to_integer<std::uint8_t>(pixels[i * 4 + channel]);
            }
        }
        graph.reset();
    }
    device.destroy(*readback);
    device.destroy(*target);
    return image;
}

// Compares `actual` with the golden image `name`, or writes it with ITH_UPDATE_GOLDEN=1.
void checkGolden(const Image& actual, std::string_view name) {
    const std::string golden = fs::joinPath(ITH_GOLDEN_DIR, std::string(name) + ".ppm");
    if (environmentIs("ITH_UPDATE_GOLDEN", "1")) {
        REQUIRE(fs::writeFileAtomic(golden, toPpm(actual)).hasValue());
        MESSAGE("wrote " << golden);
        return;
    }
    const Expected<std::string> bytes = fs::readFile(golden, std::size_t{64} << 20);
    REQUIRE_MESSAGE(bytes.hasValue(), ("no golden image " + golden + "; run with ITH_UPDATE_GOLDEN=1"));
    const std::optional<Image> expected = fromPpm(*bytes);
    REQUIRE_MESSAGE(expected.has_value(), (golden + " is not a binary PPM (is Git LFS installed?)"));
    REQUIRE(expected->width == actual.width);
    REQUIRE(expected->height == actual.height);
    std::size_t differing = 0;
    for (std::size_t pixel = 0; pixel < std::size_t{actual.width} * actual.height; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int difference =
                std::abs(int{actual.rgb[pixel * 3 + channel]} - int{expected->rgb[pixel * 3 + channel]});
            if (difference > kChannelTolerance) {
                ++differing;
                break;
            }
        }
    }
    const double share = static_cast<double>(differing) / (static_cast<double>(actual.width) * actual.height);
    MESSAGE(name << ": " << differing << " pixels differ (" << share * 100.0 << "%)");
    if (share > kDifferingPixelsAllowed) {
        const Expected<std::string> directory = fs::executableDirectory();
        if (directory) {
            static_cast<void>(
                fs::writeFileAtomic(fs::joinPath(*directory, std::string(name) + ".actual.ppm"), toPpm(actual)));
        }
    }
    CHECK(share <= kDifferingPixelsAllowed);
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
        if (!device) {
            return;
        }
        if (device->capabilities().adapter.name.find("llvmpipe") == std::string::npos) {
            MESSAGE("skipped: the software Vulkan device is not lavapipe: " << device->capabilities().adapter.name);
            return;
        }
        checkGolden(renderShell(*device), "shell_vulkan");
    }
}
