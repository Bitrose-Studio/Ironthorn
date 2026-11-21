#include "ith/asset_database/AssetDatabase.h"

#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/FileIo.h"
#include "ith/asset_database/ImportedAssetData.h"
#include "ith/foundation/StringUtils.h"
#include "ith/foundation/Uuid.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/Document.h"

#include <algorithm>
#include <array>
#include <optional>
#include <system_error>
#include <unordered_set>

namespace ith {
namespace {

using detail::asset_files::fromUtf8;
using detail::asset_files::ioError;
using detail::asset_files::readFile;
using detail::asset_files::toUtf8Checked;
using detail::asset_files::writeFileAtomically;

// Importer output doesn't depend on the target platform; cooking passes the real one.
constexpr std::string_view kImportPlatform = "any";

// The longest file or folder name most file systems accept, in bytes.
constexpr std::size_t kMaxSegmentSize = 255;

// The extension of the file name in `path`, lowercased, e.g. ".png"; empty when there is none.
std::string lowercaseExtension(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    return toLowerAscii(name.substr(dot));
}

std::string sidecarOf(std::string_view path) {
    return std::string(path) + std::string(kSidecarSuffix);
}

// The file name in `path`.
std::string_view fileName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// True for "<name>.tmp-<uuid>", a temporary file writeFileAtomically() left behind.
bool isTemporaryFile(std::string_view path) {
    constexpr std::size_t kUuidLength = 36;
    const std::string_view name = fileName(path);
    const std::size_t marker = detail::asset_files::kTemporaryMarker.size();
    return name.size() > marker + kUuidLength &&
           name.substr(name.size() - kUuidLength - marker, marker) == detail::asset_files::kTemporaryMarker &&
           Uuid::parse(name.substr(name.size() - kUuidLength)).hasValue();
}

// True when `segment`'s part before its first '.' is a device name Windows reserves in every folder.
bool isReservedDeviceName(std::string_view segment) {
    const std::string stem = toLowerAscii(segment.substr(0, segment.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9';
}

bool isValidSegment(std::string_view segment) {
    if (segment.empty() || segment.size() > kMaxSegmentSize || segment == "." || segment == ".." ||
        segment.back() == '.' || segment.back() == ' ' || isReservedDeviceName(segment)) {
        return false;
    }
    return std::ranges::none_of(segment, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7f || std::string_view(R"(<>:"\|?*)").find(character) != std::string_view::npos;
    });
}

// The name in `directory` that equals `name` ignoring ASCII case: `name` itself when it exists,
// otherwise a variant, otherwise nullopt (also when `directory` doesn't exist). Names that are not
// valid Unicode are skipped; scan() reports them.
Expected<std::optional<std::string>> findCaseVariant(const std::filesystem::path& directory, std::string_view name) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return std::optional<std::string>();
    }
    std::optional<std::string> variant;
    for (std::filesystem::directory_iterator entry(directory, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        std::optional<std::string> entryName = toUtf8Checked(entry->path().filename());
        if (!entryName) {
            continue;
        }
        if (*entryName == name) {
            return entryName;
        }
        if (!variant && equalsIgnoreCaseAscii(*entryName, name)) {
            variant = std::move(entryName);
        }
    }
    if (error) {
        return ioError("can't list", directory, error);
    }
    return variant;
}

// True when every segment of `path` exists in the Content folder with exactly this case, which a
// case-insensitive file system doesn't check by itself.
Expected<bool> existsExactly(const std::filesystem::path& content, std::string_view path) {
    std::filesystem::path directory = content;
    for (const std::string_view segment : split(path, '/')) {
        Expected<std::optional<std::string>> variant = findCaseVariant(directory, segment);
        if (!variant) {
            return makeUnexpected(std::move(variant).error());
        }
        if (!variant->has_value() || **variant != segment) {
            return false;
        }
        directory /= fromUtf8(segment);
    }
    return true;
}

Unexpected<Error> notFound(AssetId id) {
    return makeError("asset.not_found", "no asset has the id " + id.toString());
}

Unexpected<Error> invalidPath(std::string_view path) {
    return makeError("asset.invalid_path", "'" + std::string(path) + "' is not a valid asset path");
}

Unexpected<Error> fromSerialization(const serialization::Error& error) {
    return makeError(error.code, error.toString());
}

AssetProblem invalidFile(const std::string& path, std::string message) {
    return AssetProblem{.kind = AssetProblemKind::InvalidFile, .path = path, .message = std::move(message)};
}

// One asset found on disk, with what it takes to give it a new id.
struct Candidate {
    AssetRecord record;
    std::optional<Sidecar> sidecar; // imported assets
    std::string nativeText;         // native assets
};

// Every file and folder below Content, relative and sorted in byte order, and the problems of
// listing it.
struct ContentListing {
    std::vector<std::string> paths; // files and folders
    std::vector<std::string> files;
    std::vector<AssetProblem> problems;
};

// Adds the folder `directory` (`relative` to Content, empty for Content itself) to `listing`,
// with its sub-folders. A sub-folder that can't be listed and a name that isn't valid Unicode are
// problems; only Content itself failing to list is an error. Symbolic links to files count as
// files; links to folders are not followed and dangling links are skipped.
Expected<void> listFolder(const std::filesystem::path& directory, const std::string& relative,
                          ContentListing& listing) {
    std::error_code error;
    std::filesystem::directory_iterator entry(directory, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const std::optional<std::string> name = toUtf8Checked(entry->path().filename());
        if (!name) {
            listing.problems.push_back(invalidFile(relative, "a name in this folder is not valid Unicode"));
            continue;
        }
        const std::string path = relative.empty() ? *name : relative + '/' + *name;
        std::error_code typeError;
        const std::filesystem::file_status status = entry->symlink_status(typeError);
        const bool isLink = std::filesystem::is_symlink(status);
        const bool isFile = isLink ? entry->is_regular_file(typeError) : std::filesystem::is_regular_file(status);
        const bool isFolder = !isLink && std::filesystem::is_directory(status);
        listing.paths.push_back(path);
        if (isFile) {
            listing.files.push_back(path);
        }
        if (isFolder) {
            if (Expected<void> listed = listFolder(entry->path(), path, listing); !listed) {
                listing.problems.push_back(invalidFile(path, listed.error().message));
            }
        }
    }
    if (error) {
        return ioError("can't list", directory, error);
    }
    return {};
}

Expected<ContentListing> listContent(const std::filesystem::path& content) {
    ContentListing listing;
    if (Expected<void> listed = listFolder(content, {}, listing); !listed) {
        return makeUnexpected(std::move(listed).error());
    }
    std::ranges::sort(listing.paths);
    std::ranges::sort(listing.files);
    return listing;
}

// The imported asset at `path`, read from its sidecar, or the error that keeps it out.
Expected<Candidate> readImported(const std::filesystem::path& content, const std::string& path) {
    const std::string sidecarPath = sidecarOf(path);
    Expected<std::string> text = readFile(content / fromUtf8(sidecarPath), kMaxSidecarSize);
    if (!text) {
        return makeError(text.error().code, "'" + sidecarPath + "': " + text.error().message);
    }
    Expected<Sidecar, serialization::Error> sidecar = readSidecar(*text, sidecarPath);
    if (!sidecar) {
        return fromSerialization(sidecar.error());
    }
    Candidate candidate{.record = {.id = sidecar->id,
                                   .path = path,
                                   .type = sidecar->importer,
                                   .imported = true,
                                   .subAssets = sidecar->subAssets}};
    candidate.sidecar = std::move(*sidecar);
    return candidate;
}

// The native asset at `path`, or the error that keeps it out.
Expected<Candidate> readNative(const std::filesystem::path& content, const std::string& path) {
    Expected<std::string> text = readFile(content / fromUtf8(path), serialization::kMaxTextSize);
    if (!text) {
        return makeError(text.error().code, "'" + path + "': " + text.error().message);
    }
    Expected<AssetHeader, serialization::Error> header = readAssetHeader(*text, path);
    if (!header) {
        return fromSerialization(header.error());
    }
    return Candidate{.record = {.id = header->id, .path = path, .type = header->format},
                     .nativeText = std::move(*text)};
}

// Gives `candidate` the id `newId` and writes it to its sidecar or header.
Expected<void> writeNewId(const std::filesystem::path& content, Candidate& candidate, AssetId newId) {
    std::string target = candidate.record.path;
    Expected<std::string, serialization::Error> rewritten = [&] {
        if (candidate.sidecar) {
            target = sidecarOf(target);
            Sidecar copy = *candidate.sidecar;
            copy.id = newId;
            return writeSidecar(copy);
        }
        return replaceHeaderId(candidate.nativeText, newId, target);
    }();
    if (!rewritten) {
        return fromSerialization(rewritten.error());
    }
    if (Expected<void> written = writeFileAtomically(content / fromUtf8(target), *rewritten); !written) {
        return written;
    }
    if (candidate.sidecar) {
        candidate.sidecar->id = newId;
    }
    candidate.record.id = newId;
    return {};
}

// A sidecar's problems: an orphan, or one whose name differs from a file's only in case.
void checkSidecar(const std::string& path, const std::unordered_set<std::string_view>& fileSet,
                  const std::unordered_map<std::string, std::string_view>& fileByLowercase,
                  std::vector<AssetProblem>& problems) {
    const std::string_view base = std::string_view(path).substr(0, path.size() - kSidecarSuffix.size());
    if (path.ends_with(kSidecarSuffix) && fileSet.contains(base)) {
        return;
    }
    problems.push_back(AssetProblem{
        .kind = AssetProblemKind::OrphanSidecar, .path = path, .message = "the sidecar's file doesn't exist"});
    if (const auto variant = fileByLowercase.find(toLowerAscii(base)); variant != fileByLowercase.end()) {
        problems.push_back(AssetProblem{.kind = AssetProblemKind::CaseConflict,
                                        .path = path,
                                        .otherPath = std::string(variant->second),
                                        .message = "the sidecar's name differs from its file's only in case"});
    }
}

// The assets among `files` (sorted), adding the problems of the others to `problems`.
std::vector<Candidate> findCandidates(const std::filesystem::path& content, const ImporterRegistry& importers,
                                      const std::vector<std::string>& files, std::vector<AssetProblem>& problems) {
    const std::unordered_set<std::string_view> fileSet(files.begin(), files.end());
    std::unordered_map<std::string, std::string_view> fileByLowercase;
    for (const std::string& file : files) {
        fileByLowercase.emplace(toLowerAscii(file), file);
    }
    std::vector<Candidate> candidates;
    for (const std::string& path : files) {
        if (isTemporaryFile(path)) {
            problems.push_back(invalidFile(path, "a temporary file left by an interrupted write; delete it"));
            continue;
        }
        if (endsWithIgnoreCaseAscii(path, kSidecarSuffix)) {
            checkSidecar(path, fileSet, fileByLowercase, problems);
            continue;
        }
        const bool imported = fileSet.contains(sidecarOf(path));
        if (!imported && !isNativeAssetPath(path)) {
            if (importers.findForPath(path) != nullptr) {
                problems.push_back(AssetProblem{.kind = AssetProblemKind::MissingSidecar,
                                                .path = path,
                                                .message = "the file has no sidecar; import it to give it an id"});
            }
            continue;
        }
        Expected<Candidate> candidate = imported ? readImported(content, path) : readNative(content, path);
        if (!candidate) {
            problems.push_back(invalidFile(imported ? sidecarOf(path) : path, candidate.error().message));
            continue;
        }
        if (!candidate->sidecar && !formatMatchesExtension(path, candidate->record.type)) {
            problems.push_back(AssetProblem{.kind = AssetProblemKind::FormatMismatch,
                                            .path = path,
                                            .message = "a '" + candidate->record.type +
                                                       "' document doesn't belong in a '" + lowercaseExtension(path) +
                                                       "' file"});
        }
        candidates.push_back(std::move(*candidate));
    }
    return candidates;
}

// Whether the source file at `path` (which must exist exactly) has its sidecar. Fails with
// "asset.not_found" for a missing file and "asset.path_conflict" for a sidecar in another case:
// writing ours would replace it on a case-insensitive file system (losing its id) and add a second
// one on a case-sensitive file system.
Expected<bool> findSidecarForImport(const std::filesystem::path& content, std::string_view path) {
    Expected<bool> exists = existsExactly(content, path);
    if (!exists || !*exists) {
        return exists ? makeError("asset.not_found", "no file '" + std::string(path) + "' in Content")
                      : makeUnexpected(std::move(exists).error());
    }
    const std::string sidecarPath = sidecarOf(path);
    Expected<bool> hasSidecar = existsExactly(content, sidecarPath);
    if (!hasSidecar || *hasSidecar) {
        return hasSidecar;
    }
    Expected<std::optional<std::string>> variant =
        findCaseVariant((content / fromUtf8(sidecarPath)).parent_path(), fileName(sidecarPath));
    if (!variant) {
        return makeUnexpected(std::move(variant).error());
    }
    if (variant->has_value()) {
        return makeError("asset.path_conflict", "'" + std::string(path) + "' has a sidecar in another case, '" +
                                                    **variant + "'; rename it to '" +
                                                    std::string(fileName(sidecarPath)) + "'");
    }
    return false;
}

// The importer's output for `source` with the sidecar's settings: from the derived-data cache
// when every input is unchanged (`fromCache`), otherwise imported and stored there.
Expected<ImportedAssetData> importOrReuse(const DerivedDataCache& cache, const Importer& importer,
                                          const std::string& source, const Sidecar& sidecar, bool& fromCache) {
    const Expected<Hash128> key = derivedDataKey({
        .source = hash128(source),
        .importerId = importer.id(),
        .importerVersion = importer.version(),
        .settings = &sidecar.settings,
        .overrides = &sidecar.overrides,
        .platform = kImportPlatform,
        .outputFormatVersion = kImportedAssetFormat.version,
        .outputSchemaHash = serialization::schemaHash(typeOf<ImportedAssetData>()),
    });
    if (!key) {
        return makeUnexpected(key.error());
    }
    ImportedAssetData data;
    Expected<bool> hit = cache.get(*key, kImportedAssetFormat, data);
    if (!hit) {
        return makeUnexpected(std::move(hit).error());
    }
    fromCache = *hit;
    if (!*hit) {
        Expected<ImportOutput> output =
            importer.import(std::as_bytes(std::span<const char>(source.data(), source.size())), sidecar.settings);
        if (!output) {
            return makeUnexpected(std::move(output).error());
        }
        Expected<std::vector<std::string>> keys = sourceKeys(output->objects);
        if (!keys) {
            return makeUnexpected(std::move(keys).error());
        }
        data.objects.reserve(output->objects.size());
        for (std::size_t index = 0; index < output->objects.size(); ++index) {
            const std::vector<std::byte>& payload = output->objects[index].payload;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
            data.objects.push_back(ImportedObjectData{
                .key = std::move((*keys)[index]), .payload = std::vector<std::uint8_t>(bytes, bytes + payload.size())});
        }
        if (Expected<void> stored = cache.put(*key, kImportedAssetFormat, data); !stored) {
            return makeUnexpected(std::move(stored).error());
        }
    }
    std::unordered_set<std::string_view> unique;
    for (const ImportedObjectData& object : data.objects) {
        if (!unique.insert(object.key).second) {
            return makeError("import.invalid_output", "the source key '" + object.key + "' is given twice");
        }
    }
    return data;
}

} // namespace

bool isValidContentPath(std::string_view path) noexcept {
    if (path.empty() || endsWithIgnoreCaseAscii(path, kSidecarSuffix) || !isValidUtf8(path)) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (!isValidSegment(path.substr(start, end - start))) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> findCaseConflicts(const std::vector<std::string>& paths) {
    std::unordered_map<std::string, const std::string*> firstByLowercase;
    std::vector<std::pair<std::string, std::string>> conflicts;
    for (const std::string& path : paths) {
        const auto [found, inserted] = firstByLowercase.emplace(toLowerAscii(path), &path);
        if (!inserted && *found->second != path) {
            conflicts.emplace_back(*found->second, path);
        }
    }
    return conflicts;
}

AssetDatabase::AssetDatabase(const std::filesystem::path& projectDirectory, const ImporterRegistry& importers)
    : m_contentDirectory(projectDirectory / "Content"),
      m_cache(DerivedDataCache::projectDirectory(projectDirectory)),
      m_importers(importers) {}

const AssetRecord* AssetDatabase::find(AssetId id) const {
    const auto found = m_records.find(id);
    return found != m_records.end() ? &found->second : nullptr;
}

const AssetRecord* AssetDatabase::findByPath(std::string_view path) const {
    const auto found = m_byPath.find(path);
    return found != m_byPath.end() ? find(found->second) : nullptr;
}

std::vector<const AssetRecord*> AssetDatabase::records() const {
    std::vector<const AssetRecord*> result;
    result.reserve(m_byPath.size());
    for (const auto& [path, id] : m_byPath) {
        result.push_back(find(id));
    }
    return result;
}

void AssetDatabase::insert(AssetRecord record) {
    erase(record.id);
    if (const auto atPath = m_byPath.find(record.path); atPath != m_byPath.end()) {
        m_records.erase(atPath->second);
        m_byPath.erase(atPath);
    }
    m_byPath.emplace(record.path, record.id);
    m_records.emplace(record.id, std::move(record));
}

void AssetDatabase::erase(AssetId id) {
    if (const auto found = m_records.find(id); found != m_records.end()) {
        m_byPath.erase(found->second.path);
        m_records.erase(found);
    }
}

Expected<std::vector<AssetProblem>> AssetDatabase::scan() {
    Expected<ContentListing> listing = listContent(m_contentDirectory);
    if (!listing) {
        return makeUnexpected(std::move(listing).error());
    }
    std::vector<AssetProblem> problems = std::move(listing->problems);
    for (auto& [first, second] : findCaseConflicts(listing->paths)) {
        problems.push_back(AssetProblem{.kind = AssetProblemKind::CaseConflict,
                                        .path = std::move(second),
                                        .otherPath = std::move(first),
                                        .message = "two paths differ only in case"});
    }
    std::vector<Candidate> candidates = findCandidates(m_contentDirectory, m_importers, listing->files, problems);

    // Duplicate ids: the file at the path the database knew keeps the id, else the first path.
    struct Keeper {
        std::size_t index = 0;
        bool known = false;
    };

    std::unordered_map<AssetId, Keeper> keepers;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const AssetRecord* known = find(candidates[index].record.id);
        const bool isKnown = known != nullptr && known->path == candidates[index].record.path;
        const auto [keeper, inserted] = keepers.emplace(candidates[index].record.id, Keeper{index, isKnown});
        if (!inserted && isKnown) {
            keeper->second = Keeper{index, true};
        }
    }
    std::vector<AssetRecord> accepted;
    accepted.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates[index];
        const AssetId id = candidate.record.id;
        const Keeper keeper = keepers.at(id);
        if (keeper.index == index) {
            accepted.push_back(candidate.record); // a copy: later duplicates name this path
            continue;
        }
        const std::string& keeperPath = candidates[keeper.index].record.path;
        if (!keeper.known) {
            // Without history the copy can't be told from the original: change nothing on disk.
            problems.push_back(AssetProblem{.kind = AssetProblemKind::DuplicateId,
                                            .path = candidate.record.path,
                                            .otherPath = keeperPath,
                                            .message = "the id " + id.toString() + " is also used by '" + keeperPath +
                                                       "'; this file is left out until it gets a new id"});
            continue;
        }
        const AssetId newId = AssetId::generate();
        if (Expected<void> written = writeNewId(m_contentDirectory, candidate, newId); !written) {
            problems.push_back(
                invalidFile(candidate.record.path, "can't give the copy a new id: " + written.error().message));
            continue;
        }
        problems.push_back(AssetProblem{.kind = AssetProblemKind::DuplicateId,
                                        .path = candidate.record.path,
                                        .otherPath = keeperPath,
                                        .message = "the id " + id.toString() + " was taken; the copy got the new id " +
                                                   newId.toString()});
        accepted.push_back(std::move(candidate.record));
    }

