// Play In Editor in the UI (ADR-0053 decisions 4, 5, 16, 19 and 21, docs/specs/play-in-editor.md
// 3.2, 3.7, 3.8, 3.12 and 3.13), headless on the RHI's null device: the main toolbar's controls in
// each state and their commands, the status bar's chip, the play viewport's overlay, the play
// viewport drawing the game world while the others draw the editor world and render Keep Simulation
// Changes at once, picks dropped across Stop, the selected viewport opened when its panel is closed,
// the ImGui consumer yielding the keyboard while the mouse is captured and reporting Escape's users,
// and the Outliner and Details while playing. Requirement numbers are the spec's.

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
#include "ith/editor_core/PlayInput.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/ViewportInput.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/DetailsPanel.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/editor_ui/OutlinerPanel.h"
#include "ith/editor_ui/PlayIndicators.h"
#include "ith/editor_ui/ViewportPanel.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"
#include "ith/gameplay/Time.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/Value.h"
#include "ith/shapes/ShapeRenderer.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <limits>
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

Keymap engineKeymap(const EditorSession& session) {
    const Config config({{.name = "engine", .directory = "engine/config"}});
    std::vector<Error> warnings;
    return Keymap::load(config, session.commands(), warnings);
}

serialization::Value json(std::string_view text) {
    auto parsed = serialization::parseJson(text);
    REQUIRE(parsed.hasValue());
    return std::move(*parsed);
}

void run(EditorSession& session, std::string_view id, std::string_view args = "{}") {
    const Expected<serialization::Value> result = session.execute(id, json(args));
    REQUIRE_MESSAGE(result.hasValue(), (result ? std::string() : result.error().message));
}

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

// A cube in front of viewport 1's camera; returns its stable id.
Uuid createCube(EditorSession& session) {
    const Expected<serialization::Value> created =
        session.execute("ith.entity.create", addShapeArgs(Shape::Cube, session.viewports().at(0).camera));
    REQUIRE(created.hasValue());
    const Expected<Uuid> id = Uuid::parse(created->find("id")->asString());
    REQUIRE(id.hasValue());
    return *id;
}

// Spawns a shape without a stable id into the play session's game world, as a game system would,
// and runs one tick so the counters follow it.
void spawnInGame(EditorSession& session) {
    PlaySession& play = *session.play();
    static_cast<void>(play.mutableWorld().spawn(ShapeRenderer{}, LocalTransform{}, WorldTransform{}));
    play.step(1, 1.0 / 60.0);
}

std::vector<std::string_view> labelsOf(const std::vector<PlayToolbarControl>& controls) {
    std::vector<std::string_view> labels;
    for (const PlayToolbarControl& control : controls) {
        labels.push_back(control.label);
    }
    return labels;
}

const PlayToolbarControl& control(const std::vector<PlayToolbarControl>& controls, std::string_view label) {
    for (const PlayToolbarControl& found : controls) {
        if (found.label == label) {
            return found;
        }
    }
    FAIL("no toolbar control " << std::string(label));
    return controls.front();
}

// The Viewport panel in fixed windows on the null device: ith.viewport.1 at (100, 50), and with
// `second` ith.viewport.2 to its right. The viewport consumer sits under the UI's; Play captures in
// the fixture's window.
struct ViewportRig {
    ViewportRig() : session(types()), viewportInput(session), ui({.viewports = &viewportInput}, &viewportInput) {
        Expected<std::unique_ptr<ViewportPanel>> created = ViewportPanel::create(session, *ui.ui, *ui.device,
                                                                                 {.shapesVertex = kShaderCode,
                                                                                  .shapesPixel = kShaderCode,
                                                                                  .gridVertex = kShaderCode,
                                                                                  .gridPixel = kShaderCode});
        REQUIRE(created.hasValue());
        panel = std::move(*created);
        registerViewport(session.panels(), *panel);
        ui.beforeUi = [this](rg::RenderGraph& graph, const rhi::Frame& frame) { panel->addRenderPasses(graph, frame); };
        session.setPlayInput(&ui.input, kWindow);
    }

    ViewportRig(const ViewportRig&) = delete;
    ViewportRig& operator=(const ViewportRig&) = delete;
    ViewportRig(ViewportRig&&) = delete;
    ViewportRig& operator=(ViewportRig&&) = delete;

    ~ViewportRig() {
        // The session's Input goes with the UI fixture, before the session.
        session.stopPlay();
        session.setPlayInput(nullptr, kNoWindow);
        ui.graph.reset();
        panel.reset();
    }

    void frame() {
        ui.frame([this] {
            panel->newFrame();
            // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
            constexpr ImGuiWindowFlags kFixed =
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::SetNextWindowPos(ImVec2(100.0f, 50.0f));
            ImGui::SetNextWindowSize(ImVec2(560.0f, 400.0f));
            if (ImGui::Begin("Viewport###test.viewport", nullptr, kFixed)) {
                session.panels().find("ith.viewport")->draw("ith.viewport.1");
            }
            ImGui::End();
            if (second) {
                ImGui::SetNextWindowPos(ImVec2(700.0f, 50.0f));
                ImGui::SetNextWindowSize(ImVec2(480.0f, 360.0f));
                if (ImGui::Begin("Viewport 2###test.viewport2", nullptr, kFixed)) {
                    session.panels().find("ith.viewport")->draw("ith.viewport.2");
                }
                ImGui::End();
            }
        });
    }

