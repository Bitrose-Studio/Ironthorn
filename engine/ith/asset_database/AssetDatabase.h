#pragma once

// The asset database of a project (ADR-0015 rules 5, 6 and 9).
//
// Assets live in the project's Content folder. Each one has an id that survives renames, moves
// and reimports: an engine-native asset (AssetHeader.h) holds it in its header, an imported source
// file in its sidecar `<file>.meta` (Sidecar.h). The database maps ids to paths, types and
// sub-assets. It is built by scan(), and the editor's operations (import, move, rename, remove,
// duplicate, copyIn, and createFolder, moveFolder and removeFolder) change files and database
// together:
//
//   ith::ImporterRegistry importers;
//   importers.add(std::make_unique<GltfImporter>());
//   ith::AssetDatabase database(projectDirectory, importers);
//   ith::Expected<std::vector<ith::AssetProblem>> problems = database.scan();
//   ith::Expected<ith::ImportReport> imported = database.import("Meshes/Ship.gltf");
//   ith::Expected<void> moved = database.move(imported->id, "Vehicles/Ship.gltf");
//
// Paths are relative to the Content folder, UTF-8, with '/' separators, and compared case
// sensitively, so a project behaves the same on Windows and Linux; two paths that differ only in
// ASCII case are reported as a problem. Letters outside ASCII are not case-folded: "É.png" and
// "é.png" are different paths here, but the same file on Windows and macOS. Problems found on disk
// are reported, never fatal.
//
// Symbolic links: a link to a file is read as that file (and a sidecar that is a link is replaced
// by a plain file when it is rewritten); links to folders are not followed; dangling links are
// left out.
//
// A file without a sidecar is reported by scan() and gets a sidecar, with a new id, only when it
// is imported: creating one during a scan would give a file whose sidecar was lost a new identity
// before the user could relink it.
//
// The database is not thread-safe: use it from one thread (the editor's main thread). It keeps no
// file open between calls. The importer registry must outlive it.

#include "ith/asset_database/DerivedDataCache.h"
#include "ith/asset_database/Importer.h"
#include "ith/asset_database/Sidecar.h"
#include "ith/assets/AssetId.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ith {

// The largest source file import() reads, in bytes.
inline constexpr std::size_t kMaxSourceFileSize = std::size_t{4} << 30;

// The most files one import reads besides its source (Importer::dependencies). Their sizes count
// towards kMaxSourceFileSize together with the source.
inline constexpr std::size_t kMaxImportDependencies = 256;

// The name of the project folder that holds the assets; project-relative asset paths start with it.
inline constexpr std::string_view kContentFolderName = "Content";

// How deep scan() goes below the Content folder: a folder at a deeper level is reported as an
// InvalidFile problem and neither entered nor listed in folders(), so everything that walks the
// folders is bounded.
inline constexpr std::size_t kMaxFolderDepth = 64;

// The longest file or folder name isValidContentPath accepts, in bytes. File systems allow 255
// (bytes on Linux, UTF-16 units on Windows, never more than the UTF-8 bytes); a name must leave
// room for its sidecar's suffix ".meta" and for the temporary name writeFileAtomic writes first,
// "<name>.meta.tmp-<uuid>", 46 bytes more, so every accepted name can be written.
inline constexpr std::size_t kMaxContentNameSize = 255 - 46;

// One asset in the database.
struct AssetRecord {
    AssetId id;
    // The path relative to the Content folder, e.g. "Meshes/Ship.gltf".
    std::string path;
    // A native asset's document format, or an imported file's importer id.
    std::string type;
    // True for an imported source file, which has a sidecar.
    bool imported = false;
    // The sub-asset table of an imported file, sorted by key; empty for a native asset.
    std::vector<SubAssetEntry> subAssets;

    friend bool operator==(const AssetRecord&, const AssetRecord&) = default;
};

