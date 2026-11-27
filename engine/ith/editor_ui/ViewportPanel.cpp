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
#include "ith/editor_core/ViewportPick.h"
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

// The push constants of Shapes.slang. They are at rhi::kMaxPushConstantSize (128 bytes), so the
// entity's id texel (pickTexel()) rides in the words the local-to-view 3x3 leaves free after its
// first two rows, instead of a per-draw buffer (ADR-0047 item 4). The layout matches the shader's
// 16-byte packing: a float3 and a uint share each row.
struct ShapeConstants {
    std::array<Vec4, 4> clip{};
    Vec3 view0{0.0f};
    std::uint32_t entityIndex = 0;
    Vec3 view1{0.0f};
    std::uint32_t entityGeneration = 0;
    Vec4 view2{0.0f};
    Vec3 color{0.0f};
    std::uint32_t vertices = 0;
};

static_assert(sizeof(ShapeConstants) == kViewportShapesConstantsSize);
static_assert(kViewportShapesConstantsSize == rhi::kMaxPushConstantSize, "no word is left for more per-draw data");
static_assert(offsetof(ShapeConstants, entityIndex) == 76);
static_assert(offsetof(ShapeConstants, entityGeneration) == 92);
static_assert(offsetof(ShapeConstants, view2) == 96);

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
    // A click whose texel is being copied: the device frame that copies it, and how it selects.
    struct Pick {
        std::uint64_t frame = 0;
        PickMode mode = PickMode::Replace;
    };

    // "Viewport <n> colour" and "Viewport <n> ids", the targets' debug names.
    std::string name;
    std::string idName;
    rhi::TextureHandle color;
    rhi::TextureDesc desc;
    UiTextureId texture = 0;
    // The id target, created and released with the colour target; its state between graphs.
    rhi::TextureHandle ids;
    rhi::TextureDesc idDesc;
    rg::ImportedState idState;
    // At most one per instance: a newer click replaces it.
    std::optional<Pick> pick;
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
        m_instances[index]->idName = "Viewport " + std::to_string(index + 1) + " ids";
    }
}

Expected<std::unique_ptr<ViewportPanel>> ViewportPanel::create(EditorSession& session, EditorUi& ui,
                                                               rhi::Device& device, const ViewportShaderCode& shaders) {
    ITH_ASSERT_MSG(!shaders.shapesVertex.empty() && !shaders.shapesPixel.empty() && !shaders.gridVertex.empty() &&
                       !shaders.gridPixel.empty(),
                   "the viewport needs all four compiled shader stages");
    std::unique_ptr<ViewportPanel> panel(new ViewportPanel(session, ui, device));

    const std::array colorTargets{rhi::ColorTarget{.format = kViewportColorFormat, .blend = rhi::BlendMode::Opaque},
                                  rhi::ColorTarget{.format = kViewportIdFormat, .blend = rhi::BlendMode::Opaque}};
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

    // One texel slot per instance, each at the copy offset alignment; the CPU only reads it.
    panel->m_picksDesc = {.size = kMaxViewports * rhi::kTextureCopyOffsetAlignment,
                          .usage = rhi::BufferUsage::CopyDst,
                          .memory = rhi::MemoryType::Readback,
                          .debugName = "Viewport picks"};
    Expected<rhi::BufferHandle, rhi::Error> picks = device.createBuffer(panel->m_picksDesc);
    if (!picks) {
        return viewportError("the viewport picks buffer could not be created", picks.error());
    }
    panel->m_picks = *picks;
    if (device.mappedMemory(panel->m_picks).size() < panel->m_picksDesc.size) {
        return makeError("editor_ui.viewport_failed", "the viewport picks buffer has no mapped memory of its size");
    }
    return panel;
}

ViewportPanel::~ViewportPanel() {
    for (const std::unique_ptr<Instance>& instance : m_instances) {
        release(*instance);
    }
    m_device->destroy(m_picks);
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
    // A pick already copied reads the picks buffer, not the target, so it stays.
    if (instance.ids != rhi::TextureHandle{}) {
        m_device->destroy(instance.ids);
        instance.ids = {};
    }
    instance.idState = {};
    instance.rendered.reset();
}

