// The Content Browser's import, reimport and fix (ADR-0049 rules 11, 14, 20 and 28;
// docs/specs/content-browser.md 3.12, 3.13 and 3.19): ith.content.import, ith.content.reimport and
// ith.content.fix. Actions like the other file operations (ContentFileCommands.cpp, whose helpers
// they use): refused while a save is written, not undoable, never a dirty level.
//
// An import works the files out first (ImportPlan: dropped folders walked, files no importer reads
// skipped, names resolved against the disk, conflicts found), then, when every conflict has an
// answer, creates the folders, copies and imports, synchronously on the main thread (ADR-0049
// rule 11).

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/ContentCommandSupport.h"
#include "ith/editor_core/ContentPaths.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ith {

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

using detail::assetRef;
using detail::badgeOf;
using detail::contentChild;
using detail::contentError;
using detail::contentName;
using detail::contentParent;
using detail::depthOf;
using detail::fileBrowserOf;
using detail::fileRef;
using detail::fullPath;
using detail::holdsLevel;
using detail::invalidArgs;
using detail::isAtOrBelow;
using detail::itemRef;
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

// The most files and folders one import walks (dropped folders included, missing files counted):
// a drop of a whole drive must not stall the editor without end. More fails the import with
// nothing copied ("editor.content.too_many_files").
constexpr std::size_t kMaxImportEntries = 4096;

// Why a name from the file system is skipped: it can't address its entry here (fs::hasUnusableName,
// DirectoryEntry::validName).
constexpr std::string_view kUnusableName = "its name can't be used: it isn't valid Unicode or holds '\\'";

// Why a link found inside a dropped folder is skipped (a link dropped on its own is imported).
constexpr std::string_view kLinkSkipped = "a link inside a dropped folder is not followed";

enum class ConflictChoice : std::uint8_t {
    // No choice given.
    None,
    Replace,
    KeepBoth,
    Skip,
};

std::optional<ConflictChoice> parseChoice(std::string_view text) {
    if (text == "replace") {
        return ConflictChoice::Replace;
    }
    if (text == "keep_both") {
        return ConflictChoice::KeepBoth;
    }
    if (text == "skip") {
        return ConflictChoice::Skip;
    }
    return std::nullopt;
}

// One file of an import.
struct ImportSource {
    // The file, absolute and normalized.
    std::string file;
    // Where it goes (a content path), or for a file inside Content its own content path.
    std::string target;
    bool inside = false;
};

// What an import does, worked out before anything is written.
struct ImportPlan {
    std::vector<ImportSource> sources;
    // The folders to create for dropped folders, parents first.
    std::vector<std::string> folders;
    // The spelling chosen for each folder of this import, by its path without ASCII case, so folders
    // that differ only in case ("Foo" and "foo", dropped together or siblings in one drop) merge the
    // same way on every system, before any of them exists.
    std::unordered_map<std::string, std::string> folderSpelling;
    std::size_t entries = 0;
    Value skipped = Value::array();
    Value failed = Value::array();
};

// Where a dropped folder is relative to Content, worked out once for its whole tree (links inside
// are not followed, so nothing below it leads anywhere else).
struct DroppedFolder {
    // The folder's content path when it is inside Content: its files are imported in place.
    std::optional<std::string> inside;
    // Where Content is inside the folder, relative to it ("" when the folder is Content itself).
    std::optional<std::string> content;
};

// The content path of the entry at `relative` (below the dropped folder) when it is inside Content,
// "" for the Content folder itself, or nullopt outside it.
std::optional<std::string> contentPathIn(const DroppedFolder& drop, const std::string& relative) {
    if (drop.inside.has_value()) {
        return relative.empty() ? *drop.inside : contentChild(*drop.inside, relative);
    }
    if (drop.content.has_value() && detail::isInContentFolder(relative, *drop.content)) {
        const std::size_t skip = drop.content->empty() || relative.size() == drop.content->size()
                                     ? drop.content->size()
                                     : drop.content->size() + 1;
        return relative.substr(skip);
    }
    return std::nullopt;
}

void skip(ImportPlan& plan, std::string_view file, std::string reason) {
    plan.skipped.elements().push_back(withReason(fileRef(file), std::move(reason)));
}

