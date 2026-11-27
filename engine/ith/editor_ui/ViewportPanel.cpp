#include "ith/editor_ui/ViewportPanel.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/Query.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/TransformPropagation.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/AddShape.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorChanges.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_ui/Theme.h"
#include "ith/editor_ui/ViewportShaders.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Entity.h"
#include "ith/foundation/Log.h"
#include "ith/math/Types.h"
#include "ith/rhi/CommandList.h"
#include "ith/rhi/Pipeline.h"
#include "ith/serialization/Value.h"
#include "ith/shapes/ShapeMesh.h"
#include "ith/shapes/ShapeRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ith {

namespace {

ITH_LOG_CATEGORY(Viewport);

using serialization::Value;

// How much of the theme's selection colour a selected shape takes.
constexpr float kSelectionTint = 0.5f;

// The snapping steps the toolbar offers, in metres.
constexpr std::array<float, 6> kSnapSteps{0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};

// Where the grid has faded out: this far, plus this many times the camera's height, in metres.
constexpr float kGridFadeDistance = 50.0f;
constexpr float kGridFadePerHeight = 10.0f;

// The push constants of Shapes.slang.
struct ShapeConstants {
    std::array<Vec4, 4> clip{};
    std::array<Vec4, 3> view{};
    Vec3 color{0.0f};
    std::uint32_t vertices = 0;
};

static_assert(sizeof(ShapeConstants) == kViewportShapesConstantsSize);

// The push constants of Grid.slang.
struct GridConstants {
    Vec4 right{0.0f};
    Vec4 up{0.0f};
    Vec4 forward{0.0f};
    Vec4 camera{0.0f};
    std::array<std::uint32_t, 4> colors{};
};

static_assert(sizeof(GridConstants) == kViewportGridConstantsSize);

// One shape draw of a frame.
struct ShapeDraw {
    ShapeConstants constants;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

constexpr std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// The index of instance `instance` in the Viewports store.
std::optional<std::size_t> indexOf(std::string_view instance) {
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        if (Viewports::instanceId(index) == instance) {
            return index;
        }
    }
    return std::nullopt;
}

// Row `row` of `matrix` (glm is column-major).
Vec4 rowOf(const Mat4& matrix, int row) {
    return {matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
}

// The colour as the grid shader reads it: sRGB channels, red in the lowest byte, then alpha.
std::uint32_t packed(ThemeColor color) {
    return ((color.rgb >> 16) & 0xFFu) | (color.rgb & 0xFF00u) | ((color.rgb & 0xFFu) << 16) |
           (std::uint32_t{color.alpha} << 24);
}

Vec3 linearRgb(ThemeColor color) {
    const std::array<float, 4> linear = linearColor(color);
    return {linear[0], linear[1], linear[2]};
}

// A target side in whole pixels for `pixels`, at most `limit`: 0 for no area.
std::uint32_t targetSide(float pixels, std::uint32_t limit) {
    const double clamped = std::clamp(static_cast<double>(pixels), 0.0, static_cast<double>(limit));
    return static_cast<std::uint32_t>(std::lround(clamped));
}

Unexpected<Error> viewportError(const char* what, const rhi::Error& error) {
    return makeError("editor_ui.viewport_failed", std::string(what) + ": " + error.toString());
}

// Runs a toolbar command, logging a refusal.
void run(EditorSession& session, std::string_view id, const Value& args = {}) {
    if (const Expected<Value> result = session.execute(id, args); !result) {
        ITH_LOG_WARNING(Viewport, "{}", result.error().message);
    }
}

// What an instance last rendered: when any of it changes, the scene renders again. The camera's
// speed step and pivot change nothing drawn, so they are left out; so is the selection in game view.
// The target's size is not here: a new target forgets what was rendered.
struct ViewportRendered {
    Vec3 position{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovX = 0.0f;
    // EditorChanges' structure, values and document counters, summed.
    std::uint64_t document = 0;
    // EditorChanges' selection counter; 0 in game view.
    std::uint64_t selection = 0;
    bool gameView = false;

    friend bool operator==(const ViewportRendered&, const ViewportRendered&) = default;
};

ViewportRendered renderedKey(const ViewportState& state, const EditorChanges& changes) {
    return {.position = state.camera.position,
            .yaw = state.camera.yaw,
            .pitch = state.camera.pitch,
            .fovX = state.camera.fovX,
            .document = changes.structure() + changes.values() + changes.document(),
            .selection = state.gameView ? 0 : changes.selection(),
            .gameView = state.gameView};
}

} // namespace

struct ViewportPanel::Instance {
    // "Viewport <n> colour", the target's debug name.
    std::string name;
    rhi::TextureHandle color;
    rhi::TextureDesc desc;
    UiTextureId texture = 0;
    std::optional<ViewportRendered> rendered;
    // Drawn in this frame: draw() sets it, newFrame() releases the target when it is still unset
    // and clears it.
    bool drawn = false;
    // The size a target couldn't be created at, so it is tried again only at another size.
    std::uint32_t failedWidth = 0;
    std::uint32_t failedHeight = 0;
    // The image was larger than the device's largest texture once: logged.
    bool clampLogged = false;
};

ViewportPanel::ViewportPanel(EditorSession& session, EditorUi& ui, rhi::Device& device)
    : m_session(&session), m_ui(&ui), m_device(&device) {
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        m_instances[index] = std::make_unique<Instance>();
        m_instances[index]->name = "Viewport " + std::to_string(index + 1) + " colour";
    }
}

Expected<std::unique_ptr<ViewportPanel>> ViewportPanel::create(EditorSession& session, EditorUi& ui,
                                                               rhi::Device& device, const ViewportShaderCode& shaders) {
    ITH_ASSERT_MSG(!shaders.shapesVertex.empty() && !shaders.shapesPixel.empty() && !shaders.gridVertex.empty() &&
                       !shaders.gridPixel.empty(),
                   "the viewport needs all four compiled shader stages");
    std::unique_ptr<ViewportPanel> panel(new ViewportPanel(session, ui, device));

    const std::array colorTargets{rhi::ColorTarget{.format = kViewportColorFormat, .blend = rhi::BlendMode::Opaque}};
    const std::array shapeShaders{rhi::ShaderDesc{.stage = rhi::ShaderStage::Vertex,
                                                  .code = shaders.shapesVertex,
                                                  .entryPoint = kViewportShapesVertexEntry,
                                                  .constantsSize = kViewportShapesConstantsSize},
                                  rhi::ShaderDesc{.stage = rhi::ShaderStage::Pixel,
                                                  .code = shaders.shapesPixel,
                                                  .entryPoint = kViewportShapesPixelEntry,
                                                  .constantsSize = kViewportShapesConstantsSize}};
    Expected<rhi::PipelineHandle, rhi::Error> shapes =
        device.createGraphicsPipeline({.shaders = shapeShaders,
                                       .colorTargets = colorTargets,
                                       .depthFormat = rhi::Format::D32Float,
                                       .depth = {.test = true, .write = true, .compare = rhi::CompareOp::GreaterEqual},
                                       .cullMode = rhi::CullMode::None,
                                       .debugName = "Viewport shapes"});
    if (!shapes) {
        return viewportError("the viewport shapes pipeline could not be created", shapes.error());
    }
    panel->m_shapesPipeline = *shapes;

    const std::array gridTargets{rhi::ColorTarget{.format = kViewportColorFormat, .blend = rhi::BlendMode::Alpha}};
    const std::array gridShaders{rhi::ShaderDesc{.stage = rhi::ShaderStage::Vertex,
                                                 .code = shaders.gridVertex,
                                                 .entryPoint = kViewportGridVertexEntry},
                                 rhi::ShaderDesc{.stage = rhi::ShaderStage::Pixel,
                                                 .code = shaders.gridPixel,
                                                 .entryPoint = kViewportGridPixelEntry,
                                                 .constantsSize = kViewportGridConstantsSize}};
    Expected<rhi::PipelineHandle, rhi::Error> grid =
        device.createGraphicsPipeline({.shaders = gridShaders,
                                       .colorTargets = gridTargets,
                                       .depthFormat = rhi::Format::D32Float,
                                       .depth = {.test = true, .write = false, .compare = rhi::CompareOp::GreaterEqual},
                                       .cullMode = rhi::CullMode::None,
                                       .debugName = "Viewport grid"});
    if (!grid) {
        return viewportError("the viewport grid pipeline could not be created", grid.error());
    }
    panel->m_gridPipeline = *grid;

    // Every shape's unit mesh in one buffer, written once: vertices, then indices offset to their
    // shape's first vertex, so a draw needs no vertex offset.
    std::vector<ShapeVertex> vertices;
    std::vector<std::uint16_t> indices;
    for (std::size_t shape = 0; shape < kShapeChoices.size(); ++shape) {
        const ShapeMesh mesh = shapeMesh(kShapeChoices[shape].shape);
        const std::size_t base = vertices.size();
        ITH_ASSERT_MSG(base + mesh.vertices.size() <= std::numeric_limits<std::uint16_t>::max(),
                       "every shape's vertices fit 16-bit indices");
        panel->m_meshes[shape] = {.firstIndex = static_cast<std::uint32_t>(indices.size()),
                                  .indexCount = static_cast<std::uint32_t>(mesh.indices.size())};
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (const std::uint16_t index : mesh.indices) {
            indices.push_back(static_cast<std::uint16_t>(base + index));
        }
    }
    const std::uint64_t vertexBytes = vertices.size() * sizeof(ShapeVertex);
    panel->m_indexOffset = alignUp(vertexBytes, 16);
    panel->m_meshDesc = {.size = alignUp(panel->m_indexOffset + indices.size() * sizeof(std::uint16_t), 16),
                         .usage = rhi::BufferUsage::ShaderRead | rhi::BufferUsage::Index,
                         .memory = rhi::MemoryType::Upload,
                         .debugName = "Viewport shapes"};
    Expected<rhi::BufferHandle, rhi::Error> mesh = device.createBuffer(panel->m_meshDesc);
    if (!mesh) {
        return viewportError("the viewport shapes buffer could not be created", mesh.error());
    }
    panel->m_mesh = *mesh;
    const std::span<std::byte> memory = device.mappedMemory(panel->m_mesh);
    if (memory.size() < panel->m_meshDesc.size) {
        return makeError("editor_ui.viewport_failed", "the viewport shapes buffer has no mapped memory of its size");
    }
    std::memcpy(memory.data(), vertices.data(), vertexBytes);
    std::memcpy(memory.data() + panel->m_indexOffset, indices.data(), indices.size() * sizeof(std::uint16_t));
    return panel;
}

ViewportPanel::~ViewportPanel() {
    for (const std::unique_ptr<Instance>& instance : m_instances) {
        release(*instance);
    }
    m_device->destroy(m_mesh);
    m_device->destroy(m_gridPipeline);
    m_device->destroy(m_shapesPipeline);
}

void ViewportPanel::release(Instance& instance) {
    if (instance.texture != 0) {
        m_ui->unregisterUserTexture(instance.texture);
        instance.texture = 0;
    }
    if (instance.color != rhi::TextureHandle{}) {
        m_device->destroy(instance.color);
        instance.color = {};
    }
    instance.rendered.reset();
}

void ViewportPanel::resize(Instance& instance, std::uint32_t width, std::uint32_t height) {
    release(instance);
    const rhi::TextureDesc desc{.width = width,
                                .height = height,
                                .format = kViewportColorFormat,
                                .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
                                .debugName = instance.name};
    Expected<rhi::TextureHandle, rhi::Error> color = m_device->createTexture(desc);
    if (!color) {
        // Logged once: draw() tries again only at another size.
        instance.failedWidth = width;
        instance.failedHeight = height;
        ITH_LOG_WARNING(Viewport, "The viewport target of {} x {} pixels could not be created: {}", width, height,
                        color.error().toString());
        return;
    }
    instance.failedWidth = 0;
    instance.failedHeight = 0;
    instance.color = *color;
    instance.desc = desc;
    // A new target: the renderer starts it from Undefined, and the scene renders into it.
    instance.texture = m_ui->registerUserTexture(*color, desc);
    if (instance.texture == 0) {
        m_device->destroy(instance.color);
        instance.color = {};
        instance.failedWidth = width;
        instance.failedHeight = height;
    }
}

void ViewportPanel::drawToolbar(std::string_view instance, const ViewportState& state) {
    EditorSession& session = *m_session;
    const Viewports& viewports = session.viewports();
    const ViewportTool tool = viewports.tool();
    if (ImGui::RadioButton("Translate", tool == ViewportTool::Translate)) {
        run(session, "ith.viewport.translate_tool");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", tool == ViewportTool::Rotate)) {
        run(session, "ith.viewport.rotate_tool");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", tool == ViewportTool::Scale)) {
        run(session, "ith.viewport.scale_tool");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    bool snap = viewports.snapEnabled();
    if (ImGui::Checkbox("Snap", &snap)) {
        Value args = Value::object();
        args.set("enabled", Value::boolean(snap));
        run(session, "ith.viewport.set_snap", args);
    }
    ImGui::SameLine();
    std::array<char, 32> label{};
    std::snprintf(label.data(), label.size(), "%g m", static_cast<double>(viewports.snapStep()));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.25 m").x + ImGui::GetFrameHeight() * 2.0f);
    if (ImGui::BeginCombo("##snap_step", label.data())) {
        for (const float step : kSnapSteps) {
            std::snprintf(label.data(), label.size(), "%g m", static_cast<double>(step));
            if (ImGui::Selectable(label.data(), step == viewports.snapStep())) {
                Value args = Value::object();
                args.set("step", Value::real(step));
                run(session, "ith.viewport.set_snap", args);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const char* space = viewports.gizmoSpace() == GizmoSpace::World ? "World###gizmo_space" : "Local###gizmo_space";
    if (ImGui::Button(space)) {
        run(session, "ith.viewport.toggle_gizmo_space");
    }
    ImGui::SetItemTooltip("Gizmo axes: the world's, or the selection's own");
    ImGui::SameLine();
    bool gameView = state.gameView;
    if (ImGui::Checkbox("Game View", &gameView)) {
        Value args = Value::object();
        args.set("viewport", Value::string(std::string(instance)));
        run(session, "ith.viewport.game_view", args);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("Speed %d", state.camera.speedStep);
    ImGui::SetItemTooltip("The fly speed step, 1 to 8: hold the right mouse button and turn the wheel");
}

void ViewportPanel::draw(std::string_view instanceId) {
    const std::optional<std::size_t> index = indexOf(instanceId);
    ITH_ASSERT_MSG(index.has_value(), "a viewport instance is ith.viewport.1 to ith.viewport.4");
    if (!index.has_value()) {
        return;
    }
    ViewportState& state = m_session->viewports().at(*index);
    Instance& instance = *m_instances[*index];
    instance.drawn = true;
    drawToolbar(instanceId, state);

    // The image fills the rest of the window, its target sized in pixels.
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const ImVec2 display = ImGui::GetMainViewport()->Pos;
    const float pixelWidth = std::max(size.x, 0.0f) * scale.x;
    const float pixelHeight = std::max(size.y, 0.0f) * scale.y;
    // Each side at most the device's largest texture: a larger image shows a stretched target.
    const std::uint32_t limit = m_device->capabilities().limits.maxTextureDimension2D;
    const std::uint32_t width = targetSide(pixelWidth, limit);
    const std::uint32_t height = targetSide(pixelHeight, limit);
    const bool oversize = pixelWidth > static_cast<float>(limit) || pixelHeight > static_cast<float>(limit);
    if (oversize && !instance.clampLogged) {
        instance.clampLogged = true;
        ITH_LOG_WARNING(Viewport, "The viewport image is larger than the largest texture ({} pixels); clamped", limit);
    }
    if (width == 0 || height == 0) {
        release(instance);
        state.imageSize = Vec2{0.0f};
        state.imageHovered = false;
        return;
    }
    const bool sizeChanged =
        instance.color == rhi::TextureHandle{} || instance.desc.width != width || instance.desc.height != height;
    const bool failedBefore = instance.failedWidth == width && instance.failedHeight == height;
    if (sizeChanged && !failedBefore) {
        resize(instance, width, height);
    }
    // The button keeps an LMB drag on the image from moving the window; hover is false while
    // anything (a floating panel, a popup) is drawn over the image.
    ImGui::InvisibleButton("##viewport_image", size);
    const bool hovered = ImGui::IsItemHovered();
    if (instance.texture != 0) {
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(instance.texture), origin,
                                             ImVec2(origin.x + size.x, origin.y + size.y));
    }
    state.imagePosition = {(origin.x - display.x) * scale.x, (origin.y - display.y) * scale.y};
    state.imageSize = {pixelWidth, pixelHeight};
    state.imageHovered = hovered;
}

void ViewportPanel::newFrame() {
    for (const std::unique_ptr<Instance>& instance : m_instances) {
        if (!instance->drawn) {
            release(*instance);
        }
        instance->drawn = false;
    }
}

void ViewportPanel::invalidate() noexcept {
    for (const std::unique_ptr<Instance>& instance : m_instances) {
        instance->rendered.reset();
    }
}

void ViewportPanel::addRenderPasses(rg::RenderGraph& graph) {
    rg::Buffer mesh;
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        Instance& instance = *m_instances[index];
        ViewportState& state = m_session->viewports().at(index);
        if (!instance.drawn) {
            // Hidden or closed: no image takes input, and the next newFrame() releases the target.
            state.imageSize = Vec2{0.0f};
            state.imageHovered = false;
            continue;
        }
        if (instance.texture == 0) {
            continue;
        }
        const ViewportRendered now = renderedKey(state, m_session->document().changes());
        if (instance.rendered == now) {
            continue;
        }
        if (mesh.isNull()) {
            // Once per graph, before the first draw list: world matrices for what changed.
            propagateTransforms(m_session->document().mutableWorld());
            mesh = graph.importBuffer({.buffer = m_mesh, .desc = m_meshDesc, .state = &m_meshState});
        }
        if (addScenePasses(graph, instance, state, mesh)) {
            instance.rendered = now;
            ++m_renders;
        }
    }
}

bool ViewportPanel::addScenePasses(rg::RenderGraph& graph, const Instance& instance, const ViewportState& state,
                                   rg::Buffer mesh) {
    const rg::Texture color = m_ui->importUserTexture(graph, instance.texture);
    if (color.isNull()) {
        return false;
    }
    const std::uint32_t width = instance.desc.width;
    const std::uint32_t height = instance.desc.height;
    const rg::Texture depth = graph.createTexture(
        {.width = width, .height = height, .format = rhi::Format::D32Float, .debugName = "Viewport depth"});
    const ThemeTokens& theme = m_ui->theme();

    // Camera-relative (ADR-0006 rule 7): the view has no translation, and every world matrix has the
    // camera's position subtracted.
    const EditorCamera& camera = state.camera;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 view = viewMatrix(camera);
    const Mat3 rotation(view);
    const Mat4 clipFromWorld = projectionMatrix(camera, aspect, kViewportNearPlane) * Mat4(rotation);

    std::vector<ShapeDraw> draws;
    const bool tint = !state.gameView;
    const Vec3 tintColor = linearRgb(theme.viewportSelection);
    const Selection& selection = m_session->document().selection();
    World& world = m_session->document().mutableWorld();
    world.query<const ShapeRenderer, const WorldTransform>().forEach(
        [&](Entity entity, const ShapeRenderer& renderer, const WorldTransform& transform) {
            const auto shape = static_cast<std::size_t>(renderer.shape);
            if (shape >= m_meshes.size()) {
                return;
            }
            Mat4 relative = transform.matrix;
            relative[3] -= Vec4(camera.position, 0.0f);
            const Mat4 clip = clipFromWorld * relative;
            const Mat3 viewFromLocal = rotation * Mat3(transform.matrix);
            Vec3 shapeColor = renderer.color;
            if (tint) {
                const auto* id = world.get<const StableId>(entity);
                if (id != nullptr && selection.contains(id->id)) {
                    shapeColor = glm::mix(shapeColor, tintColor, kSelectionTint);
                }
            }
            ShapeDraw& draw = draws.emplace_back();
            draw.constants.clip = {rowOf(clip, 0), rowOf(clip, 1), rowOf(clip, 2), rowOf(clip, 3)};
            for (int row = 0; row < 3; ++row) {
                draw.constants.view[static_cast<std::size_t>(row)] = {viewFromLocal[0][row], viewFromLocal[1][row],
                                                                      viewFromLocal[2][row], 0.0f};
            }
            draw.constants.color = shapeColor;
            draw.firstIndex = m_meshes[shape].firstIndex;
            draw.indexCount = m_meshes[shape].indexCount;
        });
    m_lastShapeDraws = static_cast<std::uint32_t>(draws.size());

    const std::array<float, 4> clearColor = linearColor(theme.viewportBackground);
    graph.addPass(
        "ViewportShapes", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(color, rhi::LoadOp::Clear, clearColor);
            pass.depth(depth, rhi::LoadOp::Clear, 0.0f);
            pass.read(mesh, rg::BufferUse::ShaderRead, rhi::PipelineStage::VertexShader);
            pass.read(mesh, rg::BufferUse::Index);
        },
        [draws = std::move(draws), mesh, pipeline = m_shapesPipeline,
         indexOffset = m_indexOffset](rg::PassContext& context) {
            if (draws.empty()) {
                return;
            }
            rhi::CommandList& list = context.list();
            list.setPipeline(pipeline);
            list.setIndexBuffer(context.buffer(mesh), indexOffset, rhi::IndexFormat::Uint16);
            const std::uint32_t vertices = context.shaderReadIndex(mesh);
            for (const ShapeDraw& draw : draws) {
                ShapeConstants constants = draw.constants;
                constants.vertices = vertices;
                list.pushConstants(constants);
                list.drawIndexed(draw.indexCount, 1, draw.firstIndex, 0, 0);
            }
        });

    // Game view draws what a game would see: no grid.
    if (state.gameView) {
        return true;
    }
    // The projection's angles (ADR-0046): fovX across the image, the derived vertical angle up it.
    const float tanX = std::tan(0.5f * camera.fovX);
    const float tanY = std::tan(0.5f * verticalFov(camera.fovX, aspect));
    const Vec3 right(rowOf(view, 0));
    const Vec3 up(rowOf(view, 1));
    const Vec3 forward = -Vec3(rowOf(view, 2));
    const GridConstants grid{
        .right = Vec4(right * tanX, kViewportNearPlane),
        .up = Vec4(up * tanY, static_cast<float>(width)),
        .forward = Vec4(forward, static_cast<float>(height)),
        .camera = Vec4(camera.position, kGridFadeDistance + kGridFadePerHeight * std::abs(camera.position.y)),
        .colors = {packed(theme.gridMinor), packed(theme.gridMajor), packed(theme.gridAxisX), packed(theme.gridAxisZ)}};
    graph.addPass(
        "ViewportGrid", rg::PassType::Raster,
        [&](rg::PassBuilder& pass) {
            pass.renderTarget(color, rhi::LoadOp::Load);
            pass.depth(depth, rhi::LoadOp::Load);
        },
        [grid, pipeline = m_gridPipeline](rg::PassContext& context) {
            rhi::CommandList& list = context.list();
            list.setPipeline(pipeline);
            list.pushConstants(grid);
            list.draw(3);
        });
    return true;
}

UiTextureId ViewportPanel::texture(std::string_view instance) const {
    const std::optional<std::size_t> index = indexOf(instance);
    return index.has_value() ? m_instances[*index]->texture : 0;
}

std::optional<rhi::TextureDesc> ViewportPanel::colorTarget(std::string_view instance) const {
    const std::optional<std::size_t> index = indexOf(instance);
    if (!index.has_value() || m_instances[*index]->color == rhi::TextureHandle{}) {
        return std::nullopt;
    }
    return m_instances[*index]->desc;
}

void registerViewport(PanelRegistry& panels, ViewportPanel& panel) {
    panels.add({.id = std::string(kViewportPanel),
                .displayName = "Viewport",
                .defaultDock = DockPlace::Center,
                .maxInstances = kMaxViewports,
                .draw = [&panel](std::string_view instance) { panel.draw(instance); }});
}

} // namespace ith
