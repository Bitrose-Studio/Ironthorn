#pragma once

// ith::Selection: the selected entities of the editor document, by stable id (ADR-0037 rule 11).
//
//   selection.replace({doorId, lampId}); // lampId is primary
//   selection.toggle(doorId);
//   const std::optional<ith::Uuid> primary = selection.primary();
//
// An ordered list without duplicates. The last one is primary: the gizmo pivot and the entity
// Details shows first. Entities are named by stable id, so the selection survives undo and redo of
// despawns, which give an entity a new handle. Ids that no longer resolve are pruned after every
// transaction, undo and redo. Selecting alone is not undoable: undo entries carry the selection
// before and after (ADR-0037 rule 12). Main thread only.

#include "ith/foundation/Uuid.h"

#include <optional>
#include <span>
#include <vector>

namespace ith {

class World;

class Selection {
public:
    // The selected entities in selection order.
    [[nodiscard]] const std::vector<Uuid>& entities() const noexcept { return m_entities; }

    // The primary entity: the last selected; nullopt when nothing is selected.
    [[nodiscard]] std::optional<Uuid> primary() const;

    [[nodiscard]] bool contains(const Uuid& id) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_entities.empty(); }

    // Each returns true when the selection changed.

    // Selects exactly `ids`, in order; a repeated id keeps its first place.
    bool replace(std::span<const Uuid> ids);
    // Adds `id` at the end, making it primary; an id already selected moves to the end.
    bool add(const Uuid& id);
    bool remove(const Uuid& id);
    // Removes `id` when it is selected, else adds it.
    bool toggle(const Uuid& id);
    bool clear();
    // Removes the ids that name no live entity of `world`.
    bool prune(const World& world);

private:
    std::vector<Uuid> m_entities;
};

} // namespace ith