    m_records.clear();
    m_byPath.clear();
    for (AssetRecord& record : accepted) {
        insert(std::move(record));
    }
    std::ranges::stable_sort(problems, [](const AssetProblem& a, const AssetProblem& b) { return a.path < b.path; });
    return problems;
}

Expected<AssetId> AssetDatabase::assignNewId(std::string_view path) {
    if (!isValidContentPath(path)) {
        return invalidPath(path);
    }
    const std::string file(path);
    Expected<bool> exists = existsExactly(m_contentDirectory, file);
    Expected<bool> hasSidecar = existsExactly(m_contentDirectory, sidecarOf(file));
    if (!exists || !hasSidecar) {
        return makeUnexpected(!exists ? std::move(exists).error() : std::move(hasSidecar).error());
    }
    if (!*exists || (!*hasSidecar && !isNativeAssetPath(file))) {
        return makeError("asset.not_found", "no asset file '" + file + "' with a sidecar or an id of its own");
    }
    Expected<Candidate> candidate =
        *hasSidecar ? readImported(m_contentDirectory, file) : readNative(m_contentDirectory, file);
    if (!candidate) {
        return makeUnexpected(std::move(candidate).error());
    }
    const AssetId newId = AssetId::generate();
    if (Expected<void> written = writeNewId(m_contentDirectory, *candidate, newId); !written) {
        return makeUnexpected(std::move(written).error());
    }
    insert(std::move(candidate->record));
    return newId;
}