enum class AssetProblemKind : std::uint8_t {
    // Two files have the same id, typically a file copied together with its sidecar; `otherPath`
    // keeps the id. When the database knew `otherPath` under that id, the copy (`path`) got a new
    // id, written to its file. Otherwise (a first scan) nothing is written and `path` is left out
    // of the database until assignNewId() gives it a new id.
    DuplicateId,
    // A sidecar (`path`) whose source file doesn't exist. The sidecar is left alone so that it can
    // be relinked.
    OrphanSidecar,
    // A file an importer reads (`path`) without a sidecar: not in the database until imported.
    MissingSidecar,
    // `path` and `otherPath` differ only in ASCII case: the project won't work on a case-sensitive
    // or a case-insensitive file system. Both are still in the database if they are assets.
    CaseConflict,
    // A sidecar or native asset that can't be read, a folder that can't be listed, a name that is
    // not valid Unicode, or a temporary file left by an interrupted write (`path`); `message` says
    // which. Not in the database.
    InvalidFile,
    // A native asset (`path`) whose extension doesn't fit its format: a level that is not an
    // ".ithmap" file, or an ".ithmap" file that is not a level (formatMatchesExtension). It is in
    // the database; rename it to fix it.
    FormatMismatch,
};

// A problem found by scan().
struct AssetProblem {
    AssetProblemKind kind = AssetProblemKind::InvalidFile;
    std::string path;
    std::string otherPath;
    std::string message;

    friend bool operator==(const AssetProblem&, const AssetProblem&) = default;
};

// One sub-asset produced by an import.
struct ImportedSubAsset {
    std::string key;
    SubAssetId id;
    std::vector<std::byte> payload;
};

// A file or folder an operation couldn't handle, with the reason.
struct AssetFailure {
    // The path relative to the Content folder.
    std::string path;
    // "asset.io" or "asset.too_large" with the file system's message, or the reading error.
    Error error;

    friend bool operator==(const AssetFailure&, const AssetFailure&) = default;
};

// The result of removeFolder().
struct RemoveFolderReport {
    // The files and folders that are still there, sorted by path: files that couldn't be deleted,
    // entries that couldn't be listed or named, and the folders that are not empty because of them
    // (each folder up to the removed one fails with "asset.io" then).
    std::vector<AssetFailure> failures;
    // An OrphanSidecar problem for each sidecar left behind by a deleted file, sorted by path.
    std::vector<AssetProblem> orphans;
};

// The result of copyIn().
struct CopyInReport {
    // A native asset's id in the database (a new one when `newId`); nil for any other file.
    AssetId id;
    // True when the native asset's id was taken in the database, so the copy got a new id.
    bool newId = false;
    // The files copied besides it (Importer::dependencies), as paths relative to the Content
    // folder, sorted.
    std::vector<std::string> dependencies;
};

// The users of one asset (findUsers).
struct AssetUsers {
    AssetId id;
    // The assets whose native document or sidecar refers to `id`, sorted by path.
    std::vector<AssetId> users;
};

// The result of findUsers().
struct FindUsersReport {
    // One entry for each id passed, in the same order.
    std::vector<AssetUsers> uses;
    // The native assets and sidecars that couldn't be read or parsed, so their references are
    // unknown, sorted by path.
    std::vector<AssetFailure> unreadable;
};

// The result of import().
struct ImportReport {
    AssetId id;
    // True when the file had no sidecar and got one, with a new id.
    bool created = false;
    // True when the importer's output came from the derived-data cache, so the importer didn't run.
    bool fromCache = false;
    // The objects of the source file, in source order, with their ids.
    std::vector<ImportedSubAsset> subAssets;
    // The sub-assets that are no longer in the source file, kept in the sidecar marked missing.
    std::vector<SubAssetEntry> missing;
    // The files the sub-assets refer to (ImportOutput::references), as paths relative to the Content
    // folder, sorted. They are recorded, not checked: a referenced file may not exist yet.
    std::vector<std::string> references;
    // The warnings of reading the sidecar, such as unknown keys that the rewrite dropped.
    std::vector<serialization::Error> warnings;
};

class AssetDatabase {
public:
    // A database for the project in `projectDirectory` (a UTF-8 path, see ith/file_system), whose
    // assets are in its "Content" folder and whose derived-data cache is
    // DerivedDataCache::projectDirectory(projectDirectory). It is empty until scan(). `importers`
    // must outlive it.
    AssetDatabase(std::string_view projectDirectory, const ImporterRegistry& importers);