void ViewportPanel::resize(Instance& instance, std::uint32_t width, std::uint32_t height) {
    release(instance);
    const rhi::TextureDesc desc{.width = width,
                                .height = height,
                                .format = kViewportColorFormat,
                                .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
                                .debugName = instance.name};
    const rhi::TextureDesc idDesc{.width = width,
                                  .height = height,
                                  .format = kViewportIdFormat,
                                  .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
                                  .debugName = instance.idName};
    // Logged once: draw() tries again only at another size.
    const auto failed = [&](const rhi::Error& error) {
        instance.failedWidth = width;
        instance.failedHeight = height;
        ITH_LOG_WARNING(Viewport, "The viewport targets of {} x {} pixels could not be created: {}", width, height,
                        error.toString());
    };
    Expected<rhi::TextureHandle, rhi::Error> color = m_device->createTexture(desc);
    if (!color) {
        failed(color.error());
        return;
    }
    Expected<rhi::TextureHandle, rhi::Error> ids = m_device->createTexture(idDesc);
    if (!ids) {
        m_device->destroy(*color);
        failed(ids.error());
        return;
    }
    instance.failedWidth = 0;
    instance.failedHeight = 0;
    instance.color = *color;
    instance.desc = desc;
    instance.ids = *ids;
    instance.idDesc = idDesc;
    // New targets: the renderer starts the colour one from Undefined, the id one starts from
    // ImportedState's default, and the scene renders into both.
    instance.texture = m_ui->registerUserTexture(*color, desc);
    if (instance.texture == 0) {
        release(instance);
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
        instance->pick.reset();
    }
}

bool ViewportPanel::picking() const noexcept {
    return std::ranges::any_of(m_instances,
                               [](const std::unique_ptr<Instance>& instance) { return instance->pick.has_value(); });
}

void ViewportPanel::readPicks(std::uint64_t open) {
    // Inside frame `open`, beginFrame() has waited for every frame up to open - kFramesInFlight: those
    // copies are done, and their bytes are the CPU's to read (Device::mappedMemory()).
    const std::span<const std::byte> memory = m_device->mappedMemory(m_picks);
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        Instance& instance = *m_instances[index];
        if (!instance.pick.has_value() || instance.pick->frame + rhi::kFramesInFlight > open) {
            continue;
        }
        const PickMode mode = instance.pick->mode;
        instance.pick.reset();
        std::array<std::uint32_t, 2> texel{};
        std::memcpy(texel.data(), memory.data() + index * rhi::kTextureCopyOffsetAlignment, sizeof(texel));
        if (const Expected<void> applied = applyPick(*m_session, {.index = texel[0], .generation = texel[1]}, mode);
            !applied) {
            ITH_LOG_WARNING(Viewport, "{}", applied.error().message);
        }
    }
}

rg::Texture ViewportPanel::importIds(rg::RenderGraph& graph, Instance& instance) {
    return graph.importTexture(
        {.texture = instance.ids, .desc = instance.idDesc, .state = &instance.idState, .writable = true});
}