Expected<ImportReport> AssetDatabase::import(std::string_view path) {
    if (!isValidContentPath(path)) {
        return invalidPath(path);
    }
    Expected<bool> hasSidecar = findSidecarForImport(m_contentDirectory, path);
    if (!hasSidecar) {
        return makeUnexpected(std::move(hasSidecar).error());
    }
    const std::string sidecarPath = sidecarOf(path);
    const std::filesystem::path sidecarFile = m_contentDirectory / fromUtf8(sidecarPath);

    ImportReport report;
    Sidecar sidecar;
    std::string oldText;
    const Importer* importer = nullptr;
    if (*hasSidecar) {
        Expected<std::string> text = readFile(sidecarFile, kMaxSidecarSize);
        if (!text) {
            return makeUnexpected(std::move(text).error());
        }
        oldText = std::move(*text);
        Expected<Sidecar, serialization::Error> read = readSidecar(oldText, sidecarPath, &report.warnings);
        if (!read) {
            return fromSerialization(read.error());
        }
        sidecar = std::move(*read);
        importer = m_importers.find(sidecar.importer);
        if (importer == nullptr) {
            return makeError("asset.no_importer", "the importer '" + sidecar.importer + "' named by '" + sidecarPath +
                                                      "' is not registered");
        }
        // The id belongs to another path: a copy when that file is still there (it gets a new id,
        // rule 6), otherwise the file was moved outside the editor and keeps its id.
        if (const AssetRecord* other = find(sidecar.id); other != nullptr && other->path != path) {
            Expected<bool> original = existsExactly(m_contentDirectory, other->path);
            if (!original) {
                return makeUnexpected(std::move(original).error());
            }
            if (*original) {
                sidecar.id = AssetId::generate();
            }
        }
    } else {
        importer = m_importers.findForPath(path);
        if (importer == nullptr) {
            return makeError("asset.no_importer", "no importer reads '" + std::string(path) + "'");
        }
        sidecar.id = AssetId::generate();
        sidecar.importer = std::string(importer->id());
        sidecar.settings = importer->defaultSettings();
        report.created = true;
    }

    Expected<std::string> source = readFile(m_contentDirectory / fromUtf8(path), kMaxSourceFileSize);
    if (!source) {
        return makeUnexpected(std::move(source).error());
    }
    Expected<ImportedAssetData> imported = importOrReuse(m_cache, *importer, *source, sidecar, report.fromCache);
    if (!imported) {
        return makeUnexpected(std::move(imported).error());
    }
    ImportedAssetData& data = *imported;
    std::vector<std::string> keys;
    keys.reserve(data.objects.size());
    for (const ImportedObjectData& object : data.objects) {
        keys.push_back(object.key);
    }
    sidecar.subAssets = reconcileSubAssets(sidecar.subAssets, keys);
    sidecar.importerVersion = importer->version();
    Expected<std::string, serialization::Error> text = writeSidecar(sidecar);
    if (!text) {
        return fromSerialization(text.error());
    }
    if (*text != oldText) {
        if (Expected<void> written = writeFileAtomically(sidecarFile, *text); !written) {
            return makeUnexpected(std::move(written).error());
        }
    }

    std::unordered_map<std::string_view, SubAssetId> idByKey;
    for (const SubAssetEntry& entry : sidecar.subAssets) {
        idByKey.emplace(entry.key, entry.id);
        if (entry.missing) {
            report.missing.push_back(entry);
        }
    }
    for (ImportedObjectData& object : data.objects) {
        const auto* bytes = reinterpret_cast<const std::byte*>(object.payload.data());
        const SubAssetId id = idByKey.at(object.key);
        report.subAssets.push_back(
            ImportedSubAsset{.key = std::move(object.key),
                             .id = id,
                             .payload = std::vector<std::byte>(bytes, bytes + object.payload.size())});
    }
    report.id = sidecar.id;
    insert(AssetRecord{.id = sidecar.id,
                       .path = std::string(path),
                       .type = sidecar.importer,
                       .imported = true,
                       .subAssets = std::move(sidecar.subAssets)});
    return report;
}

