#pragma once

// Helpers shared by the Content Browser's commands (ContentCommands.cpp, ContentFileCommands.cpp,
// ContentImportCommands.cpp): the browser of a session, item references in arguments and results,
// and the file operations' name rule and refusals. Internal to editor_core.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith {

class EditorSession;

namespace detail {

// --- ContentCommands.cpp ---

// The session's Content Browser, or "editor.document.no_asset_database" without a project.
[[nodiscard]] Expected<ContentBrowserModel*> browserOf(EditorSession& session);

// {"id", "path"} of an asset, {"path"} of any other item.
[[nodiscard]] serialization::Value itemRef(const ContentItem& item);

// itemRef of the item with `key`; for an item that is gone, {"id"} or {"path"} from the key.
[[nodiscard]] serialization::Value keyRef(const ContentBrowserModel& model, const ContentKey& key);

// The item an item reference ({"id": <uuid>} or {"path": <content path>}) names: an asset by id, or
// any item at the path. Fails with "editor.command.invalid_args" for a malformed reference and
// "editor.content.not_found" when nothing is there.
[[nodiscard]] Expected<ContentKey> parseItem(std::string_view command, const ContentBrowserModel& model,
                                             const serialization::Value& ref);

// {selection, primary}: the content selection as item references.
[[nodiscard]] serialization::Value selectionResult(const ContentBrowserModel& model);

// --- ContentFileCommands.cpp ---

// The browser of a file operation: browserOf, then "editor.document.save_in_progress" while any save
// is written. A Save As writes a file that is not the document's path yet, and its completion
// rescans and sets the path it wrote, so no file operation runs beside it.
[[nodiscard]] Expected<ContentBrowserModel*> fileBrowserOf(EditorSession& session);

// The file system path of a content path ("" is the Content folder).
[[nodiscard]] std::string fullPath(const AssetDatabase& assets, std::string_view path);

// The number of names in a content path; 0 for the root.
[[nodiscard]] std::size_t depthOf(std::string_view path);

// The entry of the content folder `folder` that takes `name` (findCaseVariant), or nullopt.
[[nodiscard]] Expected<std::optional<std::string>> takenBy(const AssetDatabase& assets, std::string_view folder,
                                                           std::string_view name);

// "editor.content.exists": `name` is taken in `folder` by the entry `taken`.
[[nodiscard]] Unexpected<Error> nameTaken(std::string_view folder, std::string_view name, std::string_view taken);

// The database's errors as the browser reports them: a taken name is "editor.content.exists", a
// folder moved into itself "editor.content.invalid_target"; others as they are.
[[nodiscard]] Error contentError(Error error);

// The first free name of the duplicate scheme in the content folder `folder` (spec 3.10):
// `editable` + `suffix` itself when `plainFirst` and it is free, otherwise "<base>_<n>" + `suffix`
// with n from 2, the base being numberedNameBase(editable) cut at a UTF-8 character so the name
// fits kMaxContentNameSize. Fails with "editor.content.invalid_name" or "editor.content.exists" (no
// free number), or the error of findCaseVariant.
[[nodiscard]] Expected<std::string> numberedName(const AssetDatabase& assets, std::string_view folder,
                                                 std::string_view editable, std::string_view suffix, bool plainFirst);

// Whether `level` (the open level's content path, if any) is at or below the content path `path`.
[[nodiscard]] bool holdsLevel(const std::optional<std::string>& level, std::string_view path);

// The item of "item", or the primary item when it is missing; "editor.content.root" for the root,
// "editor.content.not_found" when there is neither. A copy, since operations rebuild the model.
[[nodiscard]] Expected<ContentItem> targetItem(std::string_view command, const ContentBrowserModel& model,
                                               const serialization::Value& args);

// The items of "items", or the content selection when it is missing; copies, without repeats.
// "editor.content.root" for the root.
[[nodiscard]] Expected<std::vector<ContentItem>> targetItems(std::string_view command, const ContentBrowserModel& model,
                                                             const serialization::Value& args);

// The folder named by the string argument `key` (the current folder when missing and not
// `required`): "editor.content.not_found" when nothing is there, "editor.content.invalid_target"
// when it is not a folder.
[[nodiscard]] Expected<std::string> targetFolder(std::string_view command, const ContentBrowserModel& model,
                                                 const serialization::Value& args, std::string_view key, bool required);

// Result entries: `ref` with "code" and "message", or with "reason".
[[nodiscard]] serialization::Value withError(serialization::Value ref, const Error& error);
[[nodiscard]] serialization::Value withReason(serialization::Value ref, std::string reason);

// {"path"}, {"id", "path"} and {"file"}.
[[nodiscard]] serialization::Value pathRef(std::string_view path);
[[nodiscard]] serialization::Value assetRef(AssetId id, std::string_view path);
[[nodiscard]] serialization::Value fileRef(std::string_view file);

// The string member `key` of a result entry, or "" when it has none.
[[nodiscard]] std::string_view textOf(const serialization::Value& entry, std::string_view key);

// The badge of `kind` on `item`, or null.
[[nodiscard]] const ContentBadge* badgeOf(const ContentItem& item, AssetProblemKind kind);

// Registers an ith.content command: an Action in the "Content" category.
inline void addContentAction(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
                             Expected<serialization::Value> (*execute)(EditorSession&, const serialization::Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Content",
                  .kind = CommandKind::Action,
                  .execute = execute});
}

// Registers the file operations: new_folder, rename, move, duplicate and delete.
void registerContentFileCommands(CommandRegistry& registry);

// --- ContentImportCommands.cpp ---

// Registers import, reimport and fix.
void registerContentImportCommands(CommandRegistry& registry);

} // namespace detail
} // namespace ith
