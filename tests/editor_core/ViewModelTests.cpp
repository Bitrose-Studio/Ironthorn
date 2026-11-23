// The view models of the Outliner and Details panels, headless (ADR-0029 rules 1 and 18): the
// tree view model's rows, filter, expansion and ranges; the Outliner's order, labels, selection,
// rename, delete, Enabled and drag and drop; Details' rows from reflection, multi-select, clamping,
// one-shot edits, interactions with live preview and Esc, reset, lists, optionals, components and
// entity references; and the working copy's refresh rule.

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/OutlinerModel.h"
#include "ith/editor_core/TreeViewModel.h"
#include "ith/math/Math.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeOf.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

using ith::DetailsModel;
using ith::DetailsRowKind;
using ith::DropZone;
using ith::EditorSession;
using ith::Entity;
using ith::OutlinerClick;
using ith::OutlinerModel;
using ith::TreeViewModel;
using ith::Uuid;
using ith::test::editor::Gadget;
using ith::test::editor::idOf;
using ith::test::editor::idText;
using ith::test::editor::Lamp;
using ith::test::editor::levelText;
using ith::test::editor::run;
using ith::test::editor::types;

std::string quoted(std::uint64_t n) {
    return "\"" + idText(n) + "\"";
}

void create(EditorSession& session, std::uint64_t n, const std::string& label, const std::string& extra = "") {
    run(session, "ith.entity.create", R"({"id": )" + quoted(n) + R"(, "label": ")" + label + "\"" + extra + "}");
}

Entity entityOf(const EditorSession& session, std::uint64_t n) {
    return session.document().world().findByStableId(idOf(n));
}

// The Outliner's labels with their depth as indentation, in row order.
std::vector<std::string> shown(OutlinerModel& outliner) {
    std::vector<std::string> lines;
    for (const ith::TreeRow& row : outliner.tree().rows()) {
        lines.push_back(std::string(row.depth * 2, ' ') + outliner.entries()[row.item].label);
    }
    return lines;
}

std::vector<std::string> labels(const TreeViewModel& tree, std::span<const ith::TreeRow> rows) {
    std::vector<std::string> result;
    for (const ith::TreeRow& row : rows) {
        result.push_back(std::string(row.depth, '.') + tree.items()[row.item].label);
    }
    return result;
}

std::size_t rowOf(const DetailsModel& details, std::string_view key) {
    const std::optional<std::size_t> row = details.findRow(key);
    REQUIRE_MESSAGE(row.has_value(), std::string(key));
    return *row;
}

const Gadget& gadgetOf(const EditorSession& session, std::uint64_t n) {
    const Gadget* gadget = session.document().world().get<const Gadget>(entityOf(session, n));
    REQUIRE(gadget != nullptr);
    return *gadget;
}

std::size_t historySize(const EditorSession& session) {
    return session.document().history().cursor();
}

} // namespace

