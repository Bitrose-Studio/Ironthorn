// The Content Browser panel and the content drawer (ADR-0049 rules 4, 7 and 26, docs/specs/
// content-browser.md requirements 11, 12, 51, 53, 55, 60 and 61), headless on the RHI's null device
// and driven by mouse and key events through the platform backend and the engine keymap: clicks
// with Ctrl and Shift, empty space, arrow keys in a four-column grid, double-clicks that open
// folders and levels, the unsaved-changes question, the drawer's focus, Escape, outside clicks and
// height, the search box keeping its keys, only the visible items drawn and nothing rebuilt per frame;
// and a click with a key in one batch of events, menus and dialogs keeping Escape, the list's header
// sorting with "##" in names, and the drawer closing when the panel is in the layout.

#include "ith/editor_ui/ContentBrowserPanel.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/config/Config.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/PanelRegistry.h"
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
          ui({.content = &session.content()}, &shortcuts) {
        registerContentBrowser(session, panel);
    }

    Keymap engineKeymap() const {
        const Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<Error> warnings;
        return Keymap::load(config, session.commands(), warnings);
    }

    void asset(std::string_view path) const {
        const std::string text =
            R"({"format": "ith.material", "version": 1, "id": ")" + Uuid::generate().toString() + R"(", "data": {}})";
        REQUIRE(fs::writeFileAtomic(fs::joinPath(folder.root, "Content/" + std::string(path)), text,
                                    {.durability = fs::Durability::Atomic})
                    .hasValue());
    }

    serialization::Value run(std::string_view id, std::string_view args = "{}") {
        auto parsed = serialization::parseJson(args);
        REQUIRE(parsed.hasValue());
        auto result = session.execute(id, *parsed);
        REQUIRE_MESSAGE(result.hasValue(), (result ? std::string() : result.error().message));
        return std::move(*result);
    }

    void draw() {
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
    UiFixture ui;
    ContentBrowserPanel panel{session};
    bool panelOpen = true;
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
}
