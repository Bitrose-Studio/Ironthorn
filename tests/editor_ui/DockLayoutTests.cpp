// Docking (ADR-0040 rules 10 to 13), headless on the RHI's null device: layouts applied to ImGui's
// dock tree and read back unchanged (splits, the central stack, tabs and active tabs, floating
// groups, several viewports), rebuilds only when a command changes the layout, a splitter drag read
// back through ImGui's dirty flag into the session file, a tab closed with the mouse through
// "ith.window.close_panel", focus for Open Panel, and proportions kept across a display-scale change.

#include "ith/editor_ui/DockLayout.h"

#include "ith/config/Config.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)

using namespace ith;
using namespace ith::test::ui;

namespace {

const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        return result;
    }();
    return kTypes;
}

void removeTree(const std::string& path) {
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    if (!info || !info->has_value()) {
        return;
    }
    if ((*info)->kind == fs::FileKind::Directory) {
        if (Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(path)) {
            for (const fs::DirectoryEntry& entry : *entries) {
                removeTree(fs::joinPath(path, entry.name));
            }
        }
    }
    static_cast<void>(fs::remove(path));
}

// A project folder with a user configuration layer, an editor session over it with the P2 panels
// (each drawing a line and recording whether its window has focus), and a DockLayout.
struct DockFixture {
    DockFixture() {
        std::error_code error;
        const std::u8string base = std::filesystem::temp_directory_path(error).generic_u8string();
        REQUIRE_FALSE(error);
        root = fs::joinPath(std::string(base.begin(), base.end()), "ith-dock-test-" + Uuid::generate().toString());
        REQUIRE(fs::createDirectories(root).hasValue());
        config.emplace(
            std::vector<ConfigLayer>{{.name = "engine", .directory = "engine/config"},
                                     {.name = "user", .directory = fs::joinPath(root, "User"), .writable = true}});
        session.emplace(types(), EditorSessionOptions{.projectDirectory = root, .config = &*config});
        const auto panel = [this](std::string id, std::string name, DockPlace place, std::uint32_t instances = 1) {
            session->panels().add({.id = std::move(id),
                                   .displayName = std::move(name),
                                   .defaultDock = place,
                                   .maxInstances = instances,
                                   .draw = [this](std::string_view instance) {
                                       ImGui::TextUnformatted(instance.data(), instance.data() + instance.size());
                                       focused[std::string(instance)] = ImGui::IsWindowFocused();
                                   }});
        };
        panel("ith.viewport", "Viewport", DockPlace::Center, 4);
        panel("ith.outliner", "Outliner", DockPlace::Right);
        panel("ith.details", "Details", DockPlace::Right);
        panel("ith.content_browser", "Content Browser", DockPlace::Bottom);
        panel("ith.output_log", "Output Log", DockPlace::Bottom);
        dock.emplace(*session);
    }

    DockFixture(const DockFixture&) = delete;
    DockFixture& operator=(const DockFixture&) = delete;
    DockFixture(DockFixture&&) = delete;
    DockFixture& operator=(DockFixture&&) = delete;

    ~DockFixture() {
        dock.reset();
        session.reset();
        config.reset();
        removeTree(root);
    }

    LayoutSession& layouts() { return session->layouts(); }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            ui.frame([this] { dock->draw(0.0f); });
        }
    }

    // Loads the layout whose document data is `data` through a user layout file.
    EditorLayout load(std::string_view name, std::string_view data) {
        const std::string text = R"({"format": "ith.editor_layout", "version": 1, "data": )" + std::string(data) + "}";
        REQUIRE(
            fs::writeFileAtomic(fs::joinPath(root, "User/Layouts/" + std::string(name) + ".json"), text).hasValue());
        const Expected<LayoutLoad> loaded = layouts().load(name);
        REQUIRE_MESSAGE(loaded.hasValue(), (loaded ? std::string() : loaded.error().message));
        CHECK(loaded->warnings.empty());
        return layouts().current();
    }

    UiFixture ui;
    std::string root;
    std::optional<Config> config;
    std::optional<EditorSession> session;
    std::optional<DockLayout> dock;
    std::map<std::string, bool> focused;
};

// The same shape, tabs, active tabs and central stack; ratios and rectangles within `tolerance` (the
// dock tree works in whole pixels).
void checkSame(const LayoutTree& actual, const LayoutTree& expected, double tolerance) {
    REQUIRE(actual.nodes.size() == expected.nodes.size());
    for (std::size_t index = 0; index < actual.nodes.size(); ++index) {
        CAPTURE(index);
        const LayoutNode& a = actual.nodes[index];
        const LayoutNode& e = expected.nodes[index];
        CHECK(a.split == e.split);
        CHECK(a.axis == e.axis);
        CHECK(a.first == e.first);
        CHECK(a.second == e.second);
        CHECK(std::abs(a.ratio - e.ratio) <= tolerance);
        CHECK(a.tabs == e.tabs);
        CHECK(a.active == e.active);
        CHECK(a.central == e.central);
    }
}

