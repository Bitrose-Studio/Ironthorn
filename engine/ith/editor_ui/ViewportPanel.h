#pragma once

// ith::ViewportPanel: the Viewport panel (ADR-0042 rules 8 and 13 to 26, ADR-0040 rule 3, ADR-0053
// decision 16), drawn from editor_core's Viewports store: a toolbar, and the world the instance
// draws (the editor world, or the game world in the play viewport) rendered into a texture the
// panel shows as an image.
//
//   auto viewport = ith::ViewportPanel::create(session, ui, device, shaders); // compiled by the app
//   ith::registerViewport(session.panels(), **viewport);  // "ith.viewport", instances 1 to 4
//   while (running) {
//       ui.newFrame(...);
//       (*viewport)->newFrame();                           // releases the targets of hidden viewports
//       ith::drawEditorShell(session, keymap, shell, dock); // draws the open viewports
//       ui.endFrame();
//       ... device.beginFrame(), graph.reset(), target = graph.importBackbuffer(frame) ...
//       (*viewport)->addRenderPasses(graph, frame);        // the scene passes, before the UI's
//       ui.addRenderPasses(graph, target, frame.slot, ith::rhi::LoadOp::Clear, clearColor);
//       graph.compile(); graph.execute(pool);
//   }
//   ui.abandonFrame();                                     // if the last frame wasn't rendered
//   viewport->reset();                                     // before the EditorUi
//
// Each open viewport instance ("ith.viewport.1" to "ith.viewport.4") owns a colour target
// (R8G8B8A8Srgb, RenderTarget | Sampled), sized to its image in pixels (the framebuffer scale
// applied, each side clamped to the device's maxTextureDimension2D), registered with the EditorUi as
// a user texture and shown with ImGui. A resize unregisters the old target, destroys it (the device
// defers it until the GPU is done) and makes a new one; an image without area has no target. A
// target that can't be created is logged once and tried again only when the size changes. Next to
// it the instance keeps its id target (kViewportIdFormat, R32G32Uint, RenderTarget | CopySrc, the
// colour target's size, created and released with it; ADR-0042 rule 21): it is read after the frame
// that drew it, when a click comes, so it lives as long as the picture it matches. The depth target
// (D32Float, cleared to 0) is a transient of the render graph, the colour target's size, so it needs
// no keeping. An instance not drawn in a frame (a hidden tab, a closed panel) releases its targets
// at the start of the next frame (newFrame()); shown again, it gets new targets and renders once.
//
// The scene is rendered by two raster passes before the UI's, in the same graph, on the colour
// target's one import (EditorUi::importUserTexture): "ViewportShapes" clears the targets and draws
// every enabled entity with a ShapeRenderer and a WorldTransform, one draw each, unlit with a fixed
// light from the camera; selected entities are tinted with the theme's viewportSelection. It also
// writes each fragment's entity into the id target (pickTexel(), ViewportPick.h; 0 where no shape
// is), so the id target always matches the picture.
// "ViewportGrid" draws the infinite grid on y = 0 over them, depth-tested (Grid.slang). Depth is
// reversed (greater-equal, ADR-0006 rule 6), and rendering is camera-relative: the camera's position
// is subtracted on the CPU before the matrices are built (rule 7). Nothing is culled, so a shape
// with a negative scale (mirrored) draws too, and a Plane shows from both sides. Game view
// (ViewportState::gameView) draws neither the grid nor the tint. Transforms are propagated
// (propagateTransforms) once per graph for each world drawn, before its first draw list; that writes
// only the Transient WorldTransform, so the document, its history and its dirty state are untouched.
//
// Play In Editor (ADR-0053 decisions 16 and 17): while a session runs, its play viewport draws the
// game world (the world EditorSession::drawnWorld names, through PlaySession::mutableWorld), as game
// view while possessed whatever its flag says, and the other viewports keep drawing the editor
// world. Every viewport tints the active selection
// (EditorSession::active, the play selection while playing). The play viewport shows its state over
// the image (drawnPlayOverlay, spec 3.13): a 2-point border in the theme's playActive (Playing),
// playSimulate (Simulating) or playPaused (Paused), the state's word in the top-left corner and,
// while possessed with the mouse free, kPlayCaptureHint below it.
//
// The scene re-renders only on change (ADR-0042 rule 16): when the camera's position, yaw, pitch or
// field of view, the drawn world's entities, values or open level (its EditorChanges), the active
// world's selection (except in game view, which doesn't show it) or identity (Play and Stop), the
// target's size or the game-view flag differ from what the instance last rendered. Otherwise
// addRenderPasses() adds nothing for it and the image shows the last render. A ticking session moves
// the game world's values every tick, so the play viewport renders every frame while it runs; Keep
// Simulation Changes moves the editor world's, so the other viewports render it at once.
//
// Input (rule 8): every frame a drawn instance reports its image rectangle in window pixels (ImGui
// coordinates times the framebuffer scale) and whether the image is hovered with nothing on top of
// it to its ViewportState (imagePosition, imageSize, imageHovered); an invisible button covers the
// image, so an LMB drag on it doesn't move a floating window. An instance not drawn in a frame (a
// hidden tab, a closed panel) reports a zero size and no hover. The viewport consumer routes by
// these.
//
// Click picking (ADR-0042 rule 22): addRenderPasses() takes each drawn instance's
// ViewportState::click, which the viewport consumer set, maps it to the target's texel (window
// pixels scaled by the target's size over the image's, so any framebuffer scale and a clamped target
// both map right), and adds a "ViewportPick" copy pass after the scene passes that copies that one
// texel of the id target into the instance's 8 bytes of a small Readback buffer (512-byte slots, rows
// 256-byte aligned: rhi::kTextureCopyOffsetAlignment, kTextureCopyRowAlignment). The pick then waits
// in the instance, with the click's mode and the device frame that copied it; at most one waits per
// instance, and a newer click replaces it (only the newest copy is read, after its own frame). The
// first addRenderPasses() whose device frame is at least kFramesInFlight frames after that one — so
// the device's beginFrame() has already waited for it; the panel never waits on the GPU — reads the
// texel and applies it with applyPick() (ith.selection.set; empty space clears on a plain click,
// stale hits and entities without a stable id change nothing). That happens in the render phase,
// before the scene passes: the same frame redraws the tint, and the Outliner and Details, already
// drawn, show the selection a frame later. The selection so changes exactly kFramesInFlight frames
// after the click; while a pick waits, picking() is true and the editor keeps drawing frames. A
// click on an instance without a rendered target is dropped, and so is a pick whose read comes after
// Play or Stop changed the active world (its identity is kept with the pick): its texel names an
// entity of a world no longer drawn there. A refused selection is logged as a
// warning in the "Viewport" category.
//
// The translate gizmo (ADR-0042 rules 24 to 26): when the instance shows one (viewportGizmo(),
// ViewportGizmo.h: the Translate tool, a selection, not game view), draw() draws it over the image,
// on the window's draw list after the image and clipped to it, so it is always on top of the scene
// and needs no scene render, while floating windows and popups still cover it. Its geometry is the
// layout editor core computes in image pixels at the UI's display scale (arrows kGizmoArrowLength
// points long): three arrows in the theme's gizmoAxisX, gizmoAxisY and gizmoAxisZ, and three plane
// squares in the colour of the axis normal to each, translucent; handles fade as they turn end-on or
// edge-on and vanish when they can't be grabbed. The handle a drag holds (ViewportState::gizmoDrag),
// or else the one under the pointer while the image is hovered, hit-tested as the viewport consumer
// hit-tests a press, is drawn in gizmoHovered. The viewport consumer runs the drag itself.
//
// The toolbar runs commands; nothing in it changes state directly: the transform tool
// (ith.viewport.translate_tool, rotate_tool, scale_tool), snapping and its step
// (ith.viewport.set_snap), the gizmo space (ith.viewport.toggle_gizmo_space), game view
// (ith.viewport.game_view with the instance), and the camera's fly speed step. A refused command is
// logged as a warning in the "Viewport" category.
//
// Main thread only, inside the EditorUi's lifetime. Every call that registers or unregisters a
// target (newFrame(), draw(), the destructor) runs outside the EditorUi's graph-import phase
// (between EditorUi::endFrame() and EditorUi::addRenderPasses()); EditorUi::abandonFrame() closes
// that phase when a frame isn't rendered.

