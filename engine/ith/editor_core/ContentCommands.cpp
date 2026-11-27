// The Content Browser's commands and queries of the browse slice (ADR-0049 rules 2, 5, 6 and 27):
// navigate, navigate_up, select, select_all, dismiss, set_view, set_filter, rescan, open and
// toggle_drawer, and ith.query.content, ith.query.content_tree and ith.query.content_problems.
//
// They are Actions: no undo entry and never a dirty level (a path change of the open level after a
// Rescan is not a dirty change). Without an asset database every ith.content command fails with
// "editor.document.no_asset_database"; the queries then report an empty browser. Items are named
// as {"id": <uuid>} (assets) or {"path": <content path>} (any item).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Log.h"
#include "ith/level/LevelFormat.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith {

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

using detail::idValue;
using detail::invalidArgs;
using detail::optionalString;
using detail::parseId;
using detail::requiredString;
using serialization::Value;

// The Content Browser panel's id (ADR-0040 rule 3).
constexpr std::string_view kContentBrowserPanel = "ith.content_browser";

Expected<ContentBrowserModel*> browserOf(EditorSession& session) {
    if (session.content().assets() == nullptr) {
        return makeError("editor.document.no_asset_database", "no project is open: the Content Browser has no assets");
    }
    return &session.content();
}

// {"id", "path"} of an asset, {"path"} of any other item.
Value itemRef(const ContentItem& item) {
    Value ref = Value::object();
    if (item.kind == ContentItemKind::Asset) {
        ref.set("id", idValue(item.id.uuid()));
    }
    ref.set("path", Value::string(item.path));
    return ref;
}

Value keyRef(const ContentBrowserModel& model, const ContentKey& key) {
    if (const ContentItem* item = model.find(key); item != nullptr) {
        return itemRef(*item);
    }
    Value ref = Value::object();
    if (key.id.isNil()) {
        ref.set("path", Value::string(key.path));
    } else {
        ref.set("id", idValue(key.id.uuid()));
    }
    return ref;
}

Value optionalKeyRef(const ContentBrowserModel& model, const ContentKey* key) {
    return key != nullptr ? keyRef(model, *key) : Value();
}

// The item an item reference names.
Expected<ContentKey> parseItem(std::string_view command, const ContentBrowserModel& model, const Value& ref) {
    if (ref.kind() != Value::Kind::Object) {
        return invalidArgs(command, R"(an item is {"id": <uuid>} or {"path": <content path>})");
    }
    Expected<std::optional<std::string_view>> id = optionalString(command, ref, "id");
    Expected<std::optional<std::string_view>> path = optionalString(command, ref, "path");
    if (!id || !path) {
        return makeUnexpected(!id ? std::move(id).error() : std::move(path).error());
    }
    if (id->has_value()) {
        Expected<Uuid> uuid = parseId(command, **id);
        if (!uuid) {
            return makeUnexpected(std::move(uuid).error());
        }
        ContentKey key{.id = AssetId(*uuid), .path = {}};
        if (model.find(key) == nullptr) {
            return makeError("editor.content.not_found", "no asset has the id " + uuid->toString());
        }
        return key;
    }
    if (!path->has_value()) {
        return invalidArgs(command, R"(an item needs "id" or "path")");
    }
    const ContentItem* item = model.findByPath(**path);
    if (item == nullptr) {
        return makeError("editor.content.not_found", "nothing is at '" + std::string(**path) + "' in Content");
    }
    return keyOf(*item);
}

Value selectionResult(const ContentBrowserModel& model) {
    Value list = Value::array();
    for (const ContentKey& key : model.selection()) {
        list.elements().push_back(keyRef(model, key));
    }
    Value result = Value::object();
    result.set("selection", std::move(list));
    result.set("primary", optionalKeyRef(model, model.primary()));
    return result;
}

Value folderResult(const ContentBrowserModel& model) {
    Value result = Value::object();
    result.set("folder", Value::string(model.currentFolder()));
    return result;
}

Value viewResult(const ContentViewSettings& view) {
    Value result = Value::object();
    result.set("mode", Value::string(view.mode == ContentViewMode::List ? "list" : "grid"));
    result.set("sortBy", Value::string(view.sortBy == ContentSortKey::Type ? "type" : "name"));
    result.set("descending", Value::boolean(view.descending));
    return result;
}

