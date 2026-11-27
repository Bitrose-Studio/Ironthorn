#include "ith/editor_core/EditorSession.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/assets/AssetId.h"
#include "ith/ecs/StableId.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/jobs/JobSystem.h"
#include "ith/level/Level.h"
#include "ith/serialization/Document.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(ContentBrowser);

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMapping::toId has
Uuid toStableId(void* context, Entity entity) {
    const World& world = static_cast<const EditorDocument*>(context)->world();
    const StableId* id = entity.worldId() == world.id() ? world.get<const StableId>(entity) : nullptr;
    return id != nullptr ? id->id : Uuid();
}

// NOLINTNEXTLINE(misc-const-correctness): the signature EntityMapping::toEntity has
Entity fromStableId(void* context, const Uuid& id) {
    return static_cast<const EditorDocument*>(context)->world().findByStableId(id);
}

Error fromSerialization(const serialization::Error& error) {
    std::string message = error.message;
    if (!error.source.empty()) {
        message = error.source + ": " + message;
    }
    if (!error.path.empty()) {
        message += " (at " + error.path + ")";
    }
    return Error{.code = error.code, .message = std::move(message)};
}

// The id of the level file at `fullPath` (ADR-0050 item 9): nullopt when nothing is there or the file
// is not a level document; the read's error for anything else (busy, access denied, too large, not
// a regular file), so a Save As never replaces a file it couldn't check. The file's bytes are freed
// before it returns. Any thread.
Expected<std::optional<AssetId>> levelIdAt(const std::string& fullPath, std::string_view source) {
    const Expected<std::string> existing = fs::readFile(fullPath, serialization::kMaxTextSize);
    if (!existing) {
        if (existing.error().code == "fs.not_found") {
            return std::optional<AssetId>();
        }
        return makeUnexpected(existing.error());
    }
    const Expected<AssetHeader, serialization::Error> header = readAssetHeader(*existing, source);
    if (!header || header->format != kLevelFormat) {
        return std::optional<AssetId>();
    }
    return std::optional<AssetId>(header->id);
}

// `fullPath` relative to the database's Content folder, or nullopt when it is not inside it.
std::optional<std::string> contentPathOf(const AssetDatabase& assets, const std::string& fullPath) {
    const std::string content = assets.contentDirectory() + "/";
    if (!fullPath.starts_with(content)) {
        return std::nullopt;
    }
    return fullPath.substr(content.size());
}

} // namespace

// A save or open in flight. The background step fills `bytes` or `error`; finish() completes it.
struct EditorSession::FileOperation {
    enum class Kind : std::uint8_t {
        Save,
        Open,
    };

    Kind kind = Kind::Save;
    // As given (the document keeps it) and resolved.
    std::string path;
    std::string fullPath;
    // Save: the level text. Open: the file read.
    std::string bytes;
    std::optional<Error> error;
    // Save: the saved position before this save and the one it set.
    std::uint64_t previousSaved = 0;
    std::uint64_t saved = 0;
    // Save: the level id the written file holds (ADR-0050 item 9), and whether a level file already
    // at the path gives it instead (a Save As, not a Save).
    Uuid levelId;
    bool takeTargetId = false;
};

EditorSession::EditorSession(const TypeRegistry& types, EditorSessionOptions options)
    : m_types(&types),
      m_options(std::move(options)),
      m_layouts(m_panels, m_options.config,
                m_options.projectDirectory.empty() ? std::string()
                                                   : fs::joinPath(m_options.projectDirectory, kSessionLayoutFile)),
      m_viewports(m_options.projectDirectory.empty() ? std::string()
                                                     : fs::joinPath(m_options.projectDirectory, kViewportsFile)),
      m_content(m_options.assets, m_options.projectDirectory.empty()
                                      ? std::string()
                                      : fs::joinPath(m_options.projectDirectory, kContentBrowserStateFile)),
      m_mapping{.toId = &toStableId, .toEntity = &fromStableId, .context = &m_document} {
    registerEditCommands(m_commands);
    registerFileCommands(m_commands);
    registerQueryCommands(m_commands);
    registerLayoutCommands(m_commands);
    registerViewportCommands(m_commands);
    registerContentCommands(m_commands);
}

void EditorSession::saveViewports() {
    if (!m_document.path().empty()) {
        m_viewports.saveLevel(m_document.levelId());
    }
}

EditorSession::~EditorSession() {
    waitIdle();
}

Expected<serialization::Value> EditorSession::execute(std::string_view id, const serialization::Value& args) {
    return m_commands.execute(*this, id, args);
}