TEST_SUITE("editor_core.view_models") {
    TEST_CASE("the tree view model flattens, collapses, filters and ranges by key") {
        TreeViewModel tree;
        tree.setItems({{.key = 1, .depth = 0, .label = "Table"},
                       {.key = 2, .depth = 1, .label = "Lamp"},
                       {.key = 3, .depth = 2, .label = "Bulb"},
                       {.key = 4, .depth = 1, .label = "Cup"},
                       {.key = 5, .depth = 0, .label = "Chair"}});
        CHECK(labels(tree, tree.rows()) == std::vector<std::string>{"Table", ".Lamp", "..Bulb", ".Cup", "Chair"});
        CHECK(tree.rows()[0].hasChildren);
        CHECK_FALSE(tree.rows()[4].hasChildren);
        CHECK(tree.parentOf(2) == std::optional<std::size_t>(1));
        CHECK_FALSE(tree.parentOf(0).has_value());
        CHECK(tree.isInSubtree(2, 0));
        CHECK_FALSE(tree.isInSubtree(0, 2));

        // Rows are rebuilt only when something they show changed.
        const std::uint64_t rebuilds = tree.rebuilds();
        static_cast<void>(tree.rows());
        CHECK(tree.rebuilds() == rebuilds);
        tree.setExpanded(2, false);
        CHECK(labels(tree, tree.rows()) == std::vector<std::string>{"Table", ".Lamp", ".Cup", "Chair"});
        CHECK(tree.rebuilds() == rebuilds + 1);
        tree.setExpanded(2, false); // no change
        static_cast<void>(tree.rows());
        CHECK(tree.rebuilds() == rebuilds + 1);
        tree.reveal(3);
        CHECK(tree.isExpanded(2));

        // A filter shows the matches and their ancestors, expanded, even when collapsed.
        tree.setExpanded(1, false);
        tree.setFilter("BULB");
        const std::vector<ith::TreeRow>& filtered = tree.rows();
        CHECK(labels(tree, filtered) == std::vector<std::string>{"Table", ".Lamp", "..Bulb"});
        CHECK_FALSE(filtered[0].matches);
        CHECK(filtered[2].matches);
        CHECK(filtered[0].expanded);
        tree.setFilter("");
        CHECK(labels(tree, tree.rows()) == std::vector<std::string>{"Table", "Chair"});

        // Ranges are in row order either way round; collapsed keys of items that are gone are forgotten.
        tree.setExpanded(1, true);
        CHECK(tree.range(5, 4) == std::vector<std::uint64_t>{4, 5});
        CHECK(tree.range(1, 99).empty());
        tree.setExpanded(5, false);
        tree.setItems({{.key = 1, .depth = 0, .label = "Table"}});
        tree.setItems({{.key = 1, .depth = 0, .label = "Table"}, {.key = 5, .depth = 1, .label = "Chair"}});
        CHECK(tree.isExpanded(5));
        const std::vector<std::uint64_t> selected{5, 1};
        CHECK(tree.setSelected(selected));
        CHECK_FALSE(tree.setSelected(selected));
        CHECK(tree.isSelected(1));
        CHECK_FALSE(tree.isSelected(2));
    }

    TEST_CASE("the Outliner shows the hierarchy in order, follows the selection and rebuilds only on structure") {
        EditorSession session(types());
        OutlinerModel outliner(session);
        create(session, 1, "Table");
        create(session, 2, "Lamp", R"(, "parent": )" + quoted(1));
        create(session, 3, "Chair");
        run(session, "ith.entity.create", R"({"id": )" + quoted(4) + "}");
        run(session, "ith.edit.rename", R"({"id": )" + quoted(4) + R"(, "label": ""})");
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"Table", "  Lamp", "Chair", ""});
        CHECK(outliner.find(idOf(2))->label == "Lamp");
        CHECK(outliner.tree().selected() == std::vector<std::uint64_t>{OutlinerModel::keyOf(idOf(4))});

        // A value edit doesn't rebuild the rows.
        const std::uint64_t rebuilds = outliner.tree().rebuilds();
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(3) + R"(], "path": "ith::LocalTransform.value.translation.x", "value": 2})");
        outliner.update();
        static_cast<void>(outliner.tree().rows());
        CHECK(outliner.tree().rebuilds() == rebuilds);

        // Clicks: replace, toggle, range (in row order, the clicked one primary).
        REQUIRE(outliner.click(idOf(1), OutlinerClick::Replace));
        REQUIRE(outliner.click(idOf(3), OutlinerClick::Range));
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1), idOf(2), idOf(3)});
        REQUIRE(outliner.click(idOf(2), OutlinerClick::Toggle));
        CHECK(session.document().selection().entities() == std::vector<Uuid>{idOf(1), idOf(3)});
        outliner.update();
        CHECK(outliner.tree().isSelected(OutlinerModel::keyOf(idOf(3))));
        CHECK_FALSE(outliner.tree().isSelected(OutlinerModel::keyOf(idOf(2))));
        CHECK(session.document().history().cursor() == 6); // selection makes no entry

        // A filter, and a collapsed parent revealed when its child is selected.
        outliner.setFilter("lamp");
        CHECK(shown(outliner) == std::vector<std::string>{"Table", "  Lamp"});
        outliner.setFilter("");
        outliner.tree().setExpanded(OutlinerModel::keyOf(idOf(1)), false);
        CHECK(shown(outliner).size() == 3);
        REQUIRE(outliner.click(idOf(3), OutlinerClick::Replace));
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        outliner.update();
        CHECK(shown(outliner).size() == 4);
    }

    TEST_CASE("Outliner rename, the F2 prompt, Delete, Delete with children and Enabled") {
        EditorSession session(types());
        OutlinerModel outliner(session);
        session.commands().setPrompt("ith.edit.rename", [&outliner](EditorSession&) { return outliner.beginRename(); });
        create(session, 1, "Table");
        create(session, 2, "Lamp", R"(, "parent": )" + quoted(1));
        create(session, 3, "Bulb", R"(, "parent": )" + quoted(2));
        outliner.update();

        REQUIRE(session.commands().invoke(session, "ith.edit.rename"));
        REQUIRE(outliner.renaming() == idOf(3));
        CHECK(outliner.renameText() == "Bulb");
        outliner.renameText() = "Warm bulb";
        REQUIRE(outliner.commitRename());
        CHECK_FALSE(outliner.renaming().has_value());
        outliner.update();
        CHECK(outliner.find(idOf(3))->label == "Warm bulb");
        REQUIRE(outliner.beginRename(idOf(1)));
        outliner.cancelRename();
        CHECK_FALSE(outliner.commitRename().hasValue());
        run(session, "ith.edit.select_none");
        CHECK_FALSE(outliner.beginRename());

        // Enabled for the selection when the row is selected; descendants are disabled in hierarchy.
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        REQUIRE(outliner.setEnabled(idOf(1), false));
        outliner.update();
        CHECK(outliner.find(idOf(1))->disabled);
        CHECK_FALSE(outliner.find(idOf(3))->disabled);
        CHECK(outliner.find(idOf(3))->disabledInHierarchy);
        REQUIRE(outliner.setEnabled(idOf(1), true));
        outliner.update();
        CHECK_FALSE(outliner.find(idOf(3))->disabledInHierarchy);

        // Delete detaches the other children in place; Delete with children removes the subtree.
        const std::string before = levelText(session);
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(2) + "]}");
        REQUIRE(outliner.deleteSelection(false));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"Table", "Warm bulb"});
        run(session, "ith.edit.undo");
        CHECK(levelText(session) == before);
        REQUIRE(outliner.deleteSelection(true));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"Table"});
    }

    TEST_CASE("Outliner drag and drop: zones, the whole selection, one undo entry, invalid drops refused") {
        EditorSession session(types());
        OutlinerModel outliner(session);
        create(session, 1, "A");
        create(session, 2, "B");
        create(session, 3, "C");
        create(session, 4, "D", R"(, "parent": )" + quoted(3));
        outliner.update();
        CHECK(OutlinerModel::zoneAt(0.1f) == DropZone::Before);
        CHECK(OutlinerModel::zoneAt(0.5f) == DropZone::Inside);
        CHECK(OutlinerModel::zoneAt(0.9f) == DropZone::After);

        // A dragged row that isn't selected drags alone; a selected one drags the selection.
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(4) + "," + quoted(3) + "," + quoted(1) + "]}");
        CHECK(outliner.dragSet(idOf(2)) == std::vector<Uuid>{idOf(2)});
        CHECK(outliner.dragSet(idOf(3)) == std::vector<Uuid>{idOf(1), idOf(3)}); // D moves with C

        const std::size_t entries = historySize(session);
        const std::vector<Uuid> one{idOf(1)};
        REQUIRE(outliner.drop(one, idOf(4), DropZone::Inside));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"B", "C", "  D", "    A"});
        CHECK(historySize(session) == entries + 1);

        REQUIRE(outliner.drop(one, idOf(4), DropZone::Before));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"B", "C", "  A", "  D"});
        REQUIRE(outliner.drop(one, idOf(4), DropZone::After));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"B", "C", "  D", "  A"});
        REQUIRE(outliner.drop(one, idOf(2), DropZone::Before));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"A", "B", "C", "  D"});
        CHECK(historySize(session) == entries + 4);

        // Two together keep their order; End makes them the last roots.
        const std::vector<Uuid> two{idOf(1), idOf(2)};
        REQUIRE(outliner.drop(two, idOf(4), DropZone::After));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"C", "  D", "  A", "  B"});
        REQUIRE(outliner.drop(two, std::nullopt, DropZone::End));
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"C", "  D", "A", "B"});

        // A drop where the entity already is runs nothing; into itself or below itself is refused.
        const std::size_t now = historySize(session);
        REQUIRE(outliner.drop(one, idOf(2), DropZone::Before));
        REQUIRE(outliner.drop(std::vector<Uuid>{idOf(2)}, std::nullopt, DropZone::End));
        CHECK(historySize(session) == now);
        const std::vector<Uuid> parent{idOf(3)};
        CHECK(outliner.canDrop(parent, idOf(4), DropZone::Inside).error().code == "editor.outliner.invalid_drop");
        CHECK(outliner.canDrop(parent, idOf(3), DropZone::After).error().code == "editor.outliner.invalid_drop");
        CHECK(outliner.canDrop(parent, std::nullopt, DropZone::Before).error().code == "editor.outliner.invalid_drop");
        CHECK_FALSE(outliner.drop(parent, idOf(4), DropZone::Before).hasValue());
        CHECK(historySize(session) == now);

        // Undo puts each drop back.
        run(session, "ith.edit.undo");
        outliner.update();
        CHECK(shown(outliner) == std::vector<std::string>{"C", "  D", "  A", "  B"});
    }

    TEST_CASE("Details rows come from reflection: groups, categories, labels, tooltips, inline values") {
        EditorSession session(types());
        DetailsModel details(session);
        create(session, 1, "Gizmo", R"(, "components": {"ith::test::editor::Gadget": {"parts": [{}, {}]}})");
        details.update();
        REQUIRE(details.components().size() == 2);
        CHECK(details.components()[0]->id == ith::typeOf<ith::LocalTransform>().id);
        CHECK(details.label() == "Gizmo");
        CHECK(details.canRename());
        CHECK(details.enabled() == std::optional<bool>(true));

        const ith::DetailsRow& group = details.rows()[rowOf(details, "ith::test::editor::Gadget")];
        CHECK(group.kind == DetailsRowKind::Component);
        CHECK(group.label == "Gadget");
#ifndef ITH_SHIPPING // Shipping builds compile out tooltips and categories
        CHECK(group.tooltip == "A test gadget");
#endif
        const ith::DetailsRow& power = details.rows()[rowOf(details, "ith::test::editor::Gadget.power")];
        CHECK(power.label == "Power");
#ifndef ITH_SHIPPING
        CHECK(power.tooltip == "How strong it is");
#endif
        CHECK(power.inlineValue);
        CHECK(power.isDefault);
        CHECK(*static_cast<const float*>(details.value(rowOf(details, "ith::test::editor::Gadget.power"))) == 50.0f);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.offset")].inlineValue);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.rotation")].inlineValue);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.boost")].inlineValue);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.asset")].inlineValue);
        const ith::DetailsRow& part = details.rows()[rowOf(details, "ith::test::editor::Gadget.part")];
        CHECK(part.expandable);
        CHECK(part.expanded);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.part.level")].depth == 2);
