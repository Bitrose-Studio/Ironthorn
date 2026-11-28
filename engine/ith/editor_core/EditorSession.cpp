#include "ith/editor_core/EditorSession.h"

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/AssetHeader.h"
#include "ith/assets/AssetId.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/ContentPaths.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/input/Input.h"
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
      m_mapping(detail::stableIdMapping(m_document.world())) {
    registerEditCommands(m_commands);
    registerFileCommands(m_commands);
    registerQueryCommands(m_commands);
    registerLayoutCommands(m_commands);
    registerViewportCommands(m_commands);
    registerContentCommands(m_commands);
    registerPlayCommands(m_commands);
    registerKeepSimulationChanges(m_commands);
}

ActiveWorld EditorSession::active() noexcept {
    if (m_play) {
        return {.world = m_play->world(),
                .transactions = m_play->transactions(),
                .selection = m_play->selection(),
                .changes = m_play->changes(),
                .entities = m_play->entityMapping(),
                .identity = m_play->identity()};
    }
    return {.world = m_document.world(),
            .transactions = m_document.transactions(),
            .selection = m_document.selection(),
            .changes = m_document.changes(),
            .entities = m_mapping,
            .identity = 0};
}

Expected<void> EditorSession::canStartPlay() const {
    if (m_play) {
        return makeError("editor.play.running", "a play session is running; stop it first");
    }
    if (m_document.opening()) {
        return makeError("editor.document.busy", "a level is being opened");
    }
    if (m_document.transactions().isOpen()) {
        return makeError("editor.command.interaction_open", "an interaction is in progress; finish or cancel it first");
    }
    return {};
}

Expected<void> EditorSession::startPlay(PlayMode mode, bool paused) {
    ITH_ASSERT_MSG(m_commands.depth() <= 1, "editor: Play started from inside a nested command");
    if (Expected<void> allowed = canStartPlay(); !allowed) {
        return allowed;
    }
    const std::size_t viewport = m_viewports.selected();
    Expected<std::unique_ptr<PlaySession>> started =
        PlaySession::start(m_document.world(), m_document.selection(), m_options.gameSystems, mode, paused,
                           m_playSessions + 1, viewport, m_viewports.at(viewport).camera);
    if (!started) {
        return makeUnexpected(std::move(started).error());
    }
    ++m_playSessions;
    m_play = std::move(*started);
    m_document.setPlaying(true);
    m_document.changes().bumpDocument();
    if (m_input != nullptr && m_play->possessed()) {
        m_input->setCaptureOnClick(m_inputWindow, true);
    }
    return {};
}

void EditorSession::stopPlay() {
    ITH_ASSERT_MSG(m_commands.depth() <= 1, "editor: Stop run from inside a nested command");
    if (!m_play) {
        return;
    }
    // WorldEnding goes here, once the engine has events (ADR-0053 decision 11).
    // The world is discarded with the transaction: nothing to put back.
    m_play->transactions().abandonOpen();
    releaseEditorMouse();
    if (m_input != nullptr) {
        m_input->setCaptureOnClick(m_inputWindow, false);
    }
    ViewportState& viewport = m_viewports.at(m_play->viewport());
    viewport.click.reset(); // a click on the game world names none of the editor world's entities
    viewport.camera = m_play->savedCamera();
    m_play.reset();
    m_document.setPlaying(false);
    m_document.changes().bumpDocument();
}

void EditorSession::setPossessed(bool possessed) {
    ITH_ASSERT_MSG(m_play != nullptr, "editor: Possess and Eject need a play session");
    if (!m_play) {
        return;
    }
    m_play->setPossessed(possessed, m_viewports.at(m_play->viewport()).camera);
    if (m_input != nullptr) {
        m_input->setCaptureOnClick(m_inputWindow, possessed);
    }
    if (!possessed) {
        releaseEditorMouse();
    }
}

void EditorSession::freeMouse() {
    releaseEditorMouse();
}

void EditorSession::releaseEditorMouse() {
    // Only the editor window's capture: another window's (a game window) is not Play In Editor's.
    if (m_input != nullptr && m_input->mouseCaptured(m_inputWindow)) {
        m_input->releaseMouse();
    }
}

void EditorSession::setPlayInput(Input* input, WindowId window) noexcept {
    ITH_ASSERT_MSG(m_play == nullptr, "editor: the play input is bound and unbound only while no session runs");
    if (m_play != nullptr) {
        return;
    }
    m_input = input;
    m_inputWindow = window;
}

bool EditorSession::mouseCaptured() const {
    return m_play != nullptr && m_input != nullptr && m_input->mouseCaptured(m_inputWindow);
}

const World& EditorSession::drawnWorld(std::size_t viewport) const noexcept {
    return m_play != nullptr && m_play->viewport() == viewport ? m_play->world() : m_document.world();
}

void EditorSession::saveViewports() {
    if (m_document.path().empty()) {
        return;
    }
    if (!m_play) {
        m_viewports.saveLevel(m_document.levelId());
        return;
    }
    // The pose from before the session, not one flown during it.
    EditorCamera& camera = m_viewports.at(m_play->viewport()).camera;
    const EditorCamera flown = std::exchange(camera, m_play->savedCamera());
    m_viewports.saveLevel(m_document.levelId());
    camera = flown;
}

EditorSession::~EditorSession() {
    // A session still running gives the mouse back through the bound Input, which outlives it.
    stopPlay();
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
    const std::optional<std::string> path =
        assets != nullptr ? detail::contentFileOf(assets->contentDirectory(), fullPath) : std::nullopt;
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
    if (const std::optional<std::string> path = detail::contentFileOf(assets->contentDirectory(), full); path) {
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

std::optional<std::string> EditorSession::levelContentPath() const {
    const AssetDatabase* assets = m_options.assets;
    if (m_document.path().empty() || assets == nullptr) {
        return std::nullopt;
    }
    return detail::contentFileOf(assets->contentDirectory(), resolvePath(m_document.path()));
}

void EditorSession::followContentMove(const std::optional<std::string>& levelPath, std::string_view from,
                                      std::string_view to) {
    if (!levelPath || from.empty() || from == to || !detail::isAtOrBelow(*levelPath, from)) {
        return;
    }
    ITH_ASSERT_MSG(!m_document.saving(), "the Content Browser refuses file operations while a level is saved");
    const std::string moved = std::string(to) + levelPath->substr(from.size());
    m_document.setPath(fs::isAbsolutePath(m_document.path()) ? fs::joinPath(m_options.assets->contentDirectory(), moved)
                                                             : std::string(kContentFolderName) + "/" + moved);
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