#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/foundation/Expected.h"
#include "ith/render_graph/RenderGraph.h"
#include "ith/rhi/Device.h"
#include "ith/rhi/Resources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ith {

class EditorSession;
class PanelRegistry;
class World;

// The colour target format of every viewport.
inline constexpr rhi::Format kViewportColorFormat = rhi::Format::R8G8B8A8Srgb;

// The id target format of every viewport (ADR-0042 rule 21): R = entity slot index + 1, G = its
// generation, 0 for no entity.
inline constexpr rhi::Format kViewportIdFormat = rhi::Format::R32G32Uint;

// The hint the play viewport shows while a session is possessed and the mouse is free
// (docs/specs/play-in-editor.md 3.7 rule 1).
inline constexpr std::string_view kPlayCaptureHint = "Click to control the game \xC2\xB7 Shift+F1 frees the mouse";

// What draw() drew over the play viewport's image (spec 3.13).
struct ViewportPlayOverlay {
    // The 2-point border's and the label's colour: the theme's playActive, playSimulate or playPaused.
    ThemeColor border;
    // "Playing", "Simulating" or "Paused", in the image's top-left corner.
    std::string_view label;
    // kPlayCaptureHint below the label: possessed, the mouse not captured.
    bool captureHint = false;
};

// The compiled stages of viewportShapesShaderSource() and viewportGridShaderSource()
// (ViewportShaders.h), in the device's bytecode format; read during ViewportPanel::create() only.
// The null device takes any non-empty bytes.
struct ViewportShaderCode {
    std::span<const std::byte> shapesVertex;
    std::span<const std::byte> shapesPixel;
    std::span<const std::byte> gridVertex;
    std::span<const std::byte> gridPixel;
};

