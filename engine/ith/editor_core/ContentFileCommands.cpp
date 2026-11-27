// The Content Browser's file operations (ADR-0049 rules 2, 8 to 10, 15, 21 and 28;
// docs/specs/content-browser.md 3.7 to 3.11, 3.19 and 3.20): ith.content.new_folder, rename, move,
// duplicate and delete, and the helpers they share with import, reimport and fix
// (ContentImportCommands.cpp).
//
// They are Actions: they act on disk at once, record no undo entry and never make the level dirty
// (ADR-0049 rule 2). A command that needs the user's answer (a rename without a name, a delete
// without "confirmed", an import with unresolved conflicts) changes nothing on disk, sets the
// browser's prompt and returns what the prompt shows; the UI runs it again with the answer. After
// each operation the browser is rebuilt from the database (ContentBrowserModel::refresh), without a
// scan: the paths it keeps (the current folder, the expansion, the selection, the prompt, the
// problems) follow what moved, and the problems an operation fixed or caused are updated (spec
// 3.13). The open level's path follows a rename or a move of its file or of a folder holding it,
// without a dirty change (ADR-0049 rule 23). Every file operation is refused while a save is written
// ("editor.document.save_in_progress", fileBrowserOf).
//
// Names follow one rule (spec 3.8): checkContentName, and no other entry of any kind in the folder
// with that name without ASCII case (findCaseVariant). Duplicates, new folders and Keep Both take
// the first free "<base>_<n>", n from 2 (spec 3.10).
//
// On Windows a file another program holds open without delete sharing can't be renamed, moved or
// deleted: such an operation (a rename or move of a folder holding it, a delete, an import's Replace)
// fails with "asset.io", naming the file, and changes nothing else (spec 3.20).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/ContentCommandSupport.h"
#include "ith/editor_core/ContentPaths.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"
#include "ith/level/Level.h"
#include "ith/serialization/Json.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace detail {

using serialization::Value;

namespace {

// The largest n the duplicate scheme tries; a folder never holds that many names of one base.
constexpr std::uint32_t kMaxNameNumber = 100'000;

// One item reference of the arguments, with the root refused.
Expected<ContentKey> parseTarget(std::string_view command, const ContentBrowserModel& model, const Value& ref) {
    if (const Value* path = ref.kind() == Value::Kind::Object ? ref.find("path") : nullptr;
        path != nullptr && path->kind() == Value::Kind::String && path->asString().empty() &&
        ref.find("id") == nullptr) {
        return makeError("editor.content.root", std::string(command) + ": the Content folder itself can't be changed");
    }
    return parseItem(command, model, ref);
}

} // namespace

Expected<ContentBrowserModel*> fileBrowserOf(EditorSession& session) {
    Expected<ContentBrowserModel*> model = browserOf(session);
    if (model && session.document().saving()) {
        return makeError("editor.document.save_in_progress",
                         "a level is being saved; the Content Browser changes files once it is written");
    }
    return model;
}

std::string fullPath(const AssetDatabase& assets, std::string_view path) {
    return path.empty() ? assets.contentDirectory() : fs::joinPath(assets.contentDirectory(), path);
}

std::size_t depthOf(std::string_view path) {
    return path.empty() ? 0 : static_cast<std::size_t>(std::ranges::count(path, '/')) + 1;
}

Expected<std::optional<std::string>> takenBy(const AssetDatabase& assets, std::string_view folder,
                                             std::string_view name) {
    return findCaseVariant(fullPath(assets, folder), name);
}

Unexpected<Error> nameTaken(std::string_view folder, std::string_view name, std::string_view taken) {
    return makeError("editor.content.exists", "'" + std::string(name) + "' is taken in '" +
                                                  (folder.empty() ? std::string("Content") : std::string(folder)) +
                                                  "' by '" + std::string(taken) + "'");
}

Error contentError(Error error) {
    if (error.code == "asset.path_conflict") {
        error.code = "editor.content.exists";
    } else if (error.code == "asset.invalid_target") {
        error.code = "editor.content.invalid_target";
    }
    return error;
}