void ViewportPanel::addRenderPasses(rg::RenderGraph& graph, const rhi::Frame& opened) {
    // First, so a selection a pick changes is drawn by this frame's scene passes.
    readPicks(opened.number);
    // The device frame these passes run in.
    const std::uint64_t frame = opened.number;
    rg::Buffer mesh;
    rg::Buffer picks;
    for (std::size_t index = 0; index < kMaxViewports; ++index) {
        Instance& instance = *m_instances[index];
        ViewportState& state = m_session->viewports().at(index);
        if (!instance.drawn) {
            // Hidden or closed: no image takes input, and the next newFrame() releases the target.
            state.imageSize = Vec2{0.0f};
            state.imageHovered = false;
            state.click.reset();
            continue;
        }
        if (instance.texture == 0) {
            state.click.reset();
            continue;
        }
        rg::Texture ids;
        const ViewportRendered now = renderedKey(state, m_session->document().changes());
        if (instance.rendered != now) {
            if (mesh.isNull()) {
                // Once per graph, before the first draw list: world matrices for what changed.
                propagateTransforms(m_session->document().mutableWorld());
                mesh = graph.importBuffer({.buffer = m_mesh, .desc = m_meshDesc, .state = &m_meshState});
            }
            ids = addScenePasses(graph, instance, state, mesh);
            if (!ids.isNull()) {
                instance.rendered = now;
                ++m_renders;
            }
        }
        if (!state.click.has_value()) {
            continue;
        }
        const ViewportClick click = *state.click;
        state.click.reset();
        if (!instance.rendered.has_value() || state.imageSize.x <= 0.0f || state.imageSize.y <= 0.0f) {
            continue; // nothing drawn to pick from
        }
        if (ids.isNull()) {
            ids = importIds(graph, instance);
        }
        if (picks.isNull()) {
            picks =
                graph.importBuffer({.buffer = m_picks, .desc = m_picksDesc, .state = &m_picksState, .writable = true});
        }
        // Window pixels to the target's texels: the same unless the target was clamped. Inside the
        // target, also for a position on or past the image's edge.
        const auto texelOf = [](float position, float imageSide, std::uint32_t side) -> std::uint32_t {
            ITH_ASSERT(side > 0);
            const float texel = std::floor(position * static_cast<float>(side) / imageSide);
            if (side == 0 || !(texel > 0.0f)) {
                return 0; // also NaN
            }
            return texel >= static_cast<float>(side - 1) ? side - 1 : static_cast<std::uint32_t>(texel);
        };
        const rhi::TexelRegion region{.x = texelOf(click.position.x, state.imageSize.x, instance.idDesc.width),
                                      .y = texelOf(click.position.y, state.imageSize.y, instance.idDesc.height),
                                      .width = 1,
                                      .height = 1,
                                      .depth = 1};
        const std::uint64_t offset = index * std::uint64_t{rhi::kTextureCopyOffsetAlignment};
        graph.addPass(
            "ViewportPick", rg::PassType::Copy,
            [&](rg::PassBuilder& pass) {
                pass.read(ids, rg::TextureUse::CopySrc);
                pass.write(picks, rg::BufferUse::CopyDst);
            },
            [ids, picks, offset, region](rg::PassContext& context) {
                context.list().copyTextureToBuffer({.buffer = context.buffer(picks),
                                                    .bufferOffset = offset,
                                                    .rowPitch = rhi::kTextureCopyRowAlignment,
                                                    .texture = context.texture(ids),
                                                    .region = region});
            },
            rg::PassFlags::NeverCull);
        instance.pick = Instance::Pick{.frame = frame, .mode = click.mode};
    }
}

rg::Texture ViewportPanel::addScenePasses(rg::RenderGraph& graph, Instance& instance, const ViewportState& state,
                                          rg::Buffer mesh) {
    const rg::Texture color = m_ui->importUserTexture(graph, instance.texture);
    if (color.isNull()) {
        return {};
    }
    const rg::Texture ids = importIds(graph, instance);
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
            draw.constants.view0 = {viewFromLocal[0][0], viewFromLocal[1][0], viewFromLocal[2][0]};
            draw.constants.view1 = {viewFromLocal[0][1], viewFromLocal[1][1], viewFromLocal[2][1]};
            draw.constants.view2 = {viewFromLocal[0][2], viewFromLocal[1][2], viewFromLocal[2][2], 0.0f};
            const PickTexel texel = pickTexel(entity);
            draw.constants.entityIndex = texel.index;
            draw.constants.entityGeneration = texel.generation;
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
            pass.renderTarget(ids, rhi::LoadOp::Clear, {0.0f, 0.0f, 0.0f, 0.0f}); // 0: no entity
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
        return ids;
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
    return ids;
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

std::optional<rhi::TextureDesc> ViewportPanel::idTarget(std::string_view instance) const {
    const std::optional<std::size_t> index = indexOf(instance);
    if (!index.has_value() || m_instances[*index]->ids == rhi::TextureHandle{}) {
        return std::nullopt;
    }
    return m_instances[*index]->idDesc;
}

void registerViewport(PanelRegistry& panels, ViewportPanel& panel) {
    panels.add({.id = std::string(kViewportPanel),
                .displayName = "Viewport",
                .defaultDock = DockPlace::Center,
                .maxInstances = kMaxViewports,
                .draw = [&panel](std::string_view instance) { panel.draw(instance); }});
}

} // namespace ith