    // The project's Content folder, normalized UTF-8.
    [[nodiscard]] const std::string& contentDirectory() const noexcept { return m_contentDirectory; }

    // The project's derived-data cache.
    [[nodiscard]] const DerivedDataCache& derivedDataCache() const noexcept { return m_cache; }

    // The importers it imports with: findForPath tells which files an import can read.
    [[nodiscard]] const ImporterRegistry& importers() const noexcept { return m_importers; }

    // Rebuilds the database from the Content folder and returns the problems found, sorted by
    // path. Of files that share an id, the one at the path the database knew for the id keeps it
    // and the others get new ids written to their files; when the database didn't know the id, the
    // first path in byte order keeps it in memory, nothing is written, and the others are left out
    // (DuplicateId). Otherwise sidecars and native assets are only read. Fails with "asset.io" when
    // the Content folder itself can't be listed; the database is then unchanged.
    [[nodiscard]] Expected<std::vector<AssetProblem>> scan();

    // The asset with this id, or null. Valid until the database changes.
    [[nodiscard]] const AssetRecord* find(AssetId id) const;

    // The asset at exactly this path, or null. Valid until the database changes.
    [[nodiscard]] const AssetRecord* findByPath(std::string_view path) const;

    // Every asset, sorted by path. Valid until the database changes.
    [[nodiscard]] std::vector<const AssetRecord*> records() const;

    // Every folder below the Content folder (not Content itself), as paths relative to it, sorted in
    // byte order: empty folders included, links to folders left out (they are not followed), and no
    // folder deeper than kMaxFolderDepth. Built by scan() and kept current by the operations:
    // createFolder() and moveFolder() add and rename folders, removeFolder() drops the folders it
    // deleted, move(), duplicate() and copyIn() add the folders they create, and every asset added
    // (import, assignNewId, move) adds the folders along its path. Folders made or deleted outside
    // the database otherwise appear or go at the next scan(). Empty at first.
    [[nodiscard]] const std::vector<std::string>& folders() const noexcept { return m_folders; }

    // Imports or reimports the source file at `path` (rule 9). A file without a sidecar gets a new
    // id, the importer for its extension and that importer's default settings; a file with one
    // is imported again with the importer and settings it names. When the sidecar's id belongs to
    // another path in the database, the file is a copy if that path still exists (it gets a new
    // id) and was moved outside the editor otherwise (it keeps the id). The files the importer
    // needs besides the source (Importer::dependencies: at most kMaxImportDependencies, inside the
    // source's folder, names compared case sensitively) are read too. The importer's output is
    // taken from the derived-data cache when every input, those files included, is unchanged.
    // Sub-assets are matched by source key: matches keep their ids, new objects get new ids and
    // objects that disappeared stay in the sidecar marked missing. The sidecar is rewritten only
    // when it changes. Fails, leaving the files and the database unchanged, with
    // "asset.invalid_path", "asset.not_found" (no such file, or a file the importer needs is
    // missing), "asset.path_conflict" (a sidecar exists in another case), "asset.no_importer",
    // "asset.too_large", "asset.sidecar_invalid" or the reading errors of readSidecar, the
    // importer's error, "import.invalid_output" or "asset.io".
    [[nodiscard]] Expected<ImportReport> import(std::string_view path);

    // import() of the imported asset `id`. Fails with "asset.not_found" for an unknown id and
    // "asset.not_imported" for a native asset, otherwise as import().
    [[nodiscard]] Expected<ImportReport> reimport(AssetId id);

    // Gives the asset file at `path` (a native asset, or a file with a sidecar) a new random id,
    // writes it to its header or sidecar and adds the file to the database: how the user resolves
    // a DuplicateId that scan() couldn't. Fails with "asset.invalid_path" (also when a folder on
    // the way is a link), "asset.not_found" (no such file, or neither a native asset nor a file
    // with a sidecar), the reading errors of readSidecar or readAssetHeader, "asset.too_large" or
    // "asset.io".
    [[nodiscard]] Expected<AssetId> assignNewId(std::string_view path);