class ViewportPanel {
public:
    // Creates the two pipelines, the buffer holding every shape's unit mesh, filled once, and the
    // Readback buffer of the picks. `session`, `ui` and `device` must outlive the panel, and `ui`
    // must render with `device`. Fails with "editor_ui.viewport_failed", the RHI's message inside,
    // when a pipeline or a buffer can't be created or a buffer's mapped memory is too small.
    // Precondition (asserted): no shader is empty.
    static Expected<std::unique_ptr<ViewportPanel>> create(EditorSession& session, EditorUi& ui, rhi::Device& device,
                                                           const ViewportShaderCode& shaders);

    ViewportPanel(const ViewportPanel&) = delete;
    ViewportPanel& operator=(const ViewportPanel&) = delete;
    ViewportPanel(ViewportPanel&&) = delete;
    ViewportPanel& operator=(ViewportPanel&&) = delete;
    // Unregisters and destroys every target, the pipelines and the buffers (the device defers the
    // destruction); picks still waiting are dropped. Runs outside the graph-import phase: call
    // EditorUi::abandonFrame() first when the last UI frame wasn't rendered. Execute or reset every
    // graph holding its passes first.
    ~ViewportPanel();

    // Starts a UI frame for the panel: unregisters and destroys (deferred by the device) the colour
    // and id targets of every instance not drawn in the last frame; a pick it already copied still
    // completes. Call it once per frame after EditorUi::newFrame() and before the panels draw, so
    // outside the graph-import phase; the last frame's graph must have executed. Main thread.
    void newFrame();

    // Draws instance `instance`'s toolbar and image inside its window, (re)creates its target for
    // the image's size and reports the image to its ViewportState. Main thread, inside a UI frame
    // after newFrame(), so outside the graph-import phase. Precondition (asserted): `instance` is
    // "ith.viewport.1" to "ith.viewport.4"; otherwise nothing is drawn.
    void draw(std::string_view instance);

    // Applies the picks whose copies the GPU has finished by `opened` (see the header comment); adds
    // the scene passes of every instance drawn this frame whose scene changed, each on its target's
    // import from EditorUi::importUserTexture(), and the pick copy of every drawn instance with a
    // click; and gives the instances not drawn a zero image and no hover. `opened` is the device
    // frame beginFrame() returned, still open: its number decides which picks are read and stamps
    // the new ones, so a call outside a frame can't read a copy early. Call it once per UI frame,
    // after EditorUi::endFrame() and before EditorUi::addRenderPasses() with the same graph. The
    // passes read the panel's copies of the frame's draws. An instance counts as rendered, and its
    // pick as copied, once its passes are added: when the graph then fails to execute and the caller
    // goes on, call invalidate().
    void addRenderPasses(rg::RenderGraph& graph, const rhi::Frame& opened);