Expected<ImportReport> AssetDatabase::reimport(AssetId id) {
    const AssetRecord* record = find(id);
    if (record == nullptr) {
        return notFound(id);
    }
    if (!record->imported) {
        return makeError("asset.not_imported", "'" + record->path + "' is a native asset, not an imported file");
    }
    const std::string path = record->path;
    return import(path);
}

Expected<void> AssetDatabase::checkTarget(const AssetRecord& record, const std::string& newPath) const {
    // Moving a file to another case of its own name finds the file itself on a case-insensitive
    // file system; anything else at the target, or at another case of a folder, is a conflict.
    const bool caseOfItself = equalsIgnoreCaseAscii(newPath, record.path);
    const std::string ownName(fileName(record.path));
    const std::vector<std::string_view> segments = split(newPath, '/');
    std::filesystem::path directory = m_contentDirectory;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const bool last = index + 1 == segments.size();
        // Each name to check, with the record's own file of that name.
        std::vector<std::pair<std::string, std::string>> names = {{std::string(segments[index]), ownName}};
        if (last) {
            names.emplace_back(sidecarOf(segments[index]), sidecarOf(ownName));
        }
        for (const auto& [name, own] : names) {
            Expected<std::optional<std::string>> variant = findCaseVariant(directory, name);
            if (!variant) {
                return makeUnexpected(std::move(variant).error());
            }
            const bool isFree =
                !variant->has_value() || (!last && **variant == name) || (last && caseOfItself && **variant == own);
            if (isFree) {
                continue;
            }
            return makeError("asset.path_conflict",
                             "'" + newPath + "' conflicts with '" + **variant + "', which exists in the same folder");
        }
        directory /= fromUtf8(segments[index]);
    }
    return {};
}