std::string noImporter(std::string_view path) {
    const std::string_view extension = fs::extension(contentName(path));
    return extension.empty() ? std::string("no importer for files without an extension")
                             : "no importer for " + toLowerAscii(extension);
}

// Counts one more entry; false (and the error) past kMaxImportEntries.
Expected<void> countEntry(ImportPlan& plan) {
    if (++plan.entries > kMaxImportEntries) {
        return makeError("editor.content.too_many_files", "nothing imported: an import takes at most " +
                                                              std::to_string(kMaxImportEntries) + " files and folders");
    }
    return {};
}

// The content path of the folder `name` in `parent` for a dropped folder: the spelling this import
// chose for it already, else the folder there in another ASCII case (the files merge into it, as
// file managers do, so both systems behave the same), else `name` itself.
std::string mergedFolder(const AssetDatabase& assets, ImportPlan& plan, std::string_view parent,
                         std::string_view name) {
    const std::string key = toLowerAscii(contentChild(parent, name));
    if (const auto chosen = plan.folderSpelling.find(key); chosen != plan.folderSpelling.end()) {
        return chosen->second;
    }
    const Expected<std::optional<std::string>> taken = takenBy(assets, parent, name);
    std::string spelling = contentChild(parent, taken && taken->has_value() ? std::string_view(**taken) : name);
    plan.folderSpelling.emplace(key, spelling);
    return spelling;
}

// A file: imported in place when `inside` (its content path) is set, else copied to `target`.
void addSource(ImportPlan& plan, const std::string& file, std::optional<std::string> inside, std::string target) {
    if (inside.has_value()) {
        plan.sources.push_back(ImportSource{.file = file, .target = std::move(*inside), .inside = true});
        return;
    }
    if (!isValidContentPath(target)) {
        skip(plan, file, "'" + std::string(contentName(target)) + "' is not a valid name in Content");
        return;
    }
    plan.sources.push_back(ImportSource{.file = file, .target = std::move(target), .inside = false});
}

Expected<void> walkFolder(const AssetDatabase& assets, ImportPlan& plan, const DroppedFolder& drop,
                          const std::string& directory, const std::string& relative, const std::string& target);

// Adds the folder `directory` (absolute; `relative` below the dropped folder) as the content folder
// `target`: created, unless it is inside Content already (its files are then imported in place).
Expected<void> addFolder(const AssetDatabase& assets, ImportPlan& plan, const DroppedFolder& drop,
                         const std::string& directory, const std::string& relative, const std::string& target) {
    const std::optional<std::string> inside = contentPathIn(drop, relative);
    if (depthOf(inside.value_or(target)) > kMaxFolderDepth) {
        skip(plan, directory, "more than " + std::to_string(kMaxFolderDepth) + " folders deep");
        return {};
    }
    if (!inside.has_value()) {
        if (!isValidContentPath(target)) {
            skip(plan, directory, "'" + std::string(contentName(target)) + "' is not a valid folder name in Content");
            return {};
        }
        plan.folders.push_back(target);
    }
    return walkFolder(assets, plan, drop, directory, relative, target);
}

Expected<void> walkFolder(const AssetDatabase& assets, ImportPlan& plan, const DroppedFolder& drop,
                          const std::string& directory, const std::string& relative, const std::string& target) {
    Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(directory);
    if (!entries) {
        plan.failed.elements().push_back(withError(fileRef(directory), entries.error()));
        return {};
    }
    std::ranges::sort(*entries, {}, &fs::DirectoryEntry::name);
    for (const fs::DirectoryEntry& entry : *entries) {
        if (Expected<void> counted = countEntry(plan); !counted) {
            return counted;
        }
        const std::string path = fs::joinPath(directory, entry.name);
        const std::string below = contentChild(relative, entry.name);
        if (!entry.validName) {
            skip(plan, path, std::string(kUnusableName));
        } else if (entry.kind == fs::FileKind::Directory) {
            const std::string folder = mergedFolder(assets, plan, target, entry.name);
            if (Expected<void> added = addFolder(assets, plan, drop, path, below, folder); !added) {
                return added;
            }
        } else if (entry.kind == fs::FileKind::File) {
            addSource(plan, path, contentPathIn(drop, below), contentChild(target, entry.name));
        } else if (entry.kind == fs::FileKind::FileLink || entry.kind == fs::FileKind::DirectoryLink) {
            skip(plan, path, std::string(kLinkSkipped));
        } else {
            skip(plan, path, "special files are not imported");
        }
    }
    return {};
}

