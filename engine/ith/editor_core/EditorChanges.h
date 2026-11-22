#pragma once

// ith::EditorChanges: what changed in the editor document, as generation counters that view models
// poll once per frame (ADR-0037 rule 13). There are no callbacks.
//
//   if (changes.structure() != m_seenStructure) { rebuildOutliner(); m_seenStructure = changes.structure(); }
//   for (const ith::Uuid& id : changes.takeTouched()) { refreshDetailsIfShowing(id); }
//
// - structure: entities spawned or despawned, parents, sibling order, the set of components of an
//   entity, and Outliner labels;
// - values: component values, with the stable ids of the entities touched since the last
//   takeTouched;
// - selection: the selected entities or the primary one;
// - document: the open level, its path and its dirty state.
//
// Transactions, undo and redo bump them; the editor's idle policy runs a frame when any changes.
// Main thread only, like the document.

#include "ith/foundation/Uuid.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace ith {

class EditorChanges {
public:
    [[nodiscard]] std::uint64_t structure() const noexcept { return m_structure; }

    [[nodiscard]] std::uint64_t values() const noexcept { return m_values; }

    [[nodiscard]] std::uint64_t selection() const noexcept { return m_selection; }

    [[nodiscard]] std::uint64_t document() const noexcept { return m_document; }

    // The sum of the counters: changes when any of them does.
    [[nodiscard]] std::uint64_t any() const noexcept { return m_structure + m_values + m_selection + m_document; }

    void bumpStructure() noexcept { ++m_structure; }

    void bumpSelection() noexcept { ++m_selection; }

    void bumpDocument() noexcept { ++m_document; }

    // Bumps `values` and remembers `entity` among the touched ones.
    void bumpValues(const Uuid& entity) {
        ++m_values;
        if (std::ranges::none_of(m_touched, [&entity](const Uuid& id) { return id == entity; })) {
            m_touched.push_back(entity);
        }
    }

    // The stable ids of the entities whose values changed since the last call, in first-touched
    // order, and forgets them.
    [[nodiscard]] std::vector<Uuid> takeTouched() { return std::exchange(m_touched, {}); }

private:
    std::uint64_t m_structure = 0;
    std::uint64_t m_values = 0;
    std::uint64_t m_selection = 0;
    std::uint64_t m_document = 0;
    std::vector<Uuid> m_touched;
};

} // namespace ith
