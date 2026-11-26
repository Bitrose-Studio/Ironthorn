#pragma once

// ith::OutlinerPanel: the Outliner (ADR-0008 rules 2 and 3, ADR-0040 rule 3), drawn from
// editor_core's OutlinerModel.
//
//   ith::OutlinerPanel outliner(session);
//   ith::registerOutliner(session, outliner); // the "ith.outliner" panel and the F2 rename prompt
//
// A search box, then the entities in Outliner order as a tree drawn with a list clipper, with an
// Enabled check box per row. A click selects (Ctrl toggles, Shift selects a range), a double click
// or F2 renames in place (Enter or leaving the field keeps the name, Esc drops it), dragging a row
// drops the selection before, after or inside another row, or below the rows to make them the last
// roots (OutlinerModel::zoneAt), and a click on the empty space clears the selection. Entities that
// are disabled, or below a disabled one, are dimmed. A right click opens the context menu, whose Add
// Shape submenu (Cube, Sphere, Plane, Cylinder) runs ith.entity.create with addShapeArgs
// (AddShape.h): the shape 2 m in front of the camera of ith.viewport.1, resting on the ground,
// selected. Every change runs a command through the model; a refused one is logged.

#include "ith/editor_core/OutlinerModel.h"
#include "ith/editor_ui/EditorUi.h"
#include "ith/foundation/Uuid.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ith {

class EditorSession;

class OutlinerPanel {
public:
    // `session` must outlive the panel.
    explicit OutlinerPanel(EditorSession& session);

    // Draws the panel's content inside its window. Main thread, inside a UI frame.
    void draw();

    [[nodiscard]] OutlinerModel& model() noexcept { return m_model; }

    // Starts renaming the primary selection in place, its field focused (the F2 prompt); false when
    // nothing is selected.
    bool beginRename();

    // Where the last draw put the row of `id`, or nullopt when it wasn't drawn.
    [[nodiscard]] std::optional<UiRect> rowRect(const Uuid& id) const;

    // Where the last draw put the empty space below the rows, or nullopt.
    [[nodiscard]] std::optional<UiRect> emptyRect() const { return m_empty; }

private:
    void drawRow(std::size_t rowIndex);
    void drawContextMenu();
    void drawRename(const OutlinerEntry& entry);
    void drawNode(const TreeRow& row, const OutlinerEntry& entry);
    void dragAndDrop(const OutlinerEntry& entry);

    EditorSession* m_session;
    OutlinerModel m_model;
    std::string m_search;
    // What the current drag carries.
    std::vector<Uuid> m_dragged;
    // The rename field was just opened and takes keyboard focus.
    bool m_focusRename = false;
    std::unordered_map<Uuid, UiRect> m_rows;
    std::optional<UiRect> m_empty;
};

// Registers the "ith.outliner" panel (Outliner, docked at the right) drawing `panel`, and makes it
// the prompt of ith.edit.rename, so F2 renames the primary selection in place. `panel` must outlive
// the session's use of both.
void registerOutliner(EditorSession& session, OutlinerPanel& panel);

} // namespace ith