#ifndef ITH_SHIPPING
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget#Links")].kind == DetailsRowKind::Category);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.link")].depth == 2);
#endif
        CHECK_FALSE(details.findRow("ith::test::editor::Gadget.extra.level").has_value()); // empty optional

        // Lists start collapsed; expanding shows the elements; collapsing a group hides its rows.
        const std::size_t parts = rowOf(details, "ith::test::editor::Gadget.parts");
        CHECK(details.rows()[parts].expandable);
        CHECK_FALSE(details.rows()[parts].expanded);
        details.setExpanded(parts, true);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.parts[1].on")].depth == 3);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.parts[1]")].label == "[1]");
        details.setExpanded(rowOf(details, "ith::test::editor::Gadget.pair"), true);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.pair[0]")].depth == 2);
        details.setExpanded(rowOf(details, "ith::test::editor::Gadget"), false);
        CHECK_FALSE(details.findRow("ith::test::editor::Gadget.power").has_value());
        details.setExpanded(rowOf(details, "ith::test::editor::Gadget"), true);
        // Expansion is kept across a refresh.
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::test::editor::Gadget.power", "value": 7})");
        details.update();
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.parts")].expanded);
        CHECK_FALSE(details.rows()[rowOf(details, "ith::test::editor::Gadget.power")].isDefault);

        // Hidden: Transient, World-kept, the label and Disabled. ChildOf is shown.
        create(session, 2, "Child", R"(, "parent": )" + quoted(1));
        run(session, "ith.entity.set_enabled", R"({"enabled": false})");
        details.update();
        CHECK(details.enabled() == std::optional<bool>(false));
        std::vector<std::string> names;
        for (const ith::TypeInfo* type : details.components()) {
            names.emplace_back(type->name);
        }
        CHECK(names == std::vector<std::string>{"ith::LocalTransform", "ith::ChildOf"});
        CHECK(details.rows()[rowOf(details, "ith::ChildOf.despawnWithParent")].label == "Despawn With Parent");
    }

    TEST_CASE("Details multi-select: shared components, Multiple Values, one edit for all, mixed list sizes") {
        EditorSession session(types());
        DetailsModel details(session);
        create(session, 1, "One", R"(, "components": {"ith::test::editor::Gadget": {"power": 10, "parts": [{}]}})");
        create(session, 2, "Two",
               R"(, "components": {"ith::test::editor::Gadget": {"power": 20}, "ith::test::editor::Lamp": {}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "," + quoted(2) + "]}");
        details.update();
        CHECK(details.entities() == std::vector<Uuid>{idOf(1), idOf(2)});
        CHECK(details.components().size() == 2); // Lamp is on one only
        CHECK(details.labelMixed());
        CHECK(details.label() == "Two");
        CHECK_FALSE(details.canRename());
        CHECK(details.rename("x").error().code == "editor.details.rename_one");

        const std::size_t power = rowOf(details, "ith::test::editor::Gadget.power");
        CHECK(details.rows()[power].mixed);
        CHECK_FALSE(details.rows()[power].isDefault);
        CHECK_FALSE(details.rows()[rowOf(details, "ith::test::editor::Gadget.mode")].mixed);
        const std::size_t parts = rowOf(details, "ith::test::editor::Gadget.parts");
        CHECK(details.rows()[parts].mixed);
        CHECK_FALSE(details.rows()[parts].expandable); // sizes differ: no element rows

        const float value = 30.0f;
        REQUIRE(details.setValue(power, &value));
        CHECK(gadgetOf(session, 1).power == 30.0f);
        CHECK(gadgetOf(session, 2).power == 30.0f);
        CHECK_FALSE(details.rows()[rowOf(details, "ith::test::editor::Gadget.power")].mixed);

        // Adding to a list appends to each; then the sizes match.
        REQUIRE(details.addElement(rowOf(details, "ith::test::editor::Gadget.parts")));
        CHECK(gadgetOf(session, 1).parts.size() == 2);
        CHECK(gadgetOf(session, 2).parts.size() == 1);
        run(session, "ith.entity.insert_element",
            R"({"ids": [)" + quoted(2) + R"(], "path": "ith::test::editor::Gadget.parts"})");
        details.update();
        const std::size_t list = rowOf(details, "ith::test::editor::Gadget.parts");
        CHECK(details.rows()[list].expandable);
        details.setExpanded(list, true);
        REQUIRE(details.removeElement(rowOf(details, "ith::test::editor::Gadget.parts[0]")));
        CHECK(gadgetOf(session, 1).parts.size() == 1);
        CHECK(gadgetOf(session, 2).parts.size() == 1);
        CHECK_FALSE(details.removeElement(rowOf(details, "ith::test::editor::Gadget.parts")).hasValue());
        REQUIRE(details.clearElements(rowOf(details, "ith::test::editor::Gadget.parts")));
        CHECK(gadgetOf(session, 1).parts.empty());

        // Add Component adds only where it is missing; the list offers what not every entity has.
        const std::vector<const ith::TypeInfo*> addable = details.addableComponents();
        const auto offers = [&](std::string_view name) {
            return std::ranges::any_of(addable, [&](const ith::TypeInfo* type) { return type->name == name; });
        };
        CHECK(offers("ith::test::editor::Lamp"));
        CHECK(offers("ith::test::editor::Aim"));
        CHECK_FALSE(offers("ith::test::editor::Gadget"));
        CHECK_FALSE(offers("ith::WorldTransform"));
        CHECK_FALSE(offers("ith::StableId"));
        CHECK_FALSE(offers("ith::ChildOf"));
        CHECK_FALSE(offers("ith::EditorLabel"));
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(2) + R"(], "path": "ith::test::editor::Lamp.intensity", "value": 9})");
        REQUIRE(details.addComponent(ith::typeOf<Lamp>()));
        CHECK(session.document().world().get<const Lamp>(entityOf(session, 2))->intensity == 9.0f);
        details.update();
        CHECK(details.components().size() == 3);
        REQUIRE(details.removeComponent(ith::typeOf<Lamp>()));
        details.update();
        CHECK(details.components().size() == 2);
    }

    TEST_CASE("Details clamps to Min and Max, resets to default, edits optionals and entity references") {
        EditorSession session(types());
        DetailsModel details(session);
        create(session, 1, "Gizmo", R"(, "components": {"ith::test::editor::Gadget": {}})");
        create(session, 2, "Target");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        details.update();

        const float tooMuch = 500.0f;
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.power"), &tooMuch));
        CHECK(gadgetOf(session, 1).power == 100.0f);
        const ith::Vec3 offset{-3.0f, 0.5f, 2.0f};
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.offset"), &offset));
        CHECK(gadgetOf(session, 1).offset == ith::Vec3(-1.0f, 0.5f, 1.0f));
        const std::int32_t level = -4;
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.part.level"), &level));
        CHECK(gadgetOf(session, 1).part.level == 0);

        const std::size_t power = rowOf(details, "ith::test::editor::Gadget.power");
        CHECK_FALSE(details.rows()[power].isDefault);
        REQUIRE(details.resetToDefault(power));
        CHECK(gadgetOf(session, 1).power == 50.0f);
        CHECK(details.rows()[rowOf(details, "ith::test::editor::Gadget.power")].isDefault);

        // An optional: present with its default, edited inside, then empty again.
        REQUIRE(details.setPresent(rowOf(details, "ith::test::editor::Gadget.extra"), true));
        REQUIRE(gadgetOf(session, 1).extra.has_value());
        const bool off = false;
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.extra.on"), &off));
        CHECK_FALSE(gadgetOf(session, 1).extra->on);
        REQUIRE(details.setPresent(rowOf(details, "ith::test::editor::Gadget.extra"), false));
        CHECK_FALSE(gadgetOf(session, 1).extra.has_value());
        const std::optional<float> boost = 2.5f;
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.boost"), &boost));
        CHECK(gadgetOf(session, 1).boost == std::optional<float>(2.5f));
        CHECK_FALSE(details.setPresent(rowOf(details, "ith::test::editor::Gadget.power"), true).hasValue());

        // An entity reference: picked by name, written as the entity, shown by its label.
        const std::vector<ith::EntityChoice> choices = details.entityChoices("targ");
        REQUIRE(choices.size() == 1);
        CHECK(choices[0].id == idOf(2));
        const Entity target = details.entityOf(choices[0].id);
        REQUIRE(details.setValue(rowOf(details, "ith::test::editor::Gadget.link"), &target));
        CHECK(gadgetOf(session, 1).link == entityOf(session, 2));
        const std::size_t link = rowOf(details, "ith::test::editor::Gadget.link");
        CHECK(details.entityLabel(*static_cast<const Entity*>(details.value(link))) == "Target");
        CHECK(details.entityLabel(Entity()) == "None");
        CHECK(details.entityChoices("").size() == 2);

        // ReadOnly fields are shown and refused.
        run(session, "ith.entity.add_component", R"({"type": "ith::test::editor::Lamp"})");
        details.update();
        const std::size_t serial = rowOf(details, "ith::test::editor::Lamp.serial");
        CHECK(details.rows()[serial].readOnly);
        const std::int32_t one = 1;
        CHECK(details.setValue(serial, &one).error().code == "editor.command.read_only");
        CHECK(details.beginEdit(serial).error().code == "editor.command.read_only");
        CHECK(details.resetToDefault(serial).error().code == "editor.command.read_only");
    }

    TEST_CASE("a Details drag is one transaction with a live preview; Esc and cancel put the value back") {
        EditorSession session(types());
        DetailsModel details(session);
        create(session, 1, "One", R"(, "components": {"ith::test::editor::Gadget": {}})");
        create(session, 2, "Two", R"(, "components": {"ith::test::editor::Gadget": {}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "," + quoted(2) + "]}");
        details.update();
        const std::size_t entries = historySize(session);
        const std::size_t power = rowOf(details, "ith::test::editor::Gadget.power");

        REQUIRE(details.beginEdit(power));
        CHECK(details.editing());
        CHECK(details.editingKey() == "ith::test::editor::Gadget.power");
        CHECK(details.beginEdit(power).error().code == "editor.command.interaction_open");
        CHECK(session.execute("ith.edit.undo").error().code == "editor.command.interaction_open");
        const std::uint64_t refreshes = details.refreshes();
        for (const float value : {51.0f, 60.0f, 250.0f}) {
            REQUIRE(details.previewValue(&value));
            details.update(); // its own previews don't refresh the copy
        }
        CHECK(details.refreshes() == refreshes);
        CHECK(gadgetOf(session, 1).power == 100.0f); // clamped
        CHECK(gadgetOf(session, 2).power == 100.0f);
        CHECK(*static_cast<const float*>(details.value(power)) == 100.0f);
        details.commitEdit();
        CHECK_FALSE(details.editing());
        CHECK(historySize(session) == entries + 1);
        CHECK(session.document().history().entries().back().name == "Set Property");
        run(session, "ith.edit.undo");
        CHECK(gadgetOf(session, 1).power == 50.0f);
        details.update();
        CHECK(*static_cast<const float*>(details.value(power)) == 50.0f);

        // Esc (the keymap's cancelOpen) ends the drag; the widget's next previews do nothing.
        const std::string before = levelText(session);
        REQUIRE(details.beginEdit(power));
        const float dragged = 75.0f;
        REQUIRE(details.previewValue(&dragged));
        session.document().transactions().cancelOpen();
        details.update();
        CHECK(*static_cast<const float*>(details.value(power)) == 50.0f);
        CHECK(details.previewValue(&dragged).error().code == "editor.details.edit_cancelled");
        CHECK(levelText(session) == before);
        details.commitEdit();
        CHECK(historySize(session) == entries); // the undone entry is still there to redo
        CHECK(session.document().history().canRedo());

        // cancelEdit puts the values back too.
        REQUIRE(details.beginEdit(power));
        REQUIRE(details.previewValue(&dragged));
        details.cancelEdit();
        CHECK(levelText(session) == before);
        CHECK(details.previewValue(&dragged).error().code == "editor.details.not_editing");

        // Refused while playing.
        session.document().setPlaying(true);
        CHECK(details.beginEdit(power).error().code == "editor.command.playing");
        session.document().setPlaying(false);
    }

    TEST_CASE("the Details working copy refreshes for its own entities only, and not during its interaction") {
        EditorSession session(types());
        DetailsModel details(session);
        create(session, 1, "Shown", R"(, "components": {"ith::test::editor::Gadget": {}})");
        create(session, 2, "Other", R"(, "components": {"ith::test::editor::Gadget": {}})");
        run(session, "ith.selection.set", R"({"entities": [)" + quoted(1) + "]}");
        details.update();
        const std::uint64_t refreshes = details.refreshes();
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(2) + R"(], "path": "ith::test::editor::Gadget.power", "value": 1})");
        details.update();
        CHECK(details.refreshes() == refreshes);
        run(session, "ith.entity.set_property",
            R"({"ids": [)" + quoted(1) + R"(], "path": "ith::test::editor::Gadget.power", "value": 2})");
        details.update();
        CHECK(details.refreshes() == refreshes + 1);
        CHECK(*static_cast<const float*>(details.value(rowOf(details, "ith::test::editor::Gadget.power"))) == 2.0f);
        // Undo of its own entity refreshes; a second model reads the same changes.
        DetailsModel second(session);
        second.update();
        run(session, "ith.edit.undo");
        details.update();
        second.update();
        CHECK(*static_cast<const float*>(details.value(rowOf(details, "ith::test::editor::Gadget.power"))) == 50.0f);
        CHECK(*static_cast<const float*>(second.value(rowOf(second, "ith::test::editor::Gadget.power"))) == 50.0f);
        // Nothing selected: no rows.
        run(session, "ith.edit.select_none");
        details.update();
        CHECK(details.rows().empty());
        CHECK_FALSE(details.enabled().has_value());
        CHECK(details.addableComponents().empty());
    }

    TEST_CASE("Euler degrees round-trip a rotation away from gimbal lock; display names spell words") {
        std::mt19937 random(7u);
        std::uniform_real_distribution<float> pitch(-89.0f, 89.0f);
        std::uniform_real_distribution<float> other(-179.0f, 179.0f);
        for (int i = 0; i < 1000; ++i) {
            const ith::Vec3 euler(pitch(random), other(random), other(random));
            const ith::Vec3 back = ith::eulerDegrees(ith::quatFromEulerDegrees(euler));
            CHECK(glm::all(glm::epsilonEqual(back, euler, 0.05f)));
        }
        // Yaw turns about +Y: +90 degrees takes +X to -Z (right-handed).
        const ith::Vec3 turned = ith::quatFromEulerDegrees(ith::Vec3(0.0f, 90.0f, 0.0f)) * ith::Vec3(1.0f, 0.0f, 0.0f);
        CHECK(glm::all(glm::epsilonEqual(turned, ith::Vec3(0.0f, 0.0f, -1.0f), 1e-5f)));
        const ith::Vec3 locked = ith::eulerDegrees(ith::quatFromEulerDegrees(ith::Vec3(90.0f, 30.0f, 0.0f)));
        CHECK(locked.x == doctest::Approx(90.0f).epsilon(0.001));
        CHECK(locked.y == doctest::Approx(30.0f).epsilon(0.001));

        CHECK(ith::displayNameOf("despawnWithParent") == "Despawn With Parent");
        CHECK(ith::displayNameOf("LocalTransform") == "Local Transform");
        CHECK(ith::displayNameOf("x") == "X");
        CHECK(ith::displayNameOf("HTTPServer") == "HTTP Server");
    }
}