// The files of "files", dropped folders walked with their subfolders (ADR-0049 rule 14). A file or
// folder given here is taken even when it is a link: the user picked it. Only these are compared
// with Content as the operating system sees paths (fs::pathInside); their trees are worked out from
// that, by name.
Expected<ImportPlan> planImport(const EditorSession& session, const AssetDatabase& assets, const Value& files,
                                const std::string& folder) {
    const std::string& content = assets.contentDirectory();
    ImportPlan plan;
    for (const Value& value : files.elements()) {
        if (value.kind() != Value::Kind::String || value.asString().empty()) {
            return invalidArgs("ith.content.import", R"("files" must hold file paths)");
        }
        if (Expected<void> counted = countEntry(plan); !counted) {
            return makeUnexpected(std::move(counted).error());
        }
        if (fs::hasUnusableName(value.asString())) {
            skip(plan, value.asString(), std::string(kUnusableName));
            continue;
        }
        const std::string file = session.resolvePath(value.asString());
        const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(file);
        if (!info || !info->has_value()) {
            plan.failed.elements().push_back(withError(
                fileRef(file), info ? Error{.code = "fs.not_found", .message = "no file at " + file} : info.error()));
            continue;
        }
        const fs::FileKind kind = (*info)->kind;
        if (kind == fs::FileKind::Directory || kind == fs::FileKind::DirectoryLink) {
            DroppedFolder drop{.inside = detail::contentFileOf(content, file), .content = std::nullopt};
            if (!drop.inside.has_value()) {
                drop.content = fs::pathInside(content, file);
            }
            const std::string target = mergedFolder(assets, plan, folder, fs::fileName(file));
            if (Expected<void> added = addFolder(assets, plan, drop, file, {}, target); !added) {
                return makeUnexpected(std::move(added).error());
            }
        } else if (kind == fs::FileKind::File || kind == fs::FileKind::FileLink) {
            addSource(plan, file, detail::contentFileOf(content, file), contentChild(folder, fs::fileName(file)));
        } else {
            skip(plan, file, "special files are not imported");
        }
    }
    return plan;
}

// The answers to conflicts: "conflict" for every file, and "choices", the dialog's per-file answers
// ({"<file>": "replace" | "keep_both" | "skip"}), which come first.
struct ImportChoices {
    ConflictChoice fallback = ConflictChoice::None;
    std::unordered_map<std::string, ConflictChoice> perFile;
};

Expected<ImportChoices> parseChoices(const EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.import";
    constexpr std::string_view kChoices = R"("choices" must map files to "replace", "keep_both" or "skip")";
    ImportChoices choices;
    Expected<std::optional<std::string_view>> conflict = optionalString(kCommand, args, "conflict");
    if (!conflict) {
        return makeUnexpected(std::move(conflict).error());
    }
    if (conflict->has_value()) {
        const std::optional<ConflictChoice> parsed = parseChoice(**conflict);
        if (!parsed.has_value()) {
            return invalidArgs(kCommand, R"("conflict" must be "replace", "keep_both" or "skip")");
        }
        choices.fallback = *parsed;
    }
    const Value* given = args.kind() == Value::Kind::Object ? args.find("choices") : nullptr;
    if (given == nullptr || given->kind() == Value::Kind::Null) {
        return choices;
    }
    if (given->kind() != Value::Kind::Object) {
        return invalidArgs(kCommand, std::string(kChoices));
    }
    for (const Value::Member& member : given->members()) {
        const std::optional<ConflictChoice> parsed =
            member.value.kind() == Value::Kind::String ? parseChoice(member.value.asString()) : std::nullopt;
        if (!parsed.has_value()) {
            return invalidArgs(kCommand, std::string(kChoices));
        }
        choices.perFile.insert_or_assign(session.resolvePath(member.key), *parsed);
    }
    return choices;
}

// One file of the plan with what happens to it.
struct ImportStep {
    // Its place in ImportPlan::sources.
    std::size_t source = 0;
    // A conflict: the content path of the entry that takes the name, and whether Replace may be chosen.
    std::optional<std::string> taken;
    bool canReplace = false;
    ConflictChoice choice = ConflictChoice::None;
};

