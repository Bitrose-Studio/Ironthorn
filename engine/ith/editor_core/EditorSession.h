#pragma once

// ith::EditorSession: one editor's document and commands, without any UI (ADR-0037 rules 1, 14 to
// 18).
//
//   ith::TypeRegistry types;
//   ith::generated::registerReflection_ecs(types);
//   ith::generated::registerReflection_editor_core(types);
//   ith::EditorSession session(types, {.projectDirectory = project, .jobs = &jobs});
//   session.execute("ith.entity.create", args);
//   session.execute("ith.file.save_as", saveArgs);
//   session.waitIdle(); // the file is written
//
// The session registers the built-in commands of editor_core (the Edit, Entity, Selection, File,
// Query, Window, Viewport, Content and Play commands); editor_ui and game editor modules add theirs to
// the same registry. `types` names the component types that commands and level files may use and must
// outlive the session. Headless: tests and automation use it directly. Main thread only.
//
// Files. Saving serializes the level on the main thread (it reads the world) and writes the file
// durably on the background lane; opening reads the file on the background lane and loads it on
// the main thread. Results come back through the job system's main-thread queue, which the
// application drains every frame (ADR-0035 rule 22). Without a job system both happen at once on
// the calling thread. Relative paths are relative to the project directory.
//
// Play In Editor (ADR-0053). startPlay copies the editor world into a PlaySession's game world and
// sets the document's play flag; stopPlay discards the session. While one exists, active() is the
// game world's set, so the selection commands, the four value commands of the Details panel, the
// world queries and the view models act on the game world; the file commands, the history and the
// document keep using document().

#include "ith/ecs/Schedule.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Expected.h"
#include "ith/jobs/JobCounter.h"
#include "ith/serialization/Document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class AssetDatabase;
class JobSystem;
class TypeRegistry;

struct EditorSessionOptions {
    // The project's directory (UTF-8, '/' separators), which relative file paths start from.
    std::string projectDirectory;
    // The job system for file operations; null runs them on the calling thread.
    JobSystem* jobs = nullptr;
    // The project's asset database, for opening levels by asset id and for the Content Browser, which
    // scans, and later changes, it (ADR-0049 rule 23); null: levels open by path only and the content
    // commands fail. It must outlive the session.
    AssetDatabase* assets = nullptr;
    // The layered configuration that holds the named layouts and the default choice (ADR-0040);
    // null: only the built-in default layout exists. It must outlive the session.
    Config* config = nullptr;
    // The game modules' systems, which every Play In Editor session's game world runs after the
    // engine's game-world systems (ADR-0053 decision 3, ADR-0017 rule 5); the editor world never
    // runs them. Empty in P2.
    std::vector<SystemDescriptor> gameSystems;
};

// The world commands and panels act on (ADR-0053 decision 12): the game world's set while a play
// session exists, the editor document's otherwise. Valid until the next startPlay or stopPlay.
struct ActiveWorld {
    // Read-only: changes go through `transactions`.
    const World& world;
    Transactions& transactions;
    Selection& selection;
    EditorChanges& changes;
    // Entities as stable ids in JSON, for this world.
    const serialization::EntityMapping& entities;
    // 0 for the editor world, the play session's identity for a game world: never the same for two
    // worlds one EditorSession showed, so a view model that keys its cache on it as well as on the
    // counters rebuilds whenever the world changes, even when the counters happen to match.
    std::uint64_t identity = 0;

    // True for a game world.
    [[nodiscard]] bool game() const noexcept { return identity != 0; }
};

// The session layout's file, relative to the project directory (ADR-0035 rule 13, ADR-0040 rule 12).
inline constexpr std::string_view kSessionLayoutFile = "Saved/Editor/Layout.json";

// The largest level file the editor opens.
inline constexpr std::size_t kMaxLevelFileSize = std::size_t{512} * 1024 * 1024;

class EditorSession {
public:
    explicit EditorSession(const TypeRegistry& types, EditorSessionOptions options = {});

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;
    // Waits for a file operation that is still running (waitIdle).
    ~EditorSession();

    [[nodiscard]] EditorDocument& document() noexcept { return m_document; }

    [[nodiscard]] const EditorDocument& document() const noexcept { return m_document; }

    [[nodiscard]] CommandRegistry& commands() noexcept { return m_commands; }

    [[nodiscard]] const CommandRegistry& commands() const noexcept { return m_commands; }

    // The dockable panels; editor modules register theirs (ADR-0040 rule 1).
    [[nodiscard]] PanelRegistry& panels() noexcept { return m_panels; }

    [[nodiscard]] const PanelRegistry& panels() const noexcept { return m_panels; }

    // The layout and the layout commands' state (ADR-0040). Its session file is
    // <project>/Saved/Editor/Layout.json when the session has a project directory.
    [[nodiscard]] LayoutSession& layouts() noexcept { return m_layouts; }

    [[nodiscard]] const LayoutSession& layouts() const noexcept { return m_layouts; }

