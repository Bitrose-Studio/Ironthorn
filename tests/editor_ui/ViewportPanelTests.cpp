// The Viewport panel (ADR-0042 rules 8, 13 to 16 and 30), headless on the RHI's null device: it
// registers as ith.viewport and docks in the centre of the default layout; its colour target follows
// the image's size in pixels at any framebuffer scale, re-registered as a user texture; the scene is
// rendered only when the camera, the world, the selection, the size or the game-view flag changed;
// the image is reported in window pixels, with a zero size while it isn't drawn and no hover under a
// floating window; the toolbar runs commands; the Outliner's Add Shape places a shape the viewport
// then draws; a scale set by command or typed into Details re-renders it at its new size; and the id
// target and click picking (ADR-0042 rules 21 and 22): a click is copied in the next frame and
// applied kFramesInFlight frames later, a newer click replaces one being read, and hidden viewports
// and invalidate() treat picks as documented; and the translate gizmo (ADR-0042 rules 24 to 26):
// drawn only with the translate tool, a selection and outside game view, the hovered and the dragged
// handle highlighted, with no scene render for itself, and a drag through the UI as one undo entry.

#include "ith/editor_ui/ViewportPanel.h"

#include "ith/config/Config.h"
#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/AddShape.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/ViewportGizmo.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/DetailsPanel.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/OutlinerPanel.h"
#include "ith/editor_ui/ViewportShaders.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/foundation/Uuid.h"
#include "ith/math/Transform.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/Value.h"
#include "ith/shapes/ShapeMesh.h"
#include "ith/shapes/ShapeRenderer.h"

#include "AssertCapture.h"
#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(shapes)

using namespace ith;
using namespace ith::test::ui;

namespace {

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

constexpr std::string_view kInstance = "ith.viewport.1";

Event mouseMoved(float x, float y) {
    return Event{.type = EventType::MouseMoved, .x = x, .y = y};
}

Event press(MouseButton button, float x, float y) {
    return Event{.type = EventType::MouseButtonDown, .mouseButton = button, .clicks = 1, .x = x, .y = y};
}

Event release(MouseButton button, float x, float y) {
    return Event{.type = EventType::MouseButtonUp, .mouseButton = button, .x = x, .y = y};
}

Event keyDown(Key key) {
    return Event{.type = EventType::KeyDown, .key = key};
}

Event keyUp(Key key) {
    return Event{.type = EventType::KeyUp, .key = key};
}

// A session with the Viewport panel on the null device, drawn in a fixed window at (100, 50) in
// window coordinates; the viewport consumer sits under the UI's, as in the editor.
struct ViewportFixture {
    ViewportFixture() : session(types()), viewportInput(session), ui({.viewports = &viewportInput}, &viewportInput) {
        Expected<std::unique_ptr<ViewportPanel>> created = ViewportPanel::create(session, *ui.ui, *ui.device,
                                                                                 {.shapesVertex = kShaderCode,
                                                                                  .shapesPixel = kShaderCode,
                                                                                  .gridVertex = kShaderCode,
                                                                                  .gridPixel = kShaderCode});
        REQUIRE_MESSAGE(created.hasValue(), (created ? std::string() : created.error().message));
        panel = std::move(*created);
        registerViewport(session.panels(), *panel);
        ui.beforeUi = [this](rg::RenderGraph& graph, const rhi::Frame& frame) { panel->addRenderPasses(graph, frame); };
    }

    ViewportFixture(const ViewportFixture&) = delete;
    ViewportFixture& operator=(const ViewportFixture&) = delete;
    ViewportFixture(ViewportFixture&&) = delete;
    ViewportFixture& operator=(ViewportFixture&&) = delete;

    ~ViewportFixture() {
        ui.graph.reset();
        panel.reset();
    }

    // Draws the viewport window, `size` in window coordinates; `overlay` draws a floating window over
    // the middle of the image after it.
    void drawWindow() {
        panel->newFrame(); // as the editor does after EditorUi::newFrame()
        // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
        constexpr ImGuiWindowFlags kFixed =
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        ImGui::SetNextWindowPos(ImVec2(100.0f, 50.0f));
        ImGui::SetNextWindowSize(windowSize);
        if (ImGui::Begin("Viewport###test.viewport", nullptr, kFixed)) {
            session.panels().find("ith.viewport")->draw(kInstance);
        }
        ImGui::End();
        if (second) {
            ImGui::SetNextWindowPos(ImVec2(760.0f, 50.0f));
            ImGui::SetNextWindowSize(ImVec2(480.0f, 360.0f));
            if (ImGui::Begin("Viewport 2###test.viewport2", nullptr, kFixed)) {
                session.panels().find("ith.viewport")->draw("ith.viewport.2");
            }
            ImGui::End();
        }
        if (overlay) {
            ImGui::SetNextWindowPos(ImVec2(300.0f, 200.0f));
            ImGui::SetNextWindowSize(ImVec2(200.0f, 150.0f));
            ImGui::Begin("Floating###test.floating", nullptr, kFixed);
            ImGui::TextUnformatted("On top");
            ImGui::End();
        }
    }

    void frame() {
        ui.frame([this] { drawWindow(); });
    }

    // A frame in which the viewport isn't drawn.
    void hiddenFrame() {
        ui.frame([this] { panel->newFrame(); });
    }

    [[nodiscard]] ViewportState& state() { return *session.viewports().find(kInstance); }

