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
// The session registers the built-in commands of editor_core (the Edit, Entity, Selection, File
// and Query commands); editor_ui and game editor modules add theirs to the same registry. `types`
// names the component types that commands and level files may use and must outlive the session.
// Headless: tests and automation use it directly. Main thread only.
//
// Files. Saving serializes the level on the main thread (it reads the world) and writes the file
// durably on the background lane; opening reads the file on the background lane and loads it on
// the main thread. Results come back through the job system's main-thread queue, which the
// application drains every frame (ADR-0035 rule 22). Without a job system both happen at once on
// the calling thread. Relative paths are relative to the project directory.

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/foundation/Expected.h"
#include "ith/jobs/JobCounter.h"
#include "ith/serialization/Document.h"

#include <cstddef>
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
    // The project's asset database, for opening a level by its asset id; null: by path only.
    const AssetDatabase* assets = nullptr;
};

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

    [[nodiscard]] const TypeRegistry& types() const noexcept { return *m_types; }

    [[nodiscard]] const EditorSessionOptions& options() const noexcept { return m_options; }

    // Runs a command: commands().execute(*this, id, args).
    Expected<serialization::Value> execute(std::string_view id, const serialization::Value& args = {});

    // The entity mapping between the editor world and JSON: an entity is written as its stable id
    // (the nil id when it has none), and an id reads as the live entity that has it (the null
    // handle when none does).
    [[nodiscard]] const serialization::EntityMapping& entityMapping() const noexcept { return m_mapping; }

    // `path` made absolute: relative paths are joined to the project directory.
    [[nodiscard]] std::string resolvePath(std::string_view path) const;

    // Starts saving the level to `path`, or to its own path when `path` is empty; a new path becomes
    // the level's path once the file is written. The saved marker moves at once and comes back when
    // the write fails. Fails with "editor.document.no_path" (an untitled level and no path),
    // "editor.document.save_in_progress", "editor.document.busy" (an open is running) and the
    // saveLevel errors; a failed write is reported later through lastFileError().
    Expected<void> save(std::string_view path = {});

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
    void finish();

    const TypeRegistry* m_types;
    EditorSessionOptions m_options;
    EditorDocument m_document;
    CommandRegistry m_commands;
    serialization::EntityMapping m_mapping;
    std::unique_ptr<FileOperation> m_operation;
    JobCounter m_pending;
    std::optional<Error> m_lastFileError;
    std::vector<Error> m_warnings;
    // The dropped undo references already reported by takeWarnings.
    std::size_t m_reportedDropped = 0;
};

// Register the built-in commands (EditCommands.cpp, FileCommands.cpp, QueryCommands.cpp).
void registerEditCommands(CommandRegistry& registry);
void registerFileCommands(CommandRegistry& registry);
void registerQueryCommands(CommandRegistry& registry);

} // namespace ith