namespace {

// A random value for a row the model test edits: floats, booleans, 32-bit integers and Vec3s.
std::optional<ith::OwnedValue> randomValue(const ith::TypeInfo& type, std::mt19937& random) {
    std::uniform_real_distribution<float> number(-200.0f, 200.0f);
    if (type.id == ith::typeOf<float>().id) {
        return ith::OwnedValue::of(number(random));
    }
    if (type.id == ith::typeOf<bool>().id) {
        return ith::OwnedValue::of(random() % 2 == 0);
    }
    if (type.id == ith::typeOf<std::int32_t>().id) {
        return ith::OwnedValue::of(static_cast<std::int32_t>(number(random)));
    }
    if (type.id == ith::typeOf<ith::Vec3>().id) {
        return ith::OwnedValue::of(ith::Vec3(number(random), number(random), number(random)));
    }
    return std::nullopt;
}

bool editableByTest(const ith::TypeInfo& type) {
    return type.id == ith::typeOf<float>().id || type.id == ith::typeOf<bool>().id ||
           type.id == ith::typeOf<std::int32_t>().id || type.id == ith::typeOf<ith::Vec3>().id;
}

// The Outliner's rows and selection, and every Details row's value, against the world.
void checkModels(EditorSession& session, OutlinerModel& outliner, DetailsModel& details) {
    const ith::World& world = session.document().world();
    std::vector<std::string> expected;
    std::vector<std::string> actual;
    std::vector<std::pair<Entity, std::uint32_t>> pending;
    std::vector<Entity> roots;
    world.forEachRoot([&](Entity root) { roots.push_back(root); });
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        pending.emplace_back(*it, 0);
    }
    while (!pending.empty()) {
        const auto [entity, depth] = pending.back();
        pending.pop_back();
        const auto* label = world.get<const ith::EditorLabel>(entity);
        expected.push_back(std::to_string(depth) + ":" + world.get<const ith::StableId>(entity)->id.toString() + ":" +
                           (label != nullptr ? label->name : std::string()));
        if (const auto* children = world.get<const ith::Children>(entity)) {
            for (auto child = children->entities.rbegin(); child != children->entities.rend(); ++child) {
                pending.emplace_back(*child, depth + 1);
            }
        }
    }
    for (std::size_t i = 0; i < outliner.entries().size(); ++i) {
        actual.push_back(std::to_string(outliner.tree().items()[i].depth) + ":" + outliner.entries()[i].id.toString() +
                         ":" + outliner.entries()[i].label);
    }
    REQUIRE(actual == expected);
    std::vector<std::uint64_t> selected;
    for (const Uuid& id : session.document().selection().entities()) {
        selected.push_back(OutlinerModel::keyOf(id));
    }
    REQUIRE(outliner.tree().selected() == selected);