Value categoriesValue(const ContentFilter& filter) {
    Value list = Value::array();
    for (std::size_t i = 0; i < kContentCategoryCount; ++i) {
        if (filter.has(static_cast<ContentCategory>(i))) {
            list.elements().push_back(Value::string(std::string(categoryId(static_cast<ContentCategory>(i)))));
        }
    }
    if (filter.problems) {
        list.elements().push_back(Value::string("problems"));
    }
    return list;
}

std::string_view problemKindId(AssetProblemKind kind) {
    switch (kind) {
        case AssetProblemKind::DuplicateId:
            return "duplicate_id";
        case AssetProblemKind::OrphanSidecar:
            return "orphan_sidecar";
        case AssetProblemKind::MissingSidecar:
            return "missing_sidecar";
        case AssetProblemKind::CaseConflict:
            return "case_conflict";
        case AssetProblemKind::InvalidFile:
            return "invalid_file";
        case AssetProblemKind::FormatMismatch:
            return "format_mismatch";
    }
    return "invalid_file";
}

std::string_view itemKindId(ContentItemKind kind) {
    switch (kind) {
        case ContentItemKind::Folder:
            return "folder";
        case ContentItemKind::Asset:
            return "asset";
        case ContentItemKind::Problem:
            return "problem";
    }
    return "asset";
}

Expected<Value> navigate(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.navigate";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    Expected<std::string_view> folder = requiredString(kCommand, args, "folder");
    if (!folder) {
        return makeUnexpected(std::move(folder).error());
    }
    if (Expected<void> moved = (*model)->navigate(*folder); !moved) {
        return makeUnexpected(std::move(moved).error());
    }
    return folderResult(**model);
}

Expected<Value> navigateUp(EditorSession& session, const Value& /*args*/) {
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    (*model)->navigateUp();
    return folderResult(**model);
}

std::optional<ContentSelect> parseSelectMode(std::string_view mode) {
    if (mode == "replace") {
        return ContentSelect::Replace;
    }
    if (mode == "add") {
        return ContentSelect::Add;
    }
    if (mode == "toggle") {
        return ContentSelect::Toggle;
    }
    if (mode == "remove") {
        return ContentSelect::Remove;
    }
    if (mode == "range") {
        return ContentSelect::Range;
    }
    if (mode == "add_range") {
        return ContentSelect::AddRange;
    }
    return std::nullopt;
}

Expected<Value> select(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.select";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    const Value* items = args.kind() == Value::Kind::Object ? args.find("items") : nullptr;
    if (items == nullptr || items->kind() != Value::Kind::Array) {
        return invalidArgs(kCommand, R"("items" must be an array of items)");
    }
    Expected<std::optional<std::string_view>> modeText = optionalString(kCommand, args, "mode");
    if (!modeText) {
        return makeUnexpected(std::move(modeText).error());
    }
    const std::optional<ContentSelect> mode = parseSelectMode(modeText->value_or("replace"));
    if (!mode.has_value()) {
        return invalidArgs(kCommand, R"("mode" must be "replace", "add", "toggle", "remove", "range" or "add_range")");
    }
    std::vector<ContentKey> keys;
    keys.reserve(items->elements().size());
    for (const Value& item : items->elements()) {
        Expected<ContentKey> key = parseItem(kCommand, **model, item);
        if (!key) {
            return makeUnexpected(std::move(key).error());
        }
        keys.push_back(std::move(*key));
    }
    if (Expected<void> selected = (*model)->select(keys, *mode); !selected) {
        return makeUnexpected(std::move(selected).error());
    }
    return selectionResult(**model);
}

Expected<Value> selectAll(EditorSession& session, const Value& /*args*/) {
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    (*model)->selectAll();
    return selectionResult(**model);
}

Expected<Value> dismiss(EditorSession& session, const Value& /*args*/) {
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    if (!(*model)->selection().empty()) {
        (*model)->clearSelection();
    } else if ((*model)->drawerOpen()) {
        // The drawer closes, and the keys that follow go to the level again.
        (*model)->setDrawerOpen(false);
        (*model)->setKeyboardFocus(false);
    }
    Value result = selectionResult(**model);
    result.set("drawerOpen", Value::boolean((*model)->drawerOpen()));
    return result;
}

