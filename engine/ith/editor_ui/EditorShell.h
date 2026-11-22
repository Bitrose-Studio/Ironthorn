#pragma once

// The editor's shell (ADR-0008 rule 2, ADR-0029): the main menu bar, the dock space and the status
// bar. The File and Edit menus list the commands of those categories from the command registry,
// with the first chord the keymap binds to each; choosing one runs it through the registry, as
// every action does (ADR-0037 rule 1). A command that can't run now is shown disabled with its
// reason as the tooltip; one that fails reports its error in the status bar.

#include "ith/foundation/Expected.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class EditorSession;
class Keymap;
struct Chord;

// The menus of the main menu bar, in order; each lists the commands of the category of its name.
inline constexpr std::array<std::string_view, 2> kShellMenus = {"File", "Edit"};

// What the shell keeps between frames.
struct EditorShellState {
    // The status bar's message: the last command chosen from a menu, or why it failed.
    std::string status = "Ready";
    // The id of the last command chosen from a menu, and its result.
    std::string lastCommand;
    Expected<void> lastResult;
};

// The text a menu shows for a chord: "Ctrl+Z", "Ctrl+Alt+S", "Delete".
[[nodiscard]] std::string chordLabel(const Chord& chord);

// The status bar's text for the document: the level's file name, or "Untitled", with " *" when it
// has unsaved changes.
[[nodiscard]] std::string documentTitle(const EditorSession& session);

// The commands a shell menu lists, in menu order: the built-in ones as in Unreal's editor, then
// the rest of the category by id.
[[nodiscard]] std::vector<std::string> menuCommands(const EditorSession& session, std::string_view menu);

// Draws the shell inside a UI frame (between EditorUi::newFrame and endFrame).
void drawEditorShell(EditorSession& session, const Keymap& keymap, EditorShellState& state);

// Runs the command `id` as the shell's menus do, recording the result in `state`.
void runMenuCommand(EditorSession& session, std::string_view id, EditorShellState& state);

} // namespace ith
