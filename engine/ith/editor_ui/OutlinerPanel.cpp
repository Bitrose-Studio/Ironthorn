#include "ith/editor_ui/OutlinerPanel.h"

#include "ith/editor_core/AddShape.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/editor_ui/Icons.h"
#include "ith/foundation/Log.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <string>
#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(Outliner);

// Logs a change a panel could not make.
void report(const Expected<void>& result) {
    if (!result) {
        ITH_LOG_WARNING(Outliner, "{}", result.error().message);
    }
}

// The drag and drop payload of Outliner rows; the rows themselves are the panel's m_dragged.
constexpr const char* kRowsPayload = "ith.outliner.rows";

UiRect rectOfItem() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return UiRect{.x = min.x, .y = min.y, .width = max.x - min.x, .height = max.y - min.y};
}

OutlinerClick clickMode() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift) {
        return OutlinerClick::Range;
    }
    return io.KeyCtrl ? OutlinerClick::Toggle : OutlinerClick::Replace;
}

// Whether the row just drawn was clicked: on the press for a row not selected, and on the release
// without a drag for a selected one, so dragging a selected row keeps the whole selection.
bool selectsOnClick(bool selected) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!selected || io.KeyCtrl || io.KeyShift) {
        return ImGui::IsItemClicked(ImGuiMouseButton_Left);
    }
    const float threshold = io.MouseDragThreshold * io.MouseDragThreshold;
    return ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
           io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < threshold;
}

// Draws where a drop at `zone` of the hovered row lands: a line above or below it, or a frame
// around it.
void drawDropMarker(DropZone zone) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (zone == DropZone::Before) {
        draw->AddLine(min, ImVec2(max.x, min.y), color, 2.0f);
    } else if (zone == DropZone::After) {
        draw->AddLine(ImVec2(min.x, max.y), max, color, 2.0f);
    } else {
        draw->AddRect(min, max, color, 0.0f, 2.0f);
    }
}

} // namespace

OutlinerPanel::OutlinerPanel(EditorSession& session) : m_session(&session), m_model(session) {}

std::optional<UiRect> OutlinerPanel::rowRect(const Uuid& id) const {
    const auto found = m_rows.find(id);
    return found != m_rows.end() ? std::optional<UiRect>(found->second) : std::nullopt;
}

void OutlinerPanel::draw() {
    m_model.update();
    m_rows.clear();
    m_spawnedRows.clear();
    m_empty.reset();
    if (m_session->play() != nullptr) {
        ImGui::TextDisabled("%.*s", static_cast<int>(kOutlinerPlayNote.size()), kOutlinerPlayNote.data());
    }

    std::array<char, 256> search{};
    m_search.copy(search.data(), search.size() - 1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string hint = iconText(icons::kSearch) + "  Search";
    if (ImGui::InputTextWithHint("##search", hint.c_str(), search.data(), search.size())) {
        m_search = search.data();
        m_model.setFilter(m_search);
    }
    if (!ImGui::GetDragDropPayload()) {
        m_dragged.clear();
    }

    // NOLINTNEXTLINE(bugprone-signed-bitwise): ImGui's flags are int enums
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f))) {
        // No header row, as in Details: the columns are the label and the Enabled check box.
        if (ImGui::BeginTable("##outliner", 2, kTable)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_model.tree().rows().size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    drawRow(static_cast<std::size_t>(row));
                }
            }
            ImGui::EndTable();
        }
        // An arrow toggled while the clipper walked the rows changes them only now.
        if (m_toggled.has_value()) {
            m_model.tree().setExpanded(m_toggled->first, m_toggled->second);
            m_toggled.reset();
        }
        // The empty space below the rows: a click clears the selection, a drop makes the last roots.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##empty", ImVec2(std::max(avail.x, 1.0f), std::max(avail.y, ImGui::GetFrameHeight())));
        m_empty = rectOfItem();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (Expected<serialization::Value> cleared = m_session->execute("ith.edit.select_none"); !cleared) {
                ITH_LOG_WARNING(Outliner, "{}", cleared.error().message);
            }
        }
        if (m_session->play() == nullptr && ImGui::BeginDragDropTarget()) {
            if (m_model.canDrop(m_dragged, std::nullopt, DropZone::End)) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRowsPayload);
                if (payload != nullptr && payload->IsDelivery()) {
                    report(m_model.drop(m_dragged, std::nullopt, DropZone::End));
                }
            }
            ImGui::EndDragDropTarget();
        }
        drawContextMenu();
    }
    ImGui::EndChild();
}