Expected<Value> setView(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.set_view";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    ContentViewSettings view = (*model)->view();
    Expected<std::optional<std::string_view>> mode = optionalString(kCommand, args, "mode");
    Expected<std::optional<std::string_view>> sortBy = optionalString(kCommand, args, "sortBy");
    if (!mode || !sortBy) {
        return makeUnexpected(!mode ? std::move(mode).error() : std::move(sortBy).error());
    }
    if (mode->has_value()) {
        if (**mode != "grid" && **mode != "list") {
            return invalidArgs(kCommand, R"("mode" must be "grid" or "list")");
        }
        view.mode = **mode == "list" ? ContentViewMode::List : ContentViewMode::Grid;
    }
    if (sortBy->has_value()) {
        if (**sortBy != "name" && **sortBy != "type") {
            return invalidArgs(kCommand, R"("sortBy" must be "name" or "type")");
        }
        view.sortBy = **sortBy == "type" ? ContentSortKey::Type : ContentSortKey::Name;
    }
    if (const Value* descending = args.kind() == Value::Kind::Object ? args.find("descending") : nullptr;
        descending != nullptr && descending->kind() != Value::Kind::Null) {
        if (descending->kind() != Value::Kind::Bool) {
            return invalidArgs(kCommand, R"("descending" must be true or false)");
        }
        view.descending = descending->asBool();
    }
    (*model)->setView(view);
    return viewResult((*model)->view());
}

Expected<Value> setFilter(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.set_filter";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    Expected<std::optional<std::string_view>> search = optionalString(kCommand, args, "search");
    if (!search) {
        return makeUnexpected(std::move(search).error());
    }
    std::optional<ContentFilter> filter;
    if (const Value* categories = args.kind() == Value::Kind::Object ? args.find("categories") : nullptr;
        categories != nullptr && categories->kind() != Value::Kind::Null) {
        if (categories->kind() != Value::Kind::Array) {
            return invalidArgs(kCommand, R"("categories" must be an array of category names)");
        }
        filter.emplace();
        for (const Value& element : categories->elements()) {
            const std::string_view name =
                element.kind() == Value::Kind::String ? std::string_view(element.asString()) : std::string_view();
            if (name == "problems") {
                filter->problems = true;
            } else if (const std::optional<ContentCategory> category = parseCategory(name)) {
                filter->add(*category);
            } else {
                return invalidArgs(kCommand, R"("categories" holds "level", "model", "texture", "audio", "data", )"
                                             R"("other" or "problems")");
            }
        }
    }
    if (search->has_value()) {
        (*model)->setSearch(std::string(**search));
    }
    if (filter.has_value()) {
        (*model)->setFilter(*filter);
    }
    Value result = Value::object();
    result.set("shown", Value::unsignedInteger((*model)->shown().size()));
    return result;
}

Expected<Value> rescan(EditorSession& session, const Value& /*args*/) {
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    // A save in flight is about to write a level file: scan once it is written. (An open in flight is
    // refused by the registry, as for every Action.)
    if (session.document().saving()) {
        return makeError("editor.document.save_in_progress", "the level is being saved; Rescan once it is written");
    }
    if (Expected<void> scanned = (*model)->rescan(); !scanned) {
        return makeUnexpected(std::move(scanned).error());
    }
    session.syncLevelWithAssets();
    const AssetDatabase& assets = *(*model)->assets();
    ITH_LOG_INFO(ContentBrowser, "Rescan: {} assets, {} problems", assets.records().size(),
                 (*model)->problems().size());
    Value result = Value::object();
    result.set("assets", Value::unsignedInteger(assets.records().size()));
    result.set("problems", Value::unsignedInteger((*model)->problems().size()));
    return result;
}