    [[nodiscard]] std::size_t shapePasses() const {
        std::size_t count = 0;
        for (std::size_t at = ui.lastGraph.find("'ViewportShapes'"); at != std::string::npos;
             at = ui.lastGraph.find("'ViewportShapes'", at + 1)) {
            ++count;
        }
        return count;
    }

    EditorSession session;
    ViewportConsumer viewportInput;
    UiFixture ui;
    std::unique_ptr<ViewportPanel> panel;
    bool second = false;
};

// The shell with the engine keymap's consumer under the UI and the Play In Editor consumer on top,
// as in the editor; Play captures in the fixture's window.
struct ShellRig {
    ShellRig()
        : session(types()),
          keymap(engineKeymap(session)),
          shortcuts(session, keymap),
          playKeys(session, keymap),
          ui({.playInput = &playKeys}, &shortcuts) {
        ui.input.addConsumer({.window = kWindow, .layer = ConsumerLayer::Editor, .consumer = &playKeys});
        session.setPlayInput(&ui.input, kWindow);
    }

    ShellRig(const ShellRig&) = delete;
    ShellRig& operator=(const ShellRig&) = delete;
    ShellRig(ShellRig&&) = delete;
    ShellRig& operator=(ShellRig&&) = delete;

    ~ShellRig() {
        session.stopPlay();
        session.setPlayInput(nullptr, kNoWindow);
    }

    void frame() {
        ui.frame([this] { drawEditorShell(session, keymap, state, dock); });
    }

    // A click on `rect`, a frame for each of the move, the press and the release, and one more. A
    // copy: the toolbar's controls are drawn again every frame.
    void click(UiRect rect) {
        ui.events({mouseMoved(rect.centerX(), rect.centerY())});
        frame();
        ui.events({press(MouseButton::Left, rect.centerX(), rect.centerY())});
        frame();
        ui.events({release(MouseButton::Left, rect.centerX(), rect.centerY())});
        frame();
        frame(); // the toolbar as the command left it
    }

    EditorSession session;
    Keymap keymap;
    KeymapConsumer shortcuts;
    PlayInputConsumer playKeys;
    UiFixture ui;
    EditorShellState state;
    DockLayout dock{session};
};

} // namespace