// A file inside Content: imported in place, not copied (spec 3.12 step 1), or skipped.
void planInPlace(const AssetDatabase& assets, ImportPlan& plan, std::size_t index, std::vector<ImportStep>& steps) {
    const ImportSource& source = plan.sources[index];
    const AssetRecord* record = assets.findByPath(source.target);
    if (record != nullptr && !record->imported) {
        skip(plan, source.file, "already an asset in Content");
    } else if (record != nullptr || assets.importers().findForPath(source.target) != nullptr) {
        steps.push_back(ImportStep{.source = index});
    } else if (isNativeAssetPath(source.target)) {
        skip(plan, source.file,
             "a native asset in Content that the database doesn't have: Rescan, or Fix if it is a duplicate");
    } else {
        skip(plan, source.file, noImporter(source.target));
    }
}

// What happens to each file of the plan, before anything is written (spec 3.12 steps 1 to 3): files
// in Content are imported in place, files no importer reads are skipped, and a taken name is a
// conflict with its answer. The conflicts without an answer are added to `unresolved`.
std::vector<ImportStep> planSteps(const AssetDatabase& assets, ImportPlan& plan, const ImportChoices& choices,
                                  Value& unresolved) {
    std::vector<ImportStep> steps;
    std::unordered_set<std::string> planned; // targets of this import, without ASCII case
    for (std::size_t index = 0; index < plan.sources.size(); ++index) {
        const ImportSource& source = plan.sources[index];
        if (source.inside) {
            planInPlace(assets, plan, index, steps);
            continue;
        }
        if (assets.importers().findForPath(source.target) == nullptr && !isNativeAssetPath(source.target)) {
            skip(plan, source.file, noImporter(source.target)); // before any copy (spec 3.12 step 2)
            continue;
        }
        const std::string parent = contentParent(source.target);
        Expected<std::optional<std::string>> taken = takenBy(assets, parent, contentName(source.target));
        if (!taken) {
            plan.failed.elements().push_back(withError(fileRef(source.file), taken.error()));
            continue;
        }
        ImportStep step{.source = index};
        const bool takenHere = !planned.insert(toLowerAscii(source.target)).second;
        if (taken->has_value() || takenHere) {
            step.taken = taken->has_value() ? contentChild(parent, **taken) : source.target;
            const AssetRecord* existing = taken->has_value() ? assets.findByPath(*step.taken) : nullptr;
            step.canReplace = !takenHere && existing != nullptr && existing->imported &&
                              equalsIgnoreCaseAscii(fs::extension(*step.taken), fs::extension(source.target));
            const auto chosen = choices.perFile.find(source.file);
            step.choice = chosen != choices.perFile.end() ? chosen->second : choices.fallback;
            if (step.choice == ConflictChoice::None) {
                Value conflict = fileRef(source.file);
                conflict.set("target", Value::string(*step.taken));
                conflict.set("replace", Value::boolean(step.canReplace));
                unresolved.elements().push_back(std::move(conflict));
            }
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

// What an import did: the imported assets as item references with their files, and their keys.
struct ImportResults {
    Value imported = Value::array();
    std::vector<ContentKey> keys;

    // Adds an imported asset; `flag`, when given, is set to `value` on its reference.
    void add(AssetId id, std::string_view path, std::string_view file, std::string_view flag = {}, bool value = false) {
        Value ref = assetRef(id, path);
        ref.set("file", Value::string(std::string(file)));
        if (!flag.empty()) {
            ref.set(std::string(flag), Value::boolean(value));
        }
        imported.elements().push_back(std::move(ref));
        keys.push_back(ContentKey{.id = id, .path = {}});
    }
};

// The import of a file copied into Content, or found there: the report, or the failure recorded,
// with the file left as a "Not imported" entry when `notImported` (spec 3.12).
std::optional<ImportReport> importCopy(AssetDatabase& assets, ContentBrowserModel& model, ImportPlan& plan,
                                       std::string_view file, const std::string& target, bool notImported) {
    Expected<ImportReport> report = assets.import(target);
    if (report) {
        return std::move(*report);
    }
    plan.failed.elements().push_back(withError(fileRef(file), report.error()));
    if (notImported) {
        const AssetProblem problem{.kind = AssetProblemKind::MissingSidecar,
                                   .path = target,
                                   .otherPath = {},
                                   .message = report.error().message};
        model.addProblems(std::span(&problem, 1));
    }
    return std::nullopt;
}

// After an import of `target` (a copy of `file`): the files its sub-assets refer to that are beside
// the original at the same relative path, below the copy's folder, are copied if missing and
// imported when an importer reads them (spec 3.12 step 5). A reference outside the copy's folder,
// or missing beside the original, is listed as skipped.
void copyReferences(AssetDatabase& assets, ContentBrowserModel& model, ImportPlan& plan, const std::string& file,
                    const std::string& target, const std::vector<std::string>& references, ImportResults& results) {
    const std::string folder = contentParent(target);
    for (const std::string& reference : references) {
        if (assets.findByPath(reference) != nullptr || !isValidContentPath(reference)) {
            continue; // imported already
        }
        if (!folder.empty() && !isAtOrBelow(reference, folder)) {
            skip(plan, reference, "referenced by '" + file + "' outside the folder it was copied to; copy it by hand");
            continue;
        }
        ITH_ASSERT(folder.empty() || reference.size() > folder.size());
        const std::string relative = folder.empty() ? reference : reference.substr(folder.size() + 1);
        const std::string original = fs::joinPath(fs::parentPath(file), relative);
        const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(original);
        if (!info || !info->has_value() || (*info)->kind == fs::FileKind::Directory) {
            skip(plan, original, "referenced by '" + file + "' but not beside it");
            continue;
        }
        const Expected<std::optional<std::string>> taken =
            takenBy(assets, contentParent(reference), contentName(reference));
        if (!taken || taken->has_value()) {
            continue; // there already, in some case
        }
        Expected<CopyInReport> copied = assets.copyIn(original, reference, false);
        if (!copied) {
            plan.failed.elements().push_back(withError(fileRef(original), copied.error()));
            continue;
        }
        if (!copied->id.isNil()) {
            results.add(copied->id, reference, original);
            continue;
        }
        if (assets.importers().findForPath(reference) == nullptr) {
            continue; // copied for the import that reads it
        }
        if (const std::optional<ImportReport> report = importCopy(assets, model, plan, original, reference, true)) {
            results.add(report->id, reference, original);
        }
    }
}

// Where a copy goes (spec 3.12 step 3): its target, or the answer's when the name is taken; nullopt
// when the file is skipped.
std::optional<std::string> copyTarget(const AssetDatabase& assets, const ImportStep& step, ImportPlan& plan) {
    const ImportSource& source = plan.sources[step.source];
    if (!step.taken.has_value()) {
        return source.target;
    }
    switch (step.choice) {
        case ConflictChoice::Skip:
        case ConflictChoice::None:
            skip(plan, source.file, "'" + *step.taken + "' is there already");
            return std::nullopt;
        case ConflictChoice::Replace:
            if (!step.canReplace) {
                skip(plan, source.file,
                     "'" + *step.taken + "' can't be replaced: only an imported file of the same type can be");
                return std::nullopt;
            }
            return *step.taken;
        case ConflictChoice::KeepBoth:
            break;
    }
    const std::string_view name = contentName(source.target);
    const std::string_view extension = fs::extension(name);
    Expected<std::string> free = numberedName(assets, contentParent(source.target),
                                              name.substr(0, name.size() - extension.size()), extension, false);
    if (!free) {
        plan.failed.elements().push_back(withError(fileRef(source.file), free.error()));
        return std::nullopt;
    }
    return contentChild(contentParent(source.target), *free);
}

// Runs one step (spec 3.12 steps 4 to 6): an import in place, or a copy and its import.
void runStep(AssetDatabase& assets, ContentBrowserModel& model, const ImportStep& step, ImportPlan& plan,
             ImportResults& results) {
    const ImportSource source = plan.sources[step.source]; // a copy: skip() may grow the plan's lists
    if (source.inside) {
        if (const std::optional<ImportReport> report =
                importCopy(assets, model, plan, source.file, source.target, false)) {
            model.removedPath(source.target); // no longer "Not imported"
            results.add(report->id, source.target, source.file, "fromCache", report->fromCache);
        }
        return;
    }
    const std::optional<std::string> target = copyTarget(assets, step, plan);
    if (!target.has_value()) {
        return;
    }
    const bool replace = step.taken.has_value() && step.choice == ConflictChoice::Replace;
    Expected<CopyInReport> copied = assets.copyIn(source.file, *target, replace);
    if (!copied) {
        plan.failed.elements().push_back(withError(fileRef(source.file), contentError(copied.error())));
        return;
    }
    if (!copied->id.isNil()) {
        // A native asset is in the database once copied (spec 3.12 step 6).
        results.add(copied->id, *target, source.file, "newId", copied->newId);
        return;
    }
    const std::optional<ImportReport> report = importCopy(assets, model, plan, source.file, *target, !replace);
    if (!report) {
        return;
    }
    results.add(report->id, *target, source.file, "fromCache", report->fromCache);
    if (replace) {
        results.imported.elements().back().set("replaced", Value::boolean(true));
    }
    copyReferences(assets, model, plan, source.file, *target, report->references, results);
}

void logImport(const Value& imported, const ImportPlan& plan) {
    for (const Value& ref : imported.elements()) {
        ITH_LOG_INFO(ContentBrowser, "Imported {} as {}", textOf(ref, "file"), textOf(ref, "path"));
    }
    for (const Value& ref : plan.skipped.elements()) {
        ITH_LOG_INFO(ContentBrowser, "Skipped {}: {}", textOf(ref, "file"), textOf(ref, "reason"));
    }
    for (const Value& ref : plan.failed.elements()) {
        const std::string_view file = textOf(ref, "file");
        ITH_LOG_WARNING(ContentBrowser, "Couldn't import {}: {}", file.empty() ? textOf(ref, "path") : file,
                        textOf(ref, "message"));
    }
}

Expected<Value> importFiles(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.import";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    const Value* files = args.kind() == Value::Kind::Object ? args.find("files") : nullptr;
    if (files == nullptr || files->kind() != Value::Kind::Array) {
        return invalidArgs(kCommand, R"("files" must be an array of file paths)");
    }
    Expected<std::string> folder = targetFolder(kCommand, **model, args, "folder", false);
    if (!folder) {
        return makeUnexpected(std::move(folder).error());
    }
    Expected<ImportChoices> choices = parseChoices(session, args);
    if (!choices) {
        return makeUnexpected(std::move(choices).error());
    }
    Expected<ImportPlan> plan = planImport(session, assets, *files, *folder);
    if (!plan) {
        return makeUnexpected(std::move(plan).error());
    }
    Value conflicts = Value::array();
    const std::vector<ImportStep> steps = planSteps(assets, *plan, *choices, conflicts);
    if (!conflicts.elements().empty()) {
        // The dialog asks per file (spec 3.12); nothing is copied until it is answered.
        Value result = Value::object();
        result.set("done", Value::boolean(false));
        result.set("files", *files);
        result.set("folder", Value::string(*folder));
        result.set("conflicts", std::move(conflicts));
        (*model)->setPrompt(ContentPrompt{.kind = ContentPromptKind::ImportConflicts, .item = {}, .details = result});
        return result;
    }
    for (const std::string& path : plan->folders) {
        const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(fullPath(assets, path));
        if (info && info->has_value() && (*info)->kind == fs::FileKind::Directory) {
            continue; // a folder of that name is there: the files go into it
        }
        if (Expected<void> made = assets.createFolder(path); !made) {
            plan->failed.elements().push_back(withError(pathRef(path), contentError(std::move(made).error())));
        }
    }
    ImportResults results;
    for (const ImportStep& step : steps) {
        runStep(assets, **model, step, *plan, results);
    }
    logImport(results.imported, *plan);
    (*model)->clearPrompt();
    (*model)->refresh();
    // The destination becomes the current folder, with the imported assets selected (spec 3.12).
    if ((*model)->folderExists(*folder)) {
        static_cast<void>((*model)->navigate(*folder));
    }
    (*model)->selectShown(results.keys);
    Value result = Value::object();
    result.set("imported", std::move(results.imported));
    result.set("skipped", std::move(plan->skipped));
    result.set("failed", std::move(plan->failed));
    return result;
}

// --- reimport and fix ---------------------------------------------------------------------------

constexpr std::string_view kOnlyImported = "only imported files can be reimported";

Expected<Value> reimportItems(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.reimport";
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
        return Value(); // nothing to reimport
    }
    const auto isImported = [&assets](const ContentItem& item) {
        const AssetRecord* record = item.kind == ContentItemKind::Asset ? assets.find(item.id) : nullptr;
        return record != nullptr && record->imported;
    };
    if (std::ranges::none_of(*items, isImported)) {
        return makeError("editor.content.not_imported", std::string(kOnlyImported));
    }
    Value reimported = Value::array();
    Value skipped = Value::array();
    Value failed = Value::array();
    for (const ContentItem& item : *items) {
        if (!isImported(item)) {
            skipped.elements().push_back(withReason(itemRef(item), std::string(kOnlyImported)));
            continue;
        }
        Expected<ImportReport> report = assets.reimport(item.id);
        if (!report) {
            ITH_LOG_WARNING(ContentBrowser, "Couldn't reimport {}: {}", item.path, report.error().message);
            failed.elements().push_back(withError(itemRef(item), report.error()));
            continue;
        }
        Value missing = Value::array();
        for (const SubAssetEntry& entry : report->missing) {
            missing.elements().push_back(Value::string(entry.key));
            ITH_LOG_WARNING(ContentBrowser, "{}: '{}' is no longer in the source file", item.path, entry.key);
        }
        ITH_LOG_INFO(ContentBrowser, "{} {}", report->fromCache ? "Unchanged" : "Reimported", item.path);
        Value ref = itemRef(item);
        ref.set("fromCache", Value::boolean(report->fromCache));
        ref.set("missing", std::move(missing));
        reimported.elements().push_back(std::move(ref));
    }
    (*model)->refresh();
    Value result = Value::object();
    result.set("reimported", std::move(reimported));
    result.set("skipped", std::move(skipped));
    result.set("failed", std::move(failed));
    return result;
}

Expected<Value> fixItem(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.content.fix";
    Expected<ContentBrowserModel*> model = fileBrowserOf(session);
    if (!model) {
        return makeUnexpected(std::move(model).error());
    }
    AssetDatabase& assets = *(*model)->assets();
    Expected<ContentItem> item = targetItem(kCommand, **model, args);
    if (!item) {
        return makeUnexpected(std::move(item).error());
    }
    const bool missingSidecar =
        item->kind == ContentItemKind::Problem && badgeOf(*item, AssetProblemKind::MissingSidecar) != nullptr;
    const bool duplicate =
        item->kind == ContentItemKind::Problem && badgeOf(*item, AssetProblemKind::DuplicateId) != nullptr;
    if (!missingSidecar && !duplicate) {
        return makeError("editor.content.invalid_target",
                         "'" + item->path +
                             "' has no fix: only a \"Not imported\" file or a duplicate id has one; "
                             "rename or delete it instead");
    }
    AssetId id;
    if (missingSidecar) {
        Expected<ImportReport> report = assets.import(item->path);
        if (!report) {
            return makeUnexpected(std::move(report).error());
        }
        id = report->id;
    } else {
        Expected<AssetId> assigned = assets.assignNewId(item->path);
        if (!assigned) {
            return makeUnexpected(std::move(assigned).error());
        }
        id = *assigned;
    }
    ITH_LOG_INFO(ContentBrowser, "{} {}", missingSidecar ? "Imported" : "Gave a new id to", item->path);
    (*model)->removedPath(item->path);
    (*model)->rekey(keyOf(*item), ContentKey{.id = id, .path = {}});
    (*model)->refresh();
    // The open level's file may be the duplicate that got the new id.
    if (holdsLevel(session.levelContentPath(), item->path)) {
        session.syncLevelWithAssets();
    }
    return assetRef(id, item->path);
}

} // namespace

namespace detail {

void registerContentImportCommands(CommandRegistry& registry) {
    detail::addContentAction(registry, "ith.content.import", "Import", "Copy files into Content and import them",
                             &importFiles);
    detail::addContentAction(registry, "ith.content.reimport", "Reimport", "Import the selected source files again",
                             &reimportItems);
    detail::addContentAction(registry, "ith.content.fix", "Fix",
                             "Import a file that has no sidecar, or give a duplicate a new id", &fixItem);
}

} // namespace detail
} // namespace ith
