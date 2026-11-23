#pragma once

// ith::EditorChanges: what changed in the editor document, as generation counters that view models
// poll once per frame (ADR-0037 rule 13). There are no callbacks.
//
//   if (changes.structure() != m_seenStructure) { rebuildOutliner(); m_seenStructure = changes.structure(); }
//   if (changes.values() != m_seenValues) {
//       const auto touched = changes.touchedSince(m_seenValues); // nullopt: refresh everything
//       m_seenValues = changes.values();
//       ...
//   }
//
// - structure: entities spawned or despawned, parents, sibling order, the set of components of an
//   entity, and Outliner labels;
// - values: component values, with the stable ids of the entities each bump touched;
// - selection: the selected entities or the primary one;
// - document: the open level, its path and its dirty state.
//
// Each panel keeps the `values` generation it last saw and asks which entities were touched since,
// so several panels read the same changes. The log of touched ids keeps the last kTouchedLogSize
// bumps; a panel that fell further behind refreshes everything.
//
// Transactions, undo and redo bump them; the editor's idle policy runs a frame when any changes.
// Main thread only, like the document.

#include "ith/foundation/Uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ith {

class EditorChanges {
public:
    // How many `values` bumps the touched-id log keeps.
    static constexpr std::size_t kTouchedLogSize = 4096;

    [[nodiscard]] std::uint64_t structure() const noexcept { return m_structure; }

    [[nodiscard]] std::uint64_t values() const noexcept { return m_values; }

    [[nodiscard]] std::uint64_t selection() const noexcept { return m_selection; }

    [[nodiscard]] std::uint64_t document() const noexcept { return m_document; }

    // The sum of the counters: changes when any of them does.
    [[nodiscard]] std::uint64_t any() const noexcept { return m_structure + m_values + m_selection + m_document; }

    void bumpStructure() noexcept { ++m_structure; }

    void bumpSelection() noexcept { ++m_selection; }

    void bumpDocument() noexcept { ++m_document; }

    // Bumps `values` and logs `entity` as touched by this bump.
    void bumpValues(const Uuid& entity) {
        ++m_values;
        m_touched.push_back(Touch{.generation = m_values, .entity = entity});
        if (m_touched.size() > kTouchedLogSize) {
            m_forgotten = m_touched.front().generation;
            m_touched.pop_front();
        }
    }

    // The stable ids of the entities touched by the `values` bumps after generation `since`, each
    // once, in first-touched order; nullopt when the log no longer reaches back to `since`.
    [[nodiscard]] std::optional<std::vector<Uuid>> touchedSince(std::uint64_t since) const {
        if (since < m_forgotten) {
            return std::nullopt;
        }
        std::vector<Uuid> ids;
        for (const Touch& touch : m_touched) {
            if (touch.generation > since &&
                std::ranges::none_of(ids, [&touch](const Uuid& id) { return id == touch.entity; })) {
                ids.push_back(touch.entity);
            }
        }
        return ids;
    }

private:
    struct Touch {
        std::uint64_t generation = 0;
        Uuid entity;
    };

    std::uint64_t m_structure = 0;
    std::uint64_t m_values = 0;
    std::uint64_t m_selection = 0;
    std::uint64_t m_document = 0;
    std::deque<Touch> m_touched;
    // The newest generation dropped from the log; asking from before it can't be answered.
    std::uint64_t m_forgotten = 0;
};

} // namespace ith
