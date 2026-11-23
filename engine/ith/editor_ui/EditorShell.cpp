#include "ith/editor_ui/EditorShell.h"

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/Icons.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/StringUtils.h"

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

serialization::Value argsWith(std::string_view key, std::string_view value) {
    serialization::Value args = serialization::Value::object();
    args.set(std::string(key), serialization::Value::string(std::string(value)));
    return args;
}

// A submenu of the named layouts that runs `command` with the chosen name; `userOnly` lists only the
// user's own layouts, `checked` marks one.
void drawLayoutMenu(EditorSession& session, EditorShellState& state, const char* label, std::string_view command,
                    const std::vector<LayoutListing>& layouts, bool userOnly, std::string_view checked = {}) {
    const bool any =
        std::ranges::any_of(layouts, [userOnly](const LayoutListing& layout) { return layout.user || !userOnly; });
    if (!ImGui::BeginMenu(label, any)) {
        return;
    }
    for (const LayoutListing& layout : layouts) {
        if (userOnly && !layout.user) {
            continue;
        }
        if (ImGui::MenuItem(layout.name.c_str(), nullptr, equalsIgnoreCaseAscii(layout.name, checked))) {
            runMenuCommand(session, command, state, argsWith("name", layout.name));
        }
    }
    ImGui::EndMenu();
}

// The Window menu (ADR-0040 rule 14): the panels, checked when open, then the layout commands.
void drawWindowMenu(EditorSession& session, EditorShellState& state) {
    LayoutSession& layouts = session.layouts();
    const PanelRegistry& panels = session.panels();
    for (const std::string& instance : panels.instanceIds()) {
        const std::string label = panels.title(instance) + "###" + instance;
        if (ImGui::MenuItem(label.c_str(), nullptr, isPanelOpen(layouts.current(), instance))) {
            runMenuCommand(session, "ith.window.open_panel", state, argsWith("panel", instance));
        }
    }
    if (!panels.instanceIds().empty()) {
        ImGui::Separator();
    }
    std::vector<LayoutListing> listings = layouts.list().valueOr(std::vector<LayoutListing>{});
    const std::string chosen = layouts.defaultName();
    // The default first, as Unreal lists its default layout first.
    std::ranges::stable_partition(
        listings, [&chosen](const LayoutListing& layout) { return equalsIgnoreCaseAscii(layout.name, chosen); });
    drawLayoutMenu(session, state, "Load Layout", "ith.window.load_layout", listings, false);
    if (ImGui::MenuItem("Save Layout As...")) {
        state.openSaveLayout = true;
        state.layoutName = layouts.name();
    }
    drawLayoutMenu(session, state, "Set as Default", "ith.window.set_default_layout", listings, false, chosen);
    drawLayoutMenu(session, state, "Delete Layout", "ith.window.delete_layout", listings, true);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset Layout")) {
        runMenuCommand(session, "ith.window.reset_layout", state);
    }
}

// The Save Layout As dialog: a name, then Save; an existing user layout asks to overwrite it.
void drawSaveLayoutDialog(EditorSession& session, EditorShellState& state) {
    constexpr const char* kPopup = "Save Layout As###ith.save_layout";
    if (state.openSaveLayout) {
        ImGui::OpenPopup(kPopup);
        state.openSaveLayout = false;
        state.confirmOverwrite = false;
    }
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    std::array<char, 65> name{};
    state.layoutName.copy(name.data(), name.size() - 1);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputText("Name", name.data(), name.size())) {
        state.layoutName = name.data();
        state.confirmOverwrite = false;
    }
    if (state.confirmOverwrite) {
        ImGui::Text("A layout called '%s' exists. Overwrite it?", state.layoutName.c_str());
    }
    if (ImGui::Button(state.confirmOverwrite ? "Overwrite" : "Save")) {
        serialization::Value args = argsWith("name", state.layoutName);
        args.set("overwrite", serialization::Value::boolean(state.confirmOverwrite));
        runMenuCommand(session, "ith.window.save_layout_as", state, args);
        if (state.lastResult) {
            ImGui::CloseCurrentPopup();
        } else if (state.lastResult.error().code == "editor.layout.exists") {
            state.confirmOverwrite = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    if (!state.lastResult && state.lastCommand == "ith.window.save_layout_as" && !state.confirmOverwrite) {
        ImGui::TextUnformatted(state.status.c_str());
    }
    ImGui::EndPopup();
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

void runMenuCommand(EditorSession& session, std::string_view id, EditorShellState& state,
                    const serialization::Value& args) {
    const CommandDesc* command = session.commands().find(id);
    // A menu item without arguments is a user's gesture: a prompt may ask for the input (Rename).
    const Expected<serialization::Value> result = args.kind() == serialization::Value::Kind::Null
                                                      ? session.commands().invoke(session, id)
                                                      : session.execute(id, args);
    state.lastCommand = std::string(id);
    if (result) {
        state.lastResult = {};
        state.status = command != nullptr ? command->displayName : std::string(id);
    } else {
        state.lastResult = makeUnexpected(result.error());
        state.status = result.error().message;
    }
}

void drawEditorShell(EditorSession& session, const Keymap& keymap, EditorShellState& state, DockLayout& dock) {
    if (ImGui::BeginMainMenuBar()) {
        for (const std::string_view menu : kShellMenus) {
            const std::string name(menu);
            if (ImGui::BeginMenu(name.c_str())) {
                if (menu == "Window") {
                    drawWindowMenu(session, state);
                } else {
                    drawMenu(session, keymap, state, menu);
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMainMenuBar();
    }
    drawSaveLayoutDialog(session, state);
    const float statusHeight = ImGui::GetFrameHeight();
    dock.draw(statusHeight);
    drawStatusBar(session, state, statusHeight);
}

} // namespace ith
