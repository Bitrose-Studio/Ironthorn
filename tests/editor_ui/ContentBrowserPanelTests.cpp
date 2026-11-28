// The Content Browser panel and the content drawer (ADR-0049 rules 4, 7 and 26, docs/specs/
// content-browser.md requirements 11, 12, 51, 53, 55, 60 and 61), headless on the RHI's null device
// and driven by mouse and key events through the platform backend and the engine keymap: clicks
// with Ctrl and Shift, empty space, arrow keys in a four-column grid, double-clicks that open
// folders and levels, the unsaved-changes question, the drawer's focus, Escape, outside clicks and
// height, the search box keeping its keys, only the visible items drawn and nothing rebuilt per frame;
// and a click with a key in one batch of events, menus and dialogs keeping Escape, the list's header
// sorting with "##" in names, and the drawer closing when the panel is in the layout. The file
// operations (spec 3.7 to 3.13, 3.16): the inline rename field (F2, Add > New Folder, a tree
// folder's menu; Enter, a refused name, leaving the field, Escape), the delete dialog (its text, Cancel
// focused, Enter and Escape cancelling, the drawer staying open), the import conflicts one file at a
// time with Apply to all, drags onto folders and a refused one, operating-system drops on folder
// items, tree folders, path bar segments, empty space, the drawer and outside, the open-file dialog
// through the UiHost (filters, its results, a dialog that can't be shown), the context menus, the
// empty-folder text and the status bar's Content Drawer button. What a frame drew is read from ImGui's
// log buffer.

#include "ith/editor_ui/ContentBrowserPanel.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/config/Config.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/EditorShell.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Uuid.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"

#include "EditorUiTestSupport.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <filesystem>
#include <memory>
#include <optional>
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

// The panel's fixed window: wide enough for a grid of four columns.
constexpr float kPanelWidth = 600.0f;
constexpr float kPanelHeight = 400.0f;

// A project folder in the temporary folder, removed afterwards.
struct TempFolder {
    TempFolder() {
        std::error_code error;
        const std::u8string base = std::filesystem::temp_directory_path(error).generic_u8string();
        root = fs::joinPath(std::string(base.begin(), base.end()), "ith-content-ui-" + Uuid::generate().toString());
        REQUIRE(fs::createDirectories(fs::joinPath(root, "Content")).hasValue());
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    TempFolder(TempFolder&&) = delete;
    TempFolder& operator=(TempFolder&&) = delete;

    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(root.begin(), root.end())), error);
    }

    std::string root;
};

// A session over a project with an asset database, the engine keymap's consumer under the UI, and
// the Content Browser drawn in a fixed window when `panelOpen`, then the drawer.
struct ContentFixture {
    ContentFixture()
        : assets(folder.root, importers),
          session(types(), {.projectDirectory = folder.root, .assets = &assets}),
          keymap(engineKeymap()),
          shortcuts(session, keymap),
          panel(session, &host),
          ui({.host = &host, .content = &session.content(), .contentBrowser = &panel}, &shortcuts) {
        registerContentBrowser(session, panel);
    }

    Keymap engineKeymap() const {
        const Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<Error> warnings;
        return Keymap::load(config, session.commands(), warnings);
    }

    void asset(std::string_view path, std::string_view data = "{}") const {
        write("Content/" + std::string(path), R"({"format": "ith.material", "version": 1, "id": ")" +
                                                  Uuid::generate().toString() + R"(", "data": )" + std::string(data) +
                                                  "}");
    }

    // Writes `text` at `path`, relative to the project folder; returns the file's full path.
    std::string write(const std::string& path, const std::string& text) const {
        const std::string full = fs::joinPath(folder.root, path);
        REQUIRE(fs::writeFileAtomic(full, text, {.durability = fs::Durability::Atomic}).hasValue());
        return full;
    }

    // A native asset outside Content, as a file from elsewhere; returns its full path.
    std::string outside(std::string_view name) const {
        return write("Outside/" + std::string(name), R"({"format": "ith.material", "version": 1, "id": ")" +
                                                         Uuid::generate().toString() + R"(", "data": {}})");
    }

    bool exists(std::string_view contentPath) const {
        const Expected<std::optional<fs::FileInfo>> info =
            fs::fileInfo(fs::joinPath(folder.root, "Content/" + std::string(contentPath)));
        return info && info->has_value();
    }

    // An event the platform delivers and Input doesn't route (drops, the file dialog's results),
    // handed to the UI as the editor does, then one frame.
    void platform(Event event) {
        if (event.window == kNoWindow) {
            event.window = kWindow;
        }
        ui.ui->handleEvent(event);
        frames(1);
    }

    // Drags files from the operating system to (`x`, `y`) and drops them there.
    void osDrop(const std::vector<std::string>& files, float x, float y) {
        platform({.type = EventType::DropBegin});
        platform({.type = EventType::DropPosition, .x = x, .y = y});
        for (const std::string& file : files) {
            ui.ui->handleEvent({.type = EventType::DropFile, .window = kWindow, .x = x, .y = y, .text = file});
        }
        platform({.type = EventType::DropComplete});
    }

    // Presses at (`x`, `y`), moves to (`toX`, `toY`) in steps across frames and releases there.
    void drag(float x, float y, float toX, float toY) {
        frames(30); // no double-click with an earlier click
        move(x, y);
        step({.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y});
        constexpr int kSteps = 6;
        for (int i = 1; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / kSteps;
            move(x + (toX - x) * t, y + (toY - y) * t);
        }
        frames(1);
        step({.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = toX, .y = toY});
        frames(1);
    }

    // Types `text` into the focused text field.
    void type(std::string_view text) { step({.type = EventType::TextInput, .text = text}); }

    UiRect control(std::string_view label) const {
        const std::optional<UiRect> found = panel.controlRect(label);
        REQUIRE_MESSAGE(found.has_value(), std::string(label));
        return *found;
    }

    void clickControl(std::string_view label) {
        const UiRect r = control(label);
        click(r.centerX(), r.centerY());
    }

    void rightClick(float x, float y) {
        move(x, y);
        step({.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Right, .clicks = 1, .x = x, .y = y});
        step({.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Right, .x = x, .y = y});
        frames(1);
    }

    // The text one frame of the fixture draws (EditorUiTestSupport.h).
    std::string drawnText() {
        return ith::test::ui::drawnText(ui, [this] { draw(); });
    }

    const ContentPrompt* prompt() const { return session.content().prompt(); }

    serialization::Value run(std::string_view id, std::string_view args = "{}") {
        auto parsed = serialization::parseJson(args);
        REQUIRE(parsed.hasValue());
        auto result = session.execute(id, *parsed);
        REQUIRE_MESSAGE(result.hasValue(), (result ? std::string() : result.error().message));
        return std::move(*result);
    }

    void draw() {
        if (shell) {
            // The editor's own frame: the shell, with its status bar, then the drawer.
            if (!dock.has_value()) {
                dock.emplace(session);
            }
            drawEditorShell(session, keymap, shellState, *dock);
            panel.drawDrawer();
            return;
        }
        if (panelOpen) {
            // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
            constexpr ImGuiWindowFlags kFixed =
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight));
            if (ImGui::Begin("Content Browser###test.content", nullptr, kFixed)) {
                session.panels().find("ith.content_browser")->draw("ith.content_browser");
            }
            ImGui::End();
        }
        // Another window, to move the focus away from the browser.
        ImGui::SetNextWindowPos(ImVec2(kPanelWidth + 20.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f));
        if (ImGui::Begin("Other###test.other")) {
            ImGui::TextUnformatted("elsewhere");
        }
        ImGui::End();
        panel.drawDrawer();
    }

    void frames(int count) {
        for (int i = 0; i < count; ++i) {
            ui.frame([this] { draw(); });
        }
    }

    void step(const Event& event) {
        ui.events({event});
        frames(1);
    }

    void move(float x, float y) { step({.type = EventType::MouseMoved, .x = x, .y = y}); }

    void click(float x, float y, KeyModifiers modifiers = KeyModifiers::None) {
        move(x, y);
        step({.type = EventType::MouseButtonDown,
              .modifiers = modifiers,
              .mouseButton = MouseButton::Left,
              .clicks = 1,
              .x = x,
              .y = y});
        step({.type = EventType::MouseButtonUp,
              .modifiers = modifiers,
              .mouseButton = MouseButton::Left,
              .x = x,
              .y = y});
        frames(1);
    }

    void doubleClick(float x, float y) {
        frames(30); // longer than ImGui's double-click time: no click before counts
        move(x, y);
        for (int i = 0; i < 2; ++i) {
            step({.type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y});
            step({.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y});
        }
        frames(1);
    }

    void key(Key pressed, KeyModifiers modifiers = KeyModifiers::None, char32_t layoutKey = 0) {
        step({.type = EventType::KeyDown, .key = pressed, .layoutKey = layoutKey, .modifiers = modifiers});
        step({.type = EventType::KeyUp, .key = pressed, .layoutKey = layoutKey, .modifiers = modifiers});
    }

    ContentKey keyAt(std::string_view path) const {
        const ContentItem* item = session.content().findByPath(path);
        REQUIRE_MESSAGE(item != nullptr, std::string(path));
        return keyOf(*item);
    }

    UiRect rect(std::string_view path) const {
        const std::optional<UiRect> found = panel.itemRect(keyAt(path));
        REQUIRE_MESSAGE(found.has_value(), std::string(path));
        return *found;
    }

    void clickItem(std::string_view path, KeyModifiers modifiers = KeyModifiers::None) {
        const UiRect r = rect(path);
        click(r.centerX(), r.centerY(), modifiers);
    }

    std::vector<std::string> selected() const {
        std::vector<std::string> paths;
        for (const ContentKey& key : session.content().selection()) {
            const ContentItem* item = session.content().find(key);
            paths.push_back(item != nullptr ? item->path : "?");
        }
        return paths;
    }

    TempFolder folder;
    ImporterRegistry importers;
    AssetDatabase assets;
    EditorSession session;
    Keymap keymap;
    KeymapConsumer shortcuts;
    RecordingHost host;
    // Before the UI, which hands it the drops and the dialog's results.
    ContentBrowserPanel panel;
    UiFixture ui;
    // The shell's dock space, made for the shell only.
    std::optional<DockLayout> dock;
    EditorShellState shellState;
    bool panelOpen = true;
    // Draws the editor shell instead of the fixed test windows.
    bool shell = false;
};

} // namespace