Expected<void> AssetDatabase::move(AssetId id, std::string_view newPath) {
    const AssetRecord* record = find(id);
    if (record == nullptr) {
        return notFound(id);
    }
    if (!isValidContentPath(newPath)) {
        return invalidPath(newPath);
    }
    if (newPath == record->path) {
        return {};
    }
    if (lowercaseExtension(newPath) != lowercaseExtension(record->path)) {
        return makeError("asset.extension_changed", "'" + record->path + "' can't be moved to '" +
                                                        std::string(newPath) + "': its extension changes");
    }
    const std::string target(newPath);
    if (Expected<void> available = checkTarget(*record, target); !available) {
        return available;
    }

    const std::filesystem::path oldFile = m_contentDirectory / fromUtf8(record->path);
    const std::filesystem::path newFile = m_contentDirectory / fromUtf8(target);
    std::error_code error;
    std::filesystem::create_directories(newFile.parent_path(), error);
    if (error) {
        return ioError("can't create the folder", newFile.parent_path(), error);
    }
    std::filesystem::rename(oldFile, newFile, error);
    if (error) {
        return ioError("can't move", oldFile, error);
    }
    AssetRecord moved = *record;
    moved.path = target;
    if (record->imported) {
        const std::filesystem::path oldSidecar = m_contentDirectory / fromUtf8(sidecarOf(record->path));
        const std::filesystem::path newSidecar = m_contentDirectory / fromUtf8(sidecarOf(target));
        std::filesystem::rename(oldSidecar, newSidecar, error);
        if (error) {
            std::error_code rollback;
            std::filesystem::rename(newFile, oldFile, rollback);
            if (!rollback) {
                return ioError("can't move", oldSidecar, error);
            }
            // The file is at the new path and its sidecar at the old one: say so, and keep the
            // record where the file is.
            const std::string message = "'" + record->path + "' moved to '" + target + "', but its sidecar '" +
                                        sidecarOf(record->path) + "' couldn't follow (" + error.message() +
                                        ") and the file couldn't be moved back (" + rollback.message() +
                                        "); move the sidecar to '" + sidecarOf(target) + "'";
            insert(std::move(moved));
            return makeError("asset.io", message);
        }
    }
    insert(std::move(moved));
    return {};
}