void OutlinerPanel::drawContextMenu() {
    if (!ImGui::BeginPopupContextWindow("##outliner_context")) {
        return;
    }
    // While playing the structure is read-only; values of the game world can be kept (spec 3.12).
    if (m_session->play() != nullptr) {
        const Expected<void> enabled = m_session->commands().canExecute(*m_session, kKeepSimulationChanges);
        const std::string label = iconText(icons::kKeepChanges) + "  Keep Simulation Changes";
        if (ImGui::MenuItem(label.c_str(), nullptr, false, enabled.hasValue())) {
            // A user's gesture: the shell's prompt, when registered, shows the result.
            if (const Expected<serialization::Value> kept =
                    m_session->commands().invoke(*m_session, kKeepSimulationChanges);
                !kept) {
                ITH_LOG_WARNING(Outliner, "{}", kept.error().message);
            }
        }
        if (!enabled) {
            ImGui::SetItemTooltip("%s", enabled.error().message.c_str());
        }
        ImGui::EndPopup();
        return;
    }
    if (ImGui::BeginMenu("Add Shape")) {
        for (const ShapeChoice& choice : kShapeChoices) {
            const std::string name(choice.name);
            if (ImGui::MenuItem(name.c_str())) {
                // In front of the default viewport's camera; the new entity becomes the selection.
                const ViewportState* viewport = m_session->viewports().find(kDefaultViewport);
                const EditorCamera camera = viewport != nullptr ? viewport->camera : EditorCamera{};
                if (Expected<serialization::Value> created =
                        m_session->execute("ith.entity.create", addShapeArgs(choice.shape, camera));
                    !created) {
                    ITH_LOG_WARNING(Outliner, "{}", created.error().message);
                }
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void OutlinerPanel::drawRow(std::size_t rowIndex) {
    // Copies: a command below changes what the model shows from the next frame on.
    const TreeRow row = m_model.tree().rows()[rowIndex];
    const OutlinerEntry entry = m_model.entries()[row.item];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    // The row's tree key: stable across rebuilds and unique, also for spawned entities, which share
    // the nil id.
    ImGui::PushID(std::to_string(m_model.tree().items()[row.item].key).c_str());
    // Spawned entities are greyed: they have no stable id and can't be selected (spec 3.8).
    const bool dim = entry.disabledInHierarchy || !row.matches || entry.spawned;
    if (dim) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
    // While playing, rows select but neither rename nor move: the structure is read-only.
    const bool playing = m_session->play() != nullptr;
    if (m_model.renaming() == entry.id) {
        drawRename(entry);
    } else {
        drawNode(row, entry);
        if (!playing) {
            dragAndDrop(entry);
        }
    }
    if (dim) {
        ImGui::PopStyleColor();
    }

    ImGui::TableSetColumnIndex(1);
    bool enabled = !entry.disabled;
    // Adding or removing Disabled is structural, refused while playing (ADR-0053 decision 13).
    ImGui::BeginDisabled(playing || entry.spawned);
    if (ImGui::Checkbox("##enabled", &enabled)) {
        report(m_model.setEnabled(entry.id, enabled));
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", playing ? "Enabled: can't change while playing"
                                        : "Enabled: a disabled entity and everything below it are skipped by the game");
    ImGui::PopID();
}

void OutlinerPanel::drawNode(const TreeRow& row, const OutlinerEntry& entry) {
    const std::uint64_t key = m_model.tree().items()[row.item].key;
    const bool selected = m_model.tree().isSelected(key);
    // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (!row.hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    // NOLINTEND(bugprone-signed-bitwise)
    if (row.hasChildren) {
        ImGui::SetNextItemOpen(row.expanded);
    }
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", entry.label.c_str());
    if (entry.spawned) {
        m_spawnedRows.push_back(rectOfItem());
    } else {
        m_rows[entry.id] = rectOfItem();
    }
    if (ImGui::IsItemToggledOpen() && m_model.tree().filter().empty()) {
        m_toggled = std::pair<std::uint64_t, bool>(key, open); // applied after the rows: it changes them
    } else if (entry.spawned) {
        // Not selectable (it has no stable id); expanding it still works.
    } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
               m_session->play() == nullptr) {
        m_focusRename = m_model.beginRename(entry.id);
    } else if (selectsOnClick(selected)) {
        report(m_model.click(entry.id, clickMode()));
    }
}

void OutlinerPanel::dragAndDrop(const OutlinerEntry& entry) {
    if (ImGui::BeginDragDropSource()) {
        if (m_dragged.empty()) {
            m_dragged = m_model.dragSet(entry.id);
        }
        ImGui::SetDragDropPayload(kRowsPayload, nullptr, 0);
        if (m_dragged.size() == 1) {
            ImGui::TextUnformatted(entry.label.c_str());
        } else {
            ImGui::Text("%zu entities", m_dragged.size());
        }
        ImGui::EndDragDropSource();
    }
    if (!ImGui::BeginDragDropTarget()) {
        return;
    }
    const UiRect rect = m_rows[entry.id];
    const float fraction = rect.height > 0.0f ? (ImGui::GetMousePos().y - rect.y) / rect.height : 0.5f;
    const DropZone zone = OutlinerModel::zoneAt(fraction);
    if (m_model.canDrop(m_dragged, entry.id, zone)) {
        // NOLINTBEGIN(bugprone-signed-bitwise): ImGui's flags are int enums
        constexpr ImGuiDragDropFlags kAccept =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        // NOLINTEND(bugprone-signed-bitwise)
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRowsPayload, kAccept);
        if (payload != nullptr) {
            drawDropMarker(zone);
            if (payload->IsDelivery()) {
                report(m_model.drop(m_dragged, entry.id, zone));
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void OutlinerPanel::drawRename(const OutlinerEntry& entry) {
    std::array<char, 256> text{};
    m_model.renameText().copy(text.data(), text.size() - 1);
    if (m_focusRename) {
        ImGui::SetKeyboardFocusHere();
        m_focusRename = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##rename", text.data(), text.size(), ImGuiInputTextFlags_AutoSelectAll)) {
        m_model.renameText() = text.data();
    }
    m_rows[entry.id] = rectOfItem();
    if (ImGui::IsItemDeactivated()) {
        // Esc drops the new name; Enter or leaving the field keeps it, as in Unreal.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_model.cancelRename();
        } else {
            report(m_model.commitRename());
        }
    }
}

bool OutlinerPanel::beginRename() {
    m_focusRename = m_model.beginRename();
    return m_focusRename;
}

void registerOutliner(EditorSession& session, OutlinerPanel& panel) {
    session.panels().add({.id = "ith.outliner",
                          .displayName = "Outliner",
                          .defaultDock = DockPlace::Right,
                          .draw = [&panel](std::string_view /*instance*/) { panel.draw(); }});
    session.commands().setPrompt("ith.edit.rename", [&panel](EditorSession&) { return panel.beginRename(); });
}

} // namespace ith
