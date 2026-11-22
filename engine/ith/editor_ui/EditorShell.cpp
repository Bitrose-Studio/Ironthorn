#include "ith/editor_ui/EditorShell.h"

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/Icons.h"
#include "ith/file_system/Path.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace ith {
namespace {

// Where the built-in commands appear in their menus, as in Unreal's editor; other commands of a
// menu's category follow by id.
constexpr std::array<std::string_view, 13> kMenuOrder = {
    "ith.file.new_level", "ith.file.open_level", "ith.file.save",
    "ith.file.save_as",   "ith.edit.undo",       "ith.edit.redo",
    "ith.edit.duplicate", "ith.edit.delete",     "ith.edit.delete_with_children",
    "ith.edit.rename",    "ith.edit.select_all", "ith.edit.select_none",
    "ith.selection.set",
};

std::size_t menuRank(std::string_view id) {
    return static_cast<std::size_t>(std::ranges::find(kMenuOrder, id) - kMenuOrder.begin());
}

void drawMenu(EditorSession& session, const Keymap& keymap, EditorShellState& state, std::string_view menu) {
    for (const std::string& id : menuCommands(session, menu)) {
        const CommandDesc* command = session.commands().find(id);
        const Icon* icon = commandIcon(id);
        const std::string label =
            (icon != nullptr ? iconText(*icon) + "  " : std::string()) + command->displayName + "###" + id;
        const std::vector<Chord> chords = keymap.chordsOf(id);
        const std::string shortcut = chords.empty() ? std::string() : chordLabel(chords.front());
        const Expected<void> enabled = session.commands().canExecute(session, id);
        if (ImGui::MenuItem(label.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled.hasValue())) {
            runMenuCommand(session, id, state);
        }
        const std::string& tooltip = enabled ? command->tooltip : enabled.error().message;
        if (!tooltip.empty()) {
            ImGui::SetItemTooltip("%s", tooltip.c_str());
        }
    }
}

void drawStatusBar(const EditorSession& session, const EditorShellState& state, float height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height});
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    ImGui::SetNextWindowViewport(viewport->ID);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // NOLINTEND(bugprone-signed-bitwise)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, style.WindowPadding.y * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("Status bar###ith.status_bar", nullptr, kFlags)) {
        const std::string title = documentTitle(session);
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (state.lastResult) {
            ImGui::TextUnformatted(state.status.c_str());
        } else {
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", state.status.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace

std::vector<std::string> menuCommands(const EditorSession& session, std::string_view menu) {
    std::vector<const CommandDesc*> commands;
    for (const CommandDesc* command : session.commands().commands()) {
        if (command->category == menu) {
            commands.push_back(command);
        }
    }
    // commands() is ordered by id, so a stable sort keeps that order among the unranked ones.
    std::ranges::stable_sort(
        commands, [](const CommandDesc* a, const CommandDesc* b) { return menuRank(a->id) < menuRank(b->id); });
    std::vector<std::string> ids;
    ids.reserve(commands.size());
    for (const CommandDesc* command : commands) {
        ids.push_back(command->id);
    }
    return ids;
}

std::string chordLabel(const Chord& chord) {
    std::string text = toString(chord);
    // The stable control name of a key starts with its device ("Keyboard.Z"); menus show the key.
    constexpr std::string_view kDevice = "Keyboard.";
    if (const std::size_t at = text.find(kDevice); at != std::string::npos) {
        text.erase(at, kDevice.size());
    }
    return text;
}

std::string documentTitle(const EditorSession& session) {
    const EditorDocument& document = session.document();
    std::string title = document.path().empty() ? std::string("Untitled") : std::string(fs::fileName(document.path()));
    if (document.dirty()) {
        title += " *";
    }
    return title;
}

void runMenuCommand(EditorSession& session, std::string_view id, EditorShellState& state) {
    const CommandDesc* command = session.commands().find(id);
    const Expected<serialization::Value> result = session.execute(id);
    state.lastCommand = std::string(id);
    if (result) {
        state.lastResult = {};
        state.status = command != nullptr ? command->displayName : std::string(id);
    } else {
        state.lastResult = makeUnexpected(result.error());
        state.status = result.error().message;
    }
}

void drawEditorShell(EditorSession& session, const Keymap& keymap, EditorShellState& state) {
    if (ImGui::BeginMainMenuBar()) {
        for (const std::string_view menu : kShellMenus) {
            const std::string name(menu);
            if (ImGui::BeginMenu(name.c_str())) {
                drawMenu(session, keymap, state, menu);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMainMenuBar();
    }
    const float statusHeight = ImGui::GetFrameHeight();
    drawDockSpace(statusHeight);
    drawStatusBar(session, state, statusHeight);
}

} // namespace ith
