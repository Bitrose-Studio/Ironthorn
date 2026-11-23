#include "ith/editor_core/EditorSession.h"

#include "ith/ecs/StableId.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"
#include "ith/level/Level.h"

#include <utility>

namespace ith {

namespace {

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
};

EditorSession::EditorSession(const TypeRegistry& types, EditorSessionOptions options)
    : m_types(&types),
      m_options(std::move(options)),
      m_layouts(m_panels, m_options.config,
                m_options.projectDirectory.empty() ? std::string()
                                                   : fs::joinPath(m_options.projectDirectory, kSessionLayoutFile)),
      m_mapping{.toId = &toStableId, .toEntity = &fromStableId, .context = &m_document} {
    registerEditCommands(m_commands);
    registerFileCommands(m_commands);
    registerQueryCommands(m_commands);
    registerLayoutCommands(m_commands);
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
    std::vector<serialization::Error> warnings;
    Expected<std::string, serialization::Error> text =
        saveLevel(m_document.world(), m_document.levelId(), *m_types, {.warnings = &warnings});
    if (!text) {
        return makeUnexpected(fromSerialization(text.error()));
    }
    for (const serialization::Error& warning : warnings) {
        m_warnings.push_back(fromSerialization(warning));
    }
    auto operation = std::make_unique<FileOperation>();
    operation->kind = FileOperation::Kind::Save;
    operation->path = target;
    operation->fullPath = resolvePath(target);
    operation->bytes = std::move(*text);
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
    m_document.reset();
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
        } else if (operation->path != m_document.path()) {
            m_document.setPath(operation->path);
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
    m_document.adopt(loaded->id, operation->path);
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