    REQUIRE(details.entities() == session.document().selection().entities());
    for (std::size_t row = 0; row < details.rows().size(); ++row) {
        const ith::DetailsRow& shownRow = details.rows()[row];
        if (shownRow.kind != DetailsRowKind::Property) {
            continue;
        }
        const void* value = details.value(row);
        std::vector<const void*> worldValues;
        for (const Uuid& id : details.entities()) {
            const void* component = world.get(world.findByStableId(id), *shownRow.component);
            REQUIRE(component != nullptr);
            const auto resolved = ith::resolve(shownRow.path, component);
            worldValues.push_back(resolved ? *resolved : nullptr);
        }
        CAPTURE(shownRow.key);
        if (value == nullptr) {
            REQUIRE(worldValues.back() == nullptr);
            continue;
        }
        REQUIRE(worldValues.back() != nullptr);
        REQUIRE(shownRow.type->equals(value, worldValues.back()));
        const bool same = std::ranges::all_of(
            worldValues, [&](const void* other) { return other != nullptr && shownRow.type->equals(other, value); });
        REQUIRE(shownRow.mixed == !same);
    }
}

} // namespace

TEST_SUITE("editor_core.view_models") {
    TEST_CASE("3,000 random Outliner and Details actions keep both models equal to the world") {
        std::mt19937 random(20261123u);
        EditorSession session(types());
        session.document().history().setLimits({.maxEntries = 1000000, .maxBytes = std::size_t{1} << 40});
        OutlinerModel outliner(session);
        DetailsModel details(session);
        std::uint64_t nextId = 1;
        const auto pick = [&](std::size_t below) {
            return static_cast<std::size_t>(std::uniform_int_distribution<std::uint64_t>(0, below - 1)(random));
        };
        const auto someEntry = [&]() -> std::optional<Uuid> {
            if (outliner.entries().empty()) {
                return std::nullopt;
            }
            return outliner.entries()[pick(outliner.entries().size())].id;
        };
        const auto someProperty = [&]() -> std::optional<std::size_t> {
            std::vector<std::size_t> rows;
            for (std::size_t row = 0; row < details.rows().size(); ++row) {
                const ith::DetailsRow& candidate = details.rows()[row];
                if (candidate.kind == DetailsRowKind::Property && !candidate.readOnly &&
                    editableByTest(*candidate.type)) {
                    rows.push_back(row);
                }
            }
            return rows.empty() ? std::nullopt : std::optional<std::size_t>(rows[pick(rows.size())]);
        };
        const std::string digits = "0123456789";
        std::size_t drops = 0;
        std::size_t drags = 0;

        for (int step = 0; step < 3000; ++step) {
            CAPTURE(step);
            const std::optional<Uuid> entry = someEntry();
            switch (pick(14)) {
                case 0: {
                    const std::string parent =
                        entry.has_value() && pick(2) == 0 ? R"(, "parent": ")" + entry->toString() + "\"" : "";
                    const std::string gadget =
                        pick(2) == 0 ? R"(, "components": {"ith::test::editor::Gadget": {"parts": [{}]}})" : "";
                    create(session, nextId, "e" + std::to_string(nextId), parent + gadget);
                    ++nextId;
                    break;
                }
                case 1:
                    if (entry.has_value()) {
                        REQUIRE(outliner.click(*entry, static_cast<OutlinerClick>(pick(3))));
                    }
                    break;
                case 2:
                    if (entry.has_value()) {
                        const std::vector<Uuid> dragged = outliner.dragSet(*entry);
                        const auto zone = static_cast<DropZone>(pick(4));
                        const std::optional<Uuid> target = zone == DropZone::End ? std::nullopt : someEntry();
                        if (outliner.canDrop(dragged, target, zone)) {
                            REQUIRE(outliner.drop(dragged, target, zone));
                            ++drops;
                        }
                    }
                    break;
                case 3:
                    if (!session.document().selection().empty() && pick(3) == 0) {
                        REQUIRE(outliner.deleteSelection(pick(2) == 0));
                    }
                    break;
                case 4:
                    if (entry.has_value()) {
                        REQUIRE(outliner.setEnabled(*entry, pick(2) == 0));
                    }
                    break;
                case 5:
                    if (entry.has_value() && outliner.beginRename(*entry)) {
                        outliner.renameText() += "r";
                        REQUIRE(outliner.commitRename());
                    }
                    break;
                case 6:
                    outliner.setFilter(pick(3) == 0 ? digits.substr(pick(10), 1) : std::string());
                    if (entry.has_value()) {
                        outliner.tree().setExpanded(OutlinerModel::keyOf(*entry), pick(2) == 0);
                    }
                    break;
                case 7:
                    if (const std::optional<std::size_t> row = someProperty()) {
                        const std::optional<ith::OwnedValue> value = randomValue(*details.rows()[*row].type, random);
                        REQUIRE(details.setValue(*row, value->get()));
                    }
                    break;
                case 8:
                    if (const std::optional<std::size_t> row = someProperty()) {
                        const ith::TypeInfo& type = *details.rows()[*row].type;
                        REQUIRE(details.beginEdit(*row));
                        ++drags;
                        const std::size_t frames = 1 + pick(3);
                        for (std::size_t frame = 0; frame < frames; ++frame) {
                            REQUIRE(details.previewValue(randomValue(type, random)->get()));
                            details.update();
                        }
                        const std::size_t ending = pick(3);
                        if (ending == 0) {
                            details.commitEdit();
                        } else if (ending == 1) {
                            details.cancelEdit();
                        } else {
                            session.document().transactions().cancelOpen(); // Esc
                            details.update();
                            details.commitEdit();
                        }
                    }
                    break;
                case 9:
                    if (const std::optional<std::size_t> row = someProperty()) {
                        REQUIRE(details.resetToDefault(*row));
                    }
                    break;
                case 10:
                    if (!details.entities().empty()) {
                        const ith::TypeInfo& type = pick(2) == 0 ? ith::typeOf<Gadget>() : ith::typeOf<Lamp>();
                        const bool has = std::ranges::any_of(
                            details.components(), [&](const ith::TypeInfo* shown) { return shown->id == type.id; });
                        REQUIRE((has ? details.removeComponent(type) : details.addComponent(type)));
                    }
                    break;
                case 11:
                    if (session.document().history().canUndo()) {
                        run(session, "ith.edit.undo");
                    }
                    break;
                case 12:
                    if (session.document().history().canRedo()) {
                        run(session, "ith.edit.redo");
                    }
                    break;
                default:
                    if (!details.rows().empty()) {
                        const std::size_t row = pick(details.rows().size());
                        details.setExpanded(row, !details.rows()[row].expanded);
                    }
                    break;
            }
            outliner.update();
            details.update();
            REQUIRE_FALSE(session.document().transactions().isOpen());
            checkModels(session, outliner, details);
        }
        // The run did reach drops and drags, not only creates.
        CHECK(drops > 50);
        CHECK(drags > 50);
        CHECK(session.document().history().cursor() > 100);
    }
}
