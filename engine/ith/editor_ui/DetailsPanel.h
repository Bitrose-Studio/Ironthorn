#pragma once

// ith::DetailsPanel: the Details panel (ADR-0008 rules 3 and 5, ADR-0040 rule 3), drawn from
// editor_core's DetailsModel.
//
//   ith::DetailsPanel details(session);
//   ith::registerDetails(session.panels(), details); // the "ith.details" panel
//
// The header holds the name (editable with one entity selected), the Enabled check box and Add
// Component, a searchable list of the component types the selection can take. Below it, each
// component is a collapsible group with a Remove button, and its properties are rows of a name and
// a value editor; the name's tooltip is the field's Tooltip attribute. Each value type has its
// editor, found by type id first, then by Meta(Editor = ...), then by kind:
//
// | Type | Editor |
// |---|---|
// | bool | check box |
// | integers, float, double | drag field, limited to the field's Min and Max |
// | std::string | text field, set when it loses focus or on Enter |
// | enums | combo box |
// | Vec2/3/4, IVec, UVec, DVec3 | one drag field per component, limited to Min and Max |
// | Quat | X, Y and Z in degrees (DetailsModel's eulerDegrees) |
// | Entity | a combo box with a search field over the entities by name, and None |
// | AssetRef, SoftAssetRef, Uuid | the id, read-only for now |
// | std::optional | a check box for present, and the value's editor |
// | std::vector, std::array, structs | a row that expands into its elements or fields; vectors have Add and Clear, their elements Remove |
//
// A drag field is one interaction (DetailsModel::beginEdit, previewValue each frame, commitEdit
// when released): the world changes live and the drag is one undo entry; Esc cancels it. Check
// boxes, combo boxes, pickers and text fields set their value with one command. Values that
// differ between the selected entities show "Multiple Values". A row that isn't its default shows
// a Reset to Default button. A refused change is logged.

#include "ith/editor_core/DetailsModel.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/math/Types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ith {

class EditorSession;
class PanelRegistry;

class DetailsPanel {
public:
    // `session` must outlive the panel.
    explicit DetailsPanel(EditorSession& session);

    // Draws the panel's content inside its window. Main thread, inside a UI frame.
    void draw();

    [[nodiscard]] DetailsModel& model() noexcept { return m_model; }

    // Where the last draw put the value editor of the row with `key` (DetailsRow::key), or nullopt
    // when it wasn't drawn.
    [[nodiscard]] std::optional<UiRect> valueRect(std::string_view key) const;

    // What a value editor did in a frame (DetailsPanel.cpp).
    struct EditEvents;

private:
    void drawHeader();
    void drawAddComponent();
    void drawRow(std::size_t index);
    void drawValue(std::size_t index);
    EditEvents drawEditor(const DetailsRow& row, const TypeInfo& type, void* value, bool mixed);
    EditEvents drawRotation(const DetailsRow& row, Quat& value, bool mixed);
    EditEvents drawOptional(const DetailsRow& row, const TypeInfo& type, void* value, bool mixed);
    EditEvents drawText(const DetailsRow& row, std::string& value, bool mixed);
    EditEvents drawEntity(const DetailsRow& row, Entity& value, bool mixed);

    DetailsModel m_model;
    std::unordered_map<std::string, UiRect> m_values;
    // The text field being edited and its text, kept while it is active.
    std::string m_textKey;
    std::array<char, 1024> m_text{};
    // The rotation being dragged, kept as Euler degrees so the drag doesn't drift.
    std::string m_eulerKey;
    Vec3 m_euler{0.0f};
    // The search fields of Add Component and of the entity picker.
    std::string m_componentSearch;
    std::string m_entitySearch;
};

// Registers the "ith.details" panel (Details, docked at the right) drawing `panel`, which must
// outlive the registry's use.
void registerDetails(PanelRegistry& panels, DetailsPanel& panel);

} // namespace ith