TEST_SUITE("editor_ui.content_browser") {
    TEST_CASE("the panel registers at the bottom; without a project it says so") {
        ContentFixture fixture;
        const PanelDesc* desc = fixture.session.panels().find("ith.content_browser");
        REQUIRE(desc != nullptr);
        CHECK(desc->displayName == "Content Browser");
        CHECK(desc->defaultDock == DockPlace::Bottom);
        CHECK(desc->maxInstances == 1);
        fixture.frames(2); // before a scan: an empty root
        CHECK(fixture.panel.itemsDrawn() == 0);

        EditorSession bare(types());
        ContentBrowserPanel lonely(bare);
        fixture.ui.frame([&lonely] {
            if (ImGui::Begin("Bare###test.bare")) {
                lonely.draw();
            }
            ImGui::End();
            lonely.drawDrawer();
        });
        CHECK(lonely.itemsDrawn() == 0);
        CHECK_FALSE(lonely.searchRect().has_value());
    }

    TEST_CASE("clicks select; Ctrl toggles, Shift selects the range, empty space clears (requirement 11)") {
        ContentFixture fixture;
        for (const std::string_view name : {"A", "B", "C", "D", "E", "F"}) {
            fixture.asset("S/" + std::string(name) + ".ithasset");
        }
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);

        fixture.clickItem("S/B.ithasset");
        CHECK(fixture.selected() == std::vector<std::string>{"S/B.ithasset"});
        fixture.clickItem("S/D.ithasset", KeyModifiers::Control);
        CHECK(fixture.selected() == std::vector<std::string>{"S/B.ithasset", "S/D.ithasset"});
        fixture.clickItem("S/F.ithasset", KeyModifiers::Shift); // from the anchor D
        CHECK(fixture.selected() == std::vector<std::string>{"S/D.ithasset", "S/E.ithasset", "S/F.ithasset"});
        CHECK(*fixture.session.content().anchor() == fixture.keyAt("S/D.ithasset"));
        fixture.clickItem("S/A.ithasset", KeyModifiers::Control | KeyModifiers::Shift); // D back to A, added
        CHECK(fixture.selected().size() == 6);
        CHECK(*fixture.session.content().primary() == fixture.keyAt("S/A.ithasset"));

        // Empty space below the tiles clears; the level's selection and history stay as they were.
        const UiRect last = fixture.rect("S/F.ithasset");
        fixture.click(last.x + last.width * 0.5f, last.y + last.height + 20.0f);
        CHECK(fixture.session.content().selection().empty());
        CHECK(fixture.session.document().history().cursor() == 0);
        CHECK(fixture.session.document().selection().entities().empty());
        // The browser has the keyboard focus after a click in it, and loses it to another window.
        CHECK(fixture.session.content().keyboardFocus());
        fixture.click(kPanelWidth + 100.0f, 50.0f);
        CHECK_FALSE(fixture.session.content().keyboardFocus());
    }

    TEST_CASE("arrow keys in a four-column grid move by one and by a row; Shift extends; Home and End "
              "(requirement 12)") {
        ContentFixture fixture;
        for (int i = 0; i < 12; ++i) {
            fixture.asset("G/Item" + std::to_string(10 + i) + ".ithasset");
        }
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "G"})");
        fixture.frames(3);
        REQUIRE(fixture.panel.gridColumns() == 4);
        const auto focus = [&fixture] {
            const std::optional<ContentKey>& key = fixture.session.content().focusItem();
            REQUIRE(key.has_value());
            return fixture.session.content().find(*key)->label;
        };

        fixture.clickItem("G/Item10.ithasset");
        fixture.key(Key::Right);
        CHECK(focus() == "Item11");
        fixture.key(Key::Down);
        CHECK(focus() == "Item15");
        fixture.key(Key::Left);
        CHECK(focus() == "Item14");
        CHECK(fixture.selected() == std::vector<std::string>{"G/Item14.ithasset"});
        fixture.key(Key::Down, KeyModifiers::Shift); // from the anchor Item14, which the arrow selected, to Item18
        CHECK(focus() == "Item18");
        CHECK(fixture.selected().size() == 5);
        fixture.key(Key::End);
        CHECK(focus() == "Item21");
        fixture.key(Key::Down); // stays at the last
        CHECK(focus() == "Item21");
        fixture.key(Key::Home, KeyModifiers::Shift);
        CHECK(focus() == "Item10");
        fixture.key(Key::Up);
        CHECK(focus() == "Item10");
        CHECK(fixture.selected() == std::vector<std::string>{"G/Item10.ithasset"});

        // Ctrl + A (keymap, Content context) and Escape (dismiss) with the focus in the browser.
        fixture.key(Key::A, KeyModifiers::Control, U'a');
        CHECK(fixture.selected().size() == 12);
        fixture.key(Key::Escape);
        CHECK(fixture.selected().empty());

        // List mode: Up and Down move by one.
        fixture.run("ith.content.set_view", R"({"mode": "list"})");
        fixture.frames(2);
        fixture.clickItem("G/Item12.ithasset");
        fixture.key(Key::Down);
        CHECK(focus() == "Item13");
        CHECK(fixture.panel.gridColumns() == 1);
    }

    TEST_CASE("typing in the search box edits it and keeps its keys from the shortcuts (requirement 55)") {
        ContentFixture fixture;
        fixture.asset("Rock.ithasset");
        fixture.asset("Tree.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        fixture.clickItem("Rock.ithasset");
        const std::optional<UiRect> search = fixture.panel.searchRect();
        REQUIRE(search.has_value());
        fixture.click(search->centerX(), search->centerY());
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.step({.type = EventType::TextInput, .text = "tr"});
        fixture.frames(1);
        CHECK(fixture.session.content().search() == "tr");
        CHECK(fixture.session.content().shown().size() == 1);
        // Ctrl + A selects the text, not the items; Escape leaves the field, not the selection.
        fixture.key(Key::A, KeyModifiers::Control, U'a');
        CHECK(fixture.session.content().selection().empty());

        // Ctrl + F from the asset view moves the focus to the search box.
        fixture.key(Key::Escape);
        fixture.frames(1);
        fixture.clickItem("Tree.ithasset");
        CHECK_FALSE(fixture.ui.ui->wantsTextInput());
        fixture.key(Key::F, KeyModifiers::Control, U'f');
        fixture.frames(1);
        CHECK(fixture.ui.ui->wantsTextInput());
    }

    TEST_CASE("a double-click goes into a folder or opens a level, asking about unsaved changes (requirement 51)") {
        ContentFixture fixture;
        fixture.run("ith.file.save_as", R"({"path": "Content/Levels/One.ithmap"})");
        fixture.run("ith.file.new_level");
        fixture.run("ith.file.save_as", R"({"path": "Content/Levels/Two.ithmap"})");
        fixture.frames(3);
        const UiRect levels = fixture.rect("Levels");
        fixture.doubleClick(levels.centerX(), levels.centerY());
        CHECK(fixture.session.content().currentFolder() == "Levels");

        fixture.frames(1);
        const UiRect one = fixture.rect("Levels/One.ithmap");
        fixture.doubleClick(one.centerX(), one.centerY());
        CHECK(fixture.session.document().path() == "Content/Levels/One.ithmap");

        // With unsaved changes the panel asks first; Escape cancels and nothing opens.
        fixture.run("ith.entity.create", R"({"id": ")" + Uuid(0xED, 1).toString() + R"("})");
        const UiRect two = fixture.rect("Levels/Two.ithmap");
        fixture.doubleClick(two.centerX(), two.centerY());
        fixture.frames(1);
        CHECK(fixture.panel.asking());
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK_FALSE(fixture.panel.asking());
        CHECK(fixture.session.document().path() == "Content/Levels/One.ithmap");

        // Enter asks too (the prompt of ith.content.open).
        fixture.clickItem("Levels/Two.ithmap");
        fixture.key(Key::Enter);
        CHECK(fixture.panel.asking());
        fixture.key(Key::Escape);
        CHECK(fixture.session.document().dirty());
    }

    TEST_CASE("the drawer: focus on open, Escape, a click outside, a level opened, its height (requirements 53, 54)") {
        ContentFixture fixture;
        fixture.panelOpen = false;
        fixture.asset("Props/Crate.ithasset");
        fixture.run("ith.file.save_as", R"({"path": "Content/Main.ithmap"})");
        const serialization::Value layout = fixture.run("ith.query.layout");

        // Opened: over the bottom of the window, 40 % of it, with the keyboard focus.
        CHECK(fixture.run("ith.content.toggle_drawer").find("target")->asString() == "drawer");
        fixture.frames(3);
        REQUIRE(fixture.panel.drawerRect().has_value());
        CHECK(fixture.panel.drawerRect()->height == doctest::Approx(static_cast<float>(kHeight) * 0.4f));
        CHECK(fixture.session.content().keyboardFocus());
        CHECK(fixture.run("ith.query.layout") == layout);

        // Escape with nothing selected closes it.
        fixture.key(Key::Escape);
        CHECK_FALSE(fixture.session.content().drawerOpen());
        fixture.frames(1);
        CHECK_FALSE(fixture.panel.drawerRect().has_value());
        CHECK_FALSE(fixture.session.content().keyboardFocus());

        // A click outside closes it; a click inside doesn't.
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(2);
        const UiRect drawer = *fixture.panel.drawerRect();
        fixture.click(drawer.centerX(), drawer.y + drawer.height - 30.0f);
        CHECK(fixture.session.content().drawerOpen());
        fixture.click(drawer.centerX(), drawer.y - 100.0f);
        CHECK_FALSE(fixture.session.content().drawerOpen());

        // While its unsaved-changes question is open, an outside click doesn't close it.
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(2);
        fixture.run("ith.entity.create", R"({"id": ")" + Uuid(0xED, 1).toString() + R"("})");
        const UiRect level = fixture.rect("Main.ithmap");
        fixture.doubleClick(level.centerX(), level.centerY());
        fixture.frames(1);
        REQUIRE(fixture.panel.asking());
        fixture.click(10.0f, 10.0f);
        CHECK(fixture.session.content().drawerOpen());
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK_FALSE(fixture.panel.asking());
        CHECK(fixture.session.content().drawerOpen());
        // The question kept its Escape: Select None didn't run on the level.
        CHECK_FALSE(fixture.session.document().selection().entities().empty());

        // Opening a level from it closes it.
        fixture.run("ith.edit.undo");
        fixture.run("ith.file.save");
        CHECK_FALSE(fixture.session.document().dirty());
        fixture.frames(1);
        const UiRect again = fixture.rect("Main.ithmap");
        fixture.doubleClick(again.centerX(), again.centerY());
        CHECK_FALSE(fixture.session.content().drawerOpen());

        // The height is clamped to 150 pixels and to 80 % of the window.
        fixture.session.content().setDrawerHeight(20.0f);
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(2);
        CHECK(fixture.panel.drawerRect()->height == doctest::Approx(150.0f));
        fixture.session.content().setDrawerHeight(5000.0f);
        fixture.frames(1);
        CHECK(fixture.panel.drawerRect()->height == doctest::Approx(static_cast<float>(kHeight) * 0.8f));
        CHECK(fixture.run("ith.query.layout") == layout);
    }

    TEST_CASE("only the visible items are drawn, and an idle frame rebuilds nothing (requirements 60, 61)") {
        ContentFixture fixture;
        constexpr int kItems = 2000;
        for (int i = 0; i < kItems; ++i) {
            fixture.asset("Many/Asset" + std::to_string(i) + ".ithasset");
        }
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "Many"})");
        fixture.frames(3);
        CHECK(fixture.session.content().shown().size() == kItems);
        CHECK(fixture.panel.itemsDrawn() > 0);
        CHECK(fixture.panel.itemsDrawn() < 100);
        const std::uint64_t builds = fixture.session.content().viewBuilds();
        const std::uint64_t generation = fixture.session.content().generation();
        fixture.frames(5);
        CHECK(fixture.session.content().viewBuilds() == builds);
        CHECK(fixture.session.content().generation() == generation);

        fixture.run("ith.content.set_view", R"({"mode": "list"})");
        fixture.frames(3);
        CHECK(fixture.panel.itemsDrawn() > 0);
        CHECK(fixture.panel.itemsDrawn() < 100);
        // A search over every item shows them all, still drawn by the visible rows only.
        fixture.run("ith.content.navigate", R"({"folder": ""})");
        fixture.run("ith.content.set_filter", R"({"search": "asset"})");
        fixture.frames(2);
        CHECK(fixture.session.content().shown().size() == kItems);
        CHECK(fixture.panel.itemsDrawn() < 100);
    }

    TEST_CASE("a click into the browser and a key in the same batch reach the browser, not the level") {
        ContentFixture fixture;
        fixture.asset("S/A.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.run("ith.entity.create", R"({"id": ")" + Uuid(0xED, 1).toString() + R"("})");
        fixture.frames(3);
        fixture.click(kPanelWidth + 100.0f, 50.0f); // the focus elsewhere
        REQUIRE_FALSE(fixture.session.content().keyboardFocus());
        const UiRect tile = fixture.rect("S/A.ithasset");
        fixture.move(tile.centerX(), tile.centerY());
        // Press, release and Delete before any UI frame: the press alone decides where Delete goes.
        fixture.ui.events({Event{.type = EventType::MouseButtonDown,
                                 .mouseButton = MouseButton::Left,
                                 .clicks = 1,
                                 .x = tile.centerX(),
                                 .y = tile.centerY()},
                           Event{.type = EventType::MouseButtonUp,
                                 .mouseButton = MouseButton::Left,
                                 .x = tile.centerX(),
                                 .y = tile.centerY()},
                           Event{.type = EventType::KeyDown, .key = Key::Delete}});
        CHECK(fixture.session.content().keyboardFocus());
        fixture.frames(2);
        CHECK(fixture.session.document().world().entityCount() == 1);
    }

    TEST_CASE("an open menu or dialog keeps Escape from the shortcuts") {
        ContentFixture fixture;
        fixture.asset("S/A.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        fixture.clickItem("S/A.ithasset");
        REQUIRE(fixture.selected().size() == 1);
        const std::optional<UiRect> filter = fixture.panel.filterRect();
        REQUIRE(filter.has_value());
        fixture.click(filter->centerX(), filter->centerY());
        // Escape closes the menu, and only the menu.
        fixture.key(Key::Escape);
        CHECK(fixture.selected().size() == 1);
        fixture.frames(1);
        fixture.clickItem("S/A.ithasset");
        fixture.key(Key::Escape); // no menu now: ith.content.dismiss
        CHECK(fixture.selected().empty());
    }

    TEST_CASE("the list sorts by a click on its Name and Type headers; labels with ## draw whole") {
        ContentFixture fixture;
        fixture.asset("L/Zed.ithasset");
        fixture.asset("L/A##B.ithasset");
        fixture.run("ith.file.save_as", R"({"path": "Content/L/Map.ithmap"})"); // a Level, shown at once
        fixture.run("ith.content.navigate", R"({"folder": "L"})");
        fixture.run("ith.content.set_view", R"({"mode": "list"})");
        fixture.frames(3);
        const UiRect first = fixture.rect("L/A##B.ithasset");
        CHECK(fixture.session.content().shown().front()->label == "A##B");
        fixture.clickItem("L/A##B.ithasset");
        CHECK(fixture.selected() == std::vector<std::string>{"L/A##B.ithasset"});

        // The header row is right above the first row; Type is the fixed column at the right.
        const float header = first.y - first.height * 0.5f - 2.0f;
        fixture.click(first.x + first.width - 30.0f, header);
        fixture.frames(1);
        CHECK(fixture.session.content().view().sortBy == ContentSortKey::Type);
        CHECK_FALSE(fixture.session.content().view().descending);
        // Level before Other: the level first, the materials (an unknown type) after it.
        CHECK(fixture.session.content().shown().front()->label == "Map");
        fixture.frames(30); // no double-click with the next header click
        fixture.click(first.x + first.width - 30.0f, header);
        fixture.frames(1);
        CHECK(fixture.session.content().view().descending);
        fixture.frames(30);
        fixture.click(first.x + 20.0f, header);
        fixture.frames(1);
        CHECK(fixture.session.content().view().sortBy == ContentSortKey::Name);
        CHECK(fixture.session.content().shown().front()->label == "A##B");
    }

    TEST_CASE("the drawer closes when the panel is in the layout: one browser on screen at a time") {
        ContentFixture fixture;
        fixture.panelOpen = false;
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(2);
        REQUIRE(fixture.panel.drawerRect().has_value());
        fixture.run("ith.window.open_panel", R"({"panel": "ith.content_browser"})");
        fixture.frames(1);
        CHECK_FALSE(fixture.session.content().drawerOpen());
        // With the panel in the layout, Ctrl + Space brings the panel forward instead.
        CHECK(fixture.run("ith.content.toggle_drawer").find("target")->asString() == "panel");
        fixture.frames(1);
        CHECK_FALSE(fixture.session.content().drawerOpen());
    }

    TEST_CASE("an empty folder says how to fill it (spec 3.1)") {
        ContentFixture fixture;
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.new_folder", R"({"name": "Empty"})");
        fixture.run("ith.content.navigate", R"({"folder": "Empty"})");
        fixture.frames(2);
        CHECK(fixture.panel.itemsDrawn() == 0);
        const std::string drawn = fixture.drawnText();
        CHECK(drawn.find("This folder is empty. Drop files here or use Import.") != std::string::npos);
    }

    TEST_CASE("F2 opens the inline rename field: Enter renames, an invalid name keeps it open, Escape cancels "
              "(spec 3.8)") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.asset("S/Tree.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        const ContentKey rock = fixture.keyAt("S/Rock.ithasset");
        fixture.clickItem("S/Rock.ithasset");
        fixture.key(Key::F2);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.prompt()->kind == ContentPromptKind::Rename);
        CHECK(fixture.panel.controlRect("rename").has_value());
        REQUIRE(fixture.ui.ui->wantsTextInput());
        // The field holds the name without its extension, selected: typing replaces it.
        fixture.type("Stone");
        fixture.key(Key::Enter);
        fixture.frames(1);
        CHECK(fixture.exists("S/Stone.ithasset"));
        CHECK_FALSE(fixture.exists("S/Rock.ithasset"));
        CHECK(fixture.keyAt("S/Stone.ithasset") == rock); // the id stays
        CHECK(fixture.prompt() == nullptr);
        CHECK_FALSE(fixture.panel.controlRect("rename").has_value());

        // An invalid name shows why and keeps the field open; a taken one too, after the command refuses.
        fixture.key(Key::F2);
        fixture.type("a:b");
        CHECK(fixture.drawnText().find("a:b") != std::string::npos);
        fixture.key(Key::Enter);
        fixture.frames(2);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.exists("S/Stone.ithasset"));
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.type("Tree");
        fixture.key(Key::Enter);
        fixture.frames(2);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.exists("S/Stone.ithasset"));
        CHECK(fixture.panel.notice().find("taken") != std::string::npos);
        // Escape cancels: ith.content.dismiss ends the prompt, the selection stays.
        fixture.frames(2);
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.exists("S/Stone.ithasset"));
        CHECK(fixture.selected() == std::vector<std::string>{"S/Stone.ithasset"});
    }

    TEST_CASE("Add > New Folder names the folder inline: leaving the field commits, Escape keeps the default "
              "(spec 3.7)") {
        ContentFixture fixture;
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        fixture.clickControl("Add");
        fixture.clickControl("New Folder");
        fixture.frames(2);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.prompt()->item.path == "New Folder");
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.type("Props");
        fixture.click(kPanelWidth + 100.0f, 50.0f); // away from the field: the valid name is committed
        fixture.frames(1);
        CHECK(fixture.session.content().folderExists("Props"));
        CHECK_FALSE(fixture.session.content().folderExists("New Folder"));

        fixture.clickControl("Add");
        fixture.clickControl("New Folder");
        fixture.frames(2);
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.session.content().folderExists("New Folder"));
    }

    TEST_CASE("the delete dialog lists the items, their users and unreadable files; Cancel has the focus "
              "(spec 3.11, requirement 40)") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.asset("S/Broken.ithasset");
        fixture.run("ith.content.rescan");
        const std::string rock = fixture.keyAt("S/Rock.ithasset").id.toString();
        fixture.asset("S/User.ithasset",
                      R"({"ref": {"asset": ")" + rock + R"(", "subAsset": "00000000-0000-0000-0000-000000000000"}})");
        fixture.run("ith.content.rescan");
        fixture.write("Content/S/Broken.ithasset", "not json"); // unreadable when users are looked for
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);

        fixture.clickItem("S/Rock.ithasset");
        fixture.key(Key::Delete);
        fixture.frames(1);
        CHECK(fixture.panel.dialog() == ContentDialog::Delete);
        const std::string text = fixture.drawnText();
        CHECK(text.find("Rock") != std::string::npos);
        CHECK(text.find("is used by:") != std::string::npos);
        CHECK(text.find("User (S/User.ithasset)") != std::string::npos);
        CHECK(text.find("couldn't be read") != std::string::npos);
        CHECK(text.find("S/Broken.ithasset") != std::string::npos);
        CHECK(text.find("This can't be undone. The files are deleted from disk.") != std::string::npos);
        // Enter presses the focused Cancel: nothing is deleted, the selection stays.
        fixture.key(Key::Enter);
        fixture.frames(1);
        CHECK(fixture.panel.dialog() == ContentDialog::None);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.exists("S/Rock.ithasset"));
        CHECK(fixture.selected().size() == 1);

        // Escape cancels too.
        fixture.key(Key::Delete);
        fixture.frames(1);
        REQUIRE(fixture.panel.dialog() == ContentDialog::Delete);
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.exists("S/Rock.ithasset"));

        // Delete deletes what the dialog showed.
        fixture.key(Key::Delete);
        fixture.frames(1);
        fixture.clickControl("Delete");
        fixture.frames(1);
        CHECK_FALSE(fixture.exists("S/Rock.ithasset"));
        CHECK(fixture.exists("S/User.ithasset"));
        CHECK(fixture.panel.notice() == "Deleted 1 item");
        CHECK(fixture.panel.dialog() == ContentDialog::None);
    }

    TEST_CASE("the delete dialog names the open level among the users, and when only its unsaved changes use an "
              "asset") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(2);
        // A preview as ith.content.delete returns it (its users are tested in editor_core).
        const auto details = serialization::parseJson(R"({"deleted": false, "preview": {
            "items": [{"path": "S", "kind": "folder", "label": "S", "assets": 1, "otherFiles": 3}],
            "count": 1, "assets": 1, "otherFiles": 3,
            "uses": [{"asset": {"path": "S/Rock.ithasset", "label": "Rock"},
                      "users": [{"path": "Levels/Main.ithmap", "label": "Main", "openLevel": true},
                                {"path": "", "label": "Untitled level", "openLevel": true, "unsaved": true}]}],
            "unreadable": []}})");
        REQUIRE(details.hasValue());
        fixture.session.content().setPrompt(
            ContentPrompt{.kind = ContentPromptKind::Delete, .item = {}, .details = *details});
        fixture.frames(1);
        REQUIRE(fixture.panel.dialog() == ContentDialog::Delete);
        const std::string text = fixture.drawnText();
        CHECK(text.find("1 asset inside, and 3 other files not shown in the Content Browser") != std::string::npos);
        CHECK(text.find("Rock (S/Rock.ithasset) is used by:") != std::string::npos);
        CHECK(text.find("Main (Levels/Main.ithmap), the open level") != std::string::npos);
        CHECK(text.find("Untitled level (), the open level's unsaved changes") != std::string::npos);
        CHECK(text.find("will become missing") != std::string::npos);
        // A prompt that ends elsewhere (a script's dismiss) closes the dialog.
        fixture.run("ith.content.dismiss");
        fixture.frames(1);
        CHECK(fixture.panel.dialog() == ContentDialog::None);
    }

    TEST_CASE("the delete dialog lists 20 items and the rest as a count; the drawer stays open behind it "
              "(requirement 53)") {
        ContentFixture fixture;
        fixture.panelOpen = false;
        for (int i = 0; i < 22; ++i) {
            fixture.asset("Many/Item" + std::to_string(10 + i) + ".ithasset");
        }
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "Many"})");
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(3);
        fixture.run("ith.content.select_all");
        fixture.run("ith.content.delete");
        fixture.frames(2);
        REQUIRE(fixture.panel.dialog() == ContentDialog::Delete);
        CHECK(fixture.drawnText().find("and 2 more") != std::string::npos);
        fixture.click(10.0f, 10.0f); // outside the drawer, while the dialog is open
        CHECK(fixture.session.content().drawerOpen());
        CHECK(fixture.panel.dialog() == ContentDialog::Delete);
        fixture.clickControl("Cancel");
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.session.content().drawerOpen());
        CHECK(fixture.session.content().selection().size() == 22); // Cancel keeps the selection
    }

    TEST_CASE("Import opens the platform's dialog; conflicts are asked one file at a time with Apply to all "
              "(spec 3.12)") {
        ContentFixture fixture;
        fixture.asset("A.ithasset");
        fixture.asset("B.ithasset");
        fixture.asset("C.ithasset");
        const std::vector<std::string> files = {fixture.outside("A.ithasset"), fixture.outside("B.ithasset"),
                                                fixture.outside("C.ithasset")};
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        fixture.clickControl("Import");
        REQUIRE(fixture.host.dialogs == 1);
        CHECK(fixture.host.dialogAllowMany);
        REQUIRE(fixture.host.dialogFilters.size() == 2);
        CHECK(fixture.host.dialogFilters[0].second == "ithasset;ITHASSET;ithmap;ITHMAP");
        CHECK(fixture.host.dialogFilters[1] == std::pair<std::string, std::string>("All files", "*"));

        for (const std::string& file : files) {
            fixture.ui.ui->handleEvent({.type = EventType::FileDialogFile, .window = kWindow, .text = file});
        }
        fixture.platform({.type = EventType::FileDialogClosed});
        // The notice is drawn the frame before the import runs.
        CHECK(fixture.panel.importPending());
        CHECK(fixture.panel.notice() == "Importing 3 files\xE2\x80\xA6");
        fixture.frames(2);
        CHECK_FALSE(fixture.panel.importPending());
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.prompt()->kind == ContentPromptKind::ImportConflicts);
        fixture.frames(1);
        CHECK(fixture.panel.dialog() == ContentDialog::ImportConflicts);
        CHECK(fixture.drawnText().find("File 1 of 3") != std::string::npos);

        // A native asset can't be replaced: Replace does nothing.
        fixture.clickControl("Replace");
        CHECK(fixture.drawnText().find("File 1 of 3") != std::string::npos);
        fixture.clickControl("Skip");
        CHECK(fixture.drawnText().find("File 2 of 3") != std::string::npos);
        fixture.clickControl("Apply to all remaining");
        fixture.clickControl("Keep Both");
        fixture.frames(3);
        CHECK(fixture.prompt() == nullptr);
        CHECK_FALSE(fixture.exists("A_2.ithasset"));
        CHECK(fixture.exists("B_2.ithasset"));
        CHECK(fixture.exists("C_2.ithasset"));
        CHECK(fixture.panel.notice() == "Imported 2 files, 1 skipped (see the Output Log)");
        CHECK(fixture.selected() == std::vector<std::string>{"B_2.ithasset", "C_2.ithasset"});

        // Cancel imports nothing.
        fixture.osDrop({files[0]}, 300.0f, 300.0f);
        fixture.frames(2);
        REQUIRE(fixture.panel.dialog() == ContentDialog::ImportConflicts);
        fixture.key(Key::Escape);
        fixture.frames(2);
        CHECK(fixture.prompt() == nullptr);
        CHECK_FALSE(fixture.exists("A_2.ithasset"));
    }

    TEST_CASE("files dropped from the operating system import into the folder under them; elsewhere nothing "
              "(spec 3.12)") {
        ContentFixture fixture;
        fixture.asset("Dest/T.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        const UiRect dest = fixture.rect("Dest");

        // Hovering highlights the folder under the pointer; the drop imports into it.
        fixture.platform({.type = EventType::DropBegin});
        fixture.platform({.type = EventType::DropPosition, .x = dest.centerX(), .y = dest.centerY()});
        REQUIRE(fixture.panel.dropHighlight().has_value());
        CHECK(fixture.panel.dropHighlight()->x == doctest::Approx(dest.x));
        CHECK(fixture.panel.dropHighlight()->width == doctest::Approx(dest.width));
        fixture.ui.ui->handleEvent({.type = EventType::DropFile,
                                    .window = kWindow,
                                    .x = dest.centerX(),
                                    .y = dest.centerY(),
                                    .text = fixture.outside("X.ithasset")});
        fixture.platform({.type = EventType::DropComplete});
        CHECK_FALSE(fixture.panel.dropHighlight().has_value());
        fixture.frames(2);
        CHECK(fixture.exists("Dest/X.ithasset"));

        // Empty space of the view: the current folder (the import made Dest current).
        fixture.run("ith.content.navigate", R"({"folder": ""})");
        fixture.frames(2);
        const UiRect again = fixture.rect("Dest");
        fixture.osDrop({fixture.outside("Y.ithasset")}, again.centerX(), again.y + again.height + 60.0f);
        fixture.frames(2);
        CHECK(fixture.exists("Y.ithasset"));

        // A tree folder, and a path bar segment.
        fixture.run("ith.content.navigate", R"({"folder": ""})");
        fixture.frames(2);
        const UiRect tree = *fixture.panel.folderRect("Dest");
        fixture.osDrop({fixture.outside("Z.ithasset")}, tree.centerX(), tree.centerY());
        fixture.frames(2);
        CHECK(fixture.exists("Dest/Z.ithasset"));
        CHECK(fixture.session.content().currentFolder() == "Dest");
        const UiRect root = *fixture.panel.segmentRect("");
        fixture.osDrop({fixture.outside("W.ithasset")}, root.centerX(), root.centerY());
        fixture.frames(2);
        CHECK(fixture.exists("W.ithasset"));

        // Outside the browser, from another window, or without files: nothing.
        fixture.osDrop({fixture.outside("V.ithasset")}, kPanelWidth + 100.0f, 50.0f);
        CHECK_FALSE(fixture.panel.importPending());
        fixture.ui.ui->handleEvent(
            {.type = EventType::DropFile, .window = kWindow + 1, .x = 100.0f, .y = 100.0f, .text = "elsewhere"});
        fixture.ui.ui->handleEvent({.type = EventType::DropComplete, .window = kWindow + 1});
        fixture.platform({.type = EventType::DropBegin});
        fixture.platform({.type = EventType::DropComplete});
        CHECK_FALSE(fixture.panel.importPending());
        fixture.frames(2);
        CHECK_FALSE(fixture.exists("V.ithasset"));

        // The drawer takes drops too.
        fixture.panelOpen = false;
        fixture.run("ith.content.toggle_drawer");
        fixture.frames(2);
        const UiRect drawer = *fixture.panel.drawerRect();
        fixture.osDrop({fixture.outside("V.ithasset")}, drawer.centerX(), drawer.y + drawer.height - 40.0f);
        fixture.frames(2);
        CHECK(fixture.exists("V.ithasset")); // the current folder, Content, as the last import made it
    }

    TEST_CASE("the file dialog: files import into the folder current at Import; a dialog that can't be shown "
              "says to drop files") {
        ContentFixture fixture;
        fixture.asset("Dest/T.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        const std::string_view noDialog = "drop files onto the browser";

        // Refused at once (a dialog open elsewhere), or failing later (no desktop portal, no zenity).
        fixture.host.dialogError = Error{.code = "platform.dialog_busy", .message = "a dialog is open"};
        fixture.clickControl("Import");
        CHECK(fixture.host.dialogs == 1);
        CHECK(fixture.panel.notice().find(noDialog) != std::string::npos);
        fixture.host.dialogError.reset();
        fixture.clickControl("Import");
        CHECK(fixture.host.dialogs == 2);
        fixture.platform({.type = EventType::FileDialogClosed, .text = "no dialog backend"});
        CHECK(fixture.panel.notice().find(noDialog) != std::string::npos);
        CHECK_FALSE(fixture.panel.importPending());

        // Chosen in Dest, closed after navigating away: the files go to Dest.
        fixture.run("ith.content.navigate", R"({"folder": "Dest"})");
        fixture.frames(2);
        fixture.clickControl("Import");
        fixture.run("ith.content.navigate", R"({"folder": ""})");
        const std::string x = fixture.outside("X.ithasset");
        fixture.ui.ui->handleEvent({.type = EventType::FileDialogFile, .window = kWindow, .text = x});
        fixture.platform({.type = EventType::FileDialogClosed});
        fixture.frames(2);
        CHECK(fixture.exists("Dest/X.ithasset"));
        CHECK(fixture.session.content().currentFolder() == "Dest");
        CHECK(fixture.panel.notice() == "Imported 1 file");
        // The next dialog starts where the last files came from; a cancel imports nothing.
        fixture.clickControl("Import");
        CHECK(fixture.host.dialogFolder == std::string(fs::parentPath(fs::normalizePath(x))));
        fixture.platform({.type = EventType::FileDialogClosed});
        CHECK_FALSE(fixture.panel.importPending());
#ifndef _WIN32
        // A chosen file whose name no fs call can address ('\' in a name on POSIX) is no start folder.
        fixture.clickControl("Import");
        fixture.ui.ui->handleEvent({.type = EventType::FileDialogFile,
                                    .window = kWindow,
                                    .text = fixture.folder.root + "/Odd\\Name/Y.ithasset"});
        fixture.platform({.type = EventType::FileDialogClosed});
        fixture.frames(2);
        fixture.clickControl("Import");
        CHECK(fixture.host.dialogFolder == std::string(fs::parentPath(fs::normalizePath(x))));
        fixture.platform({.type = EventType::FileDialogClosed});
#endif
        // Results without a dialog of the panel are ignored.
        fixture.ui.ui->handleEvent({.type = EventType::FileDialogFile, .window = kWindow, .text = x});
        fixture.platform({.type = EventType::FileDialogClosed});
        CHECK_FALSE(fixture.panel.importPending());

        // A panel without a host can't show one.
        ContentBrowserPanel lonely(fixture.session);
        lonely.showImportDialog();
        CHECK(lonely.notice().find(noDialog) != std::string::npos);
    }

    TEST_CASE("dragging items onto a folder moves them; a folder can't go below itself (spec 3.9)") {
        ContentFixture fixture;
        fixture.asset("Props/A.ithasset");
        fixture.asset("Props/B.ithasset");
        fixture.asset("Props/Sub/Deep/Y.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "Props"})");
        fixture.frames(3);
        const ContentKey a = fixture.keyAt("Props/A.ithasset");
        fixture.clickItem("Props/A.ithasset");
        fixture.clickItem("Props/B.ithasset", KeyModifiers::Control);
        // Dragging a selected item drags the whole selection.
        const UiRect from = fixture.rect("Props/A.ithasset");
        const UiRect sub = fixture.rect("Props/Sub");
        fixture.drag(from.centerX(), from.centerY(), sub.centerX(), sub.centerY());
        CHECK(fixture.exists("Props/Sub/A.ithasset"));
        CHECK(fixture.exists("Props/Sub/B.ithasset"));
        CHECK(fixture.keyAt("Props/Sub/A.ithasset") == a);

        // The tree folder Props/Sub onto Props/Sub/Deep: refused, with a "not allowed" cursor and the reason.
        fixture.run("ith.content.navigate", R"({"folder": "Props/Sub"})");
        fixture.frames(3);
        const UiRect row = *fixture.panel.folderRect("Props/Sub");
        const UiRect deep = fixture.rect("Props/Sub/Deep");
        fixture.frames(30);
        fixture.move(row.centerX(), row.centerY());
        fixture.step({.type = EventType::MouseButtonDown,
                      .mouseButton = MouseButton::Left,
                      .clicks = 1,
                      .x = row.centerX(),
                      .y = row.centerY()});
        for (int i = 1; i <= 6; ++i) {
            const float t = static_cast<float>(i) / 6.0f;
            fixture.move(row.centerX() + (deep.centerX() - row.centerX()) * t,
                         row.centerY() + (deep.centerY() - row.centerY()) * t);
        }
        fixture.frames(2);
        CHECK(fixture.host.cursor == CursorShape::NotAllowed);
        CHECK(fixture.drawnText().find("can't move into itself or a folder below it") != std::string::npos);
        fixture.step({.type = EventType::MouseButtonUp,
                      .mouseButton = MouseButton::Left,
                      .x = deep.centerX(),
                      .y = deep.centerY()});
        fixture.frames(2);
        CHECK(fixture.session.content().folderExists("Props/Sub/Deep"));
        CHECK_FALSE(fixture.session.content().folderExists("Props/Sub/Deep/Sub"));
    }

    TEST_CASE("context menus: item entries, Reimport greyed for native assets, the empty space and tree folders") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.asset("T/x.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        const UiRect rock = fixture.rect("S/Rock.ithasset");
        fixture.rightClick(rock.centerX(), rock.centerY());
        CHECK(fixture.selected() == std::vector<std::string>{"S/Rock.ithasset"});
        CHECK(fixture.panel.controlRect("menu:Rename").has_value());
        CHECK(fixture.panel.controlRect("menu:Delete").has_value());
        // Only native assets are selected: Reimport is greyed and runs nothing.
        fixture.clickControl("menu:Reimport");
        CHECK(fixture.panel.notice().empty());
        CHECK(fixture.panel.controlRect("menu:Reimport").has_value()); // the menu stays open
        fixture.key(Key::Escape);
        CHECK_FALSE(fixture.panel.controlRect("menu:Reimport").has_value());
        CHECK(fixture.selected().size() == 1); // the menu kept its Escape
        fixture.rightClick(rock.centerX(), rock.centerY());
        fixture.clickControl("menu:Duplicate");
        fixture.frames(1);
        CHECK(fixture.exists("S/Rock_2.ithasset"));

        // The empty space: New Folder and Import.
        fixture.rightClick(rock.centerX(), rock.y + rock.height + 80.0f);
        CHECK(fixture.panel.controlRect("menu:Import...").has_value());
        fixture.clickControl("menu:New Folder");
        fixture.frames(2);
        CHECK(fixture.session.content().folderExists("S/New Folder"));
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);

        // A tree folder the view doesn't show is renamed on its tree row.
        const UiRect t = *fixture.panel.folderRect("T");
        fixture.rightClick(t.centerX(), t.centerY());
        fixture.clickControl("menu:Rename");
        fixture.frames(2);
        const std::optional<UiRect> field = fixture.panel.controlRect("rename");
        REQUIRE(field.has_value());
        CHECK(field->centerY() == doctest::Approx(t.centerY()).epsilon(0.05));
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.type("Textures");
        fixture.key(Key::Enter);
        fixture.frames(1);
        CHECK(fixture.exists("Textures/x.ithasset"));
    }

    TEST_CASE("with the focus in the folder tree, F2 renames the current folder on its row and Delete asks "
              "about it (spec 3.6, 3.8, 3.11)") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        fixture.clickItem("S/Rock.ithasset");
        CHECK_FALSE(fixture.session.content().treeFocus());

        // A click on the current folder's row gives the tree the focus; the content selection stays.
        const UiRect s = *fixture.panel.folderRect("S");
        fixture.click(s.centerX(), s.centerY());
        REQUIRE(fixture.session.content().treeFocus());
        CHECK(fixture.selected() == std::vector<std::string>{"S/Rock.ithasset"});

        fixture.key(Key::F2);
        fixture.frames(1);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.prompt()->kind == ContentPromptKind::Rename);
        CHECK(fixture.prompt()->item.path == "S");
        const std::optional<UiRect> field = fixture.panel.controlRect("rename");
        REQUIRE(field.has_value());
        CHECK(field->centerY() == doctest::Approx(s.centerY()).epsilon(0.05));
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);

        // The tree keeps the focus: Delete asks about the folder, not the selected asset.
        REQUIRE(fixture.session.content().treeFocus());
        fixture.key(Key::Delete);
        fixture.frames(1);
        REQUIRE(fixture.panel.dialog() == ContentDialog::Delete);
        const serialization::Value* items = fixture.prompt()->details.find("preview")->find("items");
        REQUIRE(items->elements().size() == 1);
        CHECK(items->elements()[0].find("path")->asString() == "S");
        CHECK(items->elements()[0].find("kind")->asString() == "folder");
        fixture.key(Key::Escape);
        fixture.frames(1);
        CHECK(fixture.prompt() == nullptr);
        CHECK(fixture.exists("S/Rock.ithasset"));
    }

    TEST_CASE("a press and F2 or Delete in one batch act on the part of the browser the press lands in") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        fixture.clickItem("S/Rock.ithasset");
        const UiRect rock = fixture.rect("S/Rock.ithasset");
        const UiRect s = *fixture.panel.folderRect("S");
        // The tree's empty space below its rows: a press there changes nothing but the focus.
        const float treeX = s.centerX();
        const float treeY = s.centerY() + s.height * 4.0f;
        // Press, release and a key before any UI frame: the press alone decides where the key goes.
        const auto pressThen = [&fixture](float x, float y, Key key) {
            fixture.move(x, y);
            fixture.ui.events(
                {Event{
                     .type = EventType::MouseButtonDown, .mouseButton = MouseButton::Left, .clicks = 1, .x = x, .y = y},
                 Event{.type = EventType::MouseButtonUp, .mouseButton = MouseButton::Left, .x = x, .y = y},
                 Event{.type = EventType::KeyDown, .key = key}});
        };

        SUBCASE("a press on an asset, then Delete, previews the asset, not the tree's folder") {
            fixture.click(treeX, treeY);
            REQUIRE(fixture.session.content().treeFocus());
            pressThen(rock.centerX(), rock.centerY(), Key::Delete);
            CHECK_FALSE(fixture.session.content().treeFocus());
            REQUIRE(fixture.prompt() != nullptr);
            REQUIRE(fixture.prompt()->kind == ContentPromptKind::Delete);
            const serialization::Value* items = fixture.prompt()->details.find("preview")->find("items");
            REQUIRE(items->elements().size() == 1);
            CHECK(items->elements()[0].find("path")->asString() == "S/Rock.ithasset");
        }

        SUBCASE("a press in the tree, then F2, renames the current folder, not the selected asset") {
            REQUIRE(fixture.session.content().keyboardFocus());
            REQUIRE_FALSE(fixture.session.content().treeFocus());
            pressThen(treeX, treeY, Key::F2);
            CHECK(fixture.session.content().treeFocus());
            REQUIRE(fixture.prompt() != nullptr);
            CHECK(fixture.prompt()->kind == ContentPromptKind::Rename);
            CHECK(fixture.prompt()->item.path == "S");
        }
        fixture.frames(2);
        CHECK(fixture.exists("S/Rock.ithasset"));
    }

    TEST_CASE("F2 in the tree on a current folder below a collapsed one expands the tree to draw the field") {
        ContentFixture fixture;
        fixture.asset("A/B/Rock.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "A/B"})");
        fixture.frames(3);
        REQUIRE(fixture.panel.folderRect("A/B").has_value());
        fixture.session.content().setExpanded("A", false);
        fixture.frames(2);
        CHECK_FALSE(fixture.session.content().isExpanded("A"));
        CHECK_FALSE(fixture.panel.folderRect("A/B").has_value());

        // A click on the tree's empty space below its rows gives it the focus and changes nothing else.
        const UiRect a = *fixture.panel.folderRect("A");
        fixture.click(a.centerX(), a.centerY() + a.height * 4.0f);
        REQUIRE(fixture.session.content().treeFocus());
        CHECK(fixture.session.content().currentFolder() == "A/B");

        fixture.key(Key::F2);
        REQUIRE(fixture.prompt() != nullptr);
        CHECK(fixture.prompt()->item.path == "A/B");
        CHECK(fixture.session.content().isExpanded("A"));
        const std::optional<UiRect> row = fixture.panel.folderRect("A/B");
        const std::optional<UiRect> field = fixture.panel.controlRect("rename");
        REQUIRE(row.has_value());
        REQUIRE(field.has_value());
        CHECK(field->centerY() == doctest::Approx(row->centerY()).epsilon(0.05));
        REQUIRE(fixture.ui.ui->wantsTextInput());
        fixture.type("C");
        fixture.key(Key::Enter);
        fixture.frames(1);
        CHECK(fixture.exists("A/C/Rock.ithasset"));
        CHECK(fixture.session.content().currentFolder() == "A/C");
    }

    TEST_CASE("the status bar's Content Drawer button toggles the drawer on the press (spec 3.16)") {
        ContentFixture fixture;
        fixture.shell = true;
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        const float y = static_cast<float>(kHeight) - ImGui::GetFrameHeight() * 0.5f;
        fixture.click(40.0f, y);
        CHECK(fixture.session.content().drawerOpen());
        fixture.frames(2);
        CHECK(fixture.panel.drawerRect().has_value());
        fixture.click(40.0f, y);
        CHECK_FALSE(fixture.session.content().drawerOpen());
        fixture.frames(1);
        CHECK_FALSE(fixture.panel.drawerRect().has_value());
    }

    TEST_CASE("files arriving while a question is open are refused with a notice; the dialog stays open") {
        ContentFixture fixture;
        fixture.asset("S/Rock.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "S"})");
        fixture.frames(3);
        const std::string_view answerFirst = "Answer or cancel the open question first";
        fixture.clickItem("S/Rock.ithasset");
        fixture.key(Key::Delete);
        fixture.frames(1);
        REQUIRE(fixture.panel.dialog() == ContentDialog::Delete);

        // A drop: no highlight while it hovers, nothing imported, the dialog still open.
        fixture.platform({.type = EventType::DropBegin});
        fixture.platform({.type = EventType::DropPosition, .x = 50.0f, .y = 350.0f});
        CHECK_FALSE(fixture.panel.dropHighlight().has_value());
        fixture.ui.ui->handleEvent({.type = EventType::DropFile,
                                    .window = kWindow,
                                    .x = 50.0f,
                                    .y = 350.0f,
                                    .text = fixture.outside("X.ithasset")});
        fixture.platform({.type = EventType::DropComplete});
        CHECK_FALSE(fixture.panel.importPending());
        CHECK(fixture.panel.notice().find(answerFirst) != std::string::npos);
        fixture.frames(2);
        CHECK_FALSE(fixture.exists("S/X.ithasset"));
        CHECK(fixture.panel.dialog() == ContentDialog::Delete);
        // Import is refused too.
        const int dialogs = fixture.host.dialogs;
        fixture.panel.showImportDialog();
        CHECK(fixture.host.dialogs == dialogs);

        // A dialog result that lands while a question opened meanwhile is refused as well.
        fixture.key(Key::Escape);
        fixture.frames(2);
        REQUIRE(fixture.panel.dialog() == ContentDialog::None);
        fixture.clickControl("Import");
        REQUIRE(fixture.host.dialogs == dialogs + 1);
        fixture.run("ith.content.delete");
        fixture.ui.ui->handleEvent(
            {.type = EventType::FileDialogFile, .window = kWindow, .text = fixture.outside("Y.ithasset")});
        fixture.platform({.type = EventType::FileDialogClosed});
        CHECK_FALSE(fixture.panel.importPending());
        CHECK(fixture.panel.notice().find(answerFirst) != std::string::npos);
        fixture.frames(2);
        CHECK_FALSE(fixture.exists("S/Y.ithasset"));
    }

    TEST_CASE("a second import while one is queued is refused with a notice, not lost silently") {
        ContentFixture fixture;
        fixture.asset("Dest/T.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        const UiRect dest = fixture.rect("Dest");
        fixture.clickControl("Import");
        // The dialog's result and a drop in one batch of events, before any frame.
        fixture.ui.ui->handleEvent(
            {.type = EventType::FileDialogFile, .window = kWindow, .text = fixture.outside("X.ithasset")});
        fixture.ui.ui->handleEvent({.type = EventType::FileDialogClosed, .window = kWindow});
        fixture.ui.ui->handleEvent({.type = EventType::DropBegin, .window = kWindow});
        fixture.ui.ui->handleEvent(
            {.type = EventType::DropPosition, .window = kWindow, .x = dest.centerX(), .y = dest.centerY()});
        fixture.ui.ui->handleEvent({.type = EventType::DropFile,
                                    .window = kWindow,
                                    .x = dest.centerX(),
                                    .y = dest.centerY(),
                                    .text = fixture.outside("Y.ithasset")});
        fixture.ui.ui->handleEvent({.type = EventType::DropComplete, .window = kWindow});
        CHECK(fixture.panel.importPending());
        CHECK(fixture.panel.notice().starts_with("An import is waiting"));
        fixture.frames(3);
        CHECK(fixture.exists("X.ithasset"));
        CHECK_FALSE(fixture.exists("Dest/Y.ithasset"));
    }

    TEST_CASE("a drop without a position (Windows without OLE) goes to the current folder of a browser on "
              "screen") {
        ContentFixture fixture;
        fixture.asset("Dest/T.ithasset");
        fixture.run("ith.content.rescan");
        fixture.run("ith.content.navigate", R"({"folder": "Dest"})");
        fixture.frames(3);
        const auto dropAtOrigin = [&fixture](const std::string& file) {
            fixture.platform({.type = EventType::DropBegin});
            fixture.ui.ui->handleEvent({.type = EventType::DropFile, .window = kWindow, .text = file});
            fixture.platform({.type = EventType::DropComplete});
        };
        dropAtOrigin(fixture.outside("X.ithasset"));
        fixture.frames(2);
        CHECK(fixture.exists("Dest/X.ithasset"));

        // No browser drawn in the last frame: refused with a notice.
        fixture.panelOpen = false;
        fixture.frames(2);
        dropAtOrigin(fixture.outside("Y.ithasset"));
        CHECK_FALSE(fixture.panel.importPending());
        CHECK(fixture.panel.notice() == "Open the Content Browser, then drop the files onto it");
        fixture.frames(2);
        CHECK_FALSE(fixture.exists("Dest/Y.ithasset"));
        CHECK_FALSE(fixture.exists("Y.ithasset"));
    }

    TEST_CASE("drops at display scale 2 land in the folder under them") {
        ContentFixture fixture;
        fixture.ui.pixelSize = {kWidth * 2, kHeight * 2};
        fixture.asset("Dest/T.ithasset");
        fixture.run("ith.content.rescan");
        fixture.frames(3);
        const UiRect dest = fixture.rect("Dest"); // UI coordinates; events carry window pixels
        fixture.osDrop({fixture.outside("X.ithasset")}, dest.centerX() * 2.0f, dest.centerY() * 2.0f);
        fixture.frames(2);
        CHECK(fixture.exists("Dest/X.ithasset"));
    }
}