void checkSame(const EditorLayout& actual, const EditorLayout& expected) {
    constexpr double kTolerance = 0.004;
    checkSame(actual.main, expected.main, kTolerance);
    REQUIRE(actual.floating.size() == expected.floating.size());
    for (std::size_t group = 0; group < actual.floating.size(); ++group) {
        CAPTURE(group);
        checkSame(actual.floating[group].tree, expected.floating[group].tree, kTolerance);
        CHECK(std::abs(actual.floating[group].rect.x - expected.floating[group].rect.x) <= kTolerance);
        CHECK(std::abs(actual.floating[group].rect.y - expected.floating[group].rect.y) <= kTolerance);
        CHECK(std::abs(actual.floating[group].rect.width - expected.floating[group].rect.width) <= kTolerance);
        CHECK(std::abs(actual.floating[group].rect.height - expected.floating[group].rect.height) <= kTolerance);
    }
}

constexpr std::string_view kUnrealShape = R"({
    "root": {"split": "left_right", "ratio": 0.78,
             "first": {"split": "top_bottom", "ratio": 0.7,
                       "first": {"tabs": ["ith.viewport.1"], "active": "ith.viewport.1", "central": true},
                       "second": {"tabs": ["ith.content_browser", "ith.output_log"], "active": "ith.output_log"}},
             "second": {"split": "top_bottom", "ratio": 0.4,
                        "first": {"tabs": ["ith.outliner"], "active": "ith.outliner"},
                        "second": {"tabs": ["ith.details"], "active": "ith.details"}}}})";

} // namespace

