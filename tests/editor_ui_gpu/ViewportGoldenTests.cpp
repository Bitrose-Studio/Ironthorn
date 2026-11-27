// A golden image of the Viewport panel (ADR-0042 rule 30): one red cube and one blue sphere resting on
// the grid, seen from a fixed camera, rendered by the panel's shapes and grid passes and shown through
// the editor UI in a 640 x 400 window at display scale 1, on the software GPU of each backend (WARP on
// Direct3D 12, lavapipe on Vulkan) with every shader compiled from Slang at run time, read back and
// compared with tests/editor_ui_gpu/golden/viewport_<backend>.ppm (GoldenImage.h). ITH_UPDATE_GOLDEN=1
// writes the images instead. A second test on each backend resizes and hides the viewport across
// frames, so its targets are destroyed and recreated under the debug layers, and must end on the same
// image. A third shows the same scene from a low camera in a wide 1280 x 448 window, where most of the
// grid is seen at a grazing angle (viewport_grazing_<backend>.ppm): its lines must fade out there
// instead of aliasing into false curves, and the grid must not end on a visible rim.

#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/EditorUiShader.h"
#include "ith/editor_ui/ViewportPanel.h"
#include "ith/editor_ui/ViewportShaders.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/input/Input.h"
#include "ith/math/Types.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/render_graph/ResourcePool.h"
#include "ith/rhi/Device.h"
#include "ith/serialization/Value.h"
#include "ith/shader_compiler/ShaderCompiler.h"

#include "GoldenImage.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)

using namespace ith;
using namespace ith::test::golden;

namespace {

constexpr rhi::Format kFormat = rhi::Format::R8G8B8A8Srgb;

// What a golden image shows: the window's size in pixels (a width that is a multiple of 64, so rows
// read back tightly packed on every backend) and the viewport's camera.
struct Shot {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    EditorCamera camera;
};

// The default camera, pinned here so the image doesn't follow a change of the default, in a 640 x 400
// window.
Shot defaultShot() {
    Shot shot{.width = 640, .height = 400, .camera = {}};
    shot.camera.position = {3.6742346f, 3.0f, 3.6742346f};
    shot.camera.yaw = 0.25f * kPi;
    shot.camera.pitch = -kPi / 6.0f;
    shot.camera.fovY = kHalfPi;
    return shot;
}

// A camera two metres up, looking ten degrees down, in a wide 1280 x 448 window: most of the grid is
// seen at a grazing angle, where its lines lie far closer together than a pixel, and the grid runs out
// toward the horizon. The lines must fade out smoothly there instead of aliasing into false patterns.
Shot grazingShot() {
    Shot shot = defaultShot();
    shot.width = 1280;
    shot.height = 448;
    shot.camera.position = {4.0f, 2.0f, 6.0f};
    shot.camera.yaw = 0.15f * kPi;
    shot.camera.pitch = -kPi / 18.0f;
    return shot;
}

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        generated::registerReflection_shapes(result);
        return result;
    }();
    return kTypes;
}

serialization::Value vec3(float x, float y, float z) {
    serialization::Value value = serialization::Value::object();
    value.set("x", serialization::Value::real(x));
    value.set("y", serialization::Value::real(y));
    value.set("z", serialization::Value::real(z));
    return value;
}

// ith.entity.create's arguments for `shape` at `position` in `color`.
serialization::Value shapeAt(const char* shape, const serialization::Value& position,
                             const serialization::Value& color) {
    serialization::Value translation = serialization::Value::object();
    translation.set("translation", position);
    serialization::Value transform = serialization::Value::object();
    transform.set("value", std::move(translation));
    serialization::Value renderer = serialization::Value::object();
    renderer.set("shape", serialization::Value::string(shape));
    renderer.set("color", color);
    serialization::Value components = serialization::Value::object();
    components.set("ith::LocalTransform", std::move(transform));
    components.set("ith::ShapeRenderer", std::move(renderer));
    serialization::Value args = serialization::Value::object();
    args.set("components", std::move(components));
    return args;
}

// Draws the viewport in a window of `size` at the top left; a zero size draws nothing (hidden).
void drawViewport(EditorSession& session, ImVec2 size) {
    if (size.x <= 0.0f) {
        return;
    }
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(size);
    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Viewport###golden.viewport", nullptr, kFlags)) {
        session.panels().find("ith.viewport")->draw("ith.viewport.1");
    }
    ImGui::End();
}

// What renderViewport() rendered.
struct Rendered {
    Image image;
    std::uint64_t renders = 0;
    std::uint32_t shapeDraws = 0;
};

// The full window of `shot`.
ImVec2 fullWindow(const Shot& shot) {
    return {static_cast<float>(shot.width), static_cast<float>(shot.height)};
}