    // Moves the asset `id` to `newPath`, with its sidecar, and keeps its id. Missing folders are
    // created. Fails, with nothing changed, with "asset.not_found", "asset.invalid_path" (see
    // isValidContentPath, or a folder of either path that is a link), "asset.extension_changed"
    // (the extension is part of the asset's type), "asset.path_conflict" (the name at `newPath` is
    // taken, see below) or "asset.io". When the file moved but neither its sidecar could follow
    // nor the file be moved back, it fails with "asset.io" naming both places, and the asset's
    // record is at the file's new path. Moving to the same path does nothing.
    [[nodiscard]] Expected<void> move(AssetId id, std::string_view newPath);

    // Renames the file of the asset `id` to `newName` in the same folder: move() to that path.
    // Fails as move(), and with "asset.invalid_path" when `newName` contains '/'.
    [[nodiscard]] Expected<void> rename(AssetId id, std::string_view newName);

    // Deletes the asset `id`: its file and its sidecar. Fails with "asset.not_found",
    // "asset.invalid_path" (a folder on the way became a link to a folder; nothing changed), or with
    // "asset.io" when the file can't be deleted (nothing changed) or the sidecar can't be deleted
    // (the asset is gone, the sidecar is left as an orphan).
    [[nodiscard]] Expected<void> remove(AssetId id);

    // The operations below share these rules:
    // - Names: a path is free when no entry of any kind (a hidden file or a sidecar included) has
    //   its last name without ASCII case (findCaseVariant), no parent folder exists in another
    //   case, and the file system doesn't take the last name for an existing entry
    //   (another case outside ASCII on Windows, macOS or a case-folding Linux folder, or a Windows
    //   short name); otherwise "asset.path_conflict". On Windows a case-only rename outside ASCII
    //   ("été" to "Été") is refused that way.
    // - Links: a path whose parent folder is a link to a folder fails with "asset.invalid_path":
    //   links lead out of Content and are never followed. On Windows a volume mounted in a folder
    //   is a reparse point, listed as a link, and refused the same way. A Linux bind or network
    //   mount is no link: it looks like a folder and is handled as one (removeFolder deletes what
    //   is in it).
    // - Names in a listing that can't address their entry (not valid Unicode, or holding '\',
    //   which the file system takes for a separator) are never followed: scan() and removeFolder()
    //   report them.
    // - Checks come before writes but are not atomic with them: an entry created by another program
    //   between the check and an atomic write is replaced.
    // - Copies read whole files into memory on the calling thread (the editor's main thread), so a
    //   copy of a large file blocks it for as long as the read and the durable write take.

    // Creates the folder `path` (relative to Content), with its missing parents, and adds them to
    // folders(). Fails, with nothing changed, with "asset.invalid_path" (see isValidContentPath,
    // more than kMaxFolderDepth folders deep, or a parent that is a link), "asset.path_conflict"
    // (the name is taken, the folder itself included) or "asset.io" (also when a parent is a file).
    [[nodiscard]] Expected<void> createFolder(std::string_view path);

    // Moves the folder `from` to `to` (both relative to Content) with one rename on disk, so
    // everything inside goes along, files the database doesn't know and sidecars included. Every
    // asset below it keeps its id and its record changes path, and folders() follows. Missing
    // parents of `to` are created. A case-only rename of the folder itself (ASCII letters) is
    // allowed; moving to the same path does nothing. The arguments may be elements of folders().
    // Fails, with nothing changed (except created parents), with "asset.invalid_path" (an invalid
    // path, which includes the Content folder itself, a result more than kMaxFolderDepth folders
    // deep, or a link on the way), "asset.not_found" (no folder at `from` with exactly that case;
    // a link to a folder is not one), "asset.invalid_target" (`to` is inside `from`, compared
    // without ASCII case), "asset.path_conflict" (the name of `to` is taken) or "asset.io".
    [[nodiscard]] Expected<void> moveFolder(std::string_view from, std::string_view to);