    // How many ViewportShapes passes the last frame's graph had.
    [[nodiscard]] std::size_t shapePassesInLastFrame() const {
        std::size_t count = 0;
        for (std::size_t at = ui.lastGraph.find("'ViewportShapes'"); at != std::string::npos;
             at = ui.lastGraph.find("'ViewportShapes'", at + 1)) {
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool scenePassesInLastFrame() const {
        return ui.lastGraph.find("'ViewportShapes'") != std::string::npos;
    }

    EditorSession session;
    ViewportConsumer viewportInput;
    UiFixture ui;
    std::unique_ptr<ViewportPanel> panel;
    ImVec2 windowSize{640.0f, 420.0f};
    bool overlay = false;
    // Also draws ith.viewport.2 in a window to the right.
    bool second = false;
};

} // namespace

TEST_SUITE("editor_ui.viewport") {
    TEST_CASE("the panel registers as ith.viewport, docks centre, and the default layout shows instance 1") {
        UiFixture ui;
        Config config({{.name = "engine", .directory = "engine/config"}});
        EditorSession session(types(), {.config = &config});
        Expected<std::unique_ptr<ViewportPanel>> panel = ViewportPanel::create(session, *ui.ui, *ui.device,
                                                                               {.shapesVertex = kShaderCode,
                                                                                .shapesPixel = kShaderCode,
                                                                                .gridVertex = kShaderCode,
                                                                                .gridPixel = kShaderCode});
        REQUIRE(panel.hasValue());
        registerViewport(session.panels(), **panel);
        const PanelDesc* desc = session.panels().find("ith.viewport");
        REQUIRE(desc != nullptr);
        CHECK(desc->displayName == "Viewport");
        CHECK(desc->defaultDock == DockPlace::Center);
        CHECK(desc->maxInstances == kMaxViewports);
        CHECK(session.panels().instanceIds().front() == "ith.viewport.1");

        // Default.json's central stack holds the first viewport, and the dock space draws it.
        REQUIRE(session.layouts().restore().name == "Default");
        CHECK(isPanelOpen(session.layouts().current(), "ith.viewport.1"));
        bool central = false;
        for (const LayoutNode& node : session.layouts().current().main.nodes) {
            if (node.central) {
                central = node.tabs == std::vector<std::string>{"ith.viewport.1"};
            }
        }
        CHECK(central);
        std::vector<Error> warnings;
        const Keymap keymap = Keymap::load(config, session.commands(), warnings);
        EditorShellState shell;
        {
            DockLayout dock(session);
            ui.beforeUi = [&panel](rg::RenderGraph& graph, const rhi::Frame& frame) {
                (*panel)->addRenderPasses(graph, frame);
            };
            for (int frame = 0; frame < 4; ++frame) {
                ui.frame([&] {
                    (*panel)->newFrame();
                    drawEditorShell(session, keymap, shell, dock);
                });
            }
            CHECK((*panel)->texture("ith.viewport.1") != 0);
            CHECK((*panel)->texture("ith.viewport.2") == 0);
            CHECK(session.viewports().at(0).imageSize.x > 0.0f);
            // The dock tree settles over the first frames, resizing the panel; then it stays.
            const std::uint64_t renders = (*panel)->renders();
            CHECK(renders >= 1);
            for (int frame = 0; frame < 3; ++frame) {
                ui.frame([&] {
                    (*panel)->newFrame();
                    drawEditorShell(session, keymap, shell, dock);
                });
            }
            CHECK((*panel)->renders() == renders);
            ui.graph.reset();
        }
        panel->reset();
    }

    TEST_CASE("the colour target follows the image's size in pixels, and its user texture follows the target") {
        ViewportFixture fixture;
        fixture.frame();
        const std::optional<rhi::TextureDesc> first = fixture.panel->colorTarget(kInstance);
        REQUIRE(first.has_value());
        CHECK(first->format == rhi::Format::R8G8B8A8Srgb);
        CHECK(first->usage == (rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled));
        CHECK(first->debugName == "Viewport 1 colour");
        const Vec2 position = fixture.state().imagePosition;
        const Vec2 size = fixture.state().imageSize;
        // Inside the window, below the toolbar, the image fills the rest.
        CHECK(position.x > 100.0f);
        CHECK(position.y > 50.0f);
        CHECK(size.x > 600.0f);
        CHECK(size.x < 640.0f);
        CHECK(size.y > 300.0f);
        CHECK(first->width == static_cast<std::uint32_t>(std::lround(size.x)));
        CHECK(first->height == static_cast<std::uint32_t>(std::lround(size.y)));
        const UiTextureId firstTexture = fixture.panel->texture(kInstance);
        REQUIRE(firstTexture != 0);

        // The same size keeps the target.
        fixture.frame();
        CHECK(fixture.panel->texture(kInstance) == firstTexture);

        // A framebuffer scale of 2: the same window holds twice the pixels, and the image is reported
        // in window pixels. The old texture is unregistered: its graph import is null.
        fixture.ui.pixelSize = {2 * kWidth, 2 * kHeight};
        bool checked = false;
        fixture.ui.beforeUi = [&](rg::RenderGraph& graph, const rhi::Frame& frame) {
            fixture.panel->addRenderPasses(graph, frame);
            CHECK(fixture.ui.ui->importUserTexture(graph, firstTexture).isNull());
            CHECK_FALSE(fixture.ui.ui->importUserTexture(graph, fixture.panel->texture(kInstance)).isNull());
            checked = true;
        };
        fixture.frame();
        CHECK(checked);
        const std::optional<rhi::TextureDesc> doubled = fixture.panel->colorTarget(kInstance);
        REQUIRE(doubled.has_value());
        CHECK(fixture.panel->texture(kInstance) != firstTexture);
        CHECK(fixture.state().imagePosition.x == doctest::Approx(position.x * 2.0f));
        CHECK(fixture.state().imagePosition.y == doctest::Approx(position.y * 2.0f));
        CHECK(fixture.state().imageSize.x == doctest::Approx(size.x * 2.0f));
        CHECK(fixture.state().imageSize.y == doctest::Approx(size.y * 2.0f));
        CHECK(doubled->width == static_cast<std::uint32_t>(std::lround(size.x * 2.0f)));
        CHECK(doubled->height == static_cast<std::uint32_t>(std::lround(size.y * 2.0f)));
        fixture.ui.beforeUi = [&fixture](rg::RenderGraph& graph, const rhi::Frame& frame) {
            fixture.panel->addRenderPasses(graph, frame);
        };

        // A smaller window: a smaller target.
        fixture.ui.pixelSize = {kWidth, kHeight};
        fixture.windowSize = {320.0f, 240.0f};
        fixture.frame();
        const std::optional<rhi::TextureDesc> smaller = fixture.panel->colorTarget(kInstance);
        REQUIRE(smaller.has_value());
        CHECK(smaller->width < first->width);
        CHECK(smaller->height < first->height);
    }

    TEST_CASE("the reported rectangle at scale 2 is where window-pixel events find the image") {
        ViewportFixture fixture;
        fixture.ui.pixelSize = {2 * kWidth, 2 * kHeight};
        fixture.frame();
        const Vec2 centre = fixture.state().imagePosition + fixture.state().imageSize * 0.5f;
        fixture.ui.events({mouseMoved(centre.x, centre.y)});
        fixture.frame();
        CHECK(fixture.state().imageHovered);
        // A press in window pixels at the image's centre starts a fly, which the panel drew under.
        fixture.ui.events({press(MouseButton::Right, centre.x, centre.y)});
        CHECK(fixture.viewportInput.flying());
        fixture.ui.events({release(MouseButton::Right, centre.x, centre.y)});
        // Just outside the image's right edge, in pixels, is not the image.
        const Vec2 outside = fixture.state().imagePosition + Vec2{fixture.state().imageSize.x + 1.0f, 10.0f};
        CHECK_FALSE(fixture.viewportInput.viewportAt(outside.x, outside.y).has_value());
    }

    TEST_CASE("the scene renders only when the camera, the world, the selection, the size or game view changed") {
        ViewportFixture fixture;
        fixture.frame();
        CHECK(fixture.scenePassesInLastFrame());
        CHECK(fixture.ui.lastGraph.find("'ViewportGrid'") != std::string::npos);
        CHECK(fixture.panel->renders() == 1);
        CHECK(fixture.panel->lastShapeDraws() == 0);

        const auto idle = [&fixture] {
            const std::uint64_t renders = fixture.panel->renders();
            for (int frame = 0; frame < 10; ++frame) {
                fixture.frame();
                CHECK_FALSE(fixture.scenePassesInLastFrame());
            }
            CHECK(fixture.panel->renders() == renders);
        };
        // Exactly one render after `change`, then none.
        const auto rendersOnceAfter = [&](const auto& change) {
            const std::uint64_t renders = fixture.panel->renders();
            change();
            fixture.frame();
            CHECK(fixture.scenePassesInLastFrame());
            CHECK(fixture.panel->renders() == renders + 1);
            idle();
        };
        idle();

        // The camera.
        rendersOnceAfter([&] { fixture.state().camera.position.x += 1.0f; });
        // The world: a cube in front of the camera (the command also selects it).
        rendersOnceAfter([&] {
            REQUIRE(fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera))
                        .hasValue());
        });
        CHECK(fixture.panel->lastShapeDraws() == 1);
        // The selection alone.
        rendersOnceAfter([&] { REQUIRE(fixture.session.execute("ith.edit.select_none").hasValue()); });
        // The size.
        rendersOnceAfter([&] { fixture.windowSize = {500.0f, 400.0f}; });
        // Game view: no grid.
        rendersOnceAfter([&] {
            serialization::Value args = serialization::Value::object();
            args.set("viewport", serialization::Value::string(std::string(kInstance)));
            REQUIRE(fixture.session.execute("ith.viewport.game_view", args).hasValue());
        });
        // That frame was the render; the next one in game view again draws without the grid.
        fixture.state().camera.position.z += 1.0f;
        fixture.frame();
        CHECK(fixture.scenePassesInLastFrame());
        CHECK(fixture.ui.lastGraph.find("'ViewportGrid'") == std::string::npos);
    }

