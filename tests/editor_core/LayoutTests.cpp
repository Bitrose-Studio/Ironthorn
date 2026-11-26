// Panels and layouts (ADR-0040), headless: the panel registry and instance ids, the layout document
// (round trips, validation, the shipped Default.json and the built-in default), Open and Close
// Panel placement, and the layout session: the reset chain, names, Set as Default, Delete Layout,
// the session file written only when the layout changed, start-up, and that layout commands are not
// undoable.

#include "ith/config/Config.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/serialization/Json.h"

#include "AssertCapture.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::DockPlace;
using ith::EditorLayout;
using ith::EditorSession;
using ith::LayoutSession;
using ith::PanelRegistry;
using ith::serialization::Value;
using ith::test::editor::json;
using ith::test::editor::TempProject;
using ith::test::editor::types;
namespace fs = ith::fs;

std::vector<std::string> codesOf(const std::vector<ith::Error>& warnings) {
    std::vector<std::string> codes;
    for (const ith::Error& warning : warnings) {
        codes.push_back(warning.code);
    }
    return codes;
}

// A layout from the JSON text of a document's "data"; it must read.
EditorLayout layoutOf(std::string_view data, const PanelRegistry& panels, std::vector<ith::Error>* warnings = nullptr) {
    ith::Expected<EditorLayout> layout = ith::layoutFromValue(json(data), "test", panels, warnings);
    REQUIRE_MESSAGE(layout.hasValue(), (layout.hasValue() ? std::string() : layout.error().message));
    return layout ? std::move(*layout) : EditorLayout{};
}

// The error code of reading `data`, which must fail.
std::string failureOf(std::string_view data, const PanelRegistry& panels) {
    const ith::Expected<EditorLayout> layout = ith::layoutFromValue(json(data), "test", panels, nullptr);
    REQUIRE_FALSE(layout.hasValue());
    return layout ? std::string() : layout.error().code;
}

std::string documentOf(std::string_view data) {
    return R"({"format": "ith.editor_layout", "version": 1, "data": )" + std::string(data) + "}";
}

PanelRegistry& p2Panels() {
    static PanelRegistry panels;
    static const bool added = [] {
        ith::test::editor::addP2Panels(panels);
        panels.add({.id = "ith.palette", .displayName = "Palette", .defaultDock = DockPlace::Left});
        panels.add({.id = "ith.stats", .displayName = "Stats", .defaultDock = DockPlace::Floating});
        return true;
    }();
    static_cast<void>(added);
    return panels;
}

// The engine layer as shipped: the tests run in the repository root.
constexpr std::string_view kEngineConfig = "engine/config";

std::string readText(const std::string& path) {
    const ith::Expected<std::string> text = fs::readFile(path, std::size_t{1} << 20);
    return text ? *text : "<" + text.error().code + ">";
}

void writeText(const std::string& path, std::string_view text) {
    REQUIRE(fs::writeFileAtomic(path, text).hasValue());
}

} // namespace

TEST_SUITE("editor_core.panels") {
    TEST_CASE("panels resolve their instance ids and titles") {
        PanelRegistry panels;
        ith::test::editor::addP2Panels(panels);
        CHECK(panels.panels().size() == 5);
        CHECK(panels.panels().front()->id == "ith.viewport");
        CHECK(panels.resolve("ith.outliner")->number == 0);
        CHECK(panels.resolve("ith.viewport.3")->number == 3);
        CHECK(panels.resolve("ith.viewport.3")->panel == panels.find("ith.viewport"));
        for (const char* bad : {"ith.viewport", "ith.viewport.0", "ith.viewport.5", "ith.viewport.01", "ith.viewport.x",
                                "ith.outliner.1", "ith.unknown", ""}) {
            CAPTURE(bad);
            CHECK_FALSE(panels.resolve(bad).has_value());
        }
        CHECK(panels.title("ith.viewport.2") == "Viewport 2");
        CHECK(panels.title("ith.details") == "Details");
        CHECK(ith::instanceIdOf(*panels.find("ith.viewport"), 4) == "ith.viewport.4");
        CHECK(panels.instanceIds() == std::vector<std::string>{"ith.viewport.1", "ith.viewport.2", "ith.viewport.3",
                                                               "ith.viewport.4", "ith.outliner", "ith.details",
                                                               "ith.content_browser", "ith.output_log"});
    }

    TEST_CASE("registering a bad or repeated panel asserts and adds nothing") {
        PanelRegistry panels;
        ith::test::AssertCapture capture;
        panels.add({.id = "outliner", .displayName = "Outliner"});
        panels.add({.id = "ith.Outliner", .displayName = "Outliner"});
        panels.add({.id = "ith.outliner", .displayName = ""});
        panels.add({.id = "ith.outliner", .displayName = "Outliner", .maxInstances = 0});
        panels.add({.id = "ith.outliner", .displayName = "Outliner"});
        panels.add({.id = "ith.outliner", .displayName = "Outliner"});
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 5);
#endif
        CHECK(panels.panels().size() == 1);
    }
}