    // The viewports' cameras, tool, gizmo space and snapping (ADR-0042). Their file is
    // <project>/Saved/Editor/Viewports.json when the session has a project directory. When a level
    // closes (New Level, or Open Level once the new level has loaded), its cameras are saved as
    // saveViewports() does; the level that replaces it gets its own saved cameras, or the defaults.
    [[nodiscard]] Viewports& viewports() noexcept { return m_viewports; }

    [[nodiscard]] const Viewports& viewports() const noexcept { return m_viewports; }

    // The Content Browser's view model (ADR-0049): its items, selection and session state. Its state
    // file is <project>/Saved/Editor/ContentBrowser.json when the session has a project directory. It
    // shows nothing until ith.content.rescan runs, which the editor does when the project opens.
    [[nodiscard]] ContentBrowserModel& content() noexcept { return m_content; }

    [[nodiscard]] const ContentBrowserModel& content() const noexcept { return m_content; }

    // Keeps the open level's cameras and writes the viewports file when it changed (ADR-0042 rule
    // 4). An untitled level, which has no file yet, keeps none. The editor calls it at exit.
    void saveViewports();

    [[nodiscard]] const TypeRegistry& types() const noexcept { return *m_types; }

    [[nodiscard]] const EditorSessionOptions& options() const noexcept { return m_options; }

    // The world commands and panels act on now (ActiveWorld).
    [[nodiscard]] ActiveWorld active() noexcept;

    // The play session, or null when none exists.
    [[nodiscard]] PlaySession* play() noexcept { return m_play.get(); }

    [[nodiscard]] const PlaySession* play() const noexcept { return m_play.get(); }

    // Whether a session can start now, checked in the spec's order (ADR-0053 decision 7):
    // "editor.play.running" (a session exists), "editor.document.busy" (a level is being opened),
    // "editor.command.interaction_open" (an editor-world transaction is open).
    [[nodiscard]] Expected<void> canStartPlay() const;

    // Starts Play (possessed) or Simulate (ejected) on the editor world as it is in memory, paused
    // when `paused` (ADR-0053 decision 7): canStartPlay, PlaySession::start with
    // options().gameSystems, then the document's play flag. Fails, changing nothing, with
    // canStartPlay's errors, the schedule's "ecs.schedule.*" error (logged) and
    // "editor.play.copy_failed". Precondition (asserted): not called from inside a command run by
    // another command (CommandRegistry::depth at most 1), whose scopes may point at the game world.
    Expected<void> startPlay(PlayMode mode, bool paused = false);

    // Stops the session (ADR-0053 decision 10); does nothing without one. It can't fail: an open
    // game-world transaction is dropped without replaying it (Transactions::abandonOpen; the world
    // goes with it), the session and its world are destroyed, and the play flag is cleared. The
    // editor world, its history, dirty state and selection are as before Play. A scope still held on
    // the game world's transactions (a Details drag) must then be dropped with
    // TransactionScope::release, never ended. Game-world entity handles must not be kept across
    // Stop: world ids are recycled, so the next session's world may have the same id. Precondition
    // (asserted): as for startPlay.
    void stopPlay();

    // Runs a command: commands().execute(*this, id, args).
    Expected<serialization::Value> execute(std::string_view id, const serialization::Value& args = {});

    // The entity mapping between the editor world and JSON: an entity is written as its stable id
    // (the nil id when it has none or isn't of the editor world), and an id reads as the live entity
    // that has it (the null handle when none does). active().entities is the active world's.
    [[nodiscard]] const serialization::EntityMapping& entityMapping() const noexcept { return m_mapping; }

    // `path` made absolute: relative paths are joined to the project directory.
    [[nodiscard]] std::string resolvePath(std::string_view path) const;

    // Starts saving the level to `path`, or to its own path when `path` is empty; a new path becomes
    // the level's path once the file is written. The saved marker moves at once and comes back when
    // the write fails. Fails with "editor.document.no_path" (an untitled level and no path),
    // "editor.document.save_in_progress", "editor.document.busy" (an open is running) and the
    // saveLevel errors; a failed write is reported later through lastFileError().
    //
    // An existing file at `path` is replaced without asking; asking before an overwrite is the Save
    // As dialog's job.
    //
    // The level id the file gets (ADR-0050 item 9): its own path (compared after resolvePath, byte
    // for byte) is a Save and keeps the id. Another path is a Save As: the file there is read on the
    // background lane before the write (at most serialization::kMaxTextSize bytes). A level file
    // gives its id, so references to the overwritten level keep working; nothing there, or a file
    // that is not a level document, gives a new random id, so the original file keeps its own; a
    // level that has no file yet keeps its id. Any other read error (fs.busy, fs.access_denied,
    // fs.too_large, fs.is_a_directory, fs.unsupported for a FIFO or a device...) fails the save with
    // it, without writing. Another spelling of the level's own path (another case on a file system
    // that ignores it, a link) counts as a Save As and reads back the level's own id, so the id stays.
    // The document takes the resulting id and the path only once the write succeeds; a failed write
    // changes neither. The viewport cameras stay as they are and are kept under the resulting id when
    // the level closes.
    //
    // When the written file is in the Content folder and the asset database doesn't know it under the
    // resulting id (a new file, or an overwritten file whose id it had otherwise), the Content Browser
    // rescans, so it shows the level without a Rescan by the user. When that scan gives the file
    // another id (it was a copy of a known level, made outside the editor), the document takes that
    // id, so the document and its file agree.
    Expected<void> save(std::string_view path = {});