Expected<std::string> numberedName(const AssetDatabase& assets, std::string_view folder, std::string_view editable,
                                   std::string_view suffix, bool plainFirst) {
    if (plainFirst) {
        std::string plain = std::string(editable) + std::string(suffix);
        Expected<std::optional<std::string>> taken = takenBy(assets, folder, plain);
        if (!taken) {
            return makeUnexpected(std::move(taken).error());
        }
        if (!taken->has_value()) {
            if (Expected<void> valid = checkContentName(plain); !valid) {
                return makeUnexpected(std::move(valid).error());
            }
            return plain;
        }
    }
    const std::string_view base = numberedNameBase(editable);
    for (std::uint32_t n = 2; n <= kMaxNameNumber; ++n) {
        const std::string number = "_" + std::to_string(n);
        if (number.size() + suffix.size() >= kMaxContentNameSize) {
            break;
        }
        std::size_t size = std::min(base.size(), kMaxContentNameSize - number.size() - suffix.size());
        while (size > 0 && size < base.size() && (static_cast<unsigned char>(base[size]) & 0xC0U) == 0x80U) {
            --size; // not inside a UTF-8 character
        }
        std::string name = std::string(base.substr(0, size)) + number + std::string(suffix);
        if (Expected<void> valid = checkContentName(name); !valid) {
            return makeUnexpected(std::move(valid).error());
        }
        Expected<std::optional<std::string>> taken = takenBy(assets, folder, name);
        if (!taken) {
            return makeUnexpected(std::move(taken).error());
        }
        if (!taken->has_value()) {
            return name;
        }
    }
    return makeError("editor.content.exists",
                     "no free name for '" + std::string(editable) + "' in '" + std::string(folder) + "'");
}

bool holdsLevel(const std::optional<std::string>& level, std::string_view path) {
    return level.has_value() && isAtOrBelow(*level, path);
}

Expected<ContentItem> targetItem(std::string_view command, const ContentBrowserModel& model, const Value& args) {
    std::optional<ContentKey> key;
    if (const Value* item = args.kind() == Value::Kind::Object ? args.find("item") : nullptr;
        item != nullptr && item->kind() != Value::Kind::Null) {
        Expected<ContentKey> parsed = parseTarget(command, model, *item);
        if (!parsed) {
            return makeUnexpected(std::move(parsed).error());
        }
        key = std::move(*parsed);
    } else if (const ContentKey* primary = model.primary(); primary != nullptr) {
        key = *primary;
    }
    const ContentItem* found = key.has_value() ? model.find(*key) : nullptr;
    if (found == nullptr) {
        return makeError("editor.content.not_found", std::string(command) + ": no item is given or selected");
    }
    return *found;
}

Expected<std::vector<ContentItem>> targetItems(std::string_view command, const ContentBrowserModel& model,
                                               const Value& args) {
    std::vector<ContentKey> keys;
    if (const Value* items = args.kind() == Value::Kind::Object ? args.find("items") : nullptr;
        items != nullptr && items->kind() != Value::Kind::Null) {
        if (items->kind() != Value::Kind::Array) {
            return invalidArgs(command, R"("items" must be an array of items)");
        }
        for (const Value& ref : items->elements()) {
            Expected<ContentKey> key = parseTarget(command, model, ref);
            if (!key) {
                return makeUnexpected(std::move(key).error());
            }
            keys.push_back(std::move(*key));
        }
    } else {
        keys = model.selection();
    }
    std::vector<ContentItem> result;
    std::unordered_set<ContentKey, ContentKeyHash> seen;
    for (const ContentKey& key : keys) {
        const ContentItem* item = model.find(key);
        if (item != nullptr && seen.insert(key).second) {
            result.push_back(*item);
        }
    }
    return result;
}

Expected<std::string> targetFolder(std::string_view command, const ContentBrowserModel& model, const Value& args,
                                   std::string_view key, bool required) {
    Expected<std::optional<std::string_view>> folder = optionalString(command, args, key);
    if (!folder) {
        return makeUnexpected(std::move(folder).error());
    }
    if (!folder->has_value()) {
        if (required) {
            return invalidArgs(command, "\"" + std::string(key) + "\" must name a folder");
        }
        return model.currentFolder();
    }
    if (model.folderExists(**folder)) {
        return std::string(**folder);
    }
    if (model.findByPath(**folder) != nullptr) {
        return makeError("editor.content.invalid_target", "'" + std::string(**folder) + "' is not a folder");
    }
    return makeError("editor.content.not_found", "'" + std::string(**folder) + "' is not a folder in Content");
}

Value withError(Value ref, const Error& error) {
    ref.set("code", Value::string(error.code));
    ref.set("message", Value::string(error.message));
    return ref;
}

Value withReason(Value ref, std::string reason) {
    ref.set("reason", Value::string(std::move(reason)));
    return ref;
}

Value pathRef(std::string_view path) {
    Value ref = Value::object();
    ref.set("path", Value::string(std::string(path)));
    return ref;
}

Value assetRef(AssetId id, std::string_view path) {
    Value ref = Value::object();
    ref.set("id", idValue(id.uuid()));
    ref.set("path", Value::string(std::string(path)));
    return ref;
}

