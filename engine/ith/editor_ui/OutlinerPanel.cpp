#include "ith/editor_ui/OutlinerPanel.h"

#include "ith/editor_core/AddShape.h"
#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PanelRegistry.h"
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
    m_empty.reset();

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
        if (ImGui::BeginTable("##outliner", 2, kTable)) {
            ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_model.tree().rows().size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    drawRow(static_cast<std::size_t>(row));
                }
            }
            ImGui::EndTable();
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
        if (ImGui::BeginDragDropTarget()) {
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
    const std::string id = entry.id.toString();
    ImGui::PushID(id.c_str());
    const bool dim = entry.disabledInHierarchy || !row.matches;
    if (dim) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing);
    if (m_model.renaming() == entry.id) {
        drawRename(entry);
    } else {
        drawNode(row, entry);
        dragAndDrop(entry);
    }
    if (dim) {
        ImGui::PopStyleColor();
    }

    ImGui::TableSetColumnIndex(1);
    bool enabled = !entry.disabled;
    if (ImGui::Checkbox("##enabled", &enabled)) {
        report(m_model.setEnabled(entry.id, enabled));
    }
    ImGui::SetItemTooltip("Enabled: a disabled entity and everything below it are skipped by the game");
    ImGui::PopID();
}

void OutlinerPanel::drawNode(const TreeRow& row, const OutlinerEntry& entry) {
    const std::uint64_t key = OutlinerModel::keyOf(entry.id);
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
    m_rows[entry.id] = rectOfItem();
    if (ImGui::IsItemToggledOpen() && m_model.tree().filter().empty()) {
        m_model.tree().setExpanded(key, open);
    } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
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