std::string EditorSession::resolvePath(std::string_view path) const {
    if (fs::isAbsolutePath(path) || m_options.projectDirectory.empty()) {
        return fs::normalizePath(path);
    }
    return fs::joinPath(m_options.projectDirectory, path);
}

Expected<void> EditorSession::checkIdle() const {
    if (m_document.saving()) {
        return makeError("editor.document.save_in_progress", "the level is being saved");
    }
    if (m_document.opening()) {
        return makeError("editor.document.busy", "a level is being opened");
    }
    return {};
}

Expected<void> EditorSession::save(std::string_view path) {
    if (Expected<void> idle = checkIdle(); !idle) {
        return idle;
    }
    const std::string target(path.empty() ? std::string_view(m_document.path()) : path);
    if (target.empty()) {
        return makeError("editor.document.no_path", "the level has no file yet; save it with Save As");
    }
    std::string fullPath = resolvePath(target);
    // The level id of the file (ADR-0050 item 9): a Save keeps it; a Save As to a new path gives a new
    // one, except to a level that has no file yet; a level file already at the path gives its own,
    // which the background step reads (takeTargetId).
    const bool ownFile = !m_document.path().empty() && fullPath == resolvePath(m_document.path());
    const Uuid levelId = ownFile || m_document.path().empty() ? m_document.levelId() : Uuid::generate();
    std::vector<serialization::Error> warnings;
    Expected<std::string, serialization::Error> text =
        saveLevel(m_document.world(), levelId, *m_types, {.warnings = &warnings});
    if (!text) {
        return makeUnexpected(fromSerialization(text.error()));
    }
    for (const serialization::Error& warning : warnings) {
        m_warnings.push_back(fromSerialization(warning));
    }
    auto operation = std::make_unique<FileOperation>();
    operation->kind = FileOperation::Kind::Save;
    operation->path = target;
    operation->fullPath = std::move(fullPath);
    operation->bytes = std::move(*text);
    operation->levelId = levelId;
    operation->takeTargetId = !ownFile;
    operation->previousSaved = m_document.history().markSaved();
    operation->saved = m_document.history().savedPosition();
    m_document.setSaving(true);
    start(std::move(operation));
    return {};
}

Expected<void> EditorSession::open(std::string_view path) {
    if (Expected<void> idle = checkIdle(); !idle) {
        return idle;
    }
    auto operation = std::make_unique<FileOperation>();
    operation->kind = FileOperation::Kind::Open;
    operation->path = std::string(path);
    operation->fullPath = resolvePath(path);
    m_document.setOpening(true);
    start(std::move(operation));
    return {};
}

Expected<void> EditorSession::newLevel() {
    if (Expected<void> idle = checkIdle(); !idle) {
        return idle;
    }
    saveViewports();
    m_document.reset();
    m_viewports.restoreLevel(m_document.levelId());
    m_lastFileError.reset();
    return {};
}

void EditorSession::start(std::unique_ptr<FileOperation> operation) {
    m_operation = std::move(operation);
    if (m_options.jobs == nullptr) {
        runFileStep(*m_operation);
        finish();
        return;
    }
    FileOperation* step = m_operation.get();
    m_options.jobs->submit(
        JobLane::Background, "Editor file",
        [this, step] {
            runFileStep(*step);
            m_options.jobs->postToMainThread("Editor file done", [this] { finish(); }, &m_pending);
        },
        &m_pending);
}

void EditorSession::runFileStep(FileOperation& operation) {
    if (operation.kind == FileOperation::Kind::Save) {
        if (operation.takeTargetId) {
            takeTargetId(operation);
            if (operation.error) {
                return;
            }
        }
        if (Expected<void> written = fs::writeFileAtomic(operation.fullPath, operation.bytes); !written) {
            operation.error = std::move(written).error();
        }
        return;
    }
    Expected<std::string> read = fs::readFile(operation.fullPath, kMaxLevelFileSize);
    if (read) {
        operation.bytes = std::move(*read);
    } else {
        operation.error = std::move(read).error();
    }
}

void EditorSession::takeTargetId(FileOperation& operation) {
    Expected<std::optional<AssetId>> existing = levelIdAt(operation.fullPath, operation.path);
    if (!existing) {
        operation.error = std::move(existing).error();
        return;
    }
    if (!existing->has_value() || (*existing)->uuid() == operation.levelId) {
        return;
    }
    Expected<std::string, serialization::Error> replaced = replaceHeaderId(operation.bytes, **existing, operation.path);
    if (!replaced) {
        operation.error = fromSerialization(replaced.error());
        return;
    }
    operation.bytes = std::move(*replaced);
    operation.levelId = (*existing)->uuid();
}