    // Forgets what every instance rendered, so the next addRenderPasses() renders them all, and drops
    // the picks waiting for their copies (whose frame may not have run).
    void invalidate() noexcept;

    // Whether a click's pick waits for its copy to finish, in any instance.
    [[nodiscard]] bool picking() const noexcept;

    // The user texture showing `instance`, or 0 when it has no target (not drawn yet, or no area).
    [[nodiscard]] UiTextureId texture(std::string_view instance) const;

    // The description `instance`'s colour target was created with, or std::nullopt without a target.
    // Its debugName stays valid until the target changes.
    [[nodiscard]] std::optional<rhi::TextureDesc> colorTarget(std::string_view instance) const;

    // The same for `instance`'s id target, which exists exactly when the colour target does.
    [[nodiscard]] std::optional<rhi::TextureDesc> idTarget(std::string_view instance) const;

    // The handle highlighted in the translate gizmo `instance` drew in its last draw() (None when
    // none was), or std::nullopt when it drew no gizmo.
    [[nodiscard]] std::optional<GizmoHandle> drawnGizmo(std::string_view instance) const;

    // The play overlay `instance` drew in its last draw(), or std::nullopt when it drew none (it is
    // not the play viewport, or no session runs).
    [[nodiscard]] std::optional<ViewportPlayOverlay> drawnPlayOverlay(std::string_view instance) const;

    // How many times addRenderPasses() added scene passes, over every instance, since creation.
    [[nodiscard]] std::uint64_t renders() const noexcept { return m_renders; }

    // The shape draws of the last render.
    [[nodiscard]] std::uint32_t lastShapeDraws() const noexcept { return m_lastShapeDraws; }

private:
    // One instance's target and redraw state (ViewportPanel.cpp).
    struct Instance;

    // Where a shape's indices are in the mesh buffer.
    struct MeshRange {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    ViewportPanel(EditorSession& session, EditorUi& ui, rhi::Device& device);

    void drawToolbar(std::string_view instance, const ViewportState& state);
    void resize(Instance& instance, std::uint32_t width, std::uint32_t height);
    void release(Instance& instance);
    // Imports the instance's id target into `graph`.
    static rg::Texture importIds(rg::RenderGraph& graph, Instance& instance);

    // What an instance's scene shows: the world it draws and whether it draws as game view.
    struct SceneView {
        World* world = nullptr;
        bool gameView = false;
    };

    // Adds the instance's passes and returns its id target's import; null when its colour target
    // can't be imported.
    rg::Texture addScenePasses(rg::RenderGraph& graph, Instance& instance, const ViewportState& state,
                               const SceneView& scene, rg::Buffer mesh);
    // Applies every pick whose copy the GPU has finished by device frame `open`.
    void readPicks(std::uint64_t open);

    EditorSession* m_session;
    EditorUi* m_ui;
    rhi::Device* m_device;
    rhi::PipelineHandle m_shapesPipeline;
    rhi::PipelineHandle m_gridPipeline;
    // Every shape's vertices, then their 16-bit indices from m_indexOffset, each offset to its
    // shape's first vertex.
    rhi::BufferHandle m_mesh;
    rhi::BufferDesc m_meshDesc;
    rg::ImportedState m_meshState;
    std::uint64_t m_indexOffset = 0;
    // By Shape value.
    std::array<MeshRange, 4> m_meshes{};
    // The picked texels, instance i's at i * rhi::kTextureCopyOffsetAlignment (Readback memory).
    rhi::BufferHandle m_picks;
    rhi::BufferDesc m_picksDesc;
    rg::ImportedState m_picksState;
    std::array<std::unique_ptr<Instance>, kMaxViewports> m_instances;
    std::uint64_t m_renders = 0;
    std::uint32_t m_lastShapeDraws = 0;
};

// Registers the "ith.viewport" panel (Viewport, docked in the centre, instances 1 to 4) drawing
// `panel`. The registry's draw function refers to `panel`: the shell must not draw the panel (no
// drawEditorShell() or DockLayout::draw()) after it is destroyed.
void registerViewport(PanelRegistry& panels, ViewportPanel& panel);

} // namespace ith