Expected<void> AssetDatabase::rename(AssetId id, std::string_view newName) {
    const AssetRecord* record = find(id);
    if (record == nullptr) {
        return notFound(id);
    }
    if (newName.empty() || newName.find('/') != std::string_view::npos) {
        return makeError("asset.invalid_path", "'" + std::string(newName) + "' is not a file name");
    }
    const std::size_t slash = record->path.rfind('/');
    if (slash == std::string::npos) {
        return move(id, newName);
    }
    return move(id, record->path.substr(0, slash + 1) + std::string(newName));
}

Expected<void> AssetDatabase::remove(AssetId id) {
    const AssetRecord* record = find(id);
    if (record == nullptr) {
        return notFound(id);
    }
    const std::filesystem::path file = m_contentDirectory / fromUtf8(record->path);
    std::error_code error;
    std::filesystem::remove(file, error);
    if (error) {
        return ioError("can't delete", file, error);
    }
    const bool imported = record->imported;
    const std::filesystem::path sidecar = m_contentDirectory / fromUtf8(sidecarOf(record->path));
    erase(id);
    if (imported) {
        std::filesystem::remove(sidecar, error);
        if (error) {
            return ioError("can't delete the sidecar", sidecar, error);
        }
    }
    return {};
}

} // namespace ith