void EditorSession::finish() {
    const std::unique_ptr<FileOperation> operation = std::move(m_operation);
    m_lastFileError = std::move(operation->error);
    if (operation->kind == FileOperation::Kind::Save) {
        m_document.setSaving(false);
        if (m_lastFileError) {
            // Only when no other save moved the marker since; then the document is dirty again.
            if (m_document.history().savedPosition() == operation->saved) {
                m_document.history().setSavedPosition(operation->previousSaved);
            }
        } else {
            if (operation->levelId != m_document.levelId()) {
                m_document.setLevelId(operation->levelId);
            }
            if (operation->path != m_document.path()) {
                m_document.setPath(operation->path);
            }
            showSavedLevel(operation->fullPath);
        }
        m_document.changes().bumpDocument();
        return;
    }
    m_document.setOpening(false);
    if (m_lastFileError) {
        return;
    }
    std::vector<serialization::Error> warnings;
    Expected<LevelInfo, serialization::Error> loaded = loadLevel(operation->bytes, m_document.mutableWorld(), *m_types,
                                                                 {.source = operation->path, .warnings = &warnings});
    if (!loaded) {
        m_lastFileError = fromSerialization(loaded.error());
        return;
    }
    for (const serialization::Error& warning : warnings) {
        m_warnings.push_back(fromSerialization(warning));
    }
    // The level that was open closes only now; its cameras are kept before the new level's come.
    saveViewports();
    m_document.adopt(loaded->id, operation->path);
    m_viewports.restoreLevel(loaded->id);
}

void EditorSession::showSavedLevel(const std::string& fullPath) {
    const AssetDatabase* assets = m_options.assets;
    const std::optional<std::string> path = assets != nullptr ? contentPathOf(*assets, fullPath) : std::nullopt;
    if (!path) {
        return;
    }
    // A file the database knows under the id it now holds needs nothing: a Save, or a Save As over a
    // known level, whose id the saved level took. A new file, or one whose id changed, is scanned.
    if (const AssetRecord* record = assets->findByPath(*path);
        record != nullptr && record->id == AssetId(m_document.levelId())) {
        return;
    }
    ITH_ASSERT_MSG(!m_document.saving(), "the Content Browser rescans only after the save has finished");
    if (Expected<void> scanned = m_content.rescan(); !scanned) {
        ITH_LOG_WARNING(ContentBrowser, "the Content Browser can't show the saved level: {}", scanned.error().message);
        m_warnings.push_back(scanned.error());
        return;
    }
    syncLevelWithAssets();
}

void EditorSession::syncLevelWithAssets() {
    const AssetDatabase* assets = m_options.assets;
    if (m_document.path().empty() || assets == nullptr) {
        return;
    }
    const std::string full = resolvePath(m_document.path());
    // The database knows the level's own file: the level takes the id the file has now (the scan gave
    // a copy of a known level a new one) and stays on its file.
    if (const std::optional<std::string> path = contentPathOf(*assets, full); path) {
        if (const AssetRecord* own = assets->findByPath(*path); own != nullptr) {
            if (own->id != AssetId(m_document.levelId())) {
                m_document.setLevelId(own->id.uuid());
            }
            return;
        }
    }
    // The file is there but not in the database (outside Content, left out as a duplicate or an
    // invalid file), or it can't be told: the level stays as it is.
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(full);
    if (!info || info->has_value()) {
        return;
    }
    // The file is gone: moved outside the editor when the database has the id at another path.
    if (const AssetRecord* record = assets->find(AssetId(m_document.levelId())); record != nullptr) {
        m_document.setPath(std::string(kContentFolderName) + "/" + record->path);
        return;
    }
    ITH_LOG_WARNING(ContentBrowser, "the open level's file {} is gone; saving the level writes it again",
                    m_document.path());
}

void EditorSession::waitIdle() {
    if (m_options.jobs != nullptr) {
        m_options.jobs->wait(m_pending);
    }
}

std::vector<Error> EditorSession::takeWarnings() {
    if (const std::size_t dropped = m_document.transactions().droppedReferences(); dropped > m_reportedDropped) {
        m_warnings.push_back(Error{.code = "editor.undo.dropped_reference",
                                   .message = std::to_string(dropped - m_reportedDropped) +
                                              " references to entities without a stable id were dropped "
                                              "from the undo history"});
        m_reportedDropped = dropped;
    }
    return std::exchange(m_warnings, {});
}

} // namespace ith
