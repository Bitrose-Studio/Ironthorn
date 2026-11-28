// The Content Browser's browse slice (ADR-0049 rule 27, docs/specs/content-browser.md requirements 1
// to 15 (13's F2 half comes with renaming), 50 to 52, 54, 55, 57 and 58), headless: what is shown,
// problems, the type table, natural order, sorting, search and filter, selection, navigation, Rescan
// after outside changes and during a save, new levels showing at once, opening levels, the drawer,
// the Content keymap context with its widget keys, the focus between frames, the search limit, and
// the session state and its reader, each in a temporary project with an asset database.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"
#include "ith/platform_events/Event.h"

#include "ContentTestSupport.h"
#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::AssetId;
using ith::ContentKey;
using ith::EditorSession;
using ith::serialization::Value;
using ith::test::editor::badgeOf;
using ith::test::editor::exists;
using ith::test::editor::fails;
using ith::test::editor::json;
using ith::test::editor::keyDown;
using ith::test::editor::Project;
using ith::test::editor::run;
using ith::test::editor::TempProject;
using ith::test::editor::types;

} // namespace

TEST_SUITE("editor_core.content_browser") {
    TEST_CASE("the type table, categories and natural order") {
        CHECK(ith::contentTypeOf("ith.level").category == ith::ContentCategory::Level);
        CHECK(ith::contentTypeOf("gltf").category == ith::ContentCategory::Model);
        CHECK(ith::contentTypeOf("gltf").displayName == "glTF Model");
        CHECK(ith::contentTypeOf("ith.input_action").category == ith::ContentCategory::Data);
        // A type the table doesn't know: Other, shown by its raw type (requirement 4).
        CHECK(ith::contentTypeOf("test.widget").category == ith::ContentCategory::Other);
        CHECK(ith::contentTypeOf("test.widget").displayName == "test.widget");
        for (std::size_t i = 0; i < ith::kContentCategoryCount; ++i) {
            const auto category = static_cast<ith::ContentCategory>(i);
            CHECK(ith::parseCategory(ith::categoryId(category)) == category);
        }
        CHECK_FALSE(ith::parseCategory("problems").has_value());
        CHECK(ith::displayName(ith::ContentCategory::Texture) == "Texture");

        CHECK(ith::naturalCompare("Rock2", "Rock10") < 0);
        CHECK(ith::naturalCompare("rock2", "Rock10") < 0);
        CHECK(ith::naturalCompare("Rock7", "Rock07") < 0); // equal values: the shorter run first
        CHECK(ith::naturalCompare("Rock07", "Rock10") < 0);
        CHECK(ith::naturalCompare("a", "Rock1") < 0);
        CHECK(ith::naturalCompare("ROCK", "rock") == 0);
        CHECK(ith::naturalCompare("Rock", "Rock1") < 0);
        CHECK(ith::naturalCompare("Rock1b", "Rock1a") > 0);
        CHECK(ith::naturalCompare("", "") == 0);
        CHECK(ith::naturalCompare("9", "a") < 0);
    }

    TEST_CASE("what is shown: folders, assets, hidden files and the tree (requirement 1)") {
        Project p;
        p.folder("Meshes/Rocks");
        const AssetId level = p.native("Levels/Main.ithmap", "ith.level");
        const AssetId ship = p.imported("Meshes/Ship.gltf");
        p.write("Meshes/notes.txt", "not an asset");
        p.rescan();

        CHECK(p.labels() == std::vector<std::string>{"Levels", "Meshes"});
        p.navigate("Meshes");
        CHECK(p.labels() == std::vector<std::string>{"Rocks", "Ship.gltf"});
        CHECK(p.item("Meshes/Ship.gltf").id == ship);
        CHECK(p.item("Meshes/Ship.gltf").category == ith::ContentCategory::Model);
        CHECK(p.model().findByPath("Meshes/Ship.gltf.meta") == nullptr);
        CHECK(p.model().findByPath("Meshes/notes.txt") == nullptr);
        p.navigate("Levels");
        const Value content = p.execute("ith.query.content");
        const Value& main = content.find("items")->elements().at(0);
        CHECK(main.find("label")->asString() == "Main");
        CHECK(main.find("kind")->asString() == "asset");
        CHECK(main.find("id")->asString() == level.toString());
        CHECK(main.find("type")->asString() == "ith.level");
        CHECK(main.find("category")->asString() == "level");

        // Navigating to Rocks expands its ancestors: the tree shows Content, Levels, Meshes, Rocks.
        p.navigate("Meshes/Rocks");
        const Value tree = p.execute("ith.query.content_tree");
        std::vector<std::string> names;
        for (const Value& row : tree.find("folders")->elements()) {
            names.push_back(row.find("name")->asString());
        }
        CHECK(names == std::vector<std::string>{"Content", "Levels", "Meshes", "Rocks"});
        CHECK(tree.find("current")->asString() == "Meshes/Rocks");
        CHECK(p.labels().empty());
        p.model().setExpanded("Meshes", false);
        CHECK(p.model().treeRows().size() == 3);
        CHECK(p.model().isExpanded(""));
    }

    TEST_CASE("each scan problem is an entry or a badge (requirements 2 to 4)") {
        Project p;
        p.write("Meshes/Raw.glb", "glTF"); // no sidecar
        const AssetId ship = p.imported("Meshes/Ship.gltf");
        REQUIRE(ith::fs::remove(p.file("Meshes/Ship.gltf")).hasValue()); // its sidecar is an orphan
        const AssetId duplicated = p.native("Data/First.ithasset");
        p.native("Data/Second.ithasset", "ith.material", duplicated); // a cold scan can't tell which is the copy
        p.native("Data/Wrong.ithasset", "ith.level");                 // a level in an .ithasset file
        p.write("Data/Broken.ithasset", "{ not json");
        p.write("Data/Left.ithasset" + std::string(ith::fs::kTemporaryMarker) + AssetId::generate().toString(), "x");
        p.native("Data/Widget.ithasset", "test.widget");
        p.rescan();
        static_cast<void>(ship);

        const ith::ContentItem& raw = p.item("Meshes/Raw.glb");
        CHECK(raw.kind == ith::ContentItemKind::Problem);
        REQUIRE(badgeOf(raw, ith::AssetProblemKind::MissingSidecar) != nullptr);
        CHECK(badgeOf(raw, ith::AssetProblemKind::MissingSidecar)->message == "Not imported");
        const ith::ContentItem& orphan = p.item("Meshes/Ship.gltf.meta");
        CHECK(orphan.kind == ith::ContentItemKind::Problem);
        CHECK(orphan.label == "Ship.gltf.meta");
        CHECK(badgeOf(orphan, ith::AssetProblemKind::OrphanSidecar)->message == "Source file missing");
        const ith::ContentItem& second = p.item("Data/Second.ithasset");
        CHECK(second.kind == ith::ContentItemKind::Problem);
        CHECK(badgeOf(second, ith::AssetProblemKind::DuplicateId)->message == "Duplicate id of Data/First.ithasset");
        CHECK(p.item("Data/First.ithasset").kind == ith::ContentItemKind::Asset);
        const ith::ContentItem& wrong = p.item("Data/Wrong.ithasset");
        CHECK(wrong.kind == ith::ContentItemKind::Asset);
        CHECK(badgeOf(wrong, ith::AssetProblemKind::FormatMismatch) != nullptr);
        CHECK(p.item("Data/Broken.ithasset").kind == ith::ContentItemKind::Problem);
        CHECK(badgeOf(p.item("Data/Broken.ithasset"), ith::AssetProblemKind::InvalidFile) != nullptr);
        const bool leftover = std::ranges::any_of(p.model().items(), [](const ith::ContentItem& item) {
            return item.path.starts_with("Data/Left.ithasset.tmp-") && item.kind == ith::ContentItemKind::Problem &&
                   badgeOf(item, ith::AssetProblemKind::InvalidFile) != nullptr;
        });
        CHECK(leftover);
        // An unknown type: category Other with its raw type (requirement 4).
        CHECK(p.item("Data/Widget.ithasset").category == ith::ContentCategory::Other);
        CHECK(p.item("Data/Widget.ithasset").type == "test.widget");
        CHECK(p.execute("ith.query.content_problems").find("problems")->elements().size() ==
              p.model().problems().size());

        // The Problems filter: exactly the badged items and the entries, from the current folder down.
        p.execute("ith.content.set_filter", R"({"categories": ["problems"]})");
        std::vector<std::string> problems = p.labels();
        CHECK(problems.size() == 6);
        CHECK(p.model().shownProblems() == problems.size());
        p.navigate("Meshes");
        CHECK(p.model().filter().problems); // navigating keeps the filter
        CHECK(p.labels() == std::vector<std::string>{"Raw.glb", "Ship.gltf.meta"});
        CHECK(p.execute("ith.query.content").find("problems")->toNumber<int>() == 2);
    }

    TEST_CASE("names that differ only in case get a badge on both (requirement 2)") {
        Project p;
        p.native("A/Rock.ithasset");
        p.native("a/Rock.ithasset");
        const auto entries = ith::fs::listDirectory(ith::fs::joinPath(p.project.root(), "Content"));
        // Both folders must exist, or the test would pass without testing anything: a file system or a
        // folder that folds case (Windows, macOS, a case-folding Linux folder) is skipped.
        if (!entries || entries->size() < 2) {
            MESSAGE("the file system ignores case; skipped");
            return;
        }
        p.rescan();
        const ith::ContentBadge* upper = badgeOf(p.item("A"), ith::AssetProblemKind::CaseConflict);
        const ith::ContentBadge* lower = badgeOf(p.item("a"), ith::AssetProblemKind::CaseConflict);
        REQUIRE(upper != nullptr);
        REQUIRE(lower != nullptr);
        CHECK(upper->otherPath == "a");
        CHECK(lower->otherPath == "A");
    }

    TEST_CASE("natural order with folders first, ascending and descending, and by type (requirements 5, 6)") {
        Project p;
        for (const std::string_view name : {"Rock10", "rock2", "Rock1", "Rock07", "Rock7", "a"}) {
            p.native("Rocks/" + std::string(name) + ".ithasset");
        }
        p.folder("Rocks/Zed");
        p.folder("Rocks/Bin");
        p.rescan();
        p.navigate("Rocks");
        CHECK(p.labels() == std::vector<std::string>{"Bin", "Zed", "a", "Rock1", "rock2", "Rock7", "Rock07", "Rock10"});
        p.execute("ith.content.set_view", R"({"descending": true})");
        CHECK(p.labels() == std::vector<std::string>{"Zed", "Bin", "Rock10", "Rock07", "Rock7", "rock2", "Rock1", "a"});

        Project typed;
        typed.native("Mixed/Zone.ithmap", "ith.level");
        typed.native("Mixed/Alpha.ithmap", "ith.level");
        typed.imported("Mixed/Boat.gltf");
        typed.native("Mixed/Jump.ithasset", "ith.input_action");
        typed.native("Mixed/Thing.ithasset", "test.widget");
        typed.rescan();
        typed.navigate("Mixed");
        const Value view = typed.execute("ith.content.set_view", R"({"sortBy": "type", "mode": "list"})");
        CHECK(view.find("sortBy")->asString() == "type");
        CHECK(view.find("mode")->asString() == "list");
        // Data, Level, Model, Other; then by name.
        CHECK(typed.labels() == std::vector<std::string>{"Jump", "Alpha", "Zone", "Boat.gltf", "Thing"});
        fails(*typed.session, "ith.content.set_view", R"({"sortBy": "size"})", "editor.command.invalid_args");
        fails(*typed.session, "ith.content.set_view", R"({"mode": "tiles"})", "editor.command.invalid_args");
        fails(*typed.session, "ith.content.set_view", R"({"descending": 1})", "editor.command.invalid_args");
    }

    TEST_CASE("sorting more items than a small-range sort handles: descending is the exact reverse of each group") {
        // 40 assets written in a scrambled order (13 is coprime with 40), half of them levels, and
        // three folders: name order is Item1 to Item40, type order the levels before the materials.
        Project p;
        std::vector<std::string> byName;
        std::vector<std::string> levels;
        std::vector<std::string> materials;
        for (int n = 1; n <= 40; ++n) {
            byName.push_back("Item" + std::to_string(n));
            (n % 2 == 0 ? levels : materials).push_back(byName.back());
        }
        for (int step = 0; step < 40; ++step) {
            const int n = step * 13 % 40 + 1;
            const std::string name = "Many/Item" + std::to_string(n);
            if (n % 2 == 0) {
                p.native(name + ".ithmap", "ith.level");
            } else {
                p.native(name + ".ithasset");
            }
        }
        for (const std::string_view name : {"Many/F10", "Many/F2", "Many/F1"}) {
            p.folder(name);
        }
        p.rescan();
        p.navigate("Many");
        const auto withFolders = [](std::vector<std::string> folders, const std::vector<std::string>& assets) {
            folders.insert(folders.end(), assets.begin(), assets.end());
            return folders;
        };
        const auto reversed = [](std::vector<std::string> list) {
            std::ranges::reverse(list);
            return list;
        };
        const std::vector<std::string> byType = withFolders(levels, materials);
        CHECK(p.labels() == withFolders({"F1", "F2", "F10"}, byName));
        p.execute("ith.content.set_view", R"({"descending": true})");
        CHECK(p.labels() == withFolders({"F10", "F2", "F1"}, reversed(byName)));
        p.execute("ith.content.set_view", R"({"sortBy": "type", "descending": false})");
        CHECK(p.labels() == withFolders({"F1", "F2", "F10"}, byType));
        p.execute("ith.content.set_view", R"({"descending": true})");
        CHECK(p.labels() == withFolders({"F10", "F2", "F1"}, reversed(byType)));
        // A search shows the same order without the folders.
        p.model().setSearch("item");
        CHECK(p.labels() == reversed(byType));
    }

    TEST_CASE("search and filter cover the folder and its subfolders, without folder items (requirements 7 to 10)") {
        Project p;
        p.native("Props/Rock.ithasset");
        p.native("Props/Shards/RockShard.ithasset");
        p.native("Props/Élan.ithasset");
        p.native("Props/Main.ithmap", "ith.level");
        p.imported("Props/Shards/MainMesh.gltf");
        p.native("Props/Mainframe.ithasset", "ith.input_action");
        p.native("Elsewhere/MainHall.ithmap", "ith.level");
        p.rescan();
        p.navigate("Props");
        const std::vector<std::string> own = p.labels();
        // Bytes outside ASCII sort after ASCII letters.
        CHECK(own == std::vector<std::string>{"Shards", "Main", "Mainframe", "Rock", "Élan"});

        Value shown = p.execute("ith.content.set_filter", R"({"search": "ro sh"})");
        CHECK(shown.find("shown")->toNumber<int>() == 1);
        CHECK(p.labels() == std::vector<std::string>{"RockShard"});
        CHECK(p.model().searching());
        p.execute("ith.content.set_filter", R"({"search": "  "})");
        CHECK(p.labels() == own); // spaces only: no search

        // ASCII without case, other letters exactly: "é" doesn't match "É".
        p.execute("ith.content.set_filter", R"({"search": "élan"})");
        CHECK(p.labels().empty());
        p.execute("ith.content.set_filter", R"({"search": "ÉLAN"})");
        CHECK(p.labels() == std::vector<std::string>{"Élan"});
        p.execute("ith.content.set_filter", R"({"search": "LAN"})");
        CHECK(p.labels() == std::vector<std::string>{"Élan"});

        // Categories combine with OR, search and filter with AND, from the current folder down.
        p.execute("ith.content.set_filter", R"({"search": "main", "categories": ["model", "level"]})");
        CHECK(p.labels() == std::vector<std::string>{"Main", "MainMesh.gltf"});
        fails(*p.session, "ith.content.set_filter", R"({"categories": ["sounds"]})", "editor.command.invalid_args");
        fails(*p.session, "ith.content.set_filter", R"({"categories": "level"})", "editor.command.invalid_args");

        // Changing the search drops what is no longer shown from the selection.
        p.select({"Props/Main.ithmap", "Props/Shards/MainMesh.gltf"}, "replace");
        p.execute("ith.content.set_filter", R"({"categories": ["level"]})");
        CHECK(p.selected() == std::vector<std::string>{"Props/Main.ithmap"});

        // Navigating clears the search and the selection and keeps the filter.
        p.navigate("Props/Shards");
        CHECK(p.model().search().empty());
        CHECK(p.model().selection().empty());
        CHECK(p.model().filter().has(ith::ContentCategory::Level));
        p.execute("ith.content.set_filter", R"({"categories": []})");
        CHECK(p.labels() == std::vector<std::string>{"MainMesh.gltf", "RockShard"});
        const Value content = p.execute("ith.query.content");
        CHECK(content.find("categories")->elements().empty());
        CHECK(content.find("search")->asString().empty());
    }

    TEST_CASE("selection: click, Ctrl, Shift, Ctrl + Shift, empty space, primary and anchor (requirements 11, 13)") {
        Project p;
        for (const std::string_view name : {"A", "B", "C", "D", "E"}) {
            p.native("S/" + std::string(name) + ".ithasset");
        }
        p.rescan();
        p.navigate("S");
        const auto path = [](std::string_view name) { return "S/" + std::string(name) + ".ithasset"; };

        Value result = p.select({"S/B.ithasset"}, "replace");
        CHECK(result.find("primary")->find("path")->asString() == path("B"));
        CHECK(*p.model().anchor() == keyOf(p.item(path("B"))));
        p.select({"S/D.ithasset"}, "range"); // Shift: from the anchor B to D, the anchor stays
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("C"), path("D")});
        CHECK(*p.model().anchor() == keyOf(p.item(path("B"))));
        CHECK(*p.model().focusItem() == keyOf(p.item(path("D"))));
        CHECK(*p.model().primary() == keyOf(p.item(path("D"))));
        p.select({"S/A.ithasset"}, "range"); // backwards: D is gone, A is primary
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("A")});

        p.select({"S/E.ithasset"}, "toggle"); // Ctrl: in, and the new anchor
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("A"), path("E")});
        CHECK(*p.model().anchor() == keyOf(p.item(path("E"))));
        p.select({"S/A.ithasset"}, "toggle"); // out, and still the new anchor
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("E")});
        CHECK(*p.model().anchor() == keyOf(p.item(path("A"))));
        p.select({"S/C.ithasset"}, "add_range"); // Ctrl + Shift: A to C added, B keeps its place
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("E"), path("A"), path("C")});
        CHECK(*p.model().primary() == keyOf(p.item(path("C"))));
        p.select({"S/A.ithasset"}, "remove");
        CHECK(p.selected() == std::vector<std::string>{path("B"), path("E"), path("C")});
        p.select({"S/B.ithasset"}, "add"); // added again: it becomes the primary item
        CHECK(p.selected() == std::vector<std::string>{path("E"), path("C"), path("B")});
        CHECK(*p.model().primary() == keyOf(p.item(path("B"))));

        // A click on empty space clears; Shift without an anchor acts like a plain click.
        p.execute("ith.content.select", R"({"items": []})");
        CHECK(p.model().selection().empty());
        CHECK_FALSE(p.model().anchor().has_value());
        p.select({"S/C.ithasset"}, "range");
        CHECK(p.selected() == std::vector<std::string>{path("C")});

        // Items by id; refused items change nothing.
        const std::string id = p.item(path("E")).id.toString();
        result = p.execute("ith.content.select", R"({"items": [{"id": ")" + id + R"("}], "mode": "add"})");
        CHECK(result.find("selection")->elements().size() == 2);
        fails(*p.session, "ith.content.select", R"({"items": [{"path": "S/Nope.ithasset"}]})",
              "editor.content.not_found");
        fails(*p.session, "ith.content.select", R"({"items": [{"path": "S"}]})", "editor.content.not_found");
        fails(*p.session, "ith.content.select", R"({"items": [{"name": "A"}]})", "editor.command.invalid_args");
        fails(*p.session, "ith.content.select", R"({"items": [{"path": "S/A.ithasset"}], "mode": "all"})",
              "editor.command.invalid_args");
        fails(*p.session, "ith.content.select", R"({"mode": "replace"})", "editor.command.invalid_args");
        CHECK(p.model().selection().size() == 2);

        p.execute("ith.content.select_all");
        CHECK(p.model().selection().size() == 5);
        CHECK(*p.model().primary() == keyOf(p.item(path("E"))));
        const Value dismissed = p.execute("ith.content.dismiss");
        CHECK(dismissed.find("selection")->elements().empty());
        CHECK_FALSE(dismissed.find("drawerOpen")->asBool());
    }

    TEST_CASE("selecting changes neither the level, its undo history nor its entity selection (requirement 14)") {
        Project p;
        p.native("S/A.ithasset");
        p.rescan();
        p.navigate("S");
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(1) + R"(", "label": "Lamp"})");
        const Value history = p.execute("ith.query.history");
        const Value selection = p.execute("ith.query.selection");
        const Value document = p.execute("ith.query.document");
        p.select({"S/A.ithasset"}, "replace");
        p.execute("ith.content.select_all");
        p.execute("ith.content.dismiss");
        p.execute("ith.content.set_view", R"({"mode": "list"})");
        p.execute("ith.content.set_filter", R"({"search": "a"})");
        p.execute("ith.content.navigate_up");
        CHECK(p.execute("ith.query.history") == history);
        CHECK(p.execute("ith.query.selection") == selection);
        CHECK(p.execute("ith.query.document") == document);
    }

    TEST_CASE("navigation: folders, the parent folder, refusals (requirement 10)") {
        Project p;
        p.folder("A/B/C");
        p.rescan();
        CHECK(p.execute("ith.content.navigate", R"({"folder": "A/B/C"})").find("folder")->asString() == "A/B/C");
        CHECK(p.model().isExpanded("A"));
        CHECK(p.model().isExpanded("A/B"));
        CHECK(p.execute("ith.content.navigate_up").find("folder")->asString() == "A/B");
        p.execute("ith.content.navigate_up");
        p.execute("ith.content.navigate_up");
        CHECK(p.execute("ith.content.navigate_up").find("folder")->asString().empty()); // nothing at the root
        fails(*p.session, "ith.content.navigate", R"({"folder": "Nowhere"})", "editor.content.not_found");
        fails(*p.session, "ith.content.navigate", "{}", "editor.command.invalid_args");
        // Opening a folder goes into it.
        CHECK(p.execute("ith.content.open", R"({"item": {"path": "A"}})").find("folder")->asString() == "A");
        CHECK(p.execute("ith.content.open").kind() == Value::Kind::Null); // nothing selected: nothing to open
    }

    TEST_CASE("without an asset database every content command fails and the queries are empty") {
        EditorSession session(types());
        for (const std::string_view id :
             {"ith.content.navigate", "ith.content.navigate_up", "ith.content.select", "ith.content.select_all",
              "ith.content.dismiss", "ith.content.set_view", "ith.content.set_filter", "ith.content.rescan",
              "ith.content.open", "ith.content.toggle_drawer"}) {
            CAPTURE(id);
            fails(session, id, "{}", "editor.document.no_asset_database");
        }
        CHECK(run(session, "ith.query.content").find("items")->elements().empty());
        CHECK(run(session, "ith.query.content_tree").find("folders")->elements().size() == 1);
        CHECK(run(session, "ith.query.content_problems").find("problems")->elements().empty());
    }

    TEST_CASE("Rescan after outside changes keeps the folder, the selection by id and the level's path "
              "(requirements 15, 50)") {
        Project p;
        const AssetId rock = p.native("Props/Rock.ithasset");
        p.native("Props/Old.ithasset");
        p.folder("Props/Sub");
        p.rescan();
        p.navigate("Props/Sub");
        p.navigate("Props");
        p.select({"Props/Rock.ithasset", "Props/Old.ithasset"}, "replace");

        // Outside the editor: Rock renamed, Old deleted, New added.
        REQUIRE(ith::fs::move(p.file("Props/Rock.ithasset"), p.file("Props/Boulder.ithasset")).hasValue());
        REQUIRE(ith::fs::remove(p.file("Props/Old.ithasset")).hasValue());
        p.native("Props/New.ithasset");
        const Value counts = p.execute("ith.content.rescan");
        CHECK(counts.find("assets")->toNumber<int>() == 2);
        CHECK(counts.find("problems")->toNumber<int>() == 0);
        CHECK(p.labels() == std::vector<std::string>{"Sub", "Boulder", "New"});
        CHECK(p.selected() == std::vector<std::string>{"Props/Boulder.ithasset"});
        CHECK(p.model().selection().front().id == rock);

        // The current folder deleted outside: its nearest existing ancestor.
        p.navigate("Props/Sub");
        p.model().setExpanded("Props/Sub", true);
        CHECK(p.model().isExpanded("Props/Sub"));
        REQUIRE(ith::fs::remove(p.file("Props/Sub")).hasValue());
        p.rescan();
        CHECK(p.model().currentFolder() == "Props");
        CHECK_FALSE(p.model().isExpanded("Props/Sub"));
    }

    TEST_CASE("Rescan follows the open level moved outside the editor, without making it dirty (requirement 50)") {
        Project p;
        p.execute("ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        CHECK(p.model().findByPath("Levels/Main.ithmap") != nullptr); // shown without a Rescan
        CHECK(p.session->document().path() == "Content/Levels/Main.ithmap");
        REQUIRE(ith::fs::createDirectories(p.file("Maps")).hasValue());
        REQUIRE(ith::fs::move(p.file("Levels/Main.ithmap"), p.file("Maps/Harbour.ithmap")).hasValue());
        p.rescan();
        CHECK(p.session->document().path() == "Content/Maps/Harbour.ithmap");
        CHECK_FALSE(p.session->document().dirty());
        // Gone: the level stays open with its path, and saving writes it again.
        REQUIRE(ith::fs::remove(p.file("Maps/Harbour.ithmap")).hasValue());
        p.rescan();
        CHECK(p.session->document().path() == "Content/Maps/Harbour.ithmap");
        p.execute("ith.file.save");
        CHECK(exists(p.file("Maps/Harbour.ithmap")));
    }

    TEST_CASE("open: a level by id, the unsaved-changes answer, refused while playing (requirement 51)") {
        Project p;
        p.execute("ith.file.save_as", R"({"path": "Content/Levels/A.ithmap"})");
        const std::string first = p.session->document().levelId().toString();
        p.execute("ith.file.new_level");
        p.execute("ith.file.save_as", R"({"path": "Content/Levels/B.ithmap"})");
        // Both levels show without a Rescan: a save of a new level into Content adds it.
        p.navigate("Levels");

        p.execute("ith.content.open", R"({"item": {"path": "Levels/A.ithmap"}})");
        CHECK(p.session->document().path() == "Content/Levels/A.ithmap");
        CHECK(p.session->document().levelId().toString() == first);

        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(1) + R"("})");
        fails(*p.session, "ith.content.open", R"({"item": {"path": "Levels/B.ithmap"}})",
              "editor.document.unsaved_changes");
        p.execute("ith.content.open", R"({"item": {"path": "Levels/B.ithmap"}, "unsaved": "save"})");
        CHECK(p.session->document().path() == "Content/Levels/B.ithmap");

        // The saved A holds the entity; open it again without saving B's changes.
        p.execute("ith.content.open", R"({"item": {"path": "Levels/A.ithmap"}})");
        CHECK(p.session->document().world().entityCount() == 1);
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(2) + R"("})");
        p.select({"Levels/B.ithmap"}, "replace");
        p.execute("ith.content.open", R"({"unsaved": "discard"})"); // the primary item
        CHECK(p.session->document().path() == "Content/Levels/B.ithmap");
        CHECK_FALSE(p.session->document().dirty());

        p.session->document().setPlaying(true);
        fails(*p.session, "ith.content.open", R"({"item": {"path": "Levels/A.ithmap"}})", "editor.command.playing");
        // Browsing goes on while playing.
        p.execute("ith.content.navigate_up");
        p.session->document().setPlaying(false);

        // Other assets have no editor yet: nothing happens.
        p.native("Data.ithasset");
        p.rescan();
        CHECK(p.execute("ith.content.open", R"({"item": {"path": "Data.ithasset"}})").kind() == Value::Kind::Null);
        CHECK(p.session->document().path() == "Content/Levels/B.ithmap");
    }

    TEST_CASE("the drawer opens and closes, or the open panel comes to the front (requirements 52, 54)") {
        Project p;
        p.rescan();
        Value result = p.execute("ith.content.toggle_drawer");
        CHECK(result.find("open")->asBool());
        CHECK(result.find("target")->asString() == "drawer");
        CHECK(p.model().drawerOpen());
        result = p.execute("ith.content.toggle_drawer");
        CHECK_FALSE(result.find("open")->asBool());
        p.execute("ith.content.toggle_drawer", R"({"open": true})");
        CHECK(p.model().drawerOpen());
        // Escape with nothing selected closes it (ith.content.dismiss).
        CHECK_FALSE(p.execute("ith.content.dismiss").find("drawerOpen")->asBool());
        fails(*p.session, "ith.content.toggle_drawer", R"({"open": 1})", "editor.command.invalid_args");

        // With the panel in the layout, the panel is focused instead and the layout doesn't change.
        ith::test::editor::addP2Panels(p.session->panels());
        p.session->layouts().reset();
        static_cast<void>(p.session->layouts().takeFocusRequest());
        const Value layout = p.execute("ith.query.layout");
        result = p.execute("ith.content.toggle_drawer");
        CHECK(result.find("target")->asString() == "panel");
        CHECK(result.find("open")->asBool());
        CHECK_FALSE(p.model().drawerOpen());
        CHECK(p.session->layouts().takeFocusRequest() == std::optional<std::string>("ith.content_browser"));
        CHECK(p.execute("ith.query.layout") == layout);
        // Closing explicitly still works on the drawer.
        CHECK(p.execute("ith.content.toggle_drawer", R"({"open": false})").find("target")->asString() == "drawer");
    }

    TEST_CASE("the Content keymap context wins only while the browser has the focus (requirement 55)") {
        Project p;
        p.native("S/A.ithasset");
        p.native("S/B.ithasset");
        p.rescan();
        p.navigate("S");
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(1) + R"("})");
        run(*p.session, "ith.edit.select_none");

        CHECK(ith::contextOf("ith.content.select_all") == ith::KeymapContext::Content);
        CHECK(ith::contextOf("ith.content.rename") == ith::KeymapContext::Content);
        CHECK(ith::contextOf("ith.content.toggle_drawer") == ith::KeymapContext::Editor);
        CHECK(ith::contextOf("ith.edit.select_all") == ith::KeymapContext::Editor);

        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const ith::Keymap keymap = ith::Keymap::load(config, p.session->commands(), warnings);
        CHECK_FALSE(
            std::ranges::any_of(warnings, [](const ith::Error& w) { return w.code == "editor.keymap.conflict"; }));
        CHECK(*keymap.commandFor(*ith::parseChord("Keyboard.Escape"), ith::KeymapContext::Content) ==
              "ith.content.dismiss");
        CHECK(*keymap.commandFor(*ith::parseChord("Ctrl+Keyboard.Space")) == "ith.content.toggle_drawer");
        CHECK(*keymap.commandFor(*ith::parseChord("Alt+Keyboard.Up"), ith::KeymapContext::Content) ==
              "ith.content.navigate_up");
        ith::KeymapConsumer shortcuts(*p.session, keymap);

        // Focus elsewhere: Ctrl + A selects the entities.
        p.model().setKeyboardFocus(false);
        shortcuts.onInputEvent(keyDown(ith::Key::A, U'a', ith::KeyModifiers::Control));
        CHECK(p.session->document().selection().entities().size() == 1);
        CHECK(p.model().selection().empty());

        // Focus in the browser: the content commands; the entity selection stays.
        p.model().setKeyboardFocus(true);
        run(*p.session, "ith.edit.select_none");
        shortcuts.onInputEvent(keyDown(ith::Key::A, U'a', ith::KeyModifiers::Control));
        CHECK(p.model().selection().size() == 2);
        CHECK(p.session->document().selection().entities().empty());
        shortcuts.onInputEvent(keyDown(ith::Key::Escape));
        CHECK(p.model().selection().empty());
        // F2, Delete and Ctrl + D run the content file operations, never the entity commands: Delete asks
        // first, F2 starts the inline rename, Ctrl + D duplicates.
        run(*p.session, "ith.edit.select_all");
        p.select({"S/A.ithasset"}, "replace");
        const std::size_t entries = p.session->document().history().entries().size();
        CHECK(shortcuts.onInputEvent(keyDown(ith::Key::Delete)) == ith::InputReply::Consumed);
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Delete);
        CHECK(exists(p.file("S/A.ithasset")));
        CHECK(shortcuts.onInputEvent(keyDown(ith::Key::F2)) == ith::InputReply::Consumed);
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Rename);
        CHECK(shortcuts.onInputEvent(keyDown(ith::Key::D, U'd', ith::KeyModifiers::Control)) ==
              ith::InputReply::Consumed);
        CHECK(exists(p.file("S/A_2.ithasset")));
        CHECK(p.session->document().world().entityCount() == 1);
        CHECK(p.session->document().history().entries().size() == entries);
        run(*p.session, "ith.content.dismiss"); // ends the rename prompt
        CHECK(p.model().prompt() == nullptr);
        // With the content commands unbound in a higher layer, the keys stay the browser's: they never
        // fall through to the entity commands (kContentOwnedChords).
        std::vector<ith::Error> unboundWarnings;
        const ith::serialization::Value engine = json(R"({"ith.edit.delete": ["Keyboard.Delete"],
            "ith.edit.duplicate": ["Ctrl+Keyboard.D"], "ith.edit.rename": ["Keyboard.F2"]})");
        const ith::serialization::Value user =
            json(R"({"ith.content.delete": [], "ith.content.duplicate": [], "ith.content.rename": []})");
        const std::array<const ith::serialization::Value*, 2> layers = {&engine, &user};
        const ith::Keymap unbound = ith::Keymap::fromLayers(layers, p.session->commands(), unboundWarnings);
        ith::KeymapConsumer unboundShortcuts(*p.session, unbound);
        run(*p.session, "ith.edit.select_all");
        for (const ith::Event& event : {keyDown(ith::Key::Delete), keyDown(ith::Key::F2),
                                        keyDown(ith::Key::D, U'd', ith::KeyModifiers::Control)}) {
            CHECK(unboundShortcuts.onInputEvent(event) == ith::InputReply::Consumed);
        }
        CHECK(p.session->document().world().entityCount() == 1);
        CHECK(p.model().prompt() == nullptr);
        // Rebinding still works: a content command on another chord runs, and the owned chord stays inert.
        const ith::serialization::Value rebound = json(R"({"ith.content.delete": ["Keyboard.Backspace"]})");
        const std::array<const ith::serialization::Value*, 2> reboundLayers = {&engine, &rebound};
        const ith::Keymap moved = ith::Keymap::fromLayers(reboundLayers, p.session->commands(), unboundWarnings);
        ith::KeymapConsumer movedShortcuts(*p.session, moved);
        CHECK(movedShortcuts.onInputEvent(keyDown(ith::Key::Delete)) == ith::InputReply::Consumed);
        CHECK(p.session->document().world().entityCount() == 1);
        CHECK(movedShortcuts.onInputEvent(keyDown(ith::Key::Backspace)) == ith::InputReply::Consumed);
        REQUIRE(p.model().prompt() != nullptr);
        CHECK(p.model().prompt()->kind == ith::ContentPromptKind::Delete);
        run(*p.session, "ith.content.dismiss");
        run(*p.session, "ith.edit.select_none");
        // Enter opens the primary item; Alt + Up goes to the parent folder.
        shortcuts.onInputEvent(keyDown(ith::Key::Up, 0, ith::KeyModifiers::Alt));
        CHECK(p.model().currentFolder().empty());
        p.select({"S"}, "replace");
        shortcuts.onInputEvent(keyDown(ith::Key::Enter));
        CHECK(p.model().currentFolder() == "S");
        // Ctrl + Space works with the focus anywhere.
        p.model().setKeyboardFocus(false);
        shortcuts.onInputEvent(keyDown(ith::Key::Space, U' ', ith::KeyModifiers::Control));
        CHECK(p.model().drawerOpen());
    }

    TEST_CASE("the session state comes back in a new session; search, filter and selection don't "
              "(requirement 58)") {
        Project p;
        p.folder("A/B");
        p.folder("C");
        p.native("A/B/X.ithasset");
        p.rescan();
        p.navigate("A/B");
        p.execute("ith.content.set_view", R"({"mode": "list", "sortBy": "type", "descending": true})");
        p.model().setExpanded("C", true);
        p.model().setDrawerHeight(320.0f);
        p.model().setTreeWidth(0.35f);
        p.execute("ith.content.set_filter", R"({"search": "x", "categories": ["data"]})");
        p.execute("ith.content.select_all");
        p.execute("ith.content.toggle_drawer");
        REQUIRE(exists(ith::fs::joinPath(p.project.root(), ith::kContentBrowserStateFile)));

        p.reopen();
        p.rescan();
        CHECK(p.model().currentFolder() == "A/B");
        CHECK(p.model().view().mode == ith::ContentViewMode::List);
        CHECK(p.model().view().sortBy == ith::ContentSortKey::Type);
        CHECK(p.model().view().descending);
        CHECK(p.model().isExpanded("C"));
        CHECK(p.model().isExpanded("A"));
        CHECK(p.model().drawerHeight() == doctest::Approx(320.0f));
        CHECK(p.model().treeWidth() == doctest::Approx(0.35f));
        CHECK(p.model().search().empty());
        CHECK(p.model().filter().empty());
        CHECK(p.model().selection().empty());
        CHECK_FALSE(p.model().drawerOpen());

        // A saved folder that was deleted falls back to its nearest ancestor.
        p.session.reset();
        REQUIRE(ith::fs::remove(p.file("A/B/X.ithasset")).hasValue());
        REQUIRE(ith::fs::remove(p.file("A/B")).hasValue());
        p.reopen();
        p.rescan();
        CHECK(p.model().currentFolder() == "A");

        // An unreadable file: the defaults, and it is written again on the next change.
        p.session.reset();
        REQUIRE(
            ith::fs::writeFileAtomic(ith::fs::joinPath(p.project.root(), ith::kContentBrowserStateFile), "{ not json")
                .hasValue());
        p.reopen();
        p.rescan();
        CHECK(p.model().currentFolder().empty());
        CHECK(p.model().view() == ith::ContentViewSettings{});
        p.navigate("C");
        p.reopen();
        p.rescan();
        CHECK(p.model().currentFolder() == "C");
    }

    TEST_CASE("nothing is rebuilt without a change; changes bump the generation (requirement 61)") {
        Project p;
        p.native("S/A.ithasset");
        p.rescan();
        const std::uint64_t builds = p.model().viewBuilds();
        const std::uint64_t generation = p.model().generation();
        static_cast<void>(p.execute("ith.query.content"));
        static_cast<void>(p.execute("ith.query.content_tree"));
        CHECK(p.model().viewBuilds() == builds);
        CHECK(p.model().generation() == generation);
        p.execute("ith.content.set_view", R"({"descending": true})");
        CHECK(p.model().viewBuilds() == builds); // a re-sort, not a rebuild
        CHECK(p.model().generation() > generation);
        p.navigate("S");
        CHECK(p.model().viewBuilds() == builds + 1);
    }

    TEST_CASE("the asset view's widget keys run nothing while the browser has the focus: End is not Snap to "
              "Floor") {
        Project p;
        p.rescan();
        run(*p.session, "ith.entity.create",
            R"({"id": ")" + ith::test::editor::idText(1) +
                R"(", "components": {"ith::LocalTransform": {"value": {"translation": {"x": 0, "y": 3, "z": 0}}},
                                     "ith::ShapeRenderer": {"shape": "Cube"}}})");
        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const ith::Keymap keymap = ith::Keymap::load(config, p.session->commands(), warnings);
        REQUIRE(*keymap.commandFor(*ith::parseChord("Keyboard.End")) == "ith.viewport.snap_to_floor");
        ith::KeymapConsumer shortcuts(*p.session, keymap);
        const std::size_t cursor = p.session->document().history().cursor();
        const std::string level = ith::test::editor::levelText(*p.session);

        p.model().setKeyboardFocus(true);
        for (const ith::Key key :
             {ith::Key::End, ith::Key::Home, ith::Key::Up, ith::Key::Down, ith::Key::Left, ith::Key::Right}) {
            CHECK(shortcuts.onInputEvent(keyDown(key)) == ith::InputReply::Consumed);
            CHECK(shortcuts.onInputEvent(keyDown(key, 0, ith::KeyModifiers::Shift)) == ith::InputReply::Consumed);
        }
        CHECK(shortcuts.onInputEvent(keyDown(ith::Key::F, U'f', ith::KeyModifiers::Control)) ==
              ith::InputReply::Consumed);
        CHECK(p.session->document().history().cursor() == cursor);
        CHECK(ith::test::editor::levelText(*p.session) == level);
        CHECK(p.session->document().dirty());

        // Elsewhere, End is Snap to Floor again.
        p.model().setKeyboardFocus(false);
        shortcuts.onInputEvent(keyDown(ith::Key::End));
        CHECK(p.session->document().history().cursor() == cursor + 1);
    }

    TEST_CASE("Ctrl + Space gives the browser the focus at once, so a Delete right after it spares the level") {
        Project p;
        p.native("S/A.ithasset");
        p.rescan();
        run(*p.session, "ith.entity.create", R"({"id": ")" + ith::test::editor::idText(1) + R"("})");
        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const ith::Keymap keymap = ith::Keymap::load(config, p.session->commands(), warnings);
        ith::KeymapConsumer shortcuts(*p.session, keymap);
        CHECK_FALSE(p.model().keyboardFocus());

        // One batch, without a UI frame between the keys.
        shortcuts.onInputEvent(keyDown(ith::Key::Space, U' ', ith::KeyModifiers::Control));
        CHECK(p.model().drawerOpen());
        CHECK(p.model().keyboardFocus());
        shortcuts.onInputEvent(keyDown(ith::Key::Delete));
        CHECK(p.session->document().world().entityCount() == 1);
        // Escape with nothing selected closes the drawer and gives the keys back to the level.
        shortcuts.onInputEvent(keyDown(ith::Key::Escape));
        CHECK_FALSE(p.model().drawerOpen());
        CHECK_FALSE(p.model().keyboardFocus());
        shortcuts.onInputEvent(keyDown(ith::Key::Delete));
        CHECK(p.session->document().world().entityCount() == 0);

        // A mouse press inside the browser's last rectangles gives it the focus, outside takes it.
        p.model().setScreenRects({ith::ContentScreenRect{.x = 0.0f, .y = 400.0f, .width = 800.0f, .height = 200.0f}});
        p.model().pointerPressed(10.0f, 450.0f);
        CHECK(p.model().keyboardFocus());
        p.model().pointerPressed(10.0f, 100.0f);
        CHECK_FALSE(p.model().keyboardFocus());
        p.model().setScreenRects({});
        p.model().pointerPressed(10.0f, 450.0f);
        CHECK_FALSE(p.model().keyboardFocus());

        // A press inside the tree's rectangle gives the tree the focus; elsewhere in the browser, the
        // asset view.
        p.model().setScreenRects({ith::ContentScreenRect{.x = 0.0f, .y = 400.0f, .width = 800.0f, .height = 200.0f}},
                                 {ith::ContentScreenRect{.x = 0.0f, .y = 420.0f, .width = 160.0f, .height = 160.0f}});
        p.model().pointerPressed(10.0f, 450.0f);
        CHECK(p.model().keyboardFocus());
        CHECK(p.model().treeFocus());
        p.model().pointerPressed(400.0f, 450.0f);
        CHECK(p.model().keyboardFocus());
        CHECK_FALSE(p.model().treeFocus());
        p.model().pointerPressed(10.0f, 450.0f);
        p.model().pointerPressed(10.0f, 100.0f);
        CHECK_FALSE(p.model().keyboardFocus());
        CHECK_FALSE(p.model().treeFocus());
    }

    TEST_CASE("navigating to the current folder itself keeps it and expands the folders above it") {
        Project p;
        // A path longer than a short string's inline buffer: a view into the replaced string would
        // point at freed memory.
        const std::string folder = "Alpha/Bravo/Charlie/Delta/Echo";
        REQUIRE(folder.size() > 23);
        p.native(folder + "/A.ithasset");
        p.rescan();
        p.navigate(folder);
        p.model().setExpanded("Alpha", false);
        p.model().setExpanded("Alpha/Bravo", false);
        REQUIRE(p.model().navigate(p.model().currentFolder()).hasValue());
        CHECK(p.model().currentFolder() == folder);
        for (const std::string_view above :
             {"Alpha", "Alpha/Bravo", "Alpha/Bravo/Charlie", "Alpha/Bravo/Charlie/Delta"}) {
            CAPTURE(above);
            CHECK(p.model().isExpanded(above));
        }
        CHECK(p.model().shown().size() == 1);
    }

    TEST_CASE("a new level saved into Content shows at once; a Save, or a save outside Content, rescans nothing") {
        Project p;
        // The root exists before the first refresh.
        CHECK(p.execute("ith.content.navigate", R"({"folder": ""})").find("folder")->asString().empty());
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/First.ithmap"})");
        const ith::ContentItem* first = p.model().findByPath("Maps/First.ithmap");
        REQUIRE(first != nullptr);
        CHECK(first->id == AssetId(p.session->document().levelId()));
        CHECK(p.model().folderExists("Maps"));
        CHECK_FALSE(p.session->document().dirty());

        // Saving it again rescans nothing.
        const std::uint64_t builds = p.model().viewBuilds();
        p.execute("ith.file.save");
        CHECK(p.model().viewBuilds() == builds);
        // Outside Content: not an asset.
        p.execute("ith.file.new_level");
        p.execute("ith.file.save_as", R"({"path": "Scratch/Loose.ithmap"})");
        CHECK(p.model().viewBuilds() == builds);
    }

    TEST_CASE("a Save As of a known level shows the copy at once under a new id, and a Rescan finds no duplicate "
              "(ADR-0050 item 9)") {
        Project p;
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/First.ithmap"})");
        const AssetId first(p.session->document().levelId());
        p.navigate("Maps");
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/Copy.ithmap"})");
        const AssetId copy(p.session->document().levelId());
        CHECK(copy != first);
        CHECK(p.item("Maps/First.ithmap").id == first);
        CHECK(p.item("Maps/Copy.ithmap").id == copy);
        CHECK(p.labels() == std::vector<std::string>{"Copy", "First"});

        const Value counts = p.execute("ith.content.rescan");
        CHECK(counts.find("assets")->toNumber<int>() == 2);
        CHECK(counts.find("problems")->toNumber<int>() == 0);
        CHECK(p.item("Maps/First.ithmap").id == first);
        CHECK(p.item("Maps/Copy.ithmap").id == copy);
        CHECK(p.session->document().path() == "Content/Maps/Copy.ithmap");
        CHECK(AssetId(p.session->document().levelId()) == copy);

        // Opening the original by id opens the original file.
        p.execute("ith.file.open_level", R"({"id": ")" + first.toString() + R"("})");
        CHECK(p.session->document().path() == "Content/Maps/First.ithmap");
    }

    TEST_CASE("a level copied outside the editor and opened takes the id the scan gives its file; Saves keep "
              "them equal") {
        for (const bool manualRescan : {true, false}) {
            CAPTURE(manualRescan);
            Project p;
            p.execute("ith.file.save_as", R"({"path": "Content/Maps/A.ithmap"})");
            const AssetId original(p.session->document().levelId());
            const ith::Expected<std::string> text = ith::fs::readFile(p.file("Maps/A.ithmap"), std::size_t{1} << 20);
            REQUIRE(text.hasValue());
            p.write("Maps/B.ithmap", *text); // the same id, in another file
            p.execute("ith.file.open_level", R"({"path": "Content/Maps/B.ithmap"})");
            REQUIRE(AssetId(p.session->document().levelId()) == original);
            if (manualRescan) {
                p.rescan(); // B is the copy: its file gets a new id
            }
            for (int save = 0; save < 2; ++save) {
                CAPTURE(save);
                p.execute("ith.file.save");
                const AssetId fileId = p.item("Maps/B.ithmap").id;
                CHECK(fileId != original);
                CHECK(AssetId(p.session->document().levelId()) == fileId);
                const ith::Expected<std::string> saved =
                    ith::fs::readFile(p.file("Maps/B.ithmap"), std::size_t{1} << 20);
                REQUIRE(saved.hasValue());
                CHECK(saved->find(fileId.toString()) != std::string::npos);
                CHECK(p.session->document().path() == "Content/Maps/B.ithmap");
            }
            const std::uint64_t builds = p.model().viewBuilds();
            p.execute("ith.file.save");
            CHECK(p.model().viewBuilds() == builds); // nothing to rescan any more
            CHECK(p.item("Maps/A.ithmap").id == original);
        }
    }

    TEST_CASE("a Save As over a level takes its id: no Rescan when the database knows the file, shown when it "
              "doesn't (ADR-0050 item 9)") {
        Project p;
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/Target.ithmap"})");
        const AssetId target(p.session->document().levelId());
        p.execute("ith.file.new_level");
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/Mine.ithmap"})");
        const AssetId mine(p.session->document().levelId());
        p.navigate("Maps");

        const std::uint64_t builds = p.model().viewBuilds();
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/Target.ithmap"})");
        CHECK(AssetId(p.session->document().levelId()) == target);
        CHECK(p.model().viewBuilds() == builds); // known path, unchanged id
        CHECK(p.item("Maps/Target.ithmap").id == target);
        CHECK(p.item("Maps/Mine.ithmap").id == mine);
        CHECK(p.execute("ith.content.rescan").find("problems")->toNumber<int>() == 0);

        // A level file written outside the editor, which the database doesn't have yet: shown at once.
        const AssetId outside = p.native("Maps/Outside.ithmap", "ith.level");
        p.execute("ith.file.save_as", R"({"path": "Content/Maps/Outside.ithmap"})");
        CHECK(AssetId(p.session->document().levelId()) == outside);
        CHECK(p.item("Maps/Outside.ithmap").id == outside);
        CHECK(p.model().viewBuilds() > builds);
    }

    TEST_CASE("Rescan waits while a save is being written") {
        TempProject project;
        ith::ImporterRegistry importers;
        ith::AssetDatabase assets(project.root(), importers);
        ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
        EditorSession session(types(), {.projectDirectory = project.root(), .jobs = &jobs, .assets = &assets});
        run(session, "ith.file.save_as", R"({"path": "Content/Levels/Main.ithmap"})");
        REQUIRE(session.document().saving());
        fails(session, "ith.content.rescan", "{}", "editor.document.save_in_progress");
        session.waitIdle();
        CHECK(session.content().findByPath("Levels/Main.ithmap") != nullptr);
        CHECK(run(session, "ith.content.rescan").find("assets")->toNumber<int>() == 1);
    }

    TEST_CASE("the search text is kept to kMaxContentSearchSize bytes, cut at a UTF-8 character") {
        Project p;
        p.native("Rock.ithasset");
        p.rescan();
        const std::string longText(300, 'a');
        p.execute("ith.content.set_filter", R"({"search": ")" + longText + "\"}");
        CHECK(p.model().search().size() == ith::kMaxContentSearchSize);
        // 254 bytes, then a two-byte character that would end at byte 256: cut before it.
        const std::string straddling = std::string(ith::kMaxContentSearchSize - 1, 'a') + "\xC3\xA9";
        p.execute("ith.content.set_filter", R"({"search": ")" + straddling + "\"}");
        CHECK(p.model().search() == std::string(ith::kMaxContentSearchSize - 1, 'a'));
    }

    TEST_CASE("only assets have a category, category filters show assets only, and Enter on the keypad opens") {
        Project p;
        p.folder("Sub");
        p.native("Rock.ithasset");
        p.write("Raw.glb", "glTF");
        p.rescan();
        const Value content = p.execute("ith.query.content");
        for (const Value& item : content.find("items")->elements()) {
            CAPTURE(item.find("path")->asString());
            CHECK((item.find("category") != nullptr) == (item.find("kind")->asString() == "asset"));
        }
        // A category filter never shows problem entries; Problems does.
        p.execute("ith.content.set_filter", R"({"categories": ["other"]})");
        CHECK(p.labels() == std::vector<std::string>{"Rock"}); // the material is Other; Raw.glb isn't
        p.execute("ith.content.set_filter", R"({"categories": ["problems"]})");
        CHECK(p.labels() == std::vector<std::string>{"Raw.glb"});

        const ith::Config config({{.name = "engine", .directory = "engine/config"}});
        std::vector<ith::Error> warnings;
        const ith::Keymap keymap = ith::Keymap::load(config, p.session->commands(), warnings);
        CHECK(*keymap.commandFor(*ith::parseChord("Keyboard.KeypadEnter"), ith::KeymapContext::Content) ==
              "ith.content.open");
    }

    TEST_CASE("parseContentBrowserState reads every member and refuses wrong types") {
        const auto read = ith::parseContentBrowserState(
            R"({"format": "ith.content_browser_state", "version": 1, "data": {"folder": "A/B", "view": "list",
                "sortBy": "type", "descending": true, "expanded": ["A", 3, "C"], "treeWidth": 2,
                "drawerHeight": -5}})",
            "state.json");
        REQUIRE(read.hasValue());
        CHECK(read->folder == "A/B");
        CHECK(read->view == ith::ContentViewSettings{.mode = ith::ContentViewMode::List,
                                                     .sortBy = ith::ContentSortKey::Type,
                                                     .descending = true});
        CHECK(read->expanded == std::vector<std::string>{"A", "C"});
        CHECK(read->treeWidth == doctest::Approx(0.95f));
        CHECK(read->drawerHeight == 0.0f);
        const auto empty =
            ith::parseContentBrowserState(R"({"format": "ith.content_browser_state", "version": 1, "data": {}})", "s");
        REQUIRE(empty.hasValue());
        CHECK(empty->treeWidth == doctest::Approx(ith::kDefaultContentTreeWidth));

        const auto wrong = ith::parseContentBrowserState(
            R"({"format": "ith.content_browser_state", "version": 1, "data": {"folder": 7}})", "state.json");
        REQUIRE_FALSE(wrong.hasValue());
        CHECK(wrong.error().code == "editor.content_browser.invalid_state");
        CHECK(wrong.error().message.find("state.json") != std::string::npos);
        const auto array = ith::parseContentBrowserState(
            R"({"format": "ith.content_browser_state", "version": 1, "data": []})", "state.json");
        CHECK_FALSE(array.hasValue());
        CHECK_FALSE(ith::parseContentBrowserState("{ not json", "state.json").hasValue());
    }
}
