// The built-in File commands (ADR-0037 rules 4, 15 and 16): New Level, Open Level, Save and
// Save As. They are Actions: they don't record undo entries; New and Open clear the history.
//
// A dirty document refuses New and Open with "editor.document.unsaved_changes" unless the arguments
// say what to do with the changes: "unsaved": "save" saves first (and waits for the write), and
// "discard" drops them. The UI asks Save / Don't Save / Cancel and runs the command again with the
// answer; automation passes it. Saving and opening finish later, through the session's job
// system (EditorSession.h); "ith.query.document" reports the result.
//
// Save As to a new path gives the saved level a new id, and over another level file that file's id;
// the open document takes it with the path once the write succeeds (ADR-0050 item 9,
// EditorSession::save).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/assets/AssetId.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/level/LevelFormat.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ith {

namespace {

using detail::invalidArgs;
using detail::optionalId;
using detail::optionalString;
using detail::requiredString;
using serialization::Value;

Expected<void> notPlaying(EditorSession& session) {
    if (session.document().playing()) {
        return makeError("editor.command.playing", "the level can't be replaced while playing");
    }
    return {};
}

// Deals with unsaved changes before the level is replaced.
Expected<void> resolveUnsaved(EditorSession& session, std::string_view command, const Value& args) {
    if (!session.document().dirty()) {
        return {};
    }
    Expected<std::optional<std::string_view>> answer = optionalString(command, args, "unsaved");
    if (!answer) {
        return makeUnexpected(std::move(answer).error());
    }
    if (!answer->has_value()) {
        return makeError("editor.document.unsaved_changes",
                         std::string(command) +
                             R"(: the level has unsaved changes; pass "unsaved": "save" or "discard")");
    }
    if (**answer == "discard") {
        return {};
    }
    if (**answer != "save") {
        return invalidArgs(command, R"("unsaved" must be "save" or "discard")");
    }
    if (Expected<void> saving = session.save(); !saving) {
        return saving;
    }
    session.waitIdle();
    if (const std::optional<Error>& failed = session.lastFileError()) {
        return makeUnexpected(*failed);
    }
    return {};
}

Expected<Value> newLevel(EditorSession& session, const Value& args) {
    if (Expected<void> resolved = resolveUnsaved(session, "ith.file.new_level", args); !resolved) {
        return makeUnexpected(std::move(resolved).error());
    }
    if (Expected<void> created = session.newLevel(); !created) {
        return makeUnexpected(std::move(created).error());
    }
    return Value();
}

// The path of a level asset: the Content folder and its path in the asset database.
Expected<std::string> levelPathOf(const EditorSession& session, const Uuid& id) {
    const AssetDatabase* assets = session.options().assets;
    if (assets == nullptr) {
        return makeError("editor.document.no_asset_database", "levels can be opened by id only with an asset database");
    }
    const AssetRecord* record = assets->find(AssetId(id));
    if (record == nullptr) {
        return makeError("editor.asset.not_found", "no asset has the id " + id.toString());
    }
    if (record->type != kLevelFormat) {
        return invalidArgs("ith.file.open_level", record->path + " is not a level");
    }
    return std::string(kContentFolderName) + "/" + record->path;
}

Expected<Value> openLevel(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.file.open_level";
    Expected<std::optional<std::string_view>> path = optionalString(kCommand, args, "path");
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    Expected<std::optional<Uuid>> id = optionalId(kCommand, args, "id");
    if (!id) {
        return makeUnexpected(std::move(id).error());
    }
    if (path->has_value() == id->has_value()) {
        return invalidArgs(kCommand, R"(give either "path" or "id")");
    }
    std::string target;
    if (id->has_value()) {
        Expected<std::string> found = levelPathOf(session, **id);
        if (!found) {
            return makeUnexpected(std::move(found).error());
        }
        target = std::move(*found);
    } else {
        target = std::string(**path);
    }
    if (Expected<void> resolved = resolveUnsaved(session, kCommand, args); !resolved) {
        return makeUnexpected(std::move(resolved).error());
    }
    if (Expected<void> opened = session.open(target); !opened) {
        return makeUnexpected(std::move(opened).error());
    }
    return Value();
}

Expected<Value> save(EditorSession& session, const Value& /*args*/) {
    if (Expected<void> saved = session.save(); !saved) {
        return makeUnexpected(std::move(saved).error());
    }
    return Value();
}

Expected<Value> saveAs(EditorSession& session, const Value& args) {
    Expected<std::string_view> path = requiredString("ith.file.save_as", args, "path");
    if (!path) {
        return makeUnexpected(std::move(path).error());
    }
    if (path->empty()) {
        return invalidArgs("ith.file.save_as", R"("path" can't be empty)");
    }
    if (Expected<void> saved = session.save(*path); !saved) {
        return makeUnexpected(std::move(saved).error());
    }
    return Value();
}

} // namespace

void registerFileCommands(CommandRegistry& registry) {
    registry.add({.id = "ith.file.new_level",
                  .displayName = "New Level",
                  .tooltip = "Start an empty, untitled level",
                  .category = "File",
                  .kind = CommandKind::Action,
                  .canExecute = &notPlaying,
                  .execute = &newLevel});
    registry.add({.id = "ith.file.open_level",
                  .displayName = "Open Level",
                  .tooltip = "Open a level file, by path or by asset id",
                  .category = "File",
                  .kind = CommandKind::Action,
                  .canExecute = &notPlaying,
                  .execute = &openLevel});
    registry.add({.id = "ith.file.save",
                  .displayName = "Save Current Level",
                  .tooltip = "Save the level to its file",
                  .category = "File",
                  .kind = CommandKind::Action,
                  .execute = &save});
    registry.add({.id = "ith.file.save_as",
                  .displayName = "Save Current Level As",
                  .tooltip = "Save the level to another file, which becomes its file",
                  .category = "File",
                  .kind = CommandKind::Action,
                  .execute = &saveAs});
}

} // namespace ith