Value fileRef(std::string_view file) {
    Value ref = Value::object();
    ref.set("file", Value::string(std::string(file)));
    return ref;
}

std::string_view textOf(const Value& entry, std::string_view key) {
    const Value* value = entry.kind() == Value::Kind::Object ? entry.find(key) : nullptr;
    return value != nullptr && value->kind() == Value::Kind::String ? std::string_view(value->asString())
                                                                    : std::string_view();
}

const ContentBadge* badgeOf(const ContentItem& item, AssetProblemKind kind) {
    const auto found =
        std::ranges::find_if(item.problems, [kind](const ContentBadge& badge) { return badge.kind == kind; });
    return found != item.problems.end() ? &*found : nullptr;
}

} // namespace detail

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

using detail::assetRef;
using detail::contentChild;
using detail::contentError;
using detail::contentName;
using detail::depthOf;
using detail::fileBrowserOf;
using detail::fullPath;
using detail::holdsLevel;
using detail::invalidArgs;
using detail::isAtOrBelow;
using detail::itemRef;
using detail::nameTaken;
using detail::numberedName;
using detail::optionalString;
using detail::pathRef;
using detail::takenBy;
using detail::targetFolder;
using detail::targetItem;
using detail::targetItems;
using detail::textOf;
using detail::withError;
using detail::withReason;
using serialization::Value;

// The name of a new folder before the duplicate scheme (spec 3.7).
constexpr std::string_view kNewFolderName = "New Folder";

// Whether anything is at the file system path `path`.
bool exists(const std::string& path) {
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    return info && info->has_value();
}

