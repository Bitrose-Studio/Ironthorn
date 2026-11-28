#include "ith/editor_ui/EditorShell.h"

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorLayout.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/Keymap.h"
#include "ith/editor_core/LayoutSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/DockLayout.h"
#include "ith/editor_ui/Icons.h"
#include "ith/editor_ui/PlayIndicators.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/StringUtils.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ith {
namespace {

ITH_LOG_CATEGORY(Play);

// Where the built-in commands appear in their menus, as in Unreal's editor; other commands of a
// menu's category follow by id.
constexpr std::array<std::string_view, 13> kMenuOrder = {
    "ith.file.new_level", "ith.file.open_level", "ith.file.save",
    "ith.file.save_as",   "ith.edit.undo",       "ith.edit.redo",
    "ith.edit.duplicate", "ith.edit.delete",     "ith.edit.delete_with_children",
    "ith.edit.rename",    "ith.edit.select_all", "ith.edit.select_none",
    "ith.selection.set",
};

// The command of the status bar's Content Drawer button.
constexpr std::string_view kContentDrawerCommand = "ith.content.toggle_drawer";

// Every control the toolbar can show, with its icon: the one place labels get their icons, and the
// set every button's width covers, so Stop lands where Play was.
struct ToolbarChoice {
    std::string_view label;
    Icon icon;
};

constexpr std::array<ToolbarChoice, 8> kToolbarChoices{{
    {"Play", icons::kPlay},
    {"Simulate", icons::kSimulate},
    {"Stop", icons::kStop},
    {"Pause", icons::kPause},
    {"Resume", icons::kPlay},
    {"Step", icons::kStep},
    {"Possess", icons::kPossess},
    {"Eject", icons::kEject},
}};

std::size_t menuRank(std::string_view id) {
    return static_cast<std::size_t>(std::ranges::find(kMenuOrder, id) - kMenuOrder.begin());
}

ImVec4 toImGui(ThemeColor color) {
    const std::array<float, 4> channels = srgbColor(color);
    return {channels[0], channels[1], channels[2], channels[3]};
}

// One command as a menu item: its icon, display name and first chord, disabled with the reason as
// its tooltip while it can't run.
void drawMenuItem(EditorSession& session, const Keymap& keymap, EditorShellState& state, std::string_view id) {
    const CommandDesc* command = session.commands().find(id);
    if (command == nullptr) {
        return;
    }
    const Icon* icon = commandIcon(id);
    const std::string label =
        (icon != nullptr ? iconText(*icon) + "  " : std::string()) + command->displayName + "###" + std::string(id);
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

void drawMenu(EditorSession& session, const Keymap& keymap, EditorShellState& state, std::string_view menu) {
    const std::vector<std::string> ids = menuCommands(session, menu);
    for (const std::string& id : ids) {
        drawMenuItem(session, keymap, state, id);
    }
    // Keep Simulation Changes belongs to Play, but the Edit menu is where it is looked for (spec 3.12);
    // once only, should a later registration put it in the Edit category.
    if (menu == "Edit" && session.commands().find(kKeepSimulationChanges) != nullptr &&
        std::ranges::find(ids, kKeepSimulationChanges) == ids.end()) {
        ImGui::Separator();
        drawMenuItem(session, keymap, state, kKeepSimulationChanges);
    }
}

// The main toolbar (spec 3.2), `height` tall under the menu bar: the play controls, each a button of
// one width, so the controls that replace Play and Simulate keep their places.
void drawToolbar(EditorSession& session, const Keymap& keymap, EditorShellState& state, float height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize({viewport->WorkSize.x, height});
    ImGui::SetNextWindowViewport(viewport->ID);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // NOLINTEND(bugprone-signed-bitwise)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, style.WindowPadding.y * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("Main toolbar###ith.main_toolbar", nullptr, kFlags)) {
        float width = 0.0f;
        for (const ToolbarChoice& choice : kToolbarChoices) {
            const std::string text = iconText(choice.icon) + "  " + std::string(choice.label);
            width = std::max(width, ImGui::CalcTextSize(text.c_str()).x);
        }
        width += style.FramePadding.x * 2.0f;
        // A click runs once every control is drawn: it changes the session, and so the controls.
        state.toolbar = playToolbarControls(session, keymap);
        std::string_view clicked;
        for (PlayToolbarControl& control : state.toolbar) {
            if (&control != &state.toolbar.front()) {
                ImGui::SameLine();
            }
            const std::string label =
                iconText(control.icon) + "  " + std::string(control.label) + "###" + std::string(control.command);
            ImGui::BeginDisabled(!control.enabled);
            if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f))) {
                clicked = control.command;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", control.tooltip.c_str());
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            control.rect = {.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};
        }
        if (!clicked.empty()) {
            runMenuCommand(session, clicked, state);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// The status bar: the Content Drawer button (spec 3.16), the level's title, the play chip while a
// session exists (play spec 3.13) and the last command's result.
void drawStatusBar(EditorSession& session, const Keymap& keymap, EditorShellState& state, float height) {
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
        // Toggled on the press, as Ctrl + Space is: a release would first close the drawer as a click
        // outside it, then open it again.
        const std::string drawer = iconText(icons::kFolders) + " Content Drawer";
        ImGui::SmallButton(drawer.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            runMenuCommand(session, kContentDrawerCommand, state);
        }
        const std::vector<Chord> chords = keymap.chordsOf(kContentDrawerCommand);
        const std::string tooltip = "Show or hide the content drawer" +
                                    (chords.empty() ? std::string() : " (" + chordLabel(chords.front()) + ")");
        ImGui::SetItemTooltip("%s", tooltip.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        const std::string title = documentTitle(session);
        ImGui::TextUnformatted(title.c_str());
        if (const PlayIndicator indicator = playIndicator(session); indicator != PlayIndicator::None) {
            const std::string chip =
                std::string(playIndicatorLabel(indicator)) + " " + formatPlayTime(session.play()->elapsedSeconds());
            ImGui::SameLine();
            ImGui::TextColored(toImGui(playIndicatorColor(indicator, state.theme)), "%s", chip.c_str());
        }
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

// A session's play viewport (ADR-0053 decision 5) is opened once when its panel is closed; closed
// again during the session, it stays closed. EditorSession::startPlay chose it.
void showPlayViewport(EditorSession& session, EditorShellState& state) {
    const PlaySession* play = session.play();
    if (play == nullptr) {
        state.playViewportShown = 0;
        return;
    }
    if (state.playViewportShown == play->identity()) {
        return;
    }
    state.playViewportShown = play->identity();
    const std::string instance = Viewports::instanceId(play->viewport());
    if (session.panels().find(kViewportPanel) != nullptr && !isPanelOpen(session.layouts().current(), instance)) {
        if (const Expected<serialization::Value> opened =
                session.execute("ith.window.open_panel", argsWith("panel", instance));
            !opened) {
            ITH_LOG_WARNING(Play, "{}", opened.error().message);
        }
    }
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

// The size of the array member `key` of `result`; 0 when it is missing.
std::size_t countOf(const serialization::Value& result, std::string_view key) {
    const serialization::Value* list = result.kind() == serialization::Value::Kind::Object ? result.find(key) : nullptr;
    return list != nullptr && list->kind() == serialization::Value::Kind::Array ? list->elements().size() : 0;
}

// "<n> <singular>", or "<n> <plural>" unless n is 1.
std::string counted(std::size_t count, std::string_view singular, std::string_view plural) {
    return std::to_string(count) + " " + std::string(count == 1 ? singular : plural);
}

// The string member `key` of an object; empty when missing.
std::string memberText(const serialization::Value& value, std::string_view key) {
    const serialization::Value* member = value.kind() == serialization::Value::Kind::Object ? value.find(key) : nullptr;
    return member != nullptr && member->kind() == serialization::Value::Kind::String ? member->asString()
                                                                                     : std::string();
}

// Logs each warning, skipped and ignored entry of a Keep Simulation Changes result.
void logKeepDetails(const serialization::Value& result) {
    for (const std::string_view key : {std::string_view("skipped"), std::string_view("ignored")}) {
        const serialization::Value* list = result.find(key);
        if (list == nullptr || list->kind() != serialization::Value::Kind::Array) {
            continue;
        }
        for (const serialization::Value& entry : list->elements()) {
            const std::string component = memberText(entry, "component");
            ITH_LOG_WARNING(Play, "Keep Simulation Changes: {} {}{}{}: {}", key, memberText(entry, "id"),
                            component.empty() ? "" : " ", component, memberText(entry, "reason"));
        }
    }
    if (const serialization::Value* warnings = result.find("warnings");
        warnings != nullptr && warnings->kind() == serialization::Value::Kind::Array) {
        for (const serialization::Value& warning : warnings->elements()) {
            ITH_LOG_WARNING(Play, "Keep Simulation Changes: {}", memberText(warning, "message"));
        }
    }
}

} // namespace

std::vector<PlayToolbarControl> playToolbarControls(EditorSession& session, const Keymap& keymap) {
    std::vector<PlayToolbarControl> controls;
    const auto add = [&](std::string_view command, std::string_view label) {
        const CommandDesc* desc = session.commands().find(command);
        if (desc == nullptr) {
            return;
        }
        const Expected<void> enabled = session.commands().canExecute(session, command);
        const std::vector<Chord> chords = keymap.chordsOf(command);
        std::string tooltip = std::string(label);
        if (!chords.empty()) {
            tooltip += " (" + chordLabel(chords.front()) + ")";
        }
        tooltip += "\n" + (enabled ? desc->tooltip : enabled.error().message);
        const auto choice = std::ranges::find(kToolbarChoices, label, &ToolbarChoice::label);
        ITH_ASSERT_MSG(choice != kToolbarChoices.end(), "every toolbar label is in kToolbarChoices");
        if (choice == kToolbarChoices.end()) {
            return;
        }
        controls.push_back({.command = command,
                            .label = label,
                            .icon = choice->icon,
                            .enabled = enabled.hasValue(),
                            .tooltip = std::move(tooltip)});
    };
    const PlaySession* play = session.play();
    if (play == nullptr) {
        add("ith.play.play", "Play");
        add("ith.play.simulate", "Simulate");
        return controls;
    }
    add("ith.play.stop", "Stop");
    add("ith.play.pause", play->paused() ? "Resume" : "Pause");
    add("ith.play.step", "Step");
    add("ith.play.possess_or_eject", play->possessed() ? "Eject" : "Possess");
    return controls;
}

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
    // A prompt that reports its own result sets lastCommand (registerKeepSimulationChanges).
    state.lastCommand.clear();
    // A menu item without arguments is a user's gesture: a prompt may ask for the input (Rename).
    const Expected<serialization::Value> result = args.kind() == serialization::Value::Kind::Null
                                                      ? session.commands().invoke(session, id)
                                                      : session.execute(id, args);
    if (result && state.lastCommand == id) {
        return;
    }
    state.lastCommand = std::string(id);
    if (result) {
        state.lastResult = {};
        state.status = command != nullptr ? command->displayName : std::string(id);
    } else {
        state.lastResult = makeUnexpected(result.error());
        state.status = result.error().message;
    }
}

std::string keptChangesNotice(const serialization::Value& result) {
    const serialization::Value* kept =
        result.kind() == serialization::Value::Kind::Object ? result.find("kept") : nullptr;
    std::size_t components = 0;
    std::size_t entities = 0;
    if (kept != nullptr && kept->kind() == serialization::Value::Kind::Array) {
        for (const serialization::Value& entry : kept->elements()) {
            ++entities;
            components += countOf(entry, "components");
        }
    }
    std::string notice = entities == 0 ? std::string("Nothing to keep")
                                       : "Kept " + counted(components, "component", "components") + " of " +
                                             counted(entities, "entity", "entities");
    if (const std::size_t skipped = countOf(result, "skipped"); skipped > 0) {
        notice += ", " + std::to_string(skipped) + " skipped";
    }
    if (const std::size_t ignored = countOf(result, "ignored"); ignored > 0) {
        notice += ", " + std::to_string(ignored) + " ignored";
    }
    if (const std::size_t warnings = countOf(result, "warnings"); warnings > 0) {
        notice += ", " + counted(warnings, "warning", "warnings");
    }
    return notice;
}

KeepChangesPrompt registerKeepSimulationChanges(EditorSession& session, EditorShellState& state) {
    return {session, state};
}

KeepChangesPrompt::~KeepChangesPrompt() {
    m_session->commands().setPrompt(kKeepSimulationChanges, {});
}

KeepChangesPrompt::KeepChangesPrompt(EditorSession& session, EditorShellState& state) : m_session(&session) {
    session.commands().setPrompt(kKeepSimulationChanges, [&state](EditorSession& running) {
        const Expected<serialization::Value> result =
            running.execute(kKeepSimulationChanges, serialization::Value::object());
        state.lastCommand = std::string(kKeepSimulationChanges);
        if (!result) {
            ITH_LOG_WARNING(Play, "{}", result.error().message);
            state.lastResult = makeUnexpected(result.error());
            state.status = result.error().message;
            return true;
        }
        const std::string notice = keptChangesNotice(*result);
        ITH_LOG_INFO(Play, "Keep Simulation Changes: {}", notice);
        logKeepDetails(*result);
        state.lastResult = {};
        state.status = notice;
        return true;
    });
}

void drawEditorShell(EditorSession& session, const Keymap& keymap, EditorShellState& state, DockLayout& dock) {
    showPlayViewport(session, state);
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
    const float toolbarHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;
    drawToolbar(session, keymap, state, toolbarHeight);
    dock.draw(statusHeight, toolbarHeight);
    drawStatusBar(session, keymap, state, statusHeight);
}

} // namespace ith