TEST_SUITE("editor_core.layout_document") {
    TEST_CASE("a layout writes and reads back the same, and Default.json is written as shipped") {
        const PanelRegistry& panels = p2Panels();
        const std::string data = R"({
            "root": {"split": "left_right", "ratio": 0.78,
                     "first": {"split": "top_bottom", "ratio": 0.7,
                               "first": {"tabs": ["ith.viewport.1", "ith.viewport.2"], "active": "ith.viewport.2", "central": true},
                               "second": {"tabs": ["ith.content_browser", "ith.output_log"], "active": "ith.content_browser"}},
                     "second": {"tabs": ["ith.outliner"], "active": "ith.outliner"}},
            "floating": [{"rect": {"x": 0.3, "y": 0.25, "width": 0.4, "height": 0.5},
                          "node": {"split": "top_bottom", "ratio": 0.5,
                                   "first": {"tabs": ["ith.details"], "active": "ith.details"},
                                   "second": {"tabs": ["ith.stats"], "active": "ith.stats"}}}],
            "editors": {"ith.material_editor": {"root": {"tabs": ["ith.material_editor.graph"]}}}})";
        std::vector<ith::Error> warnings;
        const EditorLayout layout = layoutOf(data, panels, &warnings);
        CHECK(warnings.empty());
        CHECK(ith::layoutToValue(layout) == json(data));
        CHECK(layout.main.nodes.size() == 5);
        CHECK(layout.floating.size() == 1);
        CHECK(ith::openPanels(layout) == std::vector<std::string>{"ith.viewport.1", "ith.viewport.2",
                                                                  "ith.content_browser", "ith.output_log",
                                                                  "ith.outliner", "ith.details", "ith.stats"});
        const ith::Expected<std::string> text = ith::writeLayout(layout);
        REQUIRE(text.hasValue());
        const ith::Expected<EditorLayout> again = ith::readLayout(*text, "again", panels, nullptr);
        REQUIRE(again.hasValue());
        CHECK(*again == layout);
        CHECK(ith::writeLayout(*again).valueOr("") == *text);

        const std::string shipped = readText(fs::joinPath(kEngineConfig, "Layouts/Default.json"));
        const ith::Expected<EditorLayout> defaultLayout = ith::readLayout(shipped, "Default.json", panels, &warnings);
        REQUIRE(defaultLayout.hasValue());
        CHECK(ith::writeLayout(*defaultLayout).valueOr("") == shipped);
    }

    TEST_CASE("unknown and repeated panels are dropped with warnings, and emptied stacks give way") {
        std::vector<ith::Error> warnings;
        const EditorLayout layout = layoutOf(R"({
            "root": {"split": "left_right", "ratio": 0.6,
                     "first": {"tabs": ["ith.viewport.1", "ith.gone"], "active": "ith.gone", "central": true},
                     "second": {"split": "top_bottom", "ratio": 0.5,
                                "first": {"tabs": ["ith.gone", "ith.viewport.9"]},
                                "second": {"tabs": ["ith.outliner", "ith.viewport.1"]}}},
            "floating": [{"rect": {"x": 0, "y": 0, "width": 0.5, "height": 0.5}, "node": {"tabs": ["ith.outliner"]}}]})",
                                             p2Panels(), &warnings);
        CHECK(codesOf(warnings) ==
              std::vector<std::string>{"editor.layout.unknown_panel", "editor.layout.unknown_panel",
                                       "editor.layout.duplicate_panel", "editor.layout.duplicate_panel"});
        CHECK(ith::layoutToValue(layout) == json(R"({
            "root": {"split": "left_right", "ratio": 0.6,
                     "first": {"tabs": ["ith.viewport.1"], "active": "ith.viewport.1", "central": true},
                     "second": {"tabs": ["ith.outliner"], "active": "ith.outliner"}},
            "floating": [], "editors": {}})"));
    }

    TEST_CASE("the central stack stays when emptied, and exactly one stack is central") {
        const EditorLayout emptied = layoutOf(R"({"root": {"split": "top_bottom", "ratio": 0.5,
            "first": {"tabs": ["ith.gone"], "central": true}, "second": {"tabs": ["ith.gone2"]}}})",
                                              p2Panels());
        CHECK(ith::layoutToValue(emptied) ==
              json(R"({"root": {"tabs": [], "central": true}, "floating": [], "editors": {}})"));

        const EditorLayout none = layoutOf(R"({"root": {"split": "top_bottom", "ratio": 0.5,
            "first": {"tabs": ["ith.outliner"]}, "second": {"tabs": ["ith.details"]}},
            "floating": [{"rect": {"x": 0, "y": 0, "width": 0.5, "height": 0.5}, "node": {"tabs": ["ith.stats"], "central": true}}]})",
                                           p2Panels());
        CHECK(none.main.nodes[1].central);
        CHECK_FALSE(none.main.nodes[2].central);
        CHECK_FALSE(none.floating.front().tree.nodes.front().central);

        const EditorLayout two = layoutOf(R"({"root": {"split": "top_bottom", "ratio": 0.5,
            "first": {"tabs": ["ith.outliner"], "central": true}, "second": {"tabs": ["ith.details"], "central": true}}})",
                                          p2Panels());
        CHECK(two.main.nodes[1].central);
        CHECK_FALSE(two.main.nodes[2].central);
    }

    TEST_CASE("active tabs, ratios and rectangles are fixed, clamped and rounded") {
        std::vector<ith::Error> warnings;
        const EditorLayout layout = layoutOf(R"({
            "root": {"split": "left_right", "ratio": 1.5,
                     "first": {"tabs": ["ith.viewport.1"], "central": true},
                     "second": {"split": "top_bottom", "ratio": 0.333333333,
                                "first": {"tabs": ["ith.outliner", "ith.details"], "active": "ith.nothing"},
                                "second": {"tabs": ["ith.output_log"], "unexpected": 1}}},
            "floating": [{"rect": {"x": 0.9, "y": -1, "width": 0.05, "height": 2}, "node": {"tabs": ["ith.stats"]}}],
            "theme": "dark"})",
                                             p2Panels(), &warnings);
        CHECK(codesOf(warnings) == std::vector<std::string>{"editor.layout.unknown_key", "editor.layout.unknown_key",
                                                            "editor.layout.clamped", "editor.layout.clamped"});
        CHECK(ith::layoutToValue(layout) == json(R"({
            "root": {"split": "left_right", "ratio": 0.98,
                     "first": {"tabs": ["ith.viewport.1"], "active": "ith.viewport.1", "central": true},
                     "second": {"split": "top_bottom", "ratio": 0.3333,
                                "first": {"tabs": ["ith.outliner", "ith.details"], "active": "ith.outliner"},
                                "second": {"tabs": ["ith.output_log"], "active": "ith.output_log"}}},
            "floating": [{"rect": {"x": 0.9, "y": 0, "width": 0.1, "height": 1}, "node": {"tabs": ["ith.stats"], "active": "ith.stats"}}],
            "editors": {}})"));
    }

    TEST_CASE("documents that don't have the layout's shape fail to load") {
        const PanelRegistry& panels = p2Panels();
        CHECK(failureOf("[]", panels) == "editor.layout.invalid");
        CHECK(failureOf("{}", panels) == "editor.layout.invalid");
        CHECK(
            failureOf(R"({"root": {"split": "sideways", "ratio": 0.5, "first": {"tabs": []}, "second": {"tabs": []}}})",
                      panels) == "editor.layout.invalid");
        CHECK(
            failureOf(
                R"({"root": {"split": "left_right", "ratio": "half", "first": {"tabs": []}, "second": {"tabs": []}}})",
                panels) == "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"split": "left_right", "ratio": 0.5, "first": {"tabs": []}}})", panels) ==
              "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"panels": []}})", panels) == "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"tabs": [1]}})", panels) == "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"tabs": [], "central": "yes"}})", panels) == "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"tabs": []}, "floating": [{"node": {"tabs": []}}]})", panels) ==
              "editor.layout.invalid");
        CHECK(failureOf(R"({"root": {"tabs": []}, "editors": []})", panels) == "editor.layout.invalid");

        std::string deep = R"({"tabs": []})";
        for (std::size_t level = 0; level < ith::kMaxLayoutDepth; ++level) {
            deep = R"({"split": "left_right", "ratio": 0.5, "first": )" + deep + R"(, "second": {"tabs": []}})";
        }
        const ith::Expected<EditorLayout> tooDeep =
            ith::readLayout(documentOf(R"({"root": )" + deep + "}"), "deep.json", panels, nullptr);
        REQUIRE_FALSE(tooDeep.hasValue());
        CHECK(tooDeep.error().message.find("too deep") != std::string::npos);

        std::string many = R"({"root": {"tabs": []}, "floating": [)";
        for (std::size_t count = 0; count < ith::kMaxLayoutNodes; ++count) {
            many += std::string(count == 0 ? "" : ", ") +
                    R"({"rect": {"x": 0, "y": 0, "width": 1, "height": 1}, "node": {"tabs": []}})";
        }
        CHECK(failureOf(many + "]}", panels) == "editor.layout.invalid");

        CHECK(ith::readLayout(R"({"format": "ith.config", "version": 1, "data": {}})", "x", panels, nullptr)
                  .error()
                  .code == "editor.layout.invalid");
        const auto newer = ith::readLayout(
            R"({"format": "ith.editor_layout", "version": 2, "data": {"root": {"tabs": []}}})", "x", panels, nullptr);
        REQUIRE_FALSE(newer.hasValue());
        CHECK(newer.error().message.find("newer") != std::string::npos);
        CHECK(ith::readLayout("{ broken", "x", panels, nullptr).error().code == "editor.layout.invalid");
    }

    TEST_CASE("the built-in default is ADR-0008's layout, and Default.json is it with today's panels") {
        EditorLayout full = ith::builtInDefaultLayout();
        std::vector<ith::Error> warnings;
        ith::normalizeLayout(full, p2Panels(), &warnings);
        CHECK(warnings.empty());
        CHECK(ith::layoutToValue(full) == json(R"({
            "root": {"split": "left_right", "ratio": 0.78,
                     "first": {"split": "top_bottom", "ratio": 0.7,
                               "first": {"tabs": ["ith.viewport.1"], "active": "ith.viewport.1", "central": true},
                               "second": {"tabs": ["ith.content_browser", "ith.output_log"], "active": "ith.content_browser"}},
                     "second": {"split": "top_bottom", "ratio": 0.4,
                                "first": {"tabs": ["ith.outliner"], "active": "ith.outliner"},
                                "second": {"tabs": ["ith.details"], "active": "ith.details"}}},
            "floating": [], "editors": {}})"));

        // The shipped file holds the panels registered today (ADR-0040 rule 18).
        PanelRegistry today;
        ith::test::editor::addEditorPanels(today);
        EditorLayout builtIn = ith::builtInDefaultLayout();
        ith::normalizeLayout(builtIn, today, nullptr);
        std::vector<ith::Error> shippedWarnings;
        const ith::Expected<EditorLayout> shipped = ith::readLayout(
            readText(fs::joinPath(kEngineConfig, "Layouts/Default.json")), "Default.json", today, &shippedWarnings);
        REQUIRE(shipped.hasValue());
        CHECK(shippedWarnings.empty());
        CHECK(*shipped == builtIn);
    }
}