    // Deletes the folder `path` (relative to Content) with everything in it: every file (links
    // are removed, not followed), then the folders from the deepest up. Sidecars go after every
    // other file, and a sidecar `X.meta` only when its file `X` is gone: the file of exactly that
    // name, or, when there is none, one with that name in another ASCII case. A failure doesn't
    // stop the others: it is listed in the report, and what failed stays on disk
    // and in the database. The records of deleted assets are erased (also of assets whose file was
    // already gone), deleted folders leave folders(), and a sidecar left behind by its deleted file
    // is reported as an orphan. Entries that can't be removed by name (not valid Unicode) or folders
    // deeper than kMaxFolderDepth are left, so their folders fail as not empty. On Windows a
    // read-only file (as some version control systems leave them) can't be deleted and is a
    // failure; on POSIX it is deleted. `path` may be an element of folders(). Fails, with nothing
    // changed, with "asset.invalid_path" (also a link on the way), "asset.not_found" (no folder at
    // `path` with exactly that case; a link to a folder is not one) or "asset.io" (the folder itself
    // can't be listed).
    [[nodiscard]] Expected<RemoveFolderReport> removeFolder(std::string_view path);

    // Copies the asset `id` to `newPath` with a new random id, and adds the copy to the database:
    // a native asset with the id replaced in its header, an imported file with its sidecar, whose
    // id is replaced. Everything else is copied unchanged: the source bytes exactly, the documents
    // as replaceHeaderId writes them. Files the import reads beside the source are not copied.
    // Missing folders are created; both files are written durably. Fails, with nothing changed
    // (except folders it created, which stay in folders()), with "asset.not_found" (no such asset,
    // or its file is gone), "asset.invalid_path", "asset.extension_changed", "asset.path_conflict"
    // (the name is taken, the asset's own name included), "asset.too_large", the reading errors of
    // readAssetHeader or readSidecar, or "asset.io". When the sidecar can't be written after the
    // source file was, the copied file is deleted again; if even that fails, the error says so and
    // the copy is left as a file without a sidecar.
    [[nodiscard]] Expected<AssetId> duplicate(AssetId id, std::string_view newPath);

    // Copies the file at `externalPath` (a UTF-8 path, see ith/file_system; normally outside the
    // project) to `targetPath` (relative to Content). It doesn't import: the caller calls import()
    // for a source file.
    //
    // - When an importer reads the target's extension, the files its dependencies() name are
    //   copied too, from beside the external file to the same paths relative to the target, each
    //   only when it exists there and its name is free at the target. Beside the external file the
    //   name is looked up by the file system: without case on Windows, exactly on Linux. Names that
    //   are not valid content paths or are native assets are skipped. When dependencies() fails (a
    //   malformed source), only the file is copied; the import reports the error.
    // - A native asset (`.ithasset`, `.ithmap`) is added to the database. Its format must fit the
    //   target's extension (formatMatchesExtension). It keeps its id unless the database has that
    //   id, in which case the copy gets a new one.
    // - With `replace`, the target must be an imported asset in the database; its source is
    //   overwritten atomically (its sidecar is kept, and dependencies() is asked with its
    //   importer and settings), and the caller reimports it. Without `replace`, the target's name
    //   must be free.
    //
    // Everything is read before anything is written: the file with its dependencies up to
    // kMaxSourceFileSize together (a native asset up to serialization::kMaxTextSize), at most
    // kMaxImportDependencies of them. The dependencies are written first and deleted again when
    // the file can't be written, so a failure leaves the Content folder as it was, except folders
    // it created (which stay in folders()). Missing folders are created. Fails with
    // "asset.invalid_path", "asset.extension_changed" (the two extensions differ without ASCII
    // case), "asset.format_mismatch" (a native document whose format doesn't fit the target's
    // extension), "asset.path_conflict", "asset.not_found" (no file at `externalPath`, or with
    // `replace` no asset at `targetPath`), "asset.not_imported" (`replace` on a native asset),
    // "asset.no_importer" (`replace` whose sidecar names an importer that isn't registered),
    // "asset.too_large", the reading errors of readAssetHeader or readSidecar, or "asset.io".
    [[nodiscard]] Expected<CopyInReport> copyIn(std::string_view externalPath, std::string_view targetPath,
                                                bool replace);

