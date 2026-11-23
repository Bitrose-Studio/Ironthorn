#pragma once

// ith::TreeViewModel: the rows of a virtualized tree view (the Outliner, later the Content
// Browser's folders), as plain data the panel draws (ADR-0029 rule 1).
//
//   ith::TreeViewModel tree;
//   tree.setItems({{.key = 1, .depth = 0, .label = "Table"}, {.key = 2, .depth = 1, .label = "Lamp"}});
//   tree.setFilter("lamp");
//   for (const ith::TreeRow& row : tree.rows()) { ... draw tree.items()[row.item] at row.depth ... }
//
// The owner gives the items in tree order: each item, then its subtree depth-first, with its depth
// (0 for a root). Items are named by keys, stable across rebuilds, so expansion and selection
// survive them. rows() flattens the items the panel shows: the children of collapsed items are left
// out, and with a filter only the items whose label contains it (ASCII case-insensitive) and their
// ancestors are shown, the ancestors expanded. The rows are rebuilt only when something they depend
// on changed, so a panel can call rows() every frame and draw the visible part with a list clipper.
//
// Items are expanded unless collapsed; the collapsed keys of items that are gone are forgotten on
// setItems. Selection here is a list of keys for owners that keep no selection of their own; the
// Outliner mirrors the editor's Selection into it. Main thread only, like the panels.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ith {

struct TreeItem {
    // Stable and unique among the items.
    std::uint64_t key = 0;
    // 0 for a root; a child is one deeper than its parent, which comes before it.
    std::uint32_t depth = 0;
    // The text shown and searched.
    std::string label;
};

struct TreeRow {
    // The index of the item in items().
    std::size_t item = 0;
    std::uint32_t depth = 0;
    bool hasChildren = false;
    // Whether its children are shown: expanded, or an ancestor of a filter match.
    bool expanded = false;
    // False for an ancestor shown only because a descendant matches the filter.
    bool matches = true;
};

class TreeViewModel {
public:
    // Replaces the items. Precondition (asserted): tree order with valid depths and unique keys.
    void setItems(std::vector<TreeItem> items);

    [[nodiscard]] const std::vector<TreeItem>& items() const noexcept { return m_items; }

    // The index of the item with `key`, or nullopt.
    [[nodiscard]] std::optional<std::size_t> find(std::uint64_t key) const;

    // The index of the item's parent, or nullopt for a root. Precondition: item < items().size().
    [[nodiscard]] std::optional<std::size_t> parentOf(std::size_t item) const;

    // True when `ancestor` is `item` or one of its ancestors (both indices).
    [[nodiscard]] bool isInSubtree(std::size_t item, std::size_t ancestor) const;

    // Shows only the items whose label contains `text` and their ancestors; empty shows every item.
    void setFilter(std::string text);

    [[nodiscard]] const std::string& filter() const noexcept { return m_filter; }

    [[nodiscard]] bool isExpanded(std::uint64_t key) const { return !m_collapsed.contains(key); }

    void setExpanded(std::uint64_t key, bool expanded);

    // Expands every ancestor of the item with `key`, so its row is shown when it passes the filter.
    void reveal(std::uint64_t key);

    // The rows to draw, rebuilt when the items, the filter or the expansion changed.
    [[nodiscard]] const std::vector<TreeRow>& rows();

    // The row showing the item with `key`, or nullopt when it isn't shown.
    [[nodiscard]] std::optional<std::size_t> rowOf(std::uint64_t key);

    // The keys of the rows from the row of `from` to the row of `to`, both included, in row order
    // (a Shift-click range); empty when either isn't shown.
    [[nodiscard]] std::vector<std::uint64_t> range(std::uint64_t from, std::uint64_t to);

    [[nodiscard]] const std::vector<std::uint64_t>& selected() const noexcept { return m_selected; }

    [[nodiscard]] bool isSelected(std::uint64_t key) const { return m_selectedSet.contains(key); }

    // Replaces the selected keys; returns true when they changed.
    bool setSelected(std::span<const std::uint64_t> keys);

    // How often the rows were rebuilt, for tests of the dirty flag.
    [[nodiscard]] std::uint64_t rebuilds() const noexcept { return m_rebuilds; }

private:
    void rebuild();

    std::vector<TreeItem> m_items;
    std::vector<std::size_t> m_parents; // per item; m_items.size() for a root
    std::unordered_map<std::uint64_t, std::size_t> m_index;
    std::string m_filter;
    std::unordered_set<std::uint64_t> m_collapsed;
    std::vector<std::uint64_t> m_selected;
    std::unordered_set<std::uint64_t> m_selectedSet;
    std::vector<TreeRow> m_rows;
    bool m_dirty = true;
    std::uint64_t m_rebuilds = 0;
};

} // namespace ith
