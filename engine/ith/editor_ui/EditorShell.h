#pragma once

// The editor's shell (ADR-0008 rule 2, ADR-0029, ADR-0040, ADR-0053): the main menu bar, the main
// toolbar, the dock space with its panels and the status bar. The File and Edit menus list the
// commands of those categories from the command registry, with the first chord the keymap binds to
// each; the Edit menu ends with Keep Simulation Changes. Choosing one runs it through the registry,
// as every action does (ADR-0037 rule 1). The Window menu lists the panels (checked when open) and
// the layout commands: Load Layout, Save Layout As (a dialog asks for the name and confirms an
// overwrite), Set as Default, Delete Layout (user layouts) and Reset Layout. A command that can't
// run now is shown disabled with its reason as the tooltip; one that fails reports its error in the
// status bar.
//
// The main toolbar, a strip under the menu bar over the whole window, holds the Play In Editor
// controls (docs/specs/play-in-editor.md 3.2; playToolbarControls). The status bar starts with the
// Content Drawer button, which runs ith.content.toggle_drawer on the press
// (docs/specs/content-browser.md 3.16), then shows the level's title, while a play session exists a
// chip with its state and the game's elapsed time (spec 3.13, PlayIndicators.h), and the last
// command's result.
//
// Play In Editor plays in the selected viewport (ADR-0053 decision 5; EditorSession::startPlay plays
// in ith.viewport.1 when the selected one's panel is closed). When a session starts in a viewport
// whose panel is closed, the shell opens it, once per session.

#include "ith/editor_ui/EditorUi.h"
#include "ith/editor_ui/Icons.h"
#include "ith/editor_ui/Theme.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Value.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class DockLayout;
class EditorSession;
class Keymap;
struct Chord;

// The menus of the main menu bar, in order; File and Edit list the commands of the category of their
// name, Window the panels and the layout commands.
inline constexpr std::array<std::string_view, 3> kShellMenus = {"File", "Edit", "Window"};

// One control of the main toolbar.
struct PlayToolbarControl {
    // The command it runs, without arguments, as a user's gesture (CommandRegistry::invoke). This
    // and `label` view static strings.
    std::string_view command;
    // "Play", "Simulate", "Stop", "Pause", "Resume", "Step", "Possess" or "Eject".
    std::string_view label;
    // The icon before the label.
    Icon icon;
    // Whether the command can run now (CommandRegistry::canExecute).
    bool enabled = false;
    // The label with the first chord the keymap binds ("Play (Alt+P)"), then, on a second line, the
    // command's tooltip, or why it can't run now.
    std::string tooltip;
    // Where the shell drew it (EditorShellState::toolbar); empty from playToolbarControls.
    UiRect rect;
};

// What the shell keeps between frames.
struct EditorShellState {
    // The status bar's message: the last command chosen from a menu or the toolbar, or why it failed.
    std::string status = "Ready";
    // The id of the last command chosen from a menu, and its result.
    std::string lastCommand;
    Expected<void> lastResult;
    // The Save Layout As dialog: open it on the next frame, its name field, and whether it asks to
    // overwrite an existing layout.
    bool openSaveLayout = false;
    std::string layoutName;
    bool confirmOverwrite = false;
    // The colours of the status bar's play chip: the application sets the UI's
    // (EditorUi::theme()).
    ThemeTokens theme = defaultDarkTheme();
    // The identity of the play session whose viewport the shell last made sure is open; 0 for none.
    std::uint64_t playViewportShown = 0;
    // The main toolbar's controls as the last frame drew them, with their rectangles.
    std::vector<PlayToolbarControl> toolbar;
};

// The controls the main toolbar shows now, left to right (spec 3.2): without a session Play and
// Simulate; with one Stop, Pause or Resume, Step and Eject or Possess, so Stop takes Play's place.
// Step is disabled unless the session is paused; every control is disabled while its command can't
// run.
[[nodiscard]] std::vector<PlayToolbarControl> playToolbarControls(EditorSession& session, const Keymap& keymap);

// The text a menu shows for a chord: "Ctrl+Z", "Ctrl+Alt+S", "Delete".
[[nodiscard]] std::string chordLabel(const Chord& chord);

// The status bar's text for the document: the level's file name, or "Untitled", with " *" when it
// has unsaved changes.
[[nodiscard]] std::string documentTitle(const EditorSession& session);

// The commands a shell menu lists, in menu order: the built-in ones as in Unreal's editor, then
// the rest of the category by id. The Edit menu draws Keep Simulation Changes after them, unless the
// Edit category lists it already.
[[nodiscard]] std::vector<std::string> menuCommands(const EditorSession& session, std::string_view menu);

// Draws the shell inside a UI frame (between EditorUi::newFrame and endFrame); `dock` draws the dock
// space and the panels.
void drawEditorShell(EditorSession& session, const Keymap& keymap, EditorShellState& state, DockLayout& dock);

// Runs the command `id` with `args` as the shell's menus do, recording the result in `state`. Without
// `args` it is a user's gesture (CommandRegistry::invoke); when the command's prompt reports a result
// of its own in `state` (Keep Simulation Changes, registerKeepSimulationChanges), that one stays.
void runMenuCommand(EditorSession& session, std::string_view id, EditorShellState& state,
                    const serialization::Value& args = {});

// The notice for a result of ith.play.keep_simulation_changes ({kept, skipped, ignored, warnings};
// spec 3.12): "Kept 2 components of 1 entity", then ", 1 skipped", ", 1 ignored", ", 1 warning"
// for the lists that aren't empty; "Nothing to keep" when nothing was kept.
[[nodiscard]] std::string keptChangesNotice(const serialization::Value& result);

// The prompt registerKeepSimulationChanges set, for as long as it lives: its destructor removes the
// prompt from the session's registry. Neither copied nor moved; keep it where it is created.
class KeepChangesPrompt {
public:
    KeepChangesPrompt(const KeepChangesPrompt&) = delete;
    KeepChangesPrompt& operator=(const KeepChangesPrompt&) = delete;
    KeepChangesPrompt(KeepChangesPrompt&&) = delete;
    KeepChangesPrompt& operator=(KeepChangesPrompt&&) = delete;
    ~KeepChangesPrompt();

private:
    friend KeepChangesPrompt registerKeepSimulationChanges(EditorSession& session, EditorShellState& state);
    KeepChangesPrompt(EditorSession& session, EditorShellState& state);

    EditorSession* m_session;
};

// Makes the user's gesture for Keep Simulation Changes (the K key, the Edit menu, the Outliner's
// context menu) show its result while the returned registration lives: the command's prompt runs it
// on the play selection, logs the notice (keptChangesNotice) in the category "Play" and each
// warning, skipped and ignored entry as a warning, and puts the notice (or the error) in `state`'s
// status bar. `session` and `state` must outlive the registration; destroy it first.
[[nodiscard]] KeepChangesPrompt registerKeepSimulationChanges(EditorSession& session, EditorShellState& state);

} // namespace ith