    TEST_CASE("disabled shapes are not drawn, and transforms are propagated before the draws") {
        ViewportFixture fixture;
        const Expected<serialization::Value> created =
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Sphere, fixture.state().camera));
        REQUIRE(created.hasValue());
        REQUIRE(fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Plane, fixture.state().camera))
                    .hasValue());
        fixture.frame();
        CHECK(fixture.panel->lastShapeDraws() == 2);
        // The sphere's world matrix was propagated from its local transform, resting on the ground.
        World& world = fixture.session.document().mutableWorld();
        const Entity sphere = world.findByStableId(*Uuid::parse(created->find("id")->asString()));
        REQUIRE_FALSE(sphere.isNull());
        CHECK(world.get<const WorldTransform>(sphere)->matrix[3].y == doctest::Approx(0.5f));

        serialization::Value args = serialization::Value::object();
        args.set("id", *created->find("id"));
        args.set("enabled", serialization::Value::boolean(false));
        REQUIRE(fixture.session.execute("ith.entity.set_enabled", args).hasValue());
        fixture.frame();
        CHECK(fixture.panel->lastShapeDraws() == 1);
    }

    TEST_CASE("an image without area has no target and adds no passes") {
        ViewportFixture fixture;
        fixture.frame();
        REQUIRE(fixture.panel->texture(kInstance) != 0);
        // A window too short for anything below the toolbar.
        fixture.windowSize = {640.0f, 20.0f};
        fixture.frame();
        CHECK(fixture.panel->texture(kInstance) == 0);
        CHECK_FALSE(fixture.panel->colorTarget(kInstance).has_value());
        CHECK(fixture.state().imageSize.x == 0.0f);
        CHECK_FALSE(fixture.scenePassesInLastFrame());
        fixture.state().camera.position.x += 1.0f;
        fixture.frame();
        CHECK_FALSE(fixture.scenePassesInLastFrame());
    }

    TEST_CASE("a viewport not drawn reports no image and releases its target; shown again it renders once") {
        ViewportFixture fixture;
        fixture.ui.events({mouseMoved(400.0f, 300.0f)});
        fixture.frame();
        fixture.frame();
        REQUIRE(fixture.state().imageSize.x > 0.0f);
        REQUIRE(fixture.state().imageHovered);
        const UiTextureId shown = fixture.panel->texture(kInstance);
        REQUIRE(shown != 0);
        const std::uint64_t renders = fixture.panel->renders();

        // The frame it isn't drawn: no image takes input; the target is still there.
        fixture.hiddenFrame();
        CHECK(fixture.state().imageSize.x == 0.0f);
        CHECK(fixture.state().imageSize.y == 0.0f);
        CHECK_FALSE(fixture.state().imageHovered);
        CHECK_FALSE(fixture.viewportInput.viewportAt(400.0f, 300.0f).has_value());
        CHECK(fixture.panel->texture(kInstance) == shown);

        // The next frame releases it: the user texture is unregistered (its import is null) and the
        // target is gone. A camera move meanwhile renders nothing.
        fixture.state().camera.position.x += 1.0f;
        bool checked = false;
        fixture.ui.beforeUi = [&](rg::RenderGraph& graph, const rhi::Frame& frame) {
            fixture.panel->addRenderPasses(graph, frame);
            CHECK(fixture.ui.ui->importUserTexture(graph, shown).isNull());
            checked = true;
        };
        fixture.hiddenFrame();
        CHECK(checked);
        CHECK(fixture.panel->texture(kInstance) == 0);
        CHECK_FALSE(fixture.panel->colorTarget(kInstance).has_value());
        CHECK_FALSE(fixture.scenePassesInLastFrame());
        CHECK(fixture.panel->renders() == renders);
        fixture.ui.beforeUi = [&fixture](rg::RenderGraph& graph, const rhi::Frame& frame) {
            fixture.panel->addRenderPasses(graph, frame);
        };

        // Shown again: a new target, rendered once.
        fixture.frame();
        CHECK(fixture.panel->texture(kInstance) != 0);
        CHECK(fixture.panel->colorTarget(kInstance).has_value());
        CHECK(fixture.scenePassesInLastFrame());
        CHECK(fixture.panel->renders() == renders + 1);
        fixture.frame();
        CHECK_FALSE(fixture.scenePassesInLastFrame());
        CHECK(fixture.panel->renders() == renders + 1);
    }

    TEST_CASE("under a floating window the image is not hovered, and the viewport takes no press there") {
        ViewportFixture fixture;
        fixture.ui.events({mouseMoved(400.0f, 275.0f)});
        fixture.frame();
        fixture.frame();
        CHECK(fixture.state().imageHovered);

        fixture.overlay = true;
        fixture.frame();
        fixture.frame();
        CHECK_FALSE(fixture.state().imageHovered);
        fixture.ui.events({press(MouseButton::Right, 400.0f, 275.0f)});
        CHECK_FALSE(fixture.viewportInput.holdsDrag());
        fixture.frame();
        fixture.ui.events({release(MouseButton::Right, 400.0f, 275.0f)});
        fixture.frame();

        // Off the floating window, the image takes the press again.
        fixture.ui.events({mouseMoved(150.0f, 300.0f)});
        fixture.frame();
        CHECK(fixture.state().imageHovered);
        fixture.ui.events({press(MouseButton::Middle, 150.0f, 300.0f)});
        CHECK(fixture.viewportInput.holdsDrag());
        // During the drag ImGui is told the mouse is gone: nothing is hovered.
        fixture.frame();
        fixture.frame();
        CHECK_FALSE(fixture.state().imageHovered);
        fixture.ui.events({release(MouseButton::Middle, 150.0f, 300.0f)});
        // When the drag ends ImGui gets the mouse back, so the image is hovered without a move.
        fixture.frame();
        fixture.frame();
        CHECK(fixture.state().imageHovered);
    }

    TEST_CASE("the toolbar runs the viewport commands") {
        ViewportFixture fixture;
        fixture.frame();
        // The toolbar's first row: Translate, Rotate, Scale radio buttons, from the window's content
        // origin. Clicking where Rotate is selects the rotate tool through its command.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float y = 50.0f + style.WindowPadding.y + ImGui::GetFrameHeight() * 0.5f + ImGui::GetFrameHeight();
        CHECK(fixture.session.viewports().tool() == ViewportTool::Translate);
        // Find Rotate by walking right along the row until the tool changes.
        bool rotated = false;
        for (float x = 100.0f + style.WindowPadding.x; x < 400.0f && !rotated; x += 8.0f) {
            fixture.ui.events({mouseMoved(x, y)});
            fixture.frame();
            fixture.ui.events({press(MouseButton::Left, x, y)});
            fixture.frame();
            fixture.ui.events({release(MouseButton::Left, x, y)});
            fixture.frame();
            rotated = fixture.session.viewports().tool() == ViewportTool::Rotate;
        }
        CHECK(rotated);
    }

    TEST_CASE("the Outliner's Add Shape creates a cube in front of the viewport, which then draws it") {
        ViewportFixture fixture;
        OutlinerPanel outliner(fixture.session);
        registerOutliner(fixture.session, outliner);
        const auto draw = [&] {
            fixture.drawWindow();
            ImGui::SetNextWindowPos(ImVec2(800.0f, 50.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 500.0f));
            if (ImGui::Begin("Outliner###test.outliner")) {
                fixture.session.panels().find("ith.outliner")->draw("ith.outliner");
            }
            ImGui::End();
        };
        fixture.ui.frame(draw);
        const std::optional<UiRect> empty = outliner.emptyRect();
        REQUIRE(empty.has_value());
        const float x = empty->centerX();
        const float y = empty->centerY();
        // A right click opens the context menu; Down highlights Add Shape, Right opens its submenu on
        // Cube, Enter chooses it.
        fixture.ui.events({mouseMoved(x, y)});
        fixture.ui.frame(draw);
        fixture.ui.events({press(MouseButton::Right, x, y)});
        fixture.ui.frame(draw);
        fixture.ui.events({release(MouseButton::Right, x, y)});
        fixture.ui.frame(draw);
        fixture.ui.frame(draw);
        REQUIRE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        for (const Key key : {Key::Down, Key::Right, Key::Enter}) {
            fixture.ui.events({keyDown(key)});
            fixture.ui.frame(draw);
            fixture.ui.events({keyUp(key)});
            fixture.ui.frame(draw);
        }
        REQUIRE(fixture.session.document().selection().primary().has_value());
        World& world = fixture.session.document().mutableWorld();
        const Entity cube = world.findByStableId(*fixture.session.document().selection().primary());
        REQUIRE_FALSE(cube.isNull());
        REQUIRE(world.get<const ShapeRenderer>(cube) != nullptr);
        CHECK(world.get<const ShapeRenderer>(cube)->shape == Shape::Cube);
        CHECK(world.get<const LocalTransform>(cube)->value.translation.y == doctest::Approx(0.5f));
        CHECK(fixture.panel->lastShapeDraws() == 1);
    }

    TEST_CASE("two viewports drawn in one frame each get their own target, import and passes") {
        ViewportFixture fixture;
        fixture.second = true;
        fixture.frame();
        const UiTextureId first = fixture.panel->texture("ith.viewport.1");
        const UiTextureId second = fixture.panel->texture("ith.viewport.2");
        REQUIRE(first != 0);
        REQUIRE(second != 0);
        CHECK(first != second);
        // Both rendered in the one graph, which compiled; one shapes pass each.
        CHECK(fixture.shapePassesInLastFrame() == 2);
        CHECK(fixture.panel->renders() == 2);

        // A camera change on the second re-renders only the second.
        fixture.session.viewports().at(1).camera.position.y += 1.0f;
        fixture.frame();
        CHECK(fixture.shapePassesInLastFrame() == 1);
        CHECK(fixture.ui.lastGraph.find("'Viewport 2 colour'") != std::string::npos);
        CHECK(fixture.ui.lastGraph.find("'Viewport 1 colour': RenderTarget") == std::string::npos);
        CHECK(fixture.panel->renders() == 3);

        // A world change re-renders both, with the transforms propagated before both draw lists.
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        fixture.frame();
        CHECK(fixture.shapePassesInLastFrame() == 2);
        CHECK(fixture.panel->lastShapeDraws() == 1);

        // Hiding the second releases only its target, a frame later.
        fixture.second = false;
        fixture.frame();
        fixture.frame();
        CHECK(fixture.panel->texture("ith.viewport.2") == 0);
        CHECK(fixture.panel->texture("ith.viewport.1") == first);
    }

    TEST_CASE("the redraw key ignores the speed step and the pivot, and the selection in game view") {
        ViewportFixture fixture;
        fixture.frame();
        const std::uint64_t renders = fixture.panel->renders();
        fixture.state().camera.speedStep = 7;
        fixture.state().camera.pivot = Vec3{4.0f, 0.0f, 4.0f};
        fixture.frame();
        CHECK(fixture.panel->renders() == renders);

        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        serialization::Value args = serialization::Value::object();
        args.set("viewport", serialization::Value::string(std::string(kInstance)));
        REQUIRE(fixture.session.execute("ith.viewport.game_view", args).hasValue());
        fixture.frame();
        const std::uint64_t inGameView = fixture.panel->renders();
        REQUIRE(fixture.session.execute("ith.edit.select_none").hasValue());
        fixture.frame();
        CHECK(fixture.panel->renders() == inGameView);
        // Out of game view the tint shows again, so the changed selection renders.
        REQUIRE(fixture.session.execute("ith.viewport.game_view", args).hasValue());
        fixture.frame();
        CHECK(fixture.panel->renders() == inGameView + 1);
    }

    TEST_CASE("a mirrored shape (a scale of -1) is drawn") {
        ViewportFixture fixture;
        serialization::Value args = addShapeArgs(Shape::Cube, fixture.state().camera);
        serialization::Value scale = serialization::Value::object();
        scale.set("x", serialization::Value::real(-1.0));
        scale.set("y", serialization::Value::real(1.0));
        scale.set("z", serialization::Value::real(1.0));
        args.find("components")->find("ith::LocalTransform")->find("value")->set("scale", std::move(scale));
        const Expected<serialization::Value> created = fixture.session.execute("ith.entity.create", args);
        REQUIRE(created.hasValue());
        fixture.frame();
        CHECK(fixture.panel->lastShapeDraws() == 1);
        World& world = fixture.session.document().mutableWorld();
        const Entity cube = world.findByStableId(*fixture.session.document().selection().primary());
        CHECK(world.get<const WorldTransform>(cube)->matrix[0].x == doctest::Approx(-1.0f));
    }

    TEST_CASE("a scale set by command or typed into Details re-renders the shape at its new size") {
        ViewportFixture fixture;
        DetailsPanel details(fixture.session);
        registerDetails(fixture.session.panels(), details);
        const auto draw = [&] {
            fixture.drawWindow();
            ImGui::SetNextWindowPos(ImVec2(760.0f, 50.0f));
            ImGui::SetNextWindowSize(ImVec2(500.0f, 600.0f));
            if (ImGui::Begin("Details###test.details")) {
                fixture.session.panels().find("ith.details")->draw("ith.details");
            }
            ImGui::End();
        };
        const auto frames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                fixture.ui.frame(draw);
            }
        };
        const auto step = [&](const Event& event) {
            fixture.ui.events({event});
            frames(1);
        };
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        frames(3);
        World& world = fixture.session.document().mutableWorld();
        const Uuid id = *fixture.session.document().selection().primary();
        const Entity cube = world.findByStableId(id);
        const auto worldScaleX = [&] { return glm::length(Vec3(world.get<const WorldTransform>(cube)->matrix[0])); };
        CHECK(worldScaleX() == doctest::Approx(1.0f));

        // By command.
        std::uint64_t renders = fixture.panel->renders();
        REQUIRE(
            fixture.session
                .execute("ith.entity.set_property",
                         *serialization::parseJson(R"({"ids": [")" + id.toString() +
                                                   R"("], "path": "ith::LocalTransform.value.scale.x", "value": 2})"))
                .hasValue());
        frames(1);
        CHECK(fixture.panel->renders() == renders + 1);
        CHECK(worldScaleX() == doctest::Approx(2.0f));

        // Typed into the Details field, as a user does: a click on X, "3", Enter.
        const std::optional<UiRect> scale = details.valueRect("ith::LocalTransform.value.scale");
        REQUIRE(scale.has_value());
        const float x = scale->x + scale->width / 6.0f;
        const float y = scale->centerY();
        renders = fixture.panel->renders();
        step(mouseMoved(x, y));
        step(press(MouseButton::Left, x, y));
        step(release(MouseButton::Left, x, y));
        frames(1);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        step({.type = EventType::TextInput, .text = "3"});
        step(keyDown(Key::Enter));
        step(keyUp(Key::Enter));
        frames(2);
        CHECK(world.get<const LocalTransform>(cube)->value.scale.x == 3.0f);
        CHECK(fixture.panel->renders() > renders);
        CHECK(worldScaleX() == doctest::Approx(3.0f));
    }

    TEST_CASE("an image larger than the largest texture gets a clamped target and one warning") {
        ViewportFixture fixture;
        LogRingBuffer log(64);
        REQUIRE(addLogSink(log.sink()).hasValue());
        // A framebuffer scale of 40: the image is wider than the null device's 16384-pixel limit.
        fixture.ui.pixelSize = {40 * kWidth, 40 * kHeight};
        for (int frame = 0; frame < 3; ++frame) {
            fixture.frame();
        }
        removeLogSink(log.sink());
        const std::uint32_t limit = fixture.ui.device->capabilities().limits.maxTextureDimension2D;
        const std::optional<rhi::TextureDesc> target = fixture.panel->colorTarget(kInstance);
        REQUIRE(target.has_value());
        CHECK(target->width == limit);
        CHECK(target->height <= limit);
        std::vector<LogEntry> entries;
        static_cast<void>(log.read(0, entries));
        std::size_t warnings = 0;
        for (const LogEntry& entry : entries) {
            if (entry.level == LogLevel::Warning && std::string_view(entry.category->name()) == "Viewport") {
                ++warnings;
            }
        }
        CHECK(warnings == 1);
    }

    TEST_CASE("after a frame that wasn't rendered, abandonFrame() lets the panel go without asserting") {
        ViewportFixture fixture;
        fixture.frame();
        const UiTextureId texture = fixture.panel->texture(kInstance);
        REQUIRE(texture != 0);
        // A frame whose rendering was skipped (a minimized window): endFrame() without the passes,
        // which leaves the UI in its graph-import phase.
        fixture.ui.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}});
        fixture.drawWindow();
        fixture.ui.ui->endFrame();
        {
            test::AssertCapture capture;
            fixture.ui.ui->abandonFrame();
            fixture.ui.graph.reset();
            fixture.panel.reset();
            CHECK(capture.count() == 0);
        }
        // The registration is gone: the next frame's import of its id is null.
        bool checked = false;
        fixture.ui.beforeUi = [&](rg::RenderGraph& graph, const rhi::Frame& /*frame*/) {
            CHECK(fixture.ui.ui->importUserTexture(graph, texture).isNull());
            checked = true;
        };
        fixture.ui.frame();
        CHECK(checked);
    }

    TEST_CASE("the id target is R32G32Uint, RenderTarget | CopySrc, the colour target's size, and goes with it") {
        ViewportFixture fixture;
        fixture.frame();
        const std::optional<rhi::TextureDesc> color = fixture.panel->colorTarget(kInstance);
        const std::optional<rhi::TextureDesc> ids = fixture.panel->idTarget(kInstance);
        REQUIRE(color.has_value());
        REQUIRE(ids.has_value());
        CHECK(ids->format == rhi::Format::R32G32Uint);
        CHECK(ids->usage == (rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc));
        CHECK(ids->width == color->width);
        CHECK(ids->height == color->height);
        CHECK(ids->debugName == "Viewport 1 ids");
        // The shapes pass writes it; nothing reads it while nobody clicks.
        CHECK(fixture.ui.lastGraph.find("'Viewport 1 ids'") != std::string::npos);
        CHECK(fixture.ui.lastGraph.find("'ViewportPick'") == std::string::npos);
        fixture.windowSize = {320.0f, 240.0f};
        fixture.frame();
        REQUIRE(fixture.panel->idTarget(kInstance).has_value());
        CHECK(fixture.panel->idTarget(kInstance)->width == fixture.panel->colorTarget(kInstance)->width);
        fixture.hiddenFrame();
        fixture.hiddenFrame();
        CHECK_FALSE(fixture.panel->colorTarget(kInstance).has_value());
        CHECK_FALSE(fixture.panel->idTarget(kInstance).has_value());
    }

    // The null device copies nothing, so every pick reads the empty texel: empty space. What a hit
    // selects is covered by editor_core's applyPick tests and the GPU test.
    TEST_CASE("a click is copied in the next frame and applied kFramesInFlight frames later, never earlier") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        REQUIRE(fixture.session.execute("ith.edit.select_all").hasValue());
        const auto selected = [&fixture] { return !fixture.session.document().selection().empty(); };
        fixture.frame();
        const Vec2 centre = fixture.state().imagePosition + fixture.state().imageSize * 0.5f;
        fixture.ui.events({mouseMoved(centre.x, centre.y)});
        fixture.frame();
        REQUIRE(fixture.state().imageHovered);

        fixture.ui.events(
            {press(MouseButton::Left, centre.x, centre.y), release(MouseButton::Left, centre.x, centre.y)});
        REQUIRE(fixture.state().click.has_value());
        CHECK_FALSE(fixture.panel->picking());
        fixture.frame(); // copies the texel
        CHECK_FALSE(fixture.state().click.has_value());
        CHECK(fixture.panel->picking());
        CHECK(fixture.ui.lastGraph.find("'ViewportPick'") != std::string::npos);
        CHECK(fixture.ui.lastGraph.find("'Viewport picks'") != std::string::npos);
        for (std::uint32_t frame = 1; frame < rhi::kFramesInFlight; ++frame) {
            fixture.frame(); // the copy's frame may still run on the GPU
            CHECK(fixture.panel->picking());
            CHECK(selected());
        }
        const std::uint64_t renders = fixture.panel->renders();
        fixture.frame(); // read: empty space, and a plain click clears the selection
        CHECK_FALSE(fixture.panel->picking());
        CHECK_FALSE(selected());
        CHECK(fixture.panel->renders() == renders + 1); // the same frame drew the cleared tint
        CHECK(fixture.ui.lastGraph.find("'ViewportPick'") == std::string::npos);
    }

    TEST_CASE("Ctrl and Shift on empty space keep the selection; a newer click replaces one being read") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        REQUIRE(fixture.session.execute("ith.edit.select_all").hasValue());
        const auto selected = [&fixture] { return !fixture.session.document().selection().empty(); };
        const auto frames = [&fixture](std::uint32_t count) {
            for (std::uint32_t frame = 0; frame < count; ++frame) {
                fixture.frame();
            }
        };
        fixture.frame();
        for (const PickMode mode : {PickMode::Toggle, PickMode::Add}) {
            fixture.state().click = ViewportClick{.position = fixture.state().imageSize * 0.5f, .mode = mode};
            frames(1 + rhi::kFramesInFlight);
            CHECK_FALSE(fixture.panel->picking());
            CHECK(selected());
        }

        // A plain click is copied, then a Ctrl click replaces it before it is read: only the Ctrl
        // click is applied, after its own frame, and empty space with Ctrl keeps the selection.
        fixture.state().click = ViewportClick{.position = {1.0f, 1.0f}, .mode = PickMode::Replace};
        fixture.frame();
        fixture.state().click = ViewportClick{.position = {2.0f, 2.0f}, .mode = PickMode::Toggle};
        frames(1 + rhi::kFramesInFlight);
        CHECK_FALSE(fixture.panel->picking());
        CHECK(selected());
    }

    TEST_CASE("a copied pick completes after its viewport is hidden; invalidate() and a hidden viewport drop picks") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        const auto selectAll = [&fixture] { REQUIRE(fixture.session.execute("ith.edit.select_all").hasValue()); };
        const auto selected = [&fixture] { return !fixture.session.document().selection().empty(); };
        const ViewportClick plain{.position = {10.0f, 10.0f}, .mode = PickMode::Replace};
        selectAll();
        fixture.frame();

        // Copied, then hidden: the targets go, the pick still reads its texel and clears.
        fixture.state().click = plain;
        fixture.frame();
        REQUIRE(fixture.panel->picking());
        for (std::uint32_t frame = 0; frame < rhi::kFramesInFlight; ++frame) {
            fixture.hiddenFrame();
        }
        CHECK_FALSE(fixture.panel->colorTarget(kInstance).has_value());
        CHECK_FALSE(fixture.panel->picking());
        CHECK_FALSE(selected());

        // A click on a viewport not drawn (it has no picture to pick from) is dropped.
        selectAll();
        fixture.state().click = plain;
        fixture.hiddenFrame();
        CHECK_FALSE(fixture.state().click.has_value());
        CHECK_FALSE(fixture.panel->picking());

        // invalidate(), after a graph that failed to execute, drops the pick: its copy may not have run.
        fixture.frame();
        fixture.state().click = plain;
        fixture.frame();
        REQUIRE(fixture.panel->picking());
        fixture.panel->invalidate();
        CHECK_FALSE(fixture.panel->picking());
        for (std::uint32_t frame = 0; frame < 1 + rhi::kFramesInFlight; ++frame) {
            fixture.frame();
        }
        CHECK(selected());
    }

    TEST_CASE("an RMB click and an LMB drag over the image don't pick") {
        ViewportFixture fixture;
        fixture.frame();
        const Vec2 centre = fixture.state().imagePosition + fixture.state().imageSize * 0.5f;
        fixture.ui.events({mouseMoved(centre.x, centre.y)});
        fixture.frame();
        fixture.ui.events(
            {press(MouseButton::Right, centre.x, centre.y), release(MouseButton::Right, centre.x, centre.y)});
        fixture.ui.events({press(MouseButton::Left, centre.x, centre.y), mouseMoved(centre.x + 30.0f, centre.y),
                           release(MouseButton::Left, centre.x + 30.0f, centre.y)});
        CHECK_FALSE(fixture.state().click.has_value());
        fixture.frame();
        CHECK_FALSE(fixture.panel->picking());
    }

    TEST_CASE("UI frames without a device frame don't count: the pick completes kFramesInFlight device frames on") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        REQUIRE(fixture.session.execute("ith.edit.select_all").hasValue());
        const auto selected = [&fixture] { return !fixture.session.document().selection().empty(); };
        fixture.frame();
        fixture.state().click = ViewportClick{.position = {10.0f, 10.0f}, .mode = PickMode::Replace};
        fixture.frame(); // copies the texel
        REQUIRE(fixture.panel->picking());
        // Minimized, or a frame whose render was skipped: the UI frame runs, no device frame begins,
        // addRenderPasses() isn't called. The pick keeps waiting (the editor asks for frames while
        // picking() is true).
        for (int skipped = 0; skipped < 5; ++skipped) {
            fixture.ui.ui->newFrame({.size = {kWidth, kHeight}, .pixelSize = {kWidth, kHeight}});
            fixture.drawWindow();
            fixture.ui.ui->endFrame();
            fixture.ui.ui->abandonFrame();
            CHECK(fixture.panel->picking());
            CHECK(selected());
        }
        for (std::uint32_t frame = 1; frame < rhi::kFramesInFlight; ++frame) {
            fixture.frame();
            CHECK(fixture.panel->picking());
        }
        fixture.frame();
        CHECK_FALSE(fixture.panel->picking());
        CHECK_FALSE(selected());
    }

    TEST_CASE("the gizmo is drawn with the translate tool and a selection, and highlights the hovered handle") {
        ViewportFixture fixture;
        fixture.frame();
        CHECK_FALSE(fixture.panel->drawnGizmo(kInstance).has_value()); // nothing selected
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        fixture.frame();
        CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::None); // drawn, nothing under the pointer

        // The pointer on the Y arrow, hit-tested as the viewport consumer does.
        const std::optional<ViewportGizmo> gizmo =
            viewportGizmo(fixture.session, fixture.state(), fixture.ui.ui->displayScale());
        REQUIRE(gizmo.has_value());
        const Vec2 arrow = fixture.state().imagePosition + 0.5f * (gizmo->layout.center + gizmo->layout.axisEnds[1]);
        fixture.ui.events({mouseMoved(arrow.x, arrow.y)});
        fixture.frame();
        fixture.frame();
        CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::Y);
        // The gizmo is UI: drawing and highlighting it re-render nothing.
        CHECK_FALSE(fixture.scenePassesInLastFrame());

        SUBCASE("not with the rotate or scale tool") {
            REQUIRE(fixture.session.execute("ith.viewport.rotate_tool").hasValue());
            fixture.frame();
            CHECK_FALSE(fixture.panel->drawnGizmo(kInstance).has_value());
            REQUIRE(fixture.session.execute("ith.viewport.scale_tool").hasValue());
            fixture.frame();
            CHECK_FALSE(fixture.panel->drawnGizmo(kInstance).has_value());
        }
        SUBCASE("not in game view") {
            REQUIRE(fixture.session.execute("ith.viewport.game_view").hasValue());
            fixture.frame();
            CHECK_FALSE(fixture.panel->drawnGizmo(kInstance).has_value());
        }
        SUBCASE("not without a selection") {
            REQUIRE(fixture.session.execute("ith.edit.select_none").hasValue());
            fixture.frame();
            CHECK_FALSE(fixture.panel->drawnGizmo(kInstance).has_value());
        }
        SUBCASE("not under a floating window, which keeps its clicks") {
            fixture.overlay = true;
            fixture.ui.events({mouseMoved(400.0f, 275.0f)});
            fixture.frame();
            fixture.frame();
            CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::None);
        }
    }

    TEST_CASE("a drag on a gizmo handle through the UI moves the shape in one undo entry, highlighted while held") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        const Uuid id = fixture.session.document().selection().entities().front();
        const auto translation = [&fixture, id] {
            const World& world = fixture.session.document().world();
            return world.get<const LocalTransform>(world.findByStableId(id))->value.translation;
        };
        fixture.frame();
        const Vec3 before = translation();
        const std::size_t entries = fixture.session.document().history().cursor();
        const std::optional<ViewportGizmo> gizmo =
            viewportGizmo(fixture.session, fixture.state(), fixture.ui.ui->displayScale());
        REQUIRE(gizmo.has_value());
        const Vec2 arrow =
            fixture.state().imagePosition + 0.5f * (gizmo->layout.center + gizmo->layout.axisEnds[1]); // Y
        fixture.ui.events({mouseMoved(arrow.x, arrow.y)});
        fixture.frame();

        fixture.ui.events({press(MouseButton::Left, arrow.x, arrow.y)});
        REQUIRE(fixture.viewportInput.gesture() == ViewportGesture::Gizmo);
        fixture.ui.events({mouseMoved(arrow.x, arrow.y - 40.0f)});
        static_cast<void>(fixture.viewportInput.update(1.0f / 60.0f));
        const std::uint64_t renders = fixture.panel->renders();
        fixture.frame();
        CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::Y);
        CHECK(fixture.panel->renders() == renders + 1); // the preview changed a value: the scene redraws
        const Vec3 dragged = translation();
        CHECK(dragged.y > before.y + 0.05f);
        CHECK(dragged.x == doctest::Approx(before.x));
        CHECK(dragged.z == doctest::Approx(before.z));

        fixture.ui.events({release(MouseButton::Left, arrow.x, arrow.y - 40.0f)});
        fixture.frame();
        CHECK_FALSE(fixture.viewportInput.holdsDrag());
        CHECK_FALSE(fixture.state().click.has_value());
        CHECK(fixture.session.document().history().cursor() == entries + 1);
        CHECK(fixture.session.document().history().entries().back().name == "Translate");
        REQUIRE(fixture.session.execute("ith.edit.undo").hasValue());
        CHECK(translation() == before);
    }

    TEST_CASE("no stale highlight during an orbit or after Esc, and a waiting pick is dropped when a drag begins") {
        ViewportFixture fixture;
        REQUIRE(
            fixture.session.execute("ith.entity.create", addShapeArgs(Shape::Cube, fixture.state().camera)).hasValue());
        const std::vector<Uuid> selected = fixture.session.document().selection().entities();
        fixture.frame();
        const std::optional<ViewportGizmo> gizmo =
            viewportGizmo(fixture.session, fixture.state(), fixture.ui.ui->displayScale());
        REQUIRE(gizmo.has_value());
        const Vec2 arrow =
            fixture.state().imagePosition + 0.5f * (gizmo->layout.center + gizmo->layout.axisEnds[1]); // Y
        const Vec2 corner = fixture.state().imagePosition + Vec2{10.0f, 10.0f};

        SUBCASE("an orbit that passes over a handle highlights nothing") {
            fixture.ui.events({mouseMoved(corner.x, corner.y)});
            fixture.frame();
            Event orbit = press(MouseButton::Left, corner.x, corner.y);
            orbit.modifiers = KeyModifiers::Alt;
            fixture.ui.events({orbit});
            REQUIRE(fixture.viewportInput.gesture() == ViewportGesture::Orbit);
            fixture.ui.events({mouseMoved(arrow.x, arrow.y)});
            fixture.frame();
            fixture.frame();
            CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::None);
            fixture.ui.events({release(MouseButton::Left, arrow.x, arrow.y)});
        }
        SUBCASE("after Esc the dead drag, still held, highlights nothing") {
            fixture.ui.events({mouseMoved(arrow.x, arrow.y)});
            fixture.frame();
            fixture.ui.events({press(MouseButton::Left, arrow.x, arrow.y), mouseMoved(arrow.x, arrow.y - 30.0f)});
            static_cast<void>(fixture.viewportInput.update(1.0f / 60.0f));
            fixture.frame();
            CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::Y);
            fixture.session.document().transactions().cancelOpen(); // what Esc does through the keymap
            static_cast<void>(fixture.viewportInput.update(1.0f / 60.0f));
            fixture.frame();
            CHECK(fixture.viewportInput.holdsDrag());
            CHECK(fixture.panel->drawnGizmo(kInstance) == GizmoHandle::None);
            fixture.ui.events({release(MouseButton::Left, arrow.x, arrow.y - 30.0f)});
        }
        SUBCASE("a pick copied before the drag began is dropped, not refused") {
            fixture.state().click = ViewportClick{.position = {4.0f, 4.0f}, .mode = PickMode::Replace};
            fixture.frame(); // copies the texel
            REQUIRE(fixture.panel->picking());
            fixture.ui.events({mouseMoved(arrow.x, arrow.y)});
            fixture.frame(); // hovered; the pick waits for its copy's frame
            REQUIRE(fixture.panel->picking());
            fixture.ui.events({press(MouseButton::Left, arrow.x, arrow.y)});
            REQUIRE(fixture.viewportInput.gesture() == ViewportGesture::Gizmo);
            for (std::uint32_t frame = 0; frame < rhi::kFramesInFlight; ++frame) {
                fixture.frame();
            }
            CHECK_FALSE(fixture.panel->picking());
            CHECK(fixture.session.document().selection().entities() == selected); // empty space didn't clear
            fixture.ui.events({release(MouseButton::Left, arrow.x, arrow.y)});
        }
        CHECK_FALSE(fixture.viewportInput.holdsDrag());
    }
}