// Renders one frame per entry of `frames`, the viewport window of that size (zero: hidden), in the
// window and with the camera of `shot`, on `device` and reads the last frame back.
Rendered renderViewport(rhi::Device& device, const Shot& shot, std::span<const ImVec2> frames) {
    REQUIRE(shot.width % 64 == 0); // rows stay tightly packed at D3D12's 256-byte pitch
    const std::uint32_t width = shot.width;
    const std::uint32_t height = shot.height;
    const std::uint32_t rowPitch = width * 4;
    Expected<ShaderCompiler> compiler = ShaderCompiler::create();
    REQUIRE(compiler.hasValue());
    const rhi::ShaderFormat format = device.capabilities().shaderFormat;
    const std::vector<std::byte> uiVertex = compileStage(*compiler, format, kEditorUiShaderName, editorUiShaderSource(),
                                                         kEditorUiVertexEntry, ShaderStage::Vertex);
    const std::vector<std::byte> uiPixel = compileStage(*compiler, format, kEditorUiShaderName, editorUiShaderSource(),
                                                        kEditorUiPixelEntry, ShaderStage::Pixel);
    const std::vector<std::byte> shapesVertex =
        compileStage(*compiler, format, kViewportShapesShaderName, viewportShapesShaderSource(),
                     kViewportShapesVertexEntry, ShaderStage::Vertex);
    const std::vector<std::byte> shapesPixel =
        compileStage(*compiler, format, kViewportShapesShaderName, viewportShapesShaderSource(),
                     kViewportShapesPixelEntry, ShaderStage::Pixel);
    const std::vector<std::byte> gridVertex =
        compileStage(*compiler, format, kViewportGridShaderName, viewportGridShaderSource(), kViewportGridVertexEntry,
                     ShaderStage::Vertex);
    const std::vector<std::byte> gridPixel =
        compileStage(*compiler, format, kViewportGridShaderName, viewportGridShaderSource(), kViewportGridPixelEntry,
                     ShaderStage::Pixel);

    const rhi::TextureDesc targetDesc{.width = width,
                                      .height = height,
                                      .format = kFormat,
                                      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                                      .debugName = "Viewport golden target"};
    Expected<rhi::TextureHandle, rhi::Error> target = device.createTexture(targetDesc);
    REQUIRE(target.hasValue());
    const rhi::BufferDesc readbackDesc{.size = std::uint64_t{rowPitch} * height,
                                       .usage = rhi::BufferUsage::CopyDst,
                                       .memory = rhi::MemoryType::Readback,
                                       .debugName = "Viewport golden readback"};
    Expected<rhi::BufferHandle, rhi::Error> readback = device.createBuffer(readbackDesc);
    REQUIRE(readback.hasValue());

    EditorSession session(types());
    REQUIRE(session.execute("ith.entity.create", shapeAt("Cube", vec3(-0.8f, 0.5f, 0.0f), vec3(0.8f, 0.12f, 0.08f)))
                .hasValue());
    REQUIRE(session.execute("ith.entity.create", shapeAt("Sphere", vec3(0.9f, 0.5f, 0.4f), vec3(0.1f, 0.3f, 0.85f)))
                .hasValue());
    REQUIRE(session.execute("ith.edit.select_none").hasValue());
    session.viewports().at(0).camera = shot.camera;

    Input input;
    const Expected<std::string> executable = fs::executableDirectory();
    REQUIRE(executable.hasValue());
    const std::string fonts = fs::joinPath(*executable, "fonts");
    Rendered rendered;
    {
        Expected<std::unique_ptr<EditorUi>> ui = EditorUi::create({.device = &device,
                                                                   .targetFormat = kFormat,
                                                                   .vertexShader = uiVertex,
                                                                   .pixelShader = uiPixel,
                                                                   .fontDirectory = fonts,
                                                                   .input = &input,
                                                                   .window = 1,
                                                                   .caretBlink = false});
        REQUIRE_MESSAGE(ui.hasValue(), (ui ? std::string() : ui.error().message));
        Expected<std::unique_ptr<ViewportPanel>> panel = ViewportPanel::create(session, **ui, device,
                                                                               {.shapesVertex = shapesVertex,
                                                                                .shapesPixel = shapesPixel,
                                                                                .gridVertex = gridVertex,
                                                                                .gridPixel = gridPixel});
        REQUIRE_MESSAGE(panel.hasValue(), (panel ? std::string() : panel.error().message));
        registerViewport(session.panels(), **panel);
        rg::RenderGraph graph({.name = "Viewport golden"});
        rg::ResourcePool pool(device);
        rg::ImportedState targetState;
        rg::ImportedState readbackState;
        for (std::size_t frame = 0; frame < frames.size(); ++frame) {
            (*ui)->newFrame({.size = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)},
                             .pixelSize = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)},
                             .deltaSeconds = 1.0f / 60.0f});
            (*panel)->newFrame();
            drawViewport(session, frames[frame]);
            (*ui)->endFrame();
            Expected<std::optional<rhi::Frame>, rhi::Error> begun = device.beginFrame();
            REQUIRE(begun.hasValue());
            REQUIRE(begun->has_value());
            graph.reset();
            const rg::Texture texture =
                graph.importTexture({.texture = *target, .desc = targetDesc, .state = &targetState, .writable = true});
            (*panel)->addRenderPasses(graph);
            (*ui)->addRenderPasses(graph, texture, (*begun)->slot, rhi::LoadOp::Clear,
                                   {0.0103f, 0.0103f, 0.0116f, 1.0f});
            if (frame + 1 == frames.size()) {
                const rg::Buffer buffer = graph.importBuffer(
                    {.buffer = *readback, .desc = readbackDesc, .state = &readbackState, .writable = true});
                graph.addPass(
                    "Read back", rg::PassType::Copy,
                    [&](rg::PassBuilder& pass) {
                        pass.read(texture, rg::TextureUse::CopySrc);
                        pass.write(buffer, rg::BufferUse::CopyDst);
                    },
                    [texture, buffer, rowPitch](rg::PassContext& context) {
                        context.list().copyTextureToBuffer({.buffer = context.buffer(buffer),
                                                            .rowPitch = rowPitch,
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
        rendered = {.image = imageFromTexels(device.mappedMemory(*readback), width, height),
                    .renders = (*panel)->renders(),
                    .shapeDraws = (*panel)->lastShapeDraws()};
        graph.reset();
        panel->reset();
    }
    device.destroy(*readback);
    device.destroy(*target);
    return rendered;
}

// The golden image of `shot`: three frames of the full window. Two shape draws, rendered once: the
// later frames changed nothing.
Image renderGolden(rhi::Device& device, const Shot& shot) {
    const ImVec2 full = fullWindow(shot);
    const std::array frames{full, full, full};
    const Rendered rendered = renderViewport(device, shot, frames);
    CHECK(rendered.shapeDraws == 2);
    CHECK(rendered.renders == 1);
    return rendered.image;
}

// Resizes the viewport and hides it across frames, so targets are destroyed while earlier frames
// may still use them and released slots are reused, under the device's debug layers; then shows
// it at full size again, which must give the golden image. Four renders: the first, the resize down,
// the resize back, and the re-show after the hidden frames released the target.
Image renderResizedAndHidden(rhi::Device& device) {
    const ImVec2 smaller{400.0f, 300.0f};
    const ImVec2 hidden{0.0f, 0.0f};
    const Shot shot = defaultShot();
    const ImVec2 full = fullWindow(shot);
    const std::array frames{full, smaller, smaller, full, hidden, hidden, hidden, full, full, full};
    const Rendered rendered = renderViewport(device, shot, frames);
    CHECK(rendered.renders == 4);
    return rendered.image;
}

} // namespace

TEST_SUITE("editor_ui_gpu.viewport") {
#ifdef ITH_EDITOR_UI_GPU_HAS_D3D12
    TEST_CASE("a cube and a sphere on the grid on WARP match their golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::D3D12);
        if (!device) {
            return;
        }
        checkGolden(renderGolden(*device, defaultShot()), "viewport_d3d12");
    }

    TEST_CASE("the grid seen at a grazing angle in a wide viewport on WARP matches its golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::D3D12);
        if (!device) {
            return;
        }
        checkGolden(renderGolden(*device, grazingShot()), "viewport_grazing_d3d12");
    }

    TEST_CASE("resizing and hiding the viewport on WARP destroys and recreates its target cleanly") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::D3D12);
        if (!device) {
            return;
        }
        checkGolden(renderResizedAndHidden(*device), "viewport_d3d12");
    }
#endif

    TEST_CASE("a cube and a sphere on the grid on lavapipe match their golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::Vulkan);
        if (!device || !isLavapipe(*device)) {
            return;
        }
        checkGolden(renderGolden(*device, defaultShot()), "viewport_vulkan");
    }

    TEST_CASE("the grid seen at a grazing angle in a wide viewport on lavapipe matches its golden image") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::Vulkan);
        if (!device || !isLavapipe(*device)) {
            return;
        }
        checkGolden(renderGolden(*device, grazingShot()), "viewport_grazing_vulkan");
    }

    TEST_CASE("resizing and hiding the viewport on lavapipe destroys and recreates its target cleanly") {
        std::optional<rhi::Device> device = softwareDevice(rhi::Backend::Vulkan);
        if (!device || !isLavapipe(*device)) {
            return;
        }
        checkGolden(renderResizedAndHidden(*device), "viewport_vulkan");
    }
}
