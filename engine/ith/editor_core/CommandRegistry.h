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
// - Edit: changes the active world (EditorSession::active), inside one transaction of its
//   Transactions named by displayName. While playing, only playEditable commands run, on the game
//   world, recording nothing a user sees; the others are refused ("editor.command.playing"), as all
//   are while the play flag is set without a session. Refused while an interaction keeps a
//   transaction open in either world outside any command ("editor.command.interaction_open"). A
//   failed execute cancels the transaction, so it changes nothing. A command run by another command
//   joins its transaction.
// - Action: changes editor state without its own transaction (undo, redo, save, open, play);
//   refused while an interaction is open, unless runsDuringInteraction is set (the play commands).
//   Its canExecute decides about Play In Editor. An Action may record its own history entry through
//   the document's transactions: Keep Simulation Changes does, while playing (ADR-0053 decision 27).
// - Query: reads only; always allowed.
// While a level is being opened, Edit and Action commands are refused ("editor.document.busy").
// canExecute, when set, runs next; its error comes back as "editor.command.disabled" with its
// message, which menus show as the reason, except the refusals the Play In Editor spec names, which
// keep their code: "editor.command.playing", "editor.command.interaction_open",
// "editor.command.nested", "editor.document.busy" and every "editor.play.*" code. An unknown id is
// "editor.command.unknown". Main thread only.

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
    // Edit commands only: the command runs on the game world while a play session exists, in a
    // transaction of its Transactions that is never recorded, instead of being refused (ADR-0053
    // decision 13). Set on the four value commands of the Details panel only.
    bool playEditable = false;
    // Action commands only: the command runs while an interaction holds a transaction open, instead
    // of being refused with "editor.command.interaction_open" (ADR-0053 decisions 6, 7 and 10). Set
    // on the play commands: Stop cancels a game-world drag, Step and Pause keep the game going during
    // one, and Play and Simulate refuse in the spec's order through their canExecute.
    bool runsDuringInteraction = false;
    // Optional: whether the command can run now; the error says why not.
    std::function<Expected<void>(EditorSession&)> canExecute;
    // Required. Validates `args` (failing with "editor.command.invalid_args") and runs.
    std::function<Expected<serialization::Value>(EditorSession&, const serialization::Value& args)> execute;
};

class CommandRegistry {
public:
    // Registers a command. Preconditions (asserted; the command is then not added): a non-empty id
    // not registered yet, an execute function, playEditable only on an Edit command and
    // runsDuringInteraction only on an Action.
    void add(CommandDesc command);

    // How many executes are running: 0 outside any command, 1 inside a command run directly, more
    // inside a command run by another command. A canExecute sees the depth of its caller (0 for a
    // command run directly).
    [[nodiscard]] std::size_t depth() const noexcept { return m_depth; }

    // The command with `id`, or null.
    [[nodiscard]] const CommandDesc* find(std::string_view id) const;

    // Every command, by id.
    [[nodiscard]] std::vector<const CommandDesc*> commands() const;

    // Whether `id` can run now: the checks of execute without running it.
    [[nodiscard]] Expected<void> canExecute(EditorSession& session, std::string_view id) const;

    // Runs the command `id` with `args` (an object, or null for none); see the header comment.
    Expected<serialization::Value> execute(EditorSession& session, std::string_view id,
                                           const serialization::Value& args = {});

    // Sets the prompt of the command `id`: what a shortcut or a menu does instead of running the
    // command without arguments. Usually the command needs input from the user first (Rename starts
    // editing the name in the Outliner): the prompt starts gathering it, which later runs the command
    // with its arguments. A prompt may also run the command at once itself, to show its result (Keep
    // Simulation Changes' notice, ADR-0054 item 28). The prompt returns true when it handled the
    // gesture in either way, and false to let invoke() run the command without arguments. Set by the
    // module that brings the UI; an empty function removes it. Precondition (asserted): `id` is
    // registered.
    void setPrompt(std::string_view id, std::function<bool(EditorSession&)> prompt);

    // A user's gesture for the command `id` (a shortcut, a menu item): the checks of execute, then
    // its prompt when one is set and starts, which returns null; otherwise execute(id) without
    // arguments. Automation calls execute with arguments instead.
    Expected<serialization::Value> invoke(EditorSession& session, std::string_view id);

private:
    [[nodiscard]] Expected<void> check(EditorSession& session, const CommandDesc& command) const;

    std::map<std::string, CommandDesc, std::less<>> m_commands;
    std::map<std::string, std::function<bool(EditorSession&)>, std::less<>> m_prompts;
    // How many executes are running, to tell a command's own transaction from an interaction's.
    std::size_t m_depth = 0;
};

} // namespace ith