std::string_view itemKindName(ContentItemKind kind) {
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

// --- new_folder ---------------------------------------------------------------------------------

Expected<Value> newFolder(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.new_folder";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<std::string> parent = targetFolder(kCommand, **model, args, "parent", false);
    if (!parent) {
        return makeUnexpected(std::move(parent).error());
    }
    Expected<std::optional<std::string_view>> name = optionalString(kCommand, args, "name");
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    std::string folderName;
    if (name->has_value()) {
        if (Expected<void> valid = checkContentName(**name); !valid) {
            return makeUnexpected(std::move(valid).error());
        }
        Expected<std::optional<std::string>> taken = takenBy(assets, *parent, **name);
        if (!taken) {
            return makeUnexpected(std::move(taken).error());
        }
        if (taken->has_value()) {
            return nameTaken(*parent, **name, **taken);
        }
        folderName = std::string(**name);
    } else {
        Expected<std::string> free = numberedName(assets, *parent, kNewFolderName, {}, true);
        if (!free) {
            return makeUnexpected(std::move(free).error());
        }
        folderName = std::move(*free);
    }
    const std::string path = contentChild(*parent, folderName);
    if (Expected<void> made = assets.createFolder(path); !made) {
        return makeUnexpected(contentError(std::move(made).error()));
    }
    ITH_LOG_INFO(ContentBrowser, "New folder {}", path);
    (*model)->refresh();
    const ContentKey key{.id = AssetId(), .path = path};
    (*model)->selectShown(std::span(&key, 1));
    Value result = pathRef(path);
    if (!name->has_value()) {
        // The inline rename starts at once; Escape keeps the default name (spec 3.7).
        Value details = pathRef(path);
        details.set("name", Value::string(folderName));
        details.set("extension", Value::string(""));
        (*model)->setPrompt(ContentPrompt{.kind = ContentPromptKind::Rename, .item = key, .details = details});
        result.set("prompt", Value::string("rename"));
    }
    return result;
}

// --- rename -------------------------------------------------------------------------------------

// Renames a problem entry's file (which the database doesn't know) and the sidecar beside it, if
// any; the file moves back when its sidecar can't follow.
Expected<void> renameLooseFile(const AssetDatabase& assets, std::string_view from, std::string_view to) {
    const std::string source = fullPath(assets, from);
    const std::string target = fullPath(assets, to);
    if (Expected<void> moved = fs::move(source, target); !moved) {
        return moved;
    }
    const std::string sidecar = source + std::string(kSidecarSuffix);
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(sidecar);
    if (!info || !info->has_value()) {
        return {};
    }
    if (Expected<void> moved = fs::move(sidecar, target + std::string(kSidecarSuffix)); !moved) {
        if (!fs::move(target, source)) {
            return makeError("asset.io", "'" + std::string(to) + "' was renamed but its sidecar stayed at '" +
                                             std::string(from) + std::string(kSidecarSuffix) +
                                             "' and the file couldn't be moved back: " + moved.error().message +
                                             "; Rescan to see the files as they are");
        }
        return moved;
    }
    return {};
}

// "editor.content.exists" for a rename the file system takes for the item's own name: on Windows
// (and macOS) a change of case outside ASCII ("été" to "Été") names the same file, which the
// database refuses to rename onto itself.
Unexpected<Error> sameNameForFileSystem(const ContentItem& item, std::string_view newName) {
    return makeError("editor.content.exists", "'" + std::string(newName) + "' and '" +
                                                  std::string(contentName(item.path)) +
                                                  "' are the same name to the file system, which ignores that "
                                                  "difference of case; rename it to another name first");
}

// The checks of a rename of `item` to `newName` that need the disk (spec 3.8).
Expected<void> checkRename(const AssetDatabase& assets, const ContentItem& item, std::string_view newName) {
    // A case-only rename of the item itself (ASCII letters) is no conflict; the database checks it.
    if (equalsIgnoreCaseAscii(newName, contentName(item.path))) {
        return {};
    }
    Expected<std::optional<std::string>> taken = takenBy(assets, item.folder, newName);
    if (!taken) {
        return makeUnexpected(std::move(taken).error());
    }
    if (taken->has_value()) {
        // The entry that takes the name is the item itself when the file system resolves the new path
        // to it (findCaseVariant then reports the name as taken).
        if (fs::pathInside(fullPath(assets, contentChild(item.folder, newName)), fullPath(assets, item.path)) ==
            std::string()) {
            return sameNameForFileSystem(item, newName);
        }
        return nameTaken(item.folder, newName, **taken);
    }
    return {};
}

// Renames `item` on disk and in the database to `newPath` (in its folder).
Expected<void> renameOnDisk(AssetDatabase& assets, const ContentItem& item, const std::string& newPath) {
    switch (item.kind) {
        case ContentItemKind::Folder:
            return assets.moveFolder(item.path, newPath);
        case ContentItemKind::Asset:
            return assets.rename(item.id, contentName(newPath));
        case ContentItemKind::Problem:
            return renameLooseFile(assets, item.path, newPath);
    }
    return {};
}

Expected<Value> renameItem(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.rename";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<ContentItem> item = targetItem(kCommand, **model, args);
    if (!item) {
        return makeUnexpected(std::move(item).error());
    }
    if (item->kind == ContentItemKind::Problem && endsWithIgnoreCaseAscii(item->path, kSidecarSuffix)) {
        return makeError("editor.content.invalid_target",
                         "'" + item->path +
                             "' is a sidecar, which goes with its file; delete it, or move its file "
                             "back and Rescan");
    }
    const ContentNameParts parts = nameParts(*item);
    Expected<std::optional<std::string_view>> name = optionalString(kCommand, args, "name");
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    if (!name->has_value()) {
        Value details = pathRef(item->path);
        details.set("name", Value::string(parts.editable));
        details.set("extension", Value::string(parts.extension));
        (*model)->setPrompt(ContentPrompt{.kind = ContentPromptKind::Rename, .item = keyOf(*item), .details = details});
        return details;
    }
    Value result = itemRef(*item);
    if (**name == parts.editable) {
        (*model)->clearPrompt(); // an unchanged name does nothing
        return result;
    }
    if (name->value().empty()) {
        return makeError("editor.content.invalid_name", "a name can't be empty");
    }
    const std::string newName = std::string(**name) + parts.extension;
    if (Expected<void> valid = checkContentName(newName); !valid) {
        return makeUnexpected(std::move(valid).error());
    }
    if (Expected<void> allowed = checkRename(assets, *item, newName); !allowed) {
        return makeUnexpected(std::move(allowed).error());
    }
    std::string newPath = contentChild(item->folder, newName);
    const std::optional<std::string> level = session.levelContentPath(); // before the file moves
    const Expected<void> renamed = renameOnDisk(assets, *item, newPath);
    if (!renamed) {
        // What moved anyway (a file whose sidecar could neither follow nor the file go back) is shown
        // where it is: an asset at its record's path, a problem entry's file at the new path.
        std::optional<std::string> at;
        if (const AssetRecord* record = item->kind == ContentItemKind::Asset ? assets.find(item->id) : nullptr;
            record != nullptr && record->path != item->path) {
            at = record->path;
        } else if (item->kind == ContentItemKind::Problem && !exists(fullPath(assets, item->path)) &&
                   exists(fullPath(assets, newPath))) {
            at = newPath;
        }
        if (at.has_value()) {
            (*model)->movedPath(item->path, *at);
            session.followContentMove(level, item->path, *at);
        }
        (*model)->refresh();
        if (renamed.error().code == "asset.path_conflict" &&
            fs::pathInside(fullPath(assets, newPath), fullPath(assets, item->path)) == std::string()) {
            return sameNameForFileSystem(*item, newName);
        }
        return makeUnexpected(contentError(renamed.error()));
    }
    ITH_LOG_INFO(ContentBrowser, "Renamed {} to {}", item->path, newPath);
    (*model)->movedPath(item->path, newPath);
    session.followContentMove(level, item->path, newPath);
    (*model)->clearPrompt();
    (*model)->refresh();
    result.set("path", Value::string(std::move(newPath)));
    return result;
}

// --- move ---------------------------------------------------------------------------------------

// The checks of moving `items` into `folder`, all before anything moves (spec 3.9): no problem
// entries, no folder into itself or below it, and every name free in the target, also among the
// items moved. Returns the items that are not in `folder` already, in order.
Expected<std::vector<const ContentItem*>> checkMove(const AssetDatabase& assets, const std::string& folder,
                                                    const std::vector<ContentItem>& items) {
    for (const ContentItem& item : items) {
        if (item.kind == ContentItemKind::Problem) {
            return makeError("editor.content.invalid_target",
                             "'" + item.path + "' is a problem entry, which can't be moved; fix, rename or delete it");
        }
        if (item.kind == ContentItemKind::Folder && isAtOrBelow(folder, item.path)) {
            return makeError("editor.content.invalid_target",
                             "'" + item.path + "' can't move into itself or a folder below it");
        }
    }
    std::vector<const ContentItem*> pending;
    std::string conflicts;
    std::unordered_set<std::string> names;
    for (const ContentItem& item : items) {
        if (item.folder == folder) {
            continue; // a drop on the folder that holds it does nothing
        }
        pending.push_back(&item);
        const std::string_view name = contentName(item.path);
        Expected<std::optional<std::string>> taken = takenBy(assets, folder, name);
        if (!taken) {
            return makeUnexpected(std::move(taken).error());
        }
        if (taken->has_value() || !names.insert(toLowerAscii(name)).second) {
            conflicts += (conflicts.empty() ? "" : ", ") + std::string(name);
        }
    }
    if (!conflicts.empty()) {
        return makeError("editor.content.exists", "nothing moved: these names are taken in '" +
                                                      (folder.empty() ? std::string("Content") : folder) +
                                                      "': " + conflicts);
    }
    return pending;
}

Expected<Value> moveItems(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.move";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<std::string> folder = targetFolder(kCommand, **model, args, "folder", true);
    if (!folder) {
        return makeUnexpected(std::move(folder).error());
    }
    Expected<std::vector<ContentItem>> items = targetItems(kCommand, **model, args);
    if (!items) {
        return makeUnexpected(std::move(items).error());
    }
    if (items->empty()) {
        return Value(); // nothing to move
    }
    Expected<std::vector<const ContentItem*>> pending = checkMove(assets, *folder, *items);
    if (!pending) {
        return makeUnexpected(std::move(pending).error());
    }
    const std::optional<std::string> level = session.levelContentPath(); // before anything moves
    Value moved = Value::array();
    Value failed = Value::array();
    Value skipped = Value::array();
    bool stopped = false;
    for (const ContentItem* item : *pending) {
        if (stopped) {
            skipped.elements().push_back(withReason(itemRef(*item), "not moved: an earlier move failed"));
            continue;
        }
        const std::string to = contentChild(*folder, contentName(item->path));
        Expected<void> done =
            item->kind == ContentItemKind::Folder ? assets.moveFolder(item->path, to) : assets.move(item->id, to);
        std::string at = done ? to : item->path;
        if (!done && item->kind == ContentItemKind::Asset) {
            // A file that moved without its sidecar is at its new path in the database (spec 3.20).
            if (const AssetRecord* record = assets.find(item->id); record != nullptr) {
                at = record->path;
            }
        }
        if (at != item->path) {
            (*model)->movedPath(item->path, at);
            session.followContentMove(level, item->path, at);
        }
        if (!done) {
            failed.elements().push_back(withError(itemRef(*item), contentError(std::move(done).error())));
            stopped = true; // a failure in the middle of a move stops it (spec 3.20)
            continue;
        }
        ITH_LOG_INFO(ContentBrowser, "Moved {} to {}", item->path, to);
        Value ref = itemRef(*item);
        ref.set("path", Value::string(to));
        moved.elements().push_back(std::move(ref));
    }
    (*model)->refresh();
    Value result = Value::object();
    result.set("moved", std::move(moved));
    result.set("failed", std::move(failed));
    result.set("skipped", std::move(skipped));
    return result;
}

// --- duplicate ----------------------------------------------------------------------------------

Expected<Value> duplicateItems(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.duplicate";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<std::vector<ContentItem>> items = targetItems(kCommand, **model, args);
    if (!items) {
        return makeUnexpected(std::move(items).error());
    }
    if (items->empty()) {
        return Value(); // nothing to duplicate
    }
    const std::optional<std::string> level = session.levelContentPath();
    Value created = Value::array();
    Value skipped = Value::array();
    Value failed = Value::array();
    std::vector<ContentKey> copies;
    bool stopped = false;
    for (const ContentItem& item : *items) {
        if (item.kind != ContentItemKind::Asset) {
            skipped.elements().push_back(withReason(itemRef(item), "folders and problem entries can't be duplicated"));
            continue;
        }
        if (stopped) {
            skipped.elements().push_back(withReason(itemRef(item), "not duplicated: an earlier duplicate failed"));
            continue;
        }
        const ContentNameParts parts = nameParts(item);
        Expected<std::string> name = numberedName(assets, item.folder, parts.editable, parts.extension, false);
        Expected<AssetId> copy = name ? assets.duplicate(item.id, contentChild(item.folder, *name))
                                      : Expected<AssetId>(makeUnexpected(name.error()));
        if (!copy) {
            failed.elements().push_back(withError(itemRef(item), contentError(std::move(copy).error())));
            stopped = true; // an I/O failure in the middle of a duplicate stops it (spec 3.20)
            continue;
        }
        const std::string path = contentChild(item.folder, *name);
        ITH_LOG_INFO(ContentBrowser, "Duplicated {} as {}", item.path, path);
        if (level == item.path && session.document().dirty()) {
            ITH_LOG_WARNING(ContentBrowser,
                            "{} is a copy of the saved file: the open level's unsaved changes are "
                            "not in it",
                            path);
        }
        created.elements().push_back(assetRef(*copy, path));
        copies.push_back(ContentKey{.id = *copy, .path = {}});
    }
    (*model)->refresh();
    if (!copies.empty()) {
        (*model)->selectShown(copies); // the copies, not the originals (spec 3.10)
    }
    Value result = Value::object();
    result.set("created", std::move(created));
    result.set("skipped", std::move(skipped));
    result.set("failed", std::move(failed));
    return result;
}

// --- delete -------------------------------------------------------------------------------------

// What a deleted folder holds.
struct FolderContents {
    std::size_t assets = 0;
    // Files the browser doesn't show: neither assets, their sidecars nor problem entries.
    std::size_t otherFiles = 0;
};

void countFolder(const AssetDatabase& assets, const ContentBrowserModel& model, const std::string& folder,
                 FolderContents& contents) {
    Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(fullPath(assets, folder));
    if (!entries) {
        return; // the delete reports it
    }
    for (const fs::DirectoryEntry& entry : *entries) {
        if (!entry.validName) {
            ++contents.otherFiles;
            continue;
        }
        const std::string path = contentChild(folder, entry.name);
        if (entry.kind == fs::FileKind::Directory) {
            if (depthOf(path) <= kMaxFolderDepth) {
                countFolder(assets, model, path, contents);
            }
            continue;
        }
        if (assets.findByPath(path) != nullptr) {
            ++contents.assets;
            continue;
        }
        const bool sidecar =
            endsWithIgnoreCaseAscii(path, kSidecarSuffix) &&
            assets.findByPath(std::string_view(path).substr(0, path.size() - kSidecarSuffix.size())) != nullptr;
        if (!sidecar && model.findByPath(path) == nullptr) {
            ++contents.otherFiles;
        }
    }
}

// The asset ids the open level's current state refers to (ADR-0049 rule 10).
Expected<std::vector<AssetId>> openLevelReferences(const EditorSession& session) {
    const Expected<std::string, serialization::Error> text =
        saveLevel(session.document().world(), session.document().levelId(), session.types());
    if (!text) {
        return detail::fromSerialization(text.error());
    }
    const Expected<Value, serialization::Error> value = serialization::parseJson(*text);
    if (!value) {
        return detail::fromSerialization(value.error());
    }
    return collectAssetReferences(*value);
}

Value userRef(const AssetDatabase& assets, const ContentBrowserModel& model, AssetId id) {
    const AssetRecord* record = assets.find(id);
    Value ref = assetRef(id, record != nullptr ? std::string_view(record->path) : std::string_view());
    const ContentItem* item = model.find(ContentKey{.id = id, .path = {}});
    ref.set("label", Value::string(item != nullptr ? item->label : std::string()));
    return ref;
}

// The preview of deleting `items` (spec 3.11): the items with the contents of folders, the totals,
// each used asset with its users outside the deleted set (the open level's unsaved state included),
// and the files whose references couldn't be read.
Value deletePreview(const EditorSession& session, const AssetDatabase& assets, const ContentBrowserModel& model,
                    const std::vector<ContentItem>& items) {
    std::vector<AssetId> ids;
    std::unordered_set<AssetId> idSet;
    const auto addId = [&](AssetId id) {
        if (idSet.insert(id).second) {
            ids.push_back(id);
        }
    };
    Value list = Value::array();
    FolderContents total;
    for (const ContentItem& item : items) {
        Value entry = itemRef(item);
        entry.set("kind", Value::string(std::string(itemKindName(item.kind))));
        entry.set("label", Value::string(item.label));
        if (item.kind == ContentItemKind::Asset) {
            addId(item.id);
            ++total.assets;
        } else if (item.kind == ContentItemKind::Folder) {
            FolderContents contents;
            countFolder(assets, model, item.path, contents);
            for (const AssetRecord* record : assets.records()) {
                if (isAtOrBelow(record->path, item.path)) {
                    addId(record->id);
                }
            }
            entry.set("assets", Value::unsignedInteger(contents.assets));
            entry.set("otherFiles", Value::unsignedInteger(contents.otherFiles));
            total.assets += contents.assets;
            total.otherFiles += contents.otherFiles;
        }
        list.elements().push_back(std::move(entry));
    }
    const FindUsersReport users = assets.findUsers(ids);
    const Expected<std::vector<AssetId>> levelRefs = openLevelReferences(session);
    const AssetId levelId(session.document().levelId());
    const std::optional<std::string> levelPath = session.levelContentPath();
    Value uses = Value::array();
    for (const AssetUsers& use : users.uses) {
        const bool usedByLevel =
            levelRefs && std::ranges::binary_search(*levelRefs, use.id) && !idSet.contains(levelId);
        if (use.users.empty() && !usedByLevel) {
            continue;
        }
        Value userList = Value::array();
        bool levelListed = false;
        for (const AssetId user : use.users) {
            Value ref = userRef(assets, model, user);
            if (user == levelId) {
                ref.set("openLevel", Value::boolean(true));
                levelListed = true;
            }
            userList.elements().push_back(std::move(ref));
        }
        if (usedByLevel && !levelListed) {
            // Only the open level's unsaved state, or a level the database doesn't have, refers to it.
            Value ref = assetRef(levelId, levelPath.value_or(session.document().path()));
            ref.set("label", Value::string(session.document().path().empty()
                                               ? std::string("Untitled level")
                                               : std::string(contentName(session.document().path()))));
            ref.set("openLevel", Value::boolean(true));
            ref.set("unsaved", Value::boolean(true));
            userList.elements().push_back(std::move(ref));
        }
        Value entry = Value::object();
        entry.set("asset", userRef(assets, model, use.id));
        entry.set("users", std::move(userList));
        uses.elements().push_back(std::move(entry));
    }
    Value unreadable = Value::array();
    for (const AssetFailure& failure : users.unreadable) {
        unreadable.elements().push_back(withError(pathRef(failure.path), failure.error));
    }
    if (!levelRefs) {
        unreadable.elements().push_back(
            withError(pathRef(levelPath.value_or(session.document().path())), levelRefs.error()));
    }
    Value preview = Value::object();
    preview.set("items", std::move(list));
    preview.set("count", Value::unsignedInteger(items.size()));
    preview.set("assets", Value::unsignedInteger(total.assets));
    preview.set("otherFiles", Value::unsignedInteger(total.otherFiles));
    preview.set("uses", std::move(uses));
    preview.set("unreadable", std::move(unreadable));
    return preview;
}

// Deletes a problem entry's file, and the sidecar beside it if any (a duplicate's).
Expected<void> removeLooseFile(const AssetDatabase& assets, std::string_view path) {
    const std::string full = fullPath(assets, path);
    if (Expected<void> removed = fs::remove(full); !removed) {
        return removed;
    }
    if (endsWithIgnoreCaseAscii(path, kSidecarSuffix)) {
        return {};
    }
    const std::string sidecar = full + std::string(kSidecarSuffix);
    if (const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(sidecar); info && info->has_value()) {
        return fs::remove(sidecar);
    }
    return {};
}

// Deletes one item (spec 3.11), adding what failed to `failed` and the sidecars it left behind to
// `orphans`; true when the item is gone.
bool deleteOne(AssetDatabase& assets, const ContentItem& item, Value& failed, std::vector<AssetProblem>& orphans) {
    switch (item.kind) {
        case ContentItemKind::Asset: {
            if (assets.find(item.id) == nullptr) {
                return true; // gone already
            }
            const std::string sidecar = item.path + std::string(kSidecarSuffix);
            const Expected<void> removed = assets.remove(item.id);
            const bool gone = assets.find(item.id) == nullptr;
            if (!removed) {
                // Only the sidecar stayed when the asset is gone: an orphan now.
                failed.elements().push_back(withError(gone ? pathRef(sidecar) : itemRef(item), removed.error()));
                if (gone) {
                    orphans.push_back(AssetProblem{.kind = AssetProblemKind::OrphanSidecar,
                                                   .path = sidecar,
                                                   .otherPath = {},
                                                   .message = removed.error().message});
                }
            }
            return gone;
        }
        case ContentItemKind::Problem: {
            const Expected<void> removed = removeLooseFile(assets, item.path);
            if (!removed) {
                failed.elements().push_back(withError(itemRef(item), removed.error()));
            }
            return removed.hasValue();
        }
        case ContentItemKind::Folder: {
            if (!std::ranges::binary_search(assets.folders(), item.path)) {
                return true; // gone already
            }
            const Expected<RemoveFolderReport> report = assets.removeFolder(item.path);
            if (!report) {
                failed.elements().push_back(withError(itemRef(item), report.error()));
                return false;
            }
            for (const AssetFailure& failure : report->failures) {
                failed.elements().push_back(withError(pathRef(failure.path), failure.error));
            }
            orphans.insert(orphans.end(), report->orphans.begin(), report->orphans.end());
            return report->failures.empty();
        }
    }
    return false;
}

Expected<Value> deleteItems(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.delete";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<std::vector<ContentItem>> items = targetItems(kCommand, **model, args);
    if (!items) {
        return makeUnexpected(std::move(items).error());
    }
    bool confirmed = false;
    if (const Value* value = args.kind() == Value::Kind::Object ? args.find("confirmed") : nullptr;
        value != nullptr && value->kind() != Value::Kind::Null) {
        if (value->kind() != Value::Kind::Bool) {
            return invalidArgs(kCommand, R"("confirmed" must be true or false)");
        }
        confirmed = value->asBool();
    }
    if (items->empty()) {
        return Value(); // nothing to delete
    }
    const std::optional<std::string> level = session.levelContentPath();
    for (const ContentItem& item : *items) {
        if (holdsLevel(level, item.path)) {
            return makeError("editor.content.level_open", "'" + item.path +
                                                              "' holds the open level; open another level or a new "
                                                              "level first");
        }
    }
    if (!confirmed) {
        Value result = Value::object();
        result.set("deleted", Value::boolean(false));
        result.set("preview", deletePreview(session, assets, **model, *items));
        (*model)->setPrompt(ContentPrompt{.kind = ContentPromptKind::Delete, .item = {}, .details = result});
        return result;
    }
    // Files first, then folders from the deepest up (spec 3.11); a failure doesn't stop the others.
    std::ranges::stable_sort(*items, [](const ContentItem& a, const ContentItem& b) {
        const bool folderA = a.kind == ContentItemKind::Folder;
        const bool folderB = b.kind == ContentItemKind::Folder;
        if (folderA != folderB) {
            return folderB;
        }
        return folderA && depthOf(a.path) > depthOf(b.path);
    });
    Value deleted = Value::array();
    Value failed = Value::array();
    std::vector<AssetProblem> orphans;
    for (const ContentItem& item : *items) {
        if (deleteOne(assets, item, failed, orphans)) {
            ITH_LOG_INFO(ContentBrowser, "Deleted {}", item.path);
            (*model)->removedPath(item.path);
            deleted.elements().push_back(itemRef(item));
        }
    }
    for (const Value& failure : failed.elements()) {
        ITH_LOG_WARNING(ContentBrowser, "Couldn't delete {}: {}", textOf(failure, "path"), textOf(failure, "message"));
    }
    (*model)->addProblems(orphans);
    (*model)->clearPrompt();
    (*model)->refresh();
    Value result = Value::object();
    result.set("deleted", std::move(deleted));
    result.set("failed", std::move(failed));
    return result;
}

} // namespace

namespace detail {

void registerContentFileCommands(CommandRegistry& registry) {
    detail::addContentAction(registry, "ith.content.new_folder", "New Folder", "Create a folder and name it",
                             &newFolder);
    detail::addContentAction(registry, "ith.content.rename", "Rename", "Rename a folder or an asset; its id stays",
                             &renameItem);
    detail::addContentAction(registry, "ith.content.move", "Move", "Move folders and assets into a folder; ids stay",
                             &moveItems);
    detail::addContentAction(registry, "ith.content.duplicate", "Duplicate", "Copy the selected assets, with new ids",
                             &duplicateItems);
    detail::addContentAction(registry, "ith.content.delete", "Delete",
                             "Delete folders and assets from disk, after asking", &deleteItems);
}

} // namespace detail
} // namespace ith