Expected<Value> open(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.open";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    std::optional<ContentKey> key;
    if (const Value* item = args.kind() == Value::Kind::Object ? args.find("item") : nullptr;
        item != nullptr && item->kind() != Value::Kind::Null) {
        Expected<ContentKey> parsed = parseItem(kCommand, **model, *item);
        if (!parsed) {
            return makeUnexpected(std::move(parsed).error());
        }
        key = std::move(*parsed);
    } else if (const ContentKey* primary = (*model)->primary(); primary != nullptr) {
        key = *primary;
    }
    const ContentItem* target = key.has_value() ? (*model)->find(*key) : nullptr;
    if (target == nullptr) {
        return Value(); // nothing to open
    }
    if (target->kind == ContentItemKind::Folder) {
        const std::string folder = target->path;
        if (Expected<void> moved = (*model)->navigate(folder); !moved) {
            return makeUnexpected(std::move(moved).error());
        }
        return folderResult(**model);
    }
    if (target->kind != ContentItemKind::Asset || target->type != kLevelFormat) {
        return Value(); // other assets have no editor in v1
    }
    if (session.document().playing()) {
        return makeError("editor.command.playing", "a level can't be opened while playing");
    }
    Value openArgs = Value::object();
    openArgs.set("id", idValue(target->id.uuid()));
    Expected<std::optional<std::string_view>> unsaved = optionalString(kCommand, args, "unsaved");
    if (!unsaved) {
        return makeUnexpected(std::move(unsaved).error());
    }
    if (unsaved->has_value()) {
        openArgs.set("unsaved", Value::string(std::string(**unsaved)));
    }
    Expected<Value> opened = session.execute("ith.file.open_level", openArgs);
    if (opened && (*model)->drawerOpen()) {
        (*model)->setDrawerOpen(false); // a level opened from the drawer closes it
        (*model)->setKeyboardFocus(false);
    }
    return opened;
}

Expected<Value> toggleDrawer(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.toggle_drawer";
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    std::optional<bool> open;
    if (const Value* value = args.kind() == Value::Kind::Object ? args.find("open") : nullptr;
        value != nullptr && value->kind() != Value::Kind::Null) {
        if (value->kind() != Value::Kind::Bool) {
            return invalidArgs(kCommand, R"("open" must be true or false)");
        }
        open = value->asBool();
    }
    Value result = Value::object();
    // With the panel open, the panel comes to the front instead (ADR-0049 rule 7).
    if (isPanelOpen(session.layouts().current(), kContentBrowserPanel) && open.value_or(true)) {
        if (Expected<PanelOpened> focused = session.layouts().open(kContentBrowserPanel); !focused) {
            return makeUnexpected(std::move(focused).error());
        }
        (*model)->setDrawerOpen(false);
        result.set("open", Value::boolean(true));
        (*model)->setKeyboardFocus(true); // the keys that follow go to the browser
        result.set("target", Value::string("panel"));
        return result;
    }
    (*model)->setDrawerOpen(open.value_or(!(*model)->drawerOpen()));
    (*model)->setKeyboardFocus((*model)->drawerOpen()); // the keys that follow go where the user looks
    result.set("open", Value::boolean((*model)->drawerOpen()));
    result.set("target", Value::string("drawer"));
    return result;
}

Value itemValue(const ContentItem& item) {
    Value value = Value::object();
    value.set("kind", Value::string(std::string(itemKindId(item.kind))));
    value.set("path", Value::string(item.path));
    if (item.kind == ContentItemKind::Asset) {
        value.set("id", idValue(item.id.uuid()));
    }
    value.set("label", Value::string(item.label));
    value.set("type", Value::string(item.type));
    if (item.kind == ContentItemKind::Asset) {
        value.set("category", Value::string(std::string(categoryId(item.category))));
    }
    Value problems = Value::array();
    for (const ContentBadge& badge : item.problems) {
        Value problem = Value::object();
        problem.set("kind", Value::string(std::string(problemKindId(badge.kind))));
        problem.set("message", Value::string(badge.message));
        if (!badge.otherPath.empty()) {
            problem.set("otherPath", Value::string(badge.otherPath));
        }
        problems.elements().push_back(std::move(problem));
    }
    value.set("problems", std::move(problems));
    return value;
}