TEST_SUITE("editor_core.panel_placement") {
    TEST_CASE("Open Panel docks at the declared place, next to panels of the same place") {
        const PanelRegistry& panels = p2Panels();
        EditorLayout layout;
        ith::openPanel(layout, "ith.output_log", panels);
        ith::openPanel(layout, "ith.content_browser", panels);
        ith::openPanel(layout, "ith.outliner", panels);
        ith::openPanel(layout, "ith.details", panels);
        ith::openPanel(layout, "ith.viewport.1", panels);
        ith::openPanel(layout, "ith.viewport.2", panels);
        ith::openPanel(layout, "ith.palette", panels);
        ith::openPanel(layout, "ith.stats", panels);
        CHECK(ith::layoutToValue(layout) == json(R"({
            "root": {"split": "left_right", "ratio": 0.2,
                     "first": {"tabs": ["ith.palette"], "active": "ith.palette"},
                     "second": {"split": "left_right", "ratio": 0.78,
                                "first": {"split": "top_bottom", "ratio": 0.7,
                                          "first": {"tabs": ["ith.viewport.1", "ith.viewport.2"], "active": "ith.viewport.2", "central": true},
                                          "second": {"tabs": ["ith.output_log", "ith.content_browser"], "active": "ith.content_browser"}},
                                "second": {"tabs": ["ith.outliner", "ith.details"], "active": "ith.details"}}},
            "floating": [{"rect": {"x": 0.3, "y": 0.25, "width": 0.4, "height": 0.5},
                          "node": {"tabs": ["ith.stats"], "active": "ith.stats"}}],
            "editors": {}})"));

        ith::test::AssertCapture capture;
        ith::openPanel(layout, "ith.stats", panels);
        ith::openPanel(layout, "ith.unknown", panels);
#ifdef ITH_ENABLE_ASSERTS
        CHECK(capture.count() == 2);
#endif
    }

    TEST_CASE("Close Panel moves the active tab and removes emptied stacks and groups") {
        const PanelRegistry& panels = p2Panels();
        EditorLayout layout = ith::builtInDefaultLayout();
        ith::normalizeLayout(layout, panels, nullptr);
        ith::openPanel(layout, "ith.stats", panels);
        CHECK(ith::closePanel(layout, "ith.content_browser"));
        CHECK_FALSE(ith::closePanel(layout, "ith.content_browser"));
        CHECK(ith::closePanel(layout, "ith.details"));
        CHECK(ith::closePanel(layout, "ith.stats"));
        CHECK(ith::closePanel(layout, "ith.viewport.1"));
        CHECK(ith::layoutToValue(layout) == json(R"({
            "root": {"split": "left_right", "ratio": 0.78,
                     "first": {"split": "top_bottom", "ratio": 0.7,
                               "first": {"tabs": [], "central": true},
                               "second": {"tabs": ["ith.output_log"], "active": "ith.output_log"}},
                     "second": {"tabs": ["ith.outliner"], "active": "ith.outliner"}},
            "floating": [], "editors": {}})"));
        CHECK(ith::activatePanel(layout, "ith.outliner"));
        CHECK_FALSE(ith::activatePanel(layout, "ith.details"));
    }
}