TEST_SUITE("editor_ui.dock_layout") {
    TEST_CASE("layouts applied to the dock tree read back the same") {
        DockFixture fixture;
        const std::vector<std::pair<std::string, std::string>> layouts = {
            {"Unreal", std::string(kUnrealShape)},
            {"CentralFirstRight", R"({"root": {"split": "left_right", "ratio": 0.3,
                "first": {"tabs": ["ith.outliner"]},
                "second": {"split": "top_bottom", "ratio": 0.6,
                           "first": {"tabs": ["ith.viewport.1", "ith.viewport.2"], "active": "ith.viewport.1", "central": true},
                           "second": {"tabs": ["ith.output_log"]}}}})"},
            {"EmptyCentral", R"({"root": {"split": "top_bottom", "ratio": 0.7,
                "first": {"tabs": [], "central": true}, "second": {"tabs": ["ith.output_log"]}}})"},
            {"Floating", R"({"root": {"split": "left_right", "ratio": 0.75,
                "first": {"tabs": ["ith.viewport.1"], "central": true}, "second": {"tabs": ["ith.details"]}},
                "floating": [{"rect": {"x": 0.25, "y": 0.2, "width": 0.4, "height": 0.5},
                              "node": {"tabs": ["ith.output_log", "ith.content_browser"], "active": "ith.content_browser"}}]})"},
        };
        for (const auto& [name, data] : layouts) {
            CAPTURE(name);
            const EditorLayout loaded = fixture.load(name, data);
            fixture.frames(3);
            fixture.dock->readBack();
            checkSame(fixture.layouts().current(), loaded);
            for (const std::string& instance : openPanels(loaded)) {
                CAPTURE(instance);
                CHECK(fixture.focused.contains(instance));
            }
            fixture.focused.clear();
        }
    }

    TEST_CASE("the dock tree is rebuilt only when a command changes the layout") {
        DockFixture fixture;
        static_cast<void>(fixture.load("Unreal", kUnrealShape));
        fixture.frames(3);
        const std::uint64_t rebuilds = fixture.dock->rebuilds();
        fixture.frames(5);
        CHECK(fixture.dock->rebuilds() == rebuilds);
        REQUIRE(fixture.session
                    ->execute("ith.window.close_panel", serialization::parseJson(R"({"panel": "ith.details"})").value())
                    .hasValue());
        fixture.frames(2);
        CHECK(fixture.dock->rebuilds() == rebuilds + 1);
        fixture.dock->readBack();
        CHECK_FALSE(isPanelOpen(fixture.layouts().current(), "ith.details"));
        CHECK(fixture.layouts().current().main.nodes.front().split);
    }

    TEST_CASE("Open Panel focuses an open panel") {
        DockFixture fixture;
        static_cast<void>(fixture.load("Unreal", kUnrealShape));
        fixture.frames(3);
        REQUIRE(fixture.session
                    ->execute("ith.window.open_panel",
                              serialization::parseJson(R"({"panel": "ith.content_browser"})").value())
                    .hasValue());
        fixture.frames(2);
        CHECK(fixture.focused["ith.content_browser"]);
        fixture.dock->readBack();
        const std::vector<LayoutNode>& nodes = fixture.layouts().current().main.nodes;
        CHECK(std::ranges::any_of(nodes, [](const LayoutNode& node) { return node.active == "ith.content_browser"; }));
    }

    TEST_CASE("a splitter dragged with the mouse is read back through ImGui's dirty flag and saved once") {
        DockFixture fixture;
        static_cast<void>(fixture.load("Bottom", R"({"root": {"split": "top_bottom", "ratio": 0.7,
            "first": {"tabs": ["ith.viewport.1"], "central": true}, "second": {"tabs": ["ith.output_log"]}}})"));
        fixture.frames(3);
        const std::string sessionFile = fs::joinPath(fixture.root, kSessionLayoutFile);
        REQUIRE(fs::remove(sessionFile).hasValue());

        // The splitter between the two nodes sits at 70% of the height.
        const float x = 640.0f;
        const float y = 0.7f * static_cast<float>(kHeight);
        fixture.ui.events({{.type = EventType::MouseMoved, .x = x, .y = y}});
        fixture.frames(2);
        fixture.ui.events(
            {{.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y}});
        fixture.frames(2);
        fixture.ui.events({{.type = EventType::MouseMoved, .x = x, .y = y - 144.0f}});
        fixture.frames(2);
        fixture.ui.events(
            {{.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y - 144.0f}});
        // ImGui marks its settings dirty and reports it after IniSavingRate (2 s) of frames.
        fixture.frames(150);
        const double ratio = fixture.layouts().current().main.nodes.front().ratio;
        CHECK(std::abs(ratio - 0.5) <= 0.01);
        const Expected<std::string> saved = fs::readFile(sessionFile, std::size_t{1} << 20);
        REQUIRE(saved.hasValue());
        CHECK(*saved == writeLayout(fixture.layouts().current()).valueOr(""));
        // Nothing moved since: no new write.
        REQUIRE(fs::remove(sessionFile).hasValue());
        fixture.dock->readBack();
        fixture.frames(150);
        CHECK_FALSE(fs::fileInfo(sessionFile).valueOr(std::optional<fs::FileInfo>{}).has_value());
    }

    TEST_CASE("a tab closed with the mouse runs Close Panel") {
        DockFixture fixture;
        static_cast<void>(fixture.load("Bottom", R"({"root": {"split": "top_bottom", "ratio": 0.7,
            "first": {"tabs": ["ith.viewport.1"], "central": true},
            "second": {"tabs": ["ith.output_log", "ith.content_browser"], "active": "ith.output_log"}}})"));
        fixture.frames(3);
        // A middle click on a docked tab closes it, as its close button does. The bottom node's tab
        // bar starts at 70% of the height; its first tab follows the node's window menu button.
        const float x = 3.0f * ImGui::GetFrameHeight();
        const float y = 0.7f * static_cast<float>(kHeight) + 0.5f * ImGui::GetFrameHeight() + 2.0f;
        fixture.ui.events({{.type = EventType::MouseMoved, .x = x, .y = y}});
        fixture.frames(2);
        fixture.ui.events(
            {{.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Middle, .clicks = 1, .x = x, .y = y}});
        fixture.frames(1);
        fixture.ui.events({{.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Middle, .x = x, .y = y}});
        fixture.frames(3);
        CHECK_FALSE(isPanelOpen(fixture.layouts().current(), "ith.output_log"));
        CHECK(isPanelOpen(fixture.layouts().current(), "ith.content_browser"));
        CHECK(fixture.session->commands().find("ith.window.close_panel") != nullptr);
    }

    TEST_CASE("a display-scale change keeps the proportions") {
        DockFixture fixture;
        const EditorLayout loaded = fixture.load("Unreal", kUnrealShape);
        fixture.frames(3);
        const std::uint64_t rebuilds = fixture.dock->rebuilds();
        fixture.ui.ui->setDisplayScale(1.5f);
        fixture.dock->displayScaleChanged();
        fixture.frames(3);
        CHECK(fixture.dock->rebuilds() == rebuilds + 1);
        fixture.dock->readBack();
        checkSame(fixture.layouts().current(), loaded);
    }
}