    // For each id in `ids`, the assets in the database outside `ids` that refer to it: each native
    // asset's document and each imported file's sidecar (whose overrides may hold references) is
    // read and parsed, and collectAssetReferences finds its references; a reference to a sub-asset
    // counts as one to its asset. The source files themselves are not read: an imported file that
    // names others by path is no user. Files on disk that are not in the database (orphan sidecars,
    // unresolved duplicates) are not read. A file that can't be read or parsed is listed as
    // unreadable and the search goes on. Costs one read and parse of every native asset and sidecar
    // (O(total size of those files)), one at a time, plus O(log n) per reference found; meant for a
    // delete preview, not for every frame.
    [[nodiscard]] FindUsersReport findUsers(std::span<const AssetId> ids) const;

private:
    // A file to write: its path relative to Content and its bytes.
    struct PendingWrite {
        std::string path;
        std::string bytes;
    };

    void insert(AssetRecord record);
    void erase(AssetId id);
    // Adds the folders of the file `path` (relative to Content) that folders() doesn't list yet.
    void addFolders(std::string_view path);
    // Adds the folder `folder` (relative to Content) and its parents that folders() doesn't list yet.
    void addFolder(std::string folder);
    // Creates the folders of the file `path` (relative to Content) and adds them to folders().
    [[nodiscard]] Expected<void> createFoldersOf(std::string_view path);
    // Writes `files` (relative to Content) durably, in order, creating their folders. When one
    // fails, the ones written before it are deleted again and the error is returned, naming any
    // that couldn't be.
    [[nodiscard]] Expected<void> writeAll(const std::vector<PendingWrite>& files);
    // removeFolder()'s deletion of `files` (relative to Content): every file, then the sidecars
    // whose file is gone, adding what fails to `report`.
    void removeFiles(const std::vector<std::string>& files, RemoveFolderReport& report);

    std::string m_contentDirectory;
    DerivedDataCache m_cache;
    const ImporterRegistry& m_importers;
    std::unordered_map<AssetId, AssetRecord> m_records;
    std::map<std::string, AssetId, std::less<>> m_byPath;
    std::vector<std::string> m_folders;
};

// True when `path` can name an asset on every platform: valid UTF-8, relative, '/'-separated,
// without empty, "." or ".." segments, segments of at most kMaxContentNameSize bytes, without the
// characters < > : " \ | ? * or control characters, without a segment ending in '.' or ' ', without
// a segment whose part before the first '.' is a Windows device name (CON, PRN, AUX, NUL, CONIN$,
// CONOUT$, COM1 to COM9, LPT1 to LPT9, and COM and LPT with the superscript digits ¹, ² and ³, in
// any ASCII case), and not ending in the sidecar suffix ".meta" in any case.
[[nodiscard]] bool isValidContentPath(std::string_view path) noexcept;

// The pairs of paths in `paths` (sorted, unique) that differ only in ASCII case, each pair once,
// in the order of `paths`.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> findCaseConflicts(const std::vector<std::string>& paths);

// The name of the entry in the folder `directory` (a UTF-8 path, see ith/file_system) that equals
// `name` without ASCII case: `name` itself when it exists, otherwise a variant in another case;
// otherwise `name` itself when the file system still takes it for an existing entry (another case
// outside ASCII on Windows, macOS or a case-folding Linux folder, or a Windows short name);
// otherwise nullopt (also when `directory` doesn't exist or is not a folder). Entries of any kind
// count, hidden files and sidecars included; names that are not valid Unicode are skipped. This is
// the name rule of the database's operations and of the Content Browser: a name is taken when
// this finds anything. Fails with "asset.io" when the folder can't be listed.
[[nodiscard]] Expected<std::optional<std::string>> findCaseVariant(std::string_view directory, std::string_view name);

// The asset ids referenced in `value`: from every object, at any depth, with a member "asset"
// whose value is a string holding a non-nil UUID (Uuid::parse), the shape AssetRef and
// SoftAssetRef are written in ({"asset": "<uuid>", "subAsset": "<uuid>"}). Every member named
// "asset" counts, duplicate keys in one object included; other members are not looked at for
// this. The result is sorted and unique. Walks the tree without recursion, so any depth is safe.
[[nodiscard]] std::vector<AssetId> collectAssetReferences(const serialization::Value& value);

} // namespace ith
