#pragma once

// ith::CommandRegistry: every editor action as a named command (ADR-0037 rules 1 to 4).
//
//   registry.add({
//       .id = "ith.edit.rename",
//       .displayName = "Rename",
//       .category = "Edit",
//       .execute = [](ith::EditorSession& session, const ith::serialization::Value& args)
//           -> ith::Expected<ith::serialization::Value> { ... },
//   });
//   const auto result = registry.execute(session, "ith.edit.rename", args);
//
// Menus, the toolbar, shortcuts, panels and automation all call execute(); nothing else changes
// the editor world. Arguments and results are JSON values: entities are named by stable id,
// assets by AssetId, never by handle or pointer.
//
// What execute does, by the command's kind:
// - Edit: changes the editor world, inside one transaction named by displayName. Refused while
//   playing ("editor.command.playing") and while an interaction keeps a transaction open outside
//   any command ("editor.command.interaction_open"). A failed execute cancels the transaction, so it
//   changes nothing. A command run by another command joins its transaction.
// - Action: changes editor state without its own transaction (undo, redo, save, open); refused
//   while an interaction is open. Its canExecute decides about Play In Editor.
// - Query: reads only; always allowed.
// While a level is being opened, Edit and Action commands are refused ("editor.document.busy").
// canExecute, when set, runs first; its error comes back as "editor.command.disabled" with its
// message, which menus show as the reason. An unknown id is "editor.command.unknown". Main thread
// only.

#include "ith/foundation/Expected.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class EditorSession;

enum class CommandKind : std::uint8_t {
    Edit,
    Action,
    Query,
};

struct CommandDesc {
    // Stable, "ith.<area>.<verb>" in lower snake case; never reused for another meaning.
    std::string id;
    // In the ADR-0008 vocabulary, e.g. "Delete".
    std::string displayName;
    std::string tooltip;
    // The menu it belongs to, e.g. "Edit".
    std::string category;
    CommandKind kind = CommandKind::Edit;
    // Optional: whether the command can run now; the error says why not.
    std::function<Expected<void>(EditorSession&)> canExecute;
    // Required. Validates `args` (failing with "editor.command.invalid_args") and runs.
    std::function<Expected<serialization::Value>(EditorSession&, const serialization::Value& args)> execute;
};

class CommandRegistry {
public:
    // Registers a command. Preconditions (asserted; the command is then not added): a non-empty id
    // not registered yet, and an execute function.
    void add(CommandDesc command);

    // The command with `id`, or null.
    [[nodiscard]] const CommandDesc* find(std::string_view id) const;

    // Every command, by id.
    [[nodiscard]] std::vector<const CommandDesc*> commands() const;

    // Whether `id` can run now: the checks of execute without running it.
    [[nodiscard]] Expected<void> canExecute(EditorSession& session, std::string_view id) const;

    // Runs the command `id` with `args` (an object, or null for none); see the header comment.
    Expected<serialization::Value> execute(EditorSession& session, std::string_view id,
                                           const serialization::Value& args = {});

private:
    [[nodiscard]] Expected<void> check(EditorSession& session, const CommandDesc& command) const;

    std::map<std::string, CommandDesc, std::less<>> m_commands;
    // How many executes are running, to tell a command's own transaction from an interaction's.
    std::size_t m_depth = 0;
};

} // namespace ith