TEST_SUITE("editor_core.layout_session") {
    TEST_CASE("Reset Layout tries the user's default, the default below it, Default, then the built-in one") {
        const TempProject project;
        const std::string root = project.root();
        const auto dir = [&root](std::string_view layer) { return fs::joinPath(root, layer); };
        const auto settings = [](std::string_view name) {
            return R"({"format": "ith.config", "version": 1, "data": {"layouts": {"defaultName": ")" +
                   std::string(name) + R"("}}})";
        };
        ith::Config config({{.name = "engine", .directory = dir("engine")},
                            {.name = "project", .directory = dir("project")},
                            {.name = "user", .directory = dir("user"), .writable = true}});
        PanelRegistry panels;
        ith::test::editor::addP2Panels(panels);
        LayoutSession layouts(panels, &config, {});

        // Nothing in any layer: the built-in default.
        CHECK(layouts.reset().name.empty());
        EditorLayout builtIn = ith::builtInDefaultLayout();
        ith::normalizeLayout(builtIn, panels, nullptr);
        CHECK(layouts.current() == builtIn);

        const std::string engineLayout = documentOf(R"({"root": {"tabs": ["ith.viewport.1"], "central": true}})");
        writeText(fs::joinPath(dir("engine"), "Layouts/Default.json"), engineLayout);
        writeText(fs::joinPath(dir("project"), "Layouts/Studio.json"),
                  documentOf(R"({"root": {"tabs": ["ith.outliner", "ith.gone"], "central": true}})"));
        writeText(fs::joinPath(dir("project"), "Editor.json"), settings("Studio"));
        writeText(fs::joinPath(dir("user"), "Editor.json"), settings("Mine"));
        config.reload();

        // The user's "Mine" is missing: the project's "Studio", with its warning.
        const ith::LayoutLoad studio = layouts.reset();
        CHECK(studio.name == "Studio");
        CHECK(codesOf(studio.warnings) == std::vector<std::string>{"editor.layout.unknown_panel"});
        CHECK(ith::openPanels(layouts.current()) == std::vector<std::string>{"ith.outliner"});

        // A broken "Studio": the engine's Default.
        writeText(fs::joinPath(dir("project"), "Layouts/Studio.json"), "{ broken");
        CHECK(layouts.reset().name == "Default");
        CHECK(ith::openPanels(layouts.current()) == std::vector<std::string>{"ith.viewport.1"});

        // The user's own default wins once it exists.
        writeText(fs::joinPath(dir("user"), "Layouts/Mine.json"),
                  documentOf(R"({"root": {"tabs": ["ith.details"], "central": true}})"));
        CHECK(layouts.reset().name == "Mine");
        const std::uint64_t generation = layouts.generation();
        CHECK(layouts.reset().name == "Mine");
        CHECK(layouts.generation() == generation + 1);
    }

    TEST_CASE("names are checked, matched without case, and engine and project layouts are reserved") {
        const TempProject project;
        ith::Config config = ith::test::editor::projectConfig(project.root());
        PanelRegistry panels;
        ith::test::editor::addEditorPanels(panels);
        LayoutSession layouts(panels, &config, {});
        static_cast<void>(layouts.reset());
        for (const char* bad : {"", " Lead", "Trail ", "a/b", "a.b", "dots..", "\xc3\xa9t\xc3\xa9",
                                "12345678901234567890123456789012345678901234567890123456789012345"}) {
            CAPTURE(bad);
            CHECK(layouts.saveAs(bad, false).error().code == "editor.layout.invalid_name");
        }
        CHECK(layouts.saveAs("My Layout_2-b", false).valueOr("") == "My Layout_2-b");
        CHECK(layouts.saveAs("MY LAYOUT_2-B", true).valueOr("") == "My Layout_2-b");
        CHECK(layouts.load("my layout_2-b").hasValue());
        CHECK(layouts.name() == "My Layout_2-b");
        CHECK(layouts.saveAs("DEFAULT", true).error().code == "editor.layout.reserved_name");
        const auto listed = layouts.list();
        REQUIRE(listed.hasValue());
        CHECK(listed->size() == 2);

        LayoutSession noConfig(panels, nullptr, {});
        CHECK(noConfig.saveAs("Mine", false).error().code == "editor.layout.no_config");
        CHECK(noConfig.load("Default").error().code == "editor.layout.no_config");
        CHECK(noConfig.reset().name.empty());
        CHECK(ith::openPanels(noConfig.current()) ==
              std::vector<std::string>{"ith.viewport.1", "ith.output_log", "ith.outliner", "ith.details"});
        CHECK(noConfig.list().valueOr(std::vector<ith::LayoutListing>{{}}).empty());
    }

    TEST_CASE("Set as Default writes the user's choice only when it differs from the layers below") {
        const TempProject project;
        ith::Config config = ith::test::editor::projectConfig(project.root());
        PanelRegistry panels;
        ith::test::editor::addEditorPanels(panels);
        LayoutSession layouts(panels, &config, {});
        static_cast<void>(layouts.reset());
        const std::string userSettings = fs::joinPath(project.root(), "User/Editor.json");

        CHECK(layouts.setDefault("default").valueOr("") == "Default");
        CHECK(readText(userSettings) == "<fs.not_found>");
        REQUIRE(layouts.saveAs("Mine", false).hasValue());
        CHECK(layouts.setDefault("mine").valueOr("") == "Mine");
        CHECK(readText(userSettings).find(R"("defaultName": "Mine")") != std::string::npos);
        CHECK(layouts.defaultName() == "Mine");
        CHECK(layouts.setDefault("Nothing").error().code == "editor.layout.not_found");

        writeText(fs::joinPath(project.root(), "User/Layouts/Broken.json"), "{ broken");
        CHECK(layouts.setDefault("Broken").error().code == "editor.layout.invalid");

        // Back to the engine's default: the user's choice goes.
        REQUIRE(layouts.setDefault("Default").hasValue());
        CHECK(readText(userSettings).find("defaultName") == std::string::npos);
        CHECK(layouts.defaultName() == "Default");
    }

    TEST_CASE("Delete Layout removes user layouts only, and the user's default with them") {
        const TempProject project;
        ith::Config config = ith::test::editor::projectConfig(project.root());
        PanelRegistry panels;
        ith::test::editor::addEditorPanels(panels);
        LayoutSession layouts(panels, &config, {});
        static_cast<void>(layouts.reset());
        REQUIRE(layouts.saveAs("Mine", false).hasValue());
        REQUIRE(layouts.saveAs("Other", false).hasValue());
        REQUIRE(layouts.setDefault("Mine").hasValue());

        CHECK(layouts.remove("Default").error().code == "editor.layout.reserved_name");
        CHECK(layouts.remove("Nothing").error().code == "editor.layout.not_found");
        CHECK(layouts.remove("other").valueOr("") == "Other");
        CHECK(layouts.defaultName() == "Mine");
        CHECK(layouts.remove("Mine").valueOr("") == "Mine");
        CHECK(layouts.defaultName() == "Default");
        CHECK(layouts.list().valueOr(std::vector<ith::LayoutListing>{}).size() == 1);
        CHECK(layouts.reset().name == "Default");
    }

    TEST_CASE("the session file is written when the layout changes, and start-up restores it") {
        const TempProject project;
        ith::Config config = ith::test::editor::projectConfig(project.root());
        PanelRegistry panels;
        ith::test::editor::addP2Panels(panels);
        const std::string sessionFile = fs::joinPath(project.root(), ith::kSessionLayoutFile);

        {
            LayoutSession layouts(panels, &config, sessionFile);
            int readBacks = 0;
            layouts.setReadBack([&readBacks] { ++readBacks; });
            CHECK(layouts.restore().name == "Default"); // no session file yet
            CHECK(readText(sessionFile) == ith::writeLayout(layouts.current()).valueOr(""));

            // The same layout again: nothing is written.
            REQUIRE(fs::remove(sessionFile).hasValue());
            const std::uint64_t changes = layouts.changes();
            layouts.observe(layouts.current());
            CHECK(layouts.changes() == changes);
            CHECK(readText(sessionFile) == "<fs.not_found>");

            // A change the user made: written, without a new generation.
            EditorLayout moved = layouts.current();
            moved.main.nodes.front().ratio = 0.612345;
            const std::uint64_t generation = layouts.generation();
            layouts.observe(moved);
            CHECK(layouts.generation() == generation);
            CHECK(layouts.changes() == changes + 1);
            CHECK(layouts.current().main.nodes.front().ratio == 0.6123);
            CHECK(readText(sessionFile).find("0.6123") != std::string::npos);

            REQUIRE(layouts.open("ith.viewport").hasValue());
            CHECK(layouts.takeFocusRequest() == std::optional<std::string>("ith.viewport.2"));
            CHECK_FALSE(layouts.takeFocusRequest().has_value());
            CHECK(layouts.generation() == generation + 1);
            CHECK(layouts.close("ith.viewport.2").valueOr(false));
            CHECK(layouts.close("ith.viewport.9").error().code == "editor.layout.unknown_panel");
            CHECK(readBacks == 2);
        }
        {
            LayoutSession layouts(panels, &config, sessionFile);
            const ith::LayoutLoad restored = layouts.restore();
            CHECK(restored.name.empty());
            CHECK(layouts.current().main.nodes.front().ratio == 0.6123);
            CHECK_FALSE(ith::isPanelOpen(layouts.current(), "ith.viewport.2"));
        }
        {
            writeText(sessionFile, "{ broken");
            LayoutSession layouts(panels, &config, sessionFile);
            CHECK(layouts.restore().name == "Default");
            CHECK(readText(sessionFile) == ith::writeLayout(layouts.current()).valueOr(""));
        }
    }

    TEST_CASE("multi-instance panels open their lowest closed instance until all are open") {
        PanelRegistry panels;
        ith::test::editor::addP2Panels(panels);
        LayoutSession layouts(panels, nullptr, {});
        for (const char* expected : {"ith.viewport.1", "ith.viewport.2", "ith.viewport.3", "ith.viewport.4"}) {
            const auto opened = layouts.open("ith.viewport");
            REQUIRE(opened.hasValue());
            CHECK(opened->instance == expected);
            CHECK(opened->opened);
        }
        CHECK(layouts.open("ith.viewport").error().code == "editor.layout.no_free_instance");
        REQUIRE(layouts.close("ith.viewport.2").hasValue());
        CHECK(layouts.open("ith.viewport").valueOr(ith::PanelOpened{}).instance == "ith.viewport.2");
        CHECK(layouts.open("ith.viewport.3").valueOr(ith::PanelOpened{}).opened == false);
    }

    TEST_CASE("layout commands make no undo entry, don't dirty the level and run while playing") {
        const TempProject project;
        ith::Config config = ith::test::editor::projectConfig(project.root());
        EditorSession session(types(), {.projectDirectory = project.root(), .config = &config});
        ith::test::editor::addEditorPanels(session.panels());
        static_cast<void>(session.layouts().restore());
        ith::test::editor::run(session, "ith.window.close_panel", R"({"panel": "ith.output_log"})");
        ith::test::editor::run(session, "ith.window.open_panel", R"({"panel": "ith.output_log"})");
        ith::test::editor::run(session, "ith.window.reset_layout");
        const Value history = ith::test::editor::run(session, "ith.query.history");
        CHECK(history.find("entries")->elements().empty());
        CHECK(ith::test::editor::run(session, "ith.query.document").find("dirty")->asBool() == false);
        CHECK(fs::fileInfo(fs::joinPath(project.root(), ith::kSessionLayoutFile))
                  .valueOr(std::optional<fs::FileInfo>{})
                  .has_value());

        session.document().setPlaying(true);
        ith::test::editor::run(session, "ith.window.close_panel", R"({"panel": "ith.output_log"})");
        session.document().setPlaying(false);
        ith::test::editor::fails(session, "ith.window.open_panel", R"({"panel": 3})", "editor.command.invalid_args");
        ith::test::editor::fails(session, "ith.window.save_layout_as", R"({"name": "x", "overwrite": 1})",
                                 "editor.command.invalid_args");
    }
}
