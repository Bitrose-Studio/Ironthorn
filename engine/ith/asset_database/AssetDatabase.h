#pragma once

// The asset database of a project (ADR-0015 rules 5, 6 and 9).
//
// Assets live in the project's Content folder. Each one has an id that survives renames, moves
// and reimports: an engine-native asset (AssetHeader.h) holds it in its header, an imported source
// file in its sidecar `<file>.meta` (Sidecar.h). The database maps ids to paths, types and
// sub-assets. It is built by scan(), and the editor's operations (import, move, rename, remove)
// change files and database together:
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
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ith {

// The largest source file import() reads, in bytes.
inline constexpr std::size_t kMaxSourceFileSize = std::size_t{4} << 30;

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
    // The warnings of reading the sidecar, such as unknown keys that the rewrite dropped.
    std::vector<serialization::Error> warnings;
};

class AssetDatabase {
public:
    // A database for the project in `projectDirectory`, whose assets are in its "Content" folder
    // and whose derived-data cache is DerivedDataCache::projectDirectory(projectDirectory). It is
    // empty until scan(). `importers` must outlive it.
    AssetDatabase(const std::filesystem::path& projectDirectory, const ImporterRegistry& importers);

    // The project's Content folder.
    [[nodiscard]] const std::filesystem::path& contentDirectory() const noexcept { return m_contentDirectory; }

    // The project's derived-data cache.
    [[nodiscard]] const DerivedDataCache& derivedDataCache() const noexcept { return m_cache; }

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

    // Imports or reimports the source file at `path` (rule 9). A file without a sidecar gets a new
    // id, the importer for its extension and that importer's default settings; a file with one
    // is imported again with the importer and settings it names. When the sidecar's id belongs to
    // another path in the database, the file is a copy if that path still exists (it gets a new
    // id) and was moved outside the editor otherwise (it keeps the id). The importer's output is
    // taken from the derived-data cache when every input is unchanged. Sub-assets are matched by
    // source key: matches keep their ids, new objects get new ids and objects that disappeared
    // stay in the sidecar marked missing. The sidecar is rewritten only when it changes. Fails,
    // leaving the files and the database unchanged, with "asset.invalid_path", "asset.not_found"
    // (no such file), "asset.path_conflict" (a sidecar exists in another case), "asset.no_importer",
    // "asset.too_large", "asset.sidecar_invalid" or the reading errors of readSidecar, the
    // importer's error, "import.invalid_output" or "asset.io".
    [[nodiscard]] Expected<ImportReport> import(std::string_view path);

    // import() of the imported asset `id`. Fails with "asset.not_found" for an unknown id and
    // "asset.not_imported" for a native asset, otherwise as import().
    [[nodiscard]] Expected<ImportReport> reimport(AssetId id);

    // Gives the asset file at `path` (a native asset, or a file with a sidecar) a new random id,
    // writes it to its header or sidecar and adds the file to the database: how the user resolves
    // a DuplicateId that scan() couldn't. Fails with "asset.invalid_path", "asset.not_found" (no
    // such file, or neither a native asset nor a file with a sidecar), the reading errors of
    // readSidecar or readAssetHeader, "asset.too_large" or "asset.io".
    [[nodiscard]] Expected<AssetId> assignNewId(std::string_view path);

    // Moves the asset `id` to `newPath`, with its sidecar, and keeps its id. Missing folders are
    // created. Fails, with nothing changed, with "asset.not_found", "asset.invalid_path" (see
    // isValidContentPath), "asset.extension_changed" (the extension is part of the asset's type),
    // "asset.path_conflict" (another file or asset exists at `newPath`, or at a path differing only
    // in case) or "asset.io". When the file moved but neither its sidecar could follow nor the file
    // be moved back, it fails with "asset.io" naming both places, and the asset's record is at the
    // file's new path. Moving to the same path does nothing.
    [[nodiscard]] Expected<void> move(AssetId id, std::string_view newPath);

    // Renames the file of the asset `id` to `newName` in the same folder: move() to that path.
    // Fails as move(), and with "asset.invalid_path" when `newName` contains '/'.
    [[nodiscard]] Expected<void> rename(AssetId id, std::string_view newName);

    // Deletes the asset `id`: its file and its sidecar. Fails with "asset.not_found", or with
    // "asset.io" when the file can't be deleted (nothing changed) or the sidecar can't be deleted
    // (the asset is gone, the sidecar is left as an orphan).
    [[nodiscard]] Expected<void> remove(AssetId id);

private:
    void insert(AssetRecord record);
    void erase(AssetId id);
    [[nodiscard]] Expected<void> checkTarget(const AssetRecord& record, const std::string& newPath) const;

    std::filesystem::path m_contentDirectory;
    DerivedDataCache m_cache;
    const ImporterRegistry& m_importers;
    std::unordered_map<AssetId, AssetRecord> m_records;
    std::map<std::string, AssetId, std::less<>> m_byPath;
};

// True when `path` can name an asset on every platform: valid UTF-8, relative, '/'-separated,
// without empty, "." or ".." segments, segments of at most 255 bytes, without the characters
// < > : " \ | ? * or control characters, without a segment ending in '.' or ' ', without a segment
// whose part before the first '.' is a Windows device name (CON, PRN, AUX, NUL, COM1 to COM9,
// LPT1 to LPT9, in any case), and not ending in the sidecar suffix ".meta" in any case.
[[nodiscard]] bool isValidContentPath(std::string_view path) noexcept;

// The pairs of paths in `paths` (sorted, unique) that differ only in ASCII case, each pair once,
// in the order of `paths`.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> findCaseConflicts(const std::vector<std::string>& paths);

} // namespace ith
