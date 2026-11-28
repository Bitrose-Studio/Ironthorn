#pragma once

// ith::OutlinerModel: the Outliner's view of the editor world (ADR-0008, ADR-0031, ADR-0037), as
// plain data and commands; editor_ui draws it.
//
//   ith::OutlinerModel outliner(session);
//   // every frame:
//   outliner.update();
//   for (const ith::TreeRow& row : outliner.tree().rows()) {
//       const ith::OutlinerEntry& entry = outliner.entries()[row.item];
//       ... draw entry.label at row.depth; a click calls outliner.click(entry.id, mode) ...
//   }
//
// The rows are the editor world's entities in Outliner order: each root in the World's root order,
// then its children in their order, depth-first (ADR-0031 rule 5). An entity shows its EditorLabel,
// or its stable id without one. Entities without a StableId are not shown: the editor gives every
// entity it creates one. The rows are rebuilt only when EditorChanges' structure or document counter
// moved, and the tree's selected keys follow the editor's Selection when its counter moved, so a
// value edit or a gizmo drag never rebuilds the Outliner.
//
// Play In Editor (ADR-0053 decisions 12, 14 and 15). The model lists the active world
// (EditorSession::active): the game world while a session runs, labelled with the editor twins'
// labels, and the play selection. Entities spawned during play have no stable id; they are listed as
// "Spawned <slot index>" with `spawned` set and can't be selected. The rows also rebuild when the
// active world's identity changes, so Play and Stop always rebuild them. Rename, Delete, Enabled and
// drops are refused by their commands while playing.
//
// Every change goes through a command (ADR-0037 rule 1): clicks through ith.selection.set, rename
// through ith.edit.rename, Delete through ith.edit.delete and ith.edit.delete_with_children, the
// Enabled toggle through ith.entity.set_enabled, and drag and drop through ith.entity.attach and
// ith.entity.detach with "before", so each drop is one undo entry. Main thread only.

#include "ith/editor_core/TreeViewModel.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ith {

class EditorSession;

// One entity of the Outliner, parallel to the tree's items.
struct OutlinerEntry {
    // The nil id for an entity spawned during play.
    Uuid id;
    std::string label;
    // Spawned in the game world during play: no stable id, shown greyed, not selectable.
    bool spawned = false;
    // The entity has the Disabled tag itself.
    bool disabled = false;
    // The entity or one of its ancestors is disabled: queries skip it (ADR-0031 rule 9).
    bool disabledInHierarchy = false;
};

// How a click changes the selection: a plain click replaces it, Ctrl + click toggles the entity,
// Shift + click selects the rows from the last clicked one to this one.
enum class OutlinerClick : std::uint8_t {
    Replace,
    Toggle,
    Range,
};

// Where a drop lands relative to the row under the mouse: before it or after it as its sibling,
// inside it as its last child, or at the end of the roots (empty space below the rows).
enum class DropZone : std::uint8_t {
    Before,
    After,
    Inside,
    End,
};

class OutlinerModel {
public:
    // `session` must outlive the model.
    explicit OutlinerModel(EditorSession& session);

    // Polls EditorChanges: rebuilds the rows after a structure or document change, and mirrors the
    // selection into the tree. Call once per frame before drawing.
    void update();

    [[nodiscard]] TreeViewModel& tree() noexcept { return m_tree; }

    [[nodiscard]] const TreeViewModel& tree() const noexcept { return m_tree; }

    // The entities, parallel to tree().items().
    [[nodiscard]] const std::vector<OutlinerEntry>& entries() const noexcept { return m_entries; }

    // The entry of the entity with `id`, or null when it isn't shown.
    [[nodiscard]] const OutlinerEntry* find(const Uuid& id) const;

    // The tree key of an entity.
    [[nodiscard]] static std::uint64_t keyOf(const Uuid& id) noexcept;

    // The search box: shows the entities whose label contains `text` and their ancestors.
    void setFilter(std::string text) { m_tree.setFilter(std::move(text)); }

    // How many times the rows were rebuilt; for tests and benchmarks.
    [[nodiscard]] std::uint64_t rebuilds() const noexcept { return m_rebuilds; }

    // Selects as a click on the row of `id` does (ith.selection.set). Fails with
    // "editor.outliner.not_selectable" for the nil id (a spawned entity's row); a Shift-click range
    // skips spawned rows.
    Expected<void> click(const Uuid& id, OutlinerClick how);

    // Inline rename. beginRename starts editing the label of the primary selection (the F2 prompt;
    // false when nothing is selected) or of `id`, with its current label as the text; commitRename
    // runs ith.edit.rename with the text and ends the edit; cancelRename ends it.
    bool beginRename();
    bool beginRename(const Uuid& id);

    [[nodiscard]] const std::optional<Uuid>& renaming() const noexcept { return m_renaming; }

    [[nodiscard]] std::string& renameText() noexcept { return m_renameText; }

    Expected<void> commitRename();
    void cancelRename();

    // Delete and Delete with children of the selection (ADR-0031 rule 7).
    Expected<void> deleteSelection(bool withChildren);

    // The Enabled toggle of a row: for the whole selection when `id` is selected, else for `id`.
    Expected<void> setEnabled(const Uuid& id, bool enabled);

    // What a drag that starts on the row of `grabbed` carries: the selection when `grabbed` is
    // selected, else `grabbed`; entities below another dragged one are left out (they move with it);
    // in Outliner order.
    [[nodiscard]] std::vector<Uuid> dragSet(const Uuid& grabbed) const;

    // Whether `dragged` may drop at `zone` of `target` (no target for DropZone::End): fails with
    // "editor.outliner.invalid_drop" when the target is dragged or below a dragged entity, or a
    // target is missing.
    [[nodiscard]] Expected<void> canDrop(std::span<const Uuid> dragged, const std::optional<Uuid>& target,
                                         DropZone zone) const;

    // Drops `dragged` at `zone` of `target`, as one ith.entity.attach or ith.entity.detach with
    // "before". A drop that would change nothing runs nothing.
    Expected<void> drop(std::span<const Uuid> dragged, const std::optional<Uuid>& target, DropZone zone);

    // The zone of a row from where the mouse is in it, 0 at its top and 1 at its bottom: the top
    // quarter is Before, the bottom quarter After, the middle Inside.
    [[nodiscard]] static DropZone zoneAt(float fraction) noexcept;

private:
    void rebuild();
    void syncSelection();
    [[nodiscard]] std::optional<std::size_t> itemOf(const Uuid& id) const;
    // The item of the next sibling of `item` that is not in `skipped`, or nullopt for none.
    [[nodiscard]] std::optional<std::size_t> nextSibling(std::size_t item, std::span<const Uuid> skipped) const;

    EditorSession* m_session;
    TreeViewModel m_tree;
    std::vector<OutlinerEntry> m_entries;
    std::uint64_t m_rebuilds = 0;
    std::uint64_t m_seenWorld = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenStructure = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenDocument = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t m_seenSelection = std::numeric_limits<std::uint64_t>::max();
    std::optional<Uuid> m_renaming;
    std::string m_renameText;
    // The last row clicked without Shift: where a Shift-click range starts.
    std::optional<Uuid> m_anchor;
};

} // namespace ith