    // Keeps the open level in step with the asset database after a scan (ith.content.rescan and the
    // rescan of a save; spec 3.14), never as a dirty change. An untitled level, or a session without a
    // database, is left alone. When the database has a record at the level's path, the level takes
    // that record's id (a scan gives a copy of a known level a new one) and keeps its path. When its
    // file is there but not in the database (outside Content, left out as a duplicate id or an invalid
    // file), or can't be checked, nothing changes. Only when the file is gone does the id find it: a
    // record with the id at another path (moved outside the editor) becomes the level's path;
    // without one, a warning says that saving writes the file again.
    void syncLevelWithAssets();

    // The open level's file as a content path (relative to the asset database's Content folder),
    // or nullopt: an untitled level, a file outside Content, or no database. Compared as the
    // operating system sees paths (fs::pathInside), so another spelling of the level's path (another
    // case or a short name on Windows, a link) is recognised, and the result is spelled as on disk.
    // Costs a path resolution on disk.
    [[nodiscard]] std::optional<std::string> levelContentPath() const;

    // After the Content Browser moved the file or folder at the content path `from` to `to`:
    // `levelPath` is levelContentPath() as it was before the move (spelled as on disk, so a document
    // opened under another spelling follows too); when it is `from` or below it, the document's path
    // follows (as "Content/<path>" for a relative path, the absolute file otherwise), never as a
    // dirty change (ADR-0049 rule 23). The level id and the viewport cameras stay. Precondition
    // (asserted when it applies): no save is in flight; the content commands refuse every file
    // operation then, since a save's completion sets the path it wrote.
    void followContentMove(const std::optional<std::string>& levelPath, std::string_view from, std::string_view to);

    // Starts opening the level file at `path`: read on the background lane, then loaded all or
    // nothing. Until it finishes the document is busy (opening()). Fails with
    // "editor.document.save_in_progress" and "editor.document.busy"; a failed read or load is
    // reported later through lastFileError(), and the open level stays as it was.
    Expected<void> open(std::string_view path);

    // Replaces the level with an untitled, empty one. Fails as open does.
    Expected<void> newLevel();

    // Returns once no file operation is running: waits for the background lane and runs the
    // main-thread jobs that finish the operation. Main thread only, outside any job.
    void waitIdle();

    // The error of the last file operation that finished, or nullopt when it succeeded.
    [[nodiscard]] const std::optional<Error>& lastFileError() const noexcept { return m_lastFileError; }

    // Warnings of the last level save or load (unsaved entities, unknown components...), and of
    // undo references dropped; returns them and forgets them.
    [[nodiscard]] std::vector<Error> takeWarnings();

private:
    struct FileOperation;

    Expected<void> checkIdle() const;
    void start(std::unique_ptr<FileOperation> operation);
    static void runFileStep(FileOperation& operation);
    // Before a Save As writes: when the file at the path is a level, the text gets that level's id.
    static void takeTargetId(FileOperation& operation);
    void finish();
    // After a save to `fullPath`: a level file in the Content folder that the database doesn't know
    // under the level's id (a new file, or an overwritten one with another id) appears in the Content
    // Browser at once (a Rescan, then syncLevelWithAssets); a failure becomes a warning.
    void showSavedLevel(const std::string& fullPath);

    const TypeRegistry* m_types;
    EditorSessionOptions m_options;
    EditorDocument m_document;
    CommandRegistry m_commands;
    PanelRegistry m_panels;
    LayoutSession m_layouts;
    Viewports m_viewports;
    ContentBrowserModel m_content;
    serialization::EntityMapping m_mapping;
    std::unique_ptr<FileOperation> m_operation;
    JobCounter m_pending;
    std::optional<Error> m_lastFileError;
    std::vector<Error> m_warnings;
    // The dropped undo references already reported by takeWarnings.
    std::size_t m_reportedDropped = 0;
    // The identity the last play session got.
    std::uint64_t m_playSessions = 0;
    // Last, so it is destroyed first: nothing outlives the world it refers to.
    std::unique_ptr<PlaySession> m_play;
};

// Register the built-in commands (EditCommands.cpp, FileCommands.cpp, QueryCommands.cpp,
// LayoutCommands.cpp, ViewportCommands.cpp, ContentCommands.cpp, PlayCommands.cpp).
void registerEditCommands(CommandRegistry& registry);
void registerFileCommands(CommandRegistry& registry);
void registerQueryCommands(CommandRegistry& registry);
void registerLayoutCommands(CommandRegistry& registry);
void registerViewportCommands(CommandRegistry& registry);
void registerContentCommands(CommandRegistry& registry);
void registerPlayCommands(CommandRegistry& registry);

} // namespace ith