Expected<Value> contentQuery(EditorSession& session, const Value& /*args*/) {
    const ContentBrowserModel& model = session.content();
    Value items = Value::array();
    for (const ContentItem* item : model.shown()) {
        items.elements().push_back(itemValue(*item));
    }
    Value result = Value::object();
    result.set("folder", Value::string(model.currentFolder()));
    result.set("search", Value::string(model.search()));
    result.set("categories", categoriesValue(model.filter()));
    result.set("view", viewResult(model.view()));
    result.set("items", std::move(items));
    result.set("problems", Value::unsignedInteger(model.shownProblems()));
    Value selection = selectionResult(model);
    result.set("selection", std::move(*selection.find("selection")));
    result.set("primary", std::move(*selection.find("primary")));
    result.set("anchor", model.anchor() ? keyRef(model, *model.anchor()) : Value());
    result.set("focus", model.focusItem() ? keyRef(model, *model.focusItem()) : Value());
    // No content command waits for an answer in the browse slice (renames and dialogs come later).
    result.set("prompt", Value());
    result.set("drawerOpen", Value::boolean(model.drawerOpen()));
    return result;
}

Expected<Value> contentTreeQuery(EditorSession& session, const Value& /*args*/) {
    const ContentBrowserModel& model = session.content();
    Value rows = Value::array();
    for (const ContentFolderRow& row : model.treeRows()) {
        Value value = Value::object();
        value.set("path", Value::string(row.path));
        value.set("name", Value::string(row.name));
        value.set("depth", Value::unsignedInteger(row.depth));
        value.set("hasChildren", Value::boolean(row.hasChildren));
        value.set("expanded", Value::boolean(row.expanded));
        rows.elements().push_back(std::move(value));
    }
    Value result = Value::object();
    result.set("current", Value::string(model.currentFolder()));
    result.set("folders", std::move(rows));
    return result;
}

Expected<Value> contentProblemsQuery(EditorSession& session, const Value& /*args*/) {
    Value problems = Value::array();
    for (const AssetProblem& problem : session.content().problems()) {
        Value value = Value::object();
        value.set("kind", Value::string(std::string(problemKindId(problem.kind))));
        value.set("path", Value::string(problem.path));
        if (!problem.otherPath.empty()) {
            value.set("otherPath", Value::string(problem.otherPath));
        }
        value.set("message", Value::string(problem.message));
        problems.elements().push_back(std::move(value));
    }
    Value result = Value::object();
    result.set("problems", std::move(problems));
    return result;
}

void addAction(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
               Expected<Value> (*execute)(EditorSession&, const Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Content",
                  .kind = CommandKind::Action,
                  .execute = execute});
}

void addQuery(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
              Expected<Value> (*execute)(EditorSession&, const Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Query",
                  .kind = CommandKind::Query,
                  .execute = execute});
}

} // namespace

void registerContentCommands(CommandRegistry& registry) {
    addAction(registry, "ith.content.navigate", "Go to Folder", "Show a folder's contents", &navigate);
    addAction(registry, "ith.content.navigate_up", "Parent Folder", "Show the parent folder", &navigateUp);
    addAction(registry, "ith.content.select", "Select", "Change the Content Browser selection", &select);
    addAction(registry, "ith.content.select_all", "Select All", "Select every item shown", &selectAll);
    addAction(registry, "ith.content.dismiss", "Clear Selection",
              "Clear the Content Browser selection; with nothing selected, close the content drawer", &dismiss);
    addAction(registry, "ith.content.set_view", "View Options", "Show items as a grid or a list, and sort them",
              &setView);
    addAction(registry, "ith.content.set_filter", "Search", "Find items by name and type", &setFilter);
    addAction(registry, "ith.content.rescan", "Rescan", "Read the Content folder from disk again", &rescan);
    addAction(registry, "ith.content.open", "Open", "Open a level, or go into a folder", &open);
    addAction(registry, "ith.content.toggle_drawer", "Content Drawer",
              "Open or close the content drawer, or bring the Content Browser to the front", &toggleDrawer);
    addQuery(registry, "ith.query.content", "Content", "The Content Browser's folder, items and selection",
             &contentQuery);
    addQuery(registry, "ith.query.content_tree", "Content Tree", "The Content Browser's folder tree",
             &contentTreeQuery);
    addQuery(registry, "ith.query.content_problems", "Content Problems", "What the last scan found wrong on disk",
             &contentProblemsQuery);
}

} // namespace ith