TEST_SUITE("editor_ui.play") {
    TEST_CASE("the toolbar shows spec 3.2's controls in each state, Step only enabled while paused (5)") {
        EditorSession session(types());
        const Keymap keymap = engineKeymap(session);
        std::vector<PlayToolbarControl> controls = playToolbarControls(session, keymap);
        CHECK(labelsOf(controls) == std::vector<std::string_view>{"Play", "Simulate"});
        CHECK(control(controls, "Play").enabled);
        CHECK(control(controls, "Play").command == "ith.play.play");
        CHECK(control(controls, "Simulate").command == "ith.play.simulate");
        CHECK(control(controls, "Play").tooltip.starts_with("Play (Alt+P)\n"));
        CHECK(control(controls, "Simulate").tooltip.starts_with("Simulate (Alt+S)\n"));

        // An interaction holding a transaction disables Play and Simulate, with the reason.
        {
            TransactionScope drag = session.document().transactions().begin("Drag");
            controls = playToolbarControls(session, keymap);
            CHECK_FALSE(control(controls, "Play").enabled);
            CHECK_FALSE(control(controls, "Simulate").enabled);
            CHECK(control(controls, "Play").tooltip.find("\n") != std::string::npos);
            drag.cancel();
        }

        // Running possessed: Stop where Play was, Pause, Step disabled, Eject.
        run(session, "ith.play.play");
        controls = playToolbarControls(session, keymap);
        CHECK(labelsOf(controls) == std::vector<std::string_view>{"Stop", "Pause", "Step", "Eject"});
        CHECK(control(controls, "Stop").enabled);
        CHECK(control(controls, "Stop").tooltip.starts_with("Stop (Escape)\n"));
        CHECK(control(controls, "Pause").command == "ith.play.pause");
        CHECK_FALSE(control(controls, "Step").enabled);
        CHECK(control(controls, "Eject").command == "ith.play.possess_or_eject");
        CHECK(control(controls, "Eject").tooltip.starts_with("Eject (F8)\n"));

        // Running ejected: Possess.
        run(session, "ith.play.possess_or_eject");
        controls = playToolbarControls(session, keymap);
        CHECK(labelsOf(controls) == std::vector<std::string_view>{"Stop", "Pause", "Step", "Possess"});
        CHECK_FALSE(control(controls, "Step").enabled);

        // Paused: Resume and Step, Possess or Eject as the flag.
        run(session, "ith.play.pause", R"({"paused": true})");
        controls = playToolbarControls(session, keymap);
        CHECK(labelsOf(controls) == std::vector<std::string_view>{"Stop", "Resume", "Step", "Possess"});
        CHECK(control(controls, "Step").enabled);
        CHECK(control(controls, "Resume").enabled);

        run(session, "ith.play.stop");
        CHECK(labelsOf(playToolbarControls(session, keymap)) == std::vector<std::string_view>{"Play", "Simulate"});
    }

    TEST_CASE("each toolbar button runs its command, and Stop lands where Play was (5)") {
        ShellRig rig;
        rig.frame();
        rig.frame();
        REQUIRE(rig.state.toolbar.size() == 2);
        const UiRect play = rig.state.toolbar[0].rect;
        CHECK(play.width > 0.0f);
        // Under the menu bar: the toolbar is a strip of its own.
        CHECK(play.y > ImGui::GetFrameHeight() * 0.5f);
        rig.click(play);
        REQUIRE(rig.session.play() != nullptr);
        CHECK(rig.session.play()->possessed());
        CHECK_FALSE(rig.session.play()->paused());
        rig.frame();
        REQUIRE(rig.state.toolbar.size() == 4);
        CHECK(rig.state.toolbar[0].label == "Stop");
        CHECK(rig.state.toolbar[0].rect.x == play.x);
        CHECK(rig.state.toolbar[0].rect.width == play.width);

        rig.click(control(rig.state.toolbar, "Pause").rect);
        CHECK(rig.session.play()->paused());
        rig.click(control(rig.state.toolbar, "Step").rect);
        REQUIRE(rig.session.play()->time() != nullptr);
        CHECK(rig.session.play()->time()->frame == 1);
        rig.click(control(rig.state.toolbar, "Eject").rect);
        CHECK_FALSE(rig.session.play()->possessed());
        rig.click(control(rig.state.toolbar, "Possess").rect);
        CHECK(rig.session.play()->possessed());
        rig.click(control(rig.state.toolbar, "Resume").rect);
        CHECK_FALSE(rig.session.play()->paused());
        rig.click(control(rig.state.toolbar, "Stop").rect);
        CHECK(rig.session.play() == nullptr);
        CHECK(rig.state.status == "Stop");

        rig.frame();
        rig.click(control(rig.state.toolbar, "Simulate").rect);
        REQUIRE(rig.session.play() != nullptr);
        CHECK_FALSE(rig.session.play()->possessed());
    }

    TEST_CASE("the status bar shows the state and the game's elapsed time while playing, nothing after Stop (43)") {
        ShellRig rig;
        const auto drawn = [&rig] {
            return drawnText(rig.ui, [&rig] { drawEditorShell(rig.session, rig.keymap, rig.state, rig.dock); });
        };
        rig.frame();
        CHECK(drawn().find(" 0:0") == std::string::npos);

        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        CHECK(drawn().find("Paused 0:00.0") != std::string::npos);
        run(rig.session, "ith.play.step", R"({"frames": 3, "deltaSeconds": 0.05})");
        CHECK(drawn().find("Paused 0:00.1") != std::string::npos);
        run(rig.session, "ith.play.pause", R"({"paused": false})");
        CHECK(drawn().find("Simulating 0:00.1") != std::string::npos);
        run(rig.session, "ith.play.possess_or_eject");
        CHECK(drawn().find("Playing 0:00.1") != std::string::npos);

        run(rig.session, "ith.play.stop");
        const std::string after = drawn();
        CHECK(after.find("Playing") == std::string::npos);
        CHECK(after.find("Simulating") == std::string::npos);
        CHECK(after.find("Paused") == std::string::npos);
    }

    TEST_CASE("the indicators' words, colours and time format") {
        const ThemeTokens theme = defaultDarkTheme();
        CHECK(playIndicatorLabel(PlayIndicator::Playing) == "Playing");
        CHECK(playIndicatorLabel(PlayIndicator::Simulating) == "Simulating");
        CHECK(playIndicatorLabel(PlayIndicator::Paused) == "Paused");
        CHECK(playIndicatorLabel(PlayIndicator::None).empty());
        CHECK(playIndicatorColor(PlayIndicator::Playing, theme) == theme.playActive);
        CHECK(playIndicatorColor(PlayIndicator::Simulating, theme) == theme.playSimulate);
        CHECK(playIndicatorColor(PlayIndicator::Paused, theme) == theme.playPaused);
        CHECK(playIndicatorColor(PlayIndicator::None, theme) == theme.text);
        CHECK(theme.playActive != theme.playSimulate);
        CHECK(theme.playSimulate != theme.playPaused);

        CHECK(formatPlayTime(0.0) == "0:00.0");
        CHECK(formatPlayTime(0.19) == "0:00.1");
        CHECK(formatPlayTime(65.37) == "1:05.3");
        CHECK(formatPlayTime(600.0) == "10:00.0");
        CHECK(formatPlayTime(-3.0) == "0:00.0");
        CHECK(formatPlayTime(std::numeric_limits<double>::quiet_NaN()) == "0:00.0");
        CHECK(formatPlayTime(std::numeric_limits<double>::infinity()) == "0:00.0");
        // Clamped before the cast: at most kMaxPlayTimeSeconds (1e15 s).
        CHECK(formatPlayTime(1.0e300) == "16666666666666:40.0");
        CHECK(formatPlayTime(std::numeric_limits<double>::max()) == "16666666666666:40.0");
        CHECK(formatPlayTime(kMaxPlayTimeSeconds) == "16666666666666:40.0");
        // A tenth a hair short in doubles still counts.
        CHECK(formatPlayTime(0.3) == "0:00.3");
        CHECK(formatPlayTime(2.3) == "0:02.3");
        CHECK(formatPlayTime(0.09) == "0:00.0");
    }

    TEST_CASE("the play viewport's border, label and capture hint follow the state; nothing after Stop (43, 33)") {
        ViewportRig rig;
        rig.second = true;
        const ThemeTokens& theme = rig.ui.ui->theme();
        const Uuid cube = createCube(rig.session);
        rig.frame();
        CHECK_FALSE(rig.panel->drawnPlayOverlay("ith.viewport.1").has_value());
        CHECK(rig.panel->drawnGizmo("ith.viewport.1").has_value()); // the cube is selected

        run(rig.session, "ith.play.play", R"({"paused": true})");
        rig.frame();
        std::optional<ViewportPlayOverlay> overlay = rig.panel->drawnPlayOverlay("ith.viewport.1");
        REQUIRE(overlay.has_value());
        CHECK(overlay->label == "Paused");
        CHECK(overlay->border == theme.playPaused);
        CHECK(overlay->captureHint);
        CHECK_FALSE(rig.panel->drawnPlayOverlay("ith.viewport.2").has_value());
        // No gizmo while playing, possessed or not (33).
        CHECK(rig.session.play()->selection().contains(cube));
        CHECK_FALSE(rig.panel->drawnGizmo("ith.viewport.1").has_value());
        CHECK_FALSE(rig.panel->drawnGizmo("ith.viewport.2").has_value());

        run(rig.session, "ith.play.pause", R"({"paused": false})");
        rig.frame();
        overlay = rig.panel->drawnPlayOverlay("ith.viewport.1");
        REQUIRE(overlay.has_value());
        CHECK(overlay->label == "Playing");
        CHECK(overlay->border == theme.playActive);
        CHECK(overlay->captureHint);

        // Captured: the hint goes.
        REQUIRE(rig.ui.input.captureMouse(kWindow));
        rig.frame();
        overlay = rig.panel->drawnPlayOverlay("ith.viewport.1");
        REQUIRE(overlay.has_value());
        CHECK_FALSE(overlay->captureHint);

        run(rig.session, "ith.play.possess_or_eject");
        rig.frame();
        overlay = rig.panel->drawnPlayOverlay("ith.viewport.1");
        REQUIRE(overlay.has_value());
        CHECK(overlay->label == "Simulating");
        CHECK(overlay->border == theme.playSimulate);
        CHECK_FALSE(overlay->captureHint);
        CHECK_FALSE(rig.panel->drawnGizmo("ith.viewport.1").has_value());

        // In game view the border stays: it tells the game world from the editor world.
        run(rig.session, "ith.viewport.game_view", R"({"viewport": "ith.viewport.1"})");
        rig.frame();
        CHECK(rig.panel->drawnPlayOverlay("ith.viewport.1").has_value());

        run(rig.session, "ith.play.stop");
        rig.frame();
        CHECK_FALSE(rig.panel->drawnPlayOverlay("ith.viewport.1").has_value());
        CHECK_FALSE(rig.panel->drawnPlayOverlay("ith.viewport.2").has_value());
    }

    TEST_CASE("the play viewport draws the game world and the others the editor world; Stop draws the level again") {
        ViewportRig rig;
        static_cast<void>(createCube(rig.session));
        rig.frame();
        CHECK(rig.panel->lastShapeDraws() == 1);

        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        spawnInGame(rig.session);
        rig.frame();
        CHECK(rig.shapePasses() == 1);
        CHECK(rig.panel->lastShapeDraws() == 2); // the game world: the cube and the spawned shape

        // Viewport 2, shown now, draws the editor world; viewport 1's scene hasn't changed.
        rig.second = true;
        rig.frame();
        CHECK(rig.shapePasses() == 1);
        CHECK(rig.panel->lastShapeDraws() == 1);

        // A session in viewport 2 instead: viewport 1 draws the level.
        run(rig.session, "ith.play.stop");
        rig.frame();
        CHECK(rig.shapePasses() == 2);
        run(rig.session, "ith.window.open_panel", R"({"panel": "ith.viewport.2"})"); // in the layout, so Play uses it
        rig.session.viewports().setSelected(1);
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        REQUIRE(rig.session.play()->viewport() == 1);
        spawnInGame(rig.session);
        rig.second = false;
        rig.frame();
        CHECK(rig.panel->lastShapeDraws() == 1);
        CHECK_FALSE(rig.panel->drawnPlayOverlay("ith.viewport.1").has_value());
    }

    TEST_CASE("a game-world tick or a play selection change re-renders; possessed, the play viewport is game view") {
        ViewportRig rig;
        rig.second = true;
        static_cast<void>(createCube(rig.session));
        rig.frame();
        run(rig.session, "ith.play.play", R"({"paused": true})");
        rig.frame(); // Play changed the active world: both render
        CHECK(rig.shapePasses() == 2);
        rig.frame();
        CHECK(rig.shapePasses() == 0);

        // A tick moves the game world only: the play viewport renders.
        rig.session.play()->step(1, 1.0 / 60.0);
        rig.frame();
        CHECK(rig.shapePasses() == 1);

        // Possessed, the play viewport draws as game view: a selection change re-renders only the
        // editor-world viewport, which tints the play selection.
        run(rig.session, "ith.edit.select_none");
        rig.frame();
        CHECK(rig.shapePasses() == 1);
        run(rig.session, "ith.play.possess_or_eject"); // ejected: the tint shows again
        rig.frame();
        CHECK(rig.shapePasses() == 1);
        run(rig.session, "ith.edit.select_all");
        rig.frame();
        CHECK(rig.shapePasses() == 2);
    }

    TEST_CASE("Keep Simulation Changes re-renders the editor-world viewports at once, not the play viewport (37)") {
        ViewportRig rig;
        rig.second = true;
        const Uuid cube = createCube(rig.session);
        rig.frame();
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        rig.frame();
        run(rig.session, "ith.entity.set_property",
            R"({"ids": [")" + cube.toString() +
                R"("], "path": "ith::LocalTransform.value.translation.x", "value": 4})");
        rig.frame();
        CHECK(rig.shapePasses() == 1); // the game world's change: the play viewport only
        const std::uint64_t renders = rig.panel->renders();
        run(rig.session, "ith.play.keep_simulation_changes");
        rig.frame();
        CHECK(rig.shapePasses() == 1);
        CHECK(rig.panel->renders() == renders + 1);
        // It was viewport 2's: the level now has the kept value.
        const World& level = rig.session.document().world();
        CHECK(level.get<const LocalTransform>(level.findByStableId(cube))->value.translation.x == 4.0f);
        rig.frame();
        CHECK(rig.shapePasses() == 0);
    }

    // The null device copies nothing, so every pick reads the empty texel: a plain click would clear
    // the selection it is applied to.
    TEST_CASE("a pick copied before Stop is dropped instead of reaching the editor selection") {
        ViewportRig rig;
        const Uuid cube = createCube(rig.session);
        rig.frame();
        REQUIRE(rig.session.document().selection().contains(cube));
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        rig.frame();
        ViewportState& state = rig.session.viewports().at(0);
        state.click = ViewportClick{.position = state.imageSize * 0.5f, .mode = PickMode::Replace};
        rig.frame(); // copied
        REQUIRE(rig.panel->picking());
        run(rig.session, "ith.play.stop");
        for (std::uint32_t frame = 0; frame < rhi::kFramesInFlight; ++frame) {
            rig.frame();
        }
        CHECK_FALSE(rig.panel->picking());
        CHECK(rig.session.document().selection().contains(cube));

        // The same the other way: copied before Play, read during it.
        state.click = ViewportClick{.position = state.imageSize * 0.5f, .mode = PickMode::Replace};
        rig.frame();
        REQUIRE(rig.panel->picking());
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        for (std::uint32_t frame = 0; frame < rhi::kFramesInFlight; ++frame) {
            rig.frame();
        }
        CHECK_FALSE(rig.panel->picking());
        CHECK(rig.session.play()->selection().contains(cube));

        // Within one session a pick still applies, to the play selection.
        state.click = ViewportClick{.position = state.imageSize * 0.5f, .mode = PickMode::Replace};
        for (std::uint32_t frame = 0; frame <= rhi::kFramesInFlight; ++frame) {
            rig.frame();
        }
        CHECK(rig.session.play()->selection().empty());
        CHECK(rig.session.document().selection().contains(cube));
    }

    TEST_CASE("Play in a closed selected viewport plays in ith.viewport.1, which the shell opens once (ADR-0053 5)") {
        ViewportRig rig;
        EditorShellState state;
        DockLayout dock(rig.session);
        // The built-in default layout: ith.viewport.1 in the centre.
        static_cast<void>(rig.session.layouts().restore());
        const Keymap keymap = engineKeymap(rig.session);
        const auto shell = [&] {
            rig.ui.frame([&] {
                rig.panel->newFrame();
                drawEditorShell(rig.session, keymap, state, dock);
            });
        };
        shell();
        shell();
        REQUIRE(isPanelOpen(rig.session.layouts().current(), "ith.viewport.1"));

        // Viewport 2 open and selected: Play plays there.
        run(rig.session, "ith.window.open_panel", R"({"panel": "ith.viewport.2"})");
        shell();
        rig.session.viewports().setSelected(1);
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        CHECK(rig.session.play()->viewport() == 1);
        run(rig.session, "ith.play.stop");
        // Closed, then Play with no frame between, as one batch of commands would: viewport 1.
        CHECK(rig.session.viewports().selected() == 1);
        run(rig.session, "ith.window.close_panel", R"({"panel": "ith.viewport.2"})");
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        CHECK(rig.session.play()->viewport() == 0);
        CHECK(rig.session.viewports().selected() == 1); // the selection itself stays
        run(rig.session, "ith.play.stop");

        // A session whose viewport's panel is closed opens it.
        run(rig.session, "ith.window.close_panel", R"({"panel": "ith.viewport.1"})");
        rig.session.viewports().setSelected(0);
        shell();
        shell();
        REQUIRE_FALSE(isPanelOpen(rig.session.layouts().current(), "ith.viewport.1"));
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        shell();
        shell();
        CHECK(isPanelOpen(rig.session.layouts().current(), "ith.viewport.1"));
        CHECK(rig.panel->drawnPlayOverlay("ith.viewport.1").has_value());

        // Closed during the session, it stays closed: the user chose that.
        run(rig.session, "ith.window.close_panel", R"({"panel": "ith.viewport.1"})");
        shell();
        shell();
        CHECK_FALSE(isPanelOpen(rig.session.layouts().current(), "ith.viewport.1"));
    }

    TEST_CASE("while captured the UI neither takes nor types keys, and Escape stops even in a text field (23)") {
        ShellRig rig;
        std::array<char, 32> text{};
        bool focus = true;
        const auto field = [&] {
            ImGui::SetNextWindowPos(ImVec2(300.0f, 300.0f));
            ImGui::SetNextWindowSize(ImVec2(300.0f, 100.0f));
            ImGui::Begin("Field###test.field");
            if (focus) {
                ImGui::SetKeyboardFocusHere();
                focus = false;
            }
            ImGui::InputText("##field", text.data(), text.size());
            ImGui::End();
        };
        rig.ui.frame(field);
        rig.ui.frame(field);
        REQUIRE(rig.ui.ui->wantsTextInput());
        run(rig.session, "ith.play.play");
        REQUIRE(rig.ui.input.captureMouse(kWindow));
        rig.ui.frame(field);

        // E (the rotate tool's chord) and Ctrl + Z go to the game: past the UI and the keymap, and into
        // no text field.
        rig.ui.lower.events.clear();
        rig.ui.events({Event{.type = EventType::KeyDown, .key = Key::E, .layoutKey = U'e'},
                       Event{.type = EventType::TextInput, .text = "e"}, keyUp(Key::E)});
        rig.ui.frame(field);
        rig.ui.frame(field);
        CHECK(rig.ui.lower.saw(EventType::KeyDown));
        CHECK(std::string(text.data()).empty());
        CHECK(rig.session.viewports().tool() == ViewportTool::Translate);
        rig.ui.lower.events.clear();
        rig.ui.events(
            {Event{.type = EventType::KeyDown, .key = Key::Z, .layoutKey = U'z', .modifiers = KeyModifiers::Control},
             keyUp(Key::Z)});
        rig.ui.frame(field);
        CHECK(rig.ui.lower.saw(EventType::KeyDown));
        CHECK(rig.session.document().history().cursor() == 0);

        // Escape stops, although the text field still reports itself active.
        rig.ui.events({keyDown(Key::Escape), keyUp(Key::Escape)});
        CHECK(rig.session.play() == nullptr);

        // Free again, the field types.
        rig.ui.frame(field);
        rig.ui.events({Event{.type = EventType::TextInput, .text = "a"}});
        rig.ui.frame(field);
        CHECK(std::string(text.data()) == "a");
    }

    TEST_CASE("with the mouse free, Escape closes an open popup or leaves a text field, and only then stops (25)") {
        ShellRig rig;
        bool openPopup = false;
        bool showField = false;
        bool focusField = false;
        std::array<char, 32> text{};
        const auto draw = [&] {
            ImGui::SetNextWindowPos(ImVec2(300.0f, 300.0f));
            ImGui::SetNextWindowSize(ImVec2(300.0f, 100.0f));
            ImGui::Begin("Popups###test.popups");
            if (openPopup) {
                ImGui::OpenPopup("##menu");
                openPopup = false;
            }
            if (ImGui::BeginPopup("##menu")) {
                ImGui::TextUnformatted("A menu");
                ImGui::EndPopup();
            }
            if (showField) {
                if (focusField) {
                    ImGui::SetKeyboardFocusHere();
                    focusField = false;
                }
                ImGui::InputText("##field", text.data(), text.size());
            }
            ImGui::End();
        };
        run(rig.session, "ith.play.simulate");
        rig.ui.frame(draw);

        // A popup is open: Escape passes the Play In Editor consumer, and the UI's consumer takes it.
        openPopup = true;
        rig.ui.frame(draw);
        rig.ui.frame(draw);
        REQUIRE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
        rig.ui.lower.events.clear();
        rig.ui.events({keyDown(Key::Escape), keyUp(Key::Escape)});
        CHECK(rig.session.play() != nullptr);
        CHECK_FALSE(rig.ui.lower.saw(EventType::KeyDown));
        rig.ui.frame(draw);
        rig.ui.frame(draw);
        CHECK_FALSE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));

        // A text field is active: Escape leaves it, and the session keeps running.
        showField = true;
        focusField = true;
        rig.ui.frame(draw);
        rig.ui.frame(draw);
        REQUIRE(rig.ui.ui->wantsTextInput());
        rig.ui.events({keyDown(Key::Escape), keyUp(Key::Escape)});
        CHECK(rig.session.play() != nullptr);
        rig.ui.frame(draw);
        rig.ui.frame(draw);
        CHECK_FALSE(rig.ui.ui->wantsTextInput());

        // Nothing uses Escape now: it stops.
        rig.ui.events({keyDown(Key::Escape), keyUp(Key::Escape)});
        CHECK(rig.session.play() == nullptr);
    }

    TEST_CASE("Keep Simulation Changes' notice: in the status bar and the log, from K and the Edit menu (42a)") {
        ShellRig rig;
        const KeepChangesPrompt prompt = registerKeepSimulationChanges(rig.session, rig.state);
        const Uuid cube = createCube(rig.session);
        run(rig.session, "ith.play.simulate", R"({"paused": true})");
        run(rig.session, "ith.entity.set_property",
            R"({"ids": [")" + cube.toString() +
                R"("], "path": "ith::LocalTransform.value.translation.x", "value": 2})");
        rig.frame();

        LogRingBuffer log(64);
        REQUIRE(addLogSink(log.sink()).hasValue());
        // K, with the mouse free: the keymap invokes it, the prompt reports.
        rig.ui.events({Event{.type = EventType::KeyDown, .key = Key::K, .layoutKey = U'k'}, keyUp(Key::K)});
        rig.frame();
        CHECK(rig.state.status == "Kept 1 component of 1 entity");
        CHECK(rig.state.lastResult.hasValue());
        CHECK(rig.state.lastCommand == kKeepSimulationChanges);

        // The Edit menu's entry runs it through the same prompt; nothing new to keep now.
        runMenuCommand(rig.session, kKeepSimulationChanges, rig.state);
        CHECK(rig.state.status == "Nothing to keep");
        removeLogSink(log.sink());
        std::vector<LogEntry> entries;
        static_cast<void>(log.read(0, entries));
        std::size_t notices = 0;
        for (const LogEntry& entry : entries) {
            CHECK_MESSAGE(entry.level != LogLevel::Warning, entry.message);
            if (std::string_view(entry.category->name()) == "Play" && entry.level == LogLevel::Info) {
                ++notices;
            }
        }
        CHECK(notices == 2);
    }

    TEST_CASE("the notice counts what was kept, skipped, ignored and warned about") {
        CHECK(keptChangesNotice(json(R"({"kept": [], "skipped": [], "ignored": [], "warnings": []})")) ==
              "Nothing to keep");
        CHECK(keptChangesNotice(
                  json(R"({"kept": [{"id": "a", "components": ["X", "Y"]}, {"id": "b", "components": ["X"]}],
                                         "skipped": [{"id": "c", "reason": "not in the level"}],
                                         "ignored": [{"id": "a", "component": "Z", "reason": "added during play"},
                                                     {"id": "b", "component": "Z", "reason": "parent changed"}],
                                         "warnings": [{"code": "editor.play.reference_dropped", "message": "m"}]})")) ==
              "Kept 3 components of 2 entities, 1 skipped, 2 ignored, 1 warning");
        CHECK(keptChangesNotice(json("{}")) == "Nothing to keep");
    }

    TEST_CASE(
        "the Outliner and Details show the game world read-only with their notes; spawned rows don't select (28)") {
        EditorSession session(types());
        const Keymap keymap = engineKeymap(session);
        KeymapConsumer shortcuts(session, keymap);
        UiFixture ui({}, &shortcuts);
        OutlinerPanel outliner(session);
        registerOutliner(session, outliner);
        DetailsPanel details(session);
        registerDetails(session.panels(), details);
        const auto draw = [&] {
            // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
            constexpr ImGuiWindowFlags kFixed =
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 700.0f));
            if (ImGui::Begin("Outliner###test.outliner", nullptr, kFixed)) {
                outliner.draw();
            }
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(400.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(880.0f, 700.0f));
            if (ImGui::Begin("Details###test.details", nullptr, kFixed)) {
                details.draw();
            }
            ImGui::End();
        };
        const auto click = [&](float x, float y, MouseButton button = MouseButton::Left) {
            ui.events({mouseMoved(x, y)});
            ui.frame(draw);
            ui.events({press(button, x, y)});
            ui.frame(draw);
            ui.events({release(button, x, y)});
            ui.frame(draw);
            ui.frame(draw);
        };
        const Uuid table = createCube(session);
        const Uuid lamp = createCube(session);
        run(session, "ith.selection.set", R"({"entities": [")" + table.toString() + R"("]})");
        ui.frame(draw);
        std::string text = drawnText(ui, draw);
        CHECK(text.find(kOutlinerPlayNote) == std::string::npos);
        CHECK(text.find(kDetailsPlayNote) == std::string::npos);

        run(session, "ith.play.simulate", R"({"paused": true})");
        spawnInGame(session);
        ui.frame(draw);
        text = drawnText(ui, draw);
        CHECK(text.find(kOutlinerPlayNote) != std::string::npos);
        CHECK(text.find(kDetailsPlayNote) != std::string::npos);
        CHECK(text.find("Spawned") != std::string::npos);
        ui.frame(draw);
        REQUIRE(outliner.spawnedRows().size() == 1);

        LogRingBuffer log(64);
        REQUIRE(addLogSink(log.sink()).hasValue());
        // A spawned row changes nothing and logs nothing.
        const UiRect spawned = outliner.spawnedRows().front();
        click(spawned.centerX(), spawned.centerY());
        CHECK(session.play()->selection().contains(table));
        CHECK(session.play()->selection().entities().size() == 1);
        // A level entity's row selects it in the play selection; the editor selection stays.
        const UiRect lampRow = *outliner.rowRect(lamp);
        click(lampRow.centerX(), lampRow.centerY());
        CHECK(session.play()->selection().contains(lamp));
        CHECK_FALSE(session.play()->selection().contains(table));
        CHECK(session.document().selection().contains(table));
        // The Enabled check box is disabled: nothing changes.
        click(lampRow.x + lampRow.width + 12.0f, lampRow.centerY());
        const World& game = session.play()->world();
        CHECK(game.get<const Disabled>(game.findByStableId(lamp)) == nullptr);
        // A double click starts no rename.
        click(lampRow.centerX(), lampRow.centerY());
        click(lampRow.centerX(), lampRow.centerY());
        CHECK_FALSE(outliner.model().renaming().has_value());

        // The context menu offers Keep Simulation Changes instead of Add Shape.
        const UiRect empty = *outliner.emptyRect();
        click(empty.centerX(), empty.y + 10.0f, MouseButton::Right);
        text = drawnText(ui, draw);
        CHECK(text.find("Keep Simulation Changes") != std::string::npos);
        CHECK(text.find("Add Shape") == std::string::npos);
        removeLogSink(log.sink());
        std::vector<LogEntry> entries;
        static_cast<void>(log.read(0, entries));
        for (const LogEntry& entry : entries) {
            CHECK_MESSAGE(entry.level != LogLevel::Warning, entry.message);
        }

        run(session, "ith.play.stop");
        ui.frame(draw);
        text = drawnText(ui, draw);
        CHECK(text.find(kOutlinerPlayNote) == std::string::npos);
        CHECK(text.find("Spawned") == std::string::npos);
    }

    TEST_CASE("a Details drag across Stop changes nothing in the level: its rest is refused until released") {
        EditorSession session(types());
        const Keymap keymap = engineKeymap(session);
        KeymapConsumer shortcuts(session, keymap);
        UiFixture ui({}, &shortcuts);
        DetailsPanel details(session);
        const auto draw = [&] {
            // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
            constexpr ImGuiWindowFlags kFixed =
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(880.0f, 700.0f));
            if (ImGui::Begin("Details###test.details", nullptr, kFixed)) {
                details.draw();
            }
            ImGui::End();
        };
        const auto step = [&](const Event& event) {
            ui.events({event});
            ui.frame(draw);
        };
        const Uuid cube = createCube(session);
        const World& level = session.document().world();
        const float before = level.get<const LocalTransform>(level.findByStableId(cube))->value.translation.x;
        const std::size_t entries = session.document().history().cursor();
        const bool dirty = session.document().dirty();
        run(session, "ith.play.simulate", R"({"paused": true})");
        ui.frame(draw);
        ui.frame(draw);
        const std::optional<UiRect> field = details.valueRect("ith::LocalTransform.value.translation");
        REQUIRE(field.has_value());
        const float x = field->x + field->width / 6.0f;
        const float y = field->centerY();
        step(mouseMoved(x, y));
        step(press(MouseButton::Left, x, y));
        for (int i = 1; i <= 4; ++i) {
            step(mouseMoved(x + 10.0f * static_cast<float>(i), y));
        }
        REQUIRE(details.model().editing());
        // Stop mid-drag (the toolbar or F8 would do the same): the drag goes on, and must not write.
        run(session, "ith.play.stop");
        for (int i = 5; i <= 7; ++i) {
            step(mouseMoved(x + 10.0f * static_cast<float>(i), y));
        }
        step(release(MouseButton::Left, x + 70.0f, y));
        ui.frame(draw);
        CHECK(session.document().history().cursor() == entries);
        CHECK(session.document().dirty() == dirty);
        CHECK(level.get<const LocalTransform>(level.findByStableId(cube))->value.translation.x == before);
        CHECK_FALSE(details.model().editing());

        // The next drag edits the level as usual (after the double-click time, so it is no double click).
        for (int i = 0; i < 30; ++i) {
            ui.frame(draw);
        }
        step(mouseMoved(x, y));
        step(press(MouseButton::Left, x, y));
        for (int i = 1; i <= 4; ++i) {
            step(mouseMoved(x + 10.0f * static_cast<float>(i), y));
        }
        step(release(MouseButton::Left, x + 40.0f, y));
        ui.frame(draw);
        CHECK(session.document().history().cursor() == entries + 1);
    }

    TEST_CASE("a spawned parent with a spawned child expands and collapses in the Outliner") {
        EditorSession session(types());
        UiFixture ui;
        OutlinerPanel outliner(session);
        const auto draw = [&] {
            // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
            constexpr ImGuiWindowFlags kFixed =
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(400.0f, 700.0f));
            if (ImGui::Begin("Outliner###test.outliner", nullptr, kFixed)) {
                outliner.draw();
            }
            ImGui::End();
        };
        run(session, "ith.play.simulate", R"({"paused": true})");
        World& game = session.play()->mutableWorld();
        const Entity parent = game.spawn(ShapeRenderer{}, LocalTransform{}, WorldTransform{});
        static_cast<void>(game.spawn(ShapeRenderer{}, LocalTransform{}, WorldTransform{}, ChildOf{.parent = parent}));
        session.play()->step(1, 1.0 / 60.0);
        ui.frame(draw);
        ui.frame(draw);
        const std::size_t shown = outliner.spawnedRows().size();
        REQUIRE(shown >= 1);
        const auto toggle = [&] {
            // The arrow at the start of the parent's row.
            const UiRect row = outliner.spawnedRows().front();
            const float x = row.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f;
            ui.events({mouseMoved(x, row.centerY())});
            ui.frame(draw);
            ui.events({press(MouseButton::Left, x, row.centerY())});
            ui.frame(draw);
            ui.events({release(MouseButton::Left, x, row.centerY())});
            ui.frame(draw);
            ui.frame(draw);
        };
        toggle();
        const std::size_t toggled = outliner.spawnedRows().size();
        CHECK(toggled != shown);
        CHECK((toggled == 2 || shown == 2));
        toggle();
        CHECK(outliner.spawnedRows().size() == shown);
        CHECK(session.play()->selection().empty()); // the arrow selects nothing
    }

    TEST_CASE("a modifier released while captured is released in ImGui too") {
        ShellRig rig;
        rig.frame();
        rig.ui.events({Event{.type = EventType::KeyDown, .key = Key::LeftAlt, .modifiers = KeyModifiers::Alt}});
        rig.frame();
        REQUIRE(ImGui::GetIO().KeyAlt);
        run(rig.session, "ith.play.play");
        REQUIRE(rig.ui.input.captureMouse(kWindow));
        rig.frame();
        rig.ui.events({keyUp(Key::LeftAlt)});
        rig.frame();
        // Shift + F1 frees the mouse; Shift went down while captured, so ImGui never saw it.
        rig.ui.events({Event{.type = EventType::KeyDown, .key = Key::LeftShift, .modifiers = KeyModifiers::Shift},
                       Event{.type = EventType::KeyDown, .key = Key::F1, .modifiers = KeyModifiers::Shift}});
        CHECK_FALSE(rig.session.mouseCaptured());
        rig.ui.events(
            {Event{.type = EventType::KeyUp, .key = Key::F1, .modifiers = KeyModifiers::Shift}, keyUp(Key::LeftShift)});
        rig.frame();
        rig.frame();
        CHECK_FALSE(ImGui::GetIO().KeyAlt);
        CHECK_FALSE(ImGui::GetIO().KeyShift);
        CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_LeftAlt));
    }
}
