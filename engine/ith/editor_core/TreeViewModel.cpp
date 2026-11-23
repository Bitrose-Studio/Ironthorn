#include "ith/editor_core/TreeViewModel.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/StringUtils.h"

#include <algorithm>
#include <utility>

namespace ith {

void TreeViewModel::setItems(std::vector<TreeItem> items) {
    m_items = std::move(items);
    m_parents.assign(m_items.size(), m_items.size());
    m_index.clear();
    m_index.reserve(m_items.size());
    // The last item seen at each depth: the parent of the next item one deeper.
    std::vector<std::size_t> path;
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        const std::uint32_t depth = m_items[i].depth;
        [[maybe_unused]] const bool validDepth = depth <= path.size();
        [[maybe_unused]] const bool unique = m_index.emplace(m_items[i].key, i).second;
        ITH_ASSERT_MSG(validDepth && unique, "TreeViewModel: items must be in tree order with unique keys");
        const std::size_t level = std::min<std::size_t>(depth, path.size());
        path.resize(level);
        if (level > 0) {
            m_parents[i] = path.back();
        }
        path.push_back(i);
    }
    std::erase_if(m_collapsed, [this](std::uint64_t key) { return !m_index.contains(key); });
    m_dirty = true;
}

std::optional<std::size_t> TreeViewModel::find(std::uint64_t key) const {
    const auto found = m_index.find(key);
    return found != m_index.end() ? std::optional<std::size_t>(found->second) : std::nullopt;
}

std::optional<std::size_t> TreeViewModel::parentOf(std::size_t item) const {
    ITH_ASSERT(item < m_parents.size());
    if (item >= m_parents.size() || m_parents[item] == m_items.size()) {
        return std::nullopt;
    }
    return m_parents[item];
}

bool TreeViewModel::isInSubtree(std::size_t item, std::size_t ancestor) const {
    for (std::optional<std::size_t> at = item; at.has_value(); at = parentOf(*at)) {
        if (*at == ancestor) {
            return true;
        }
    }
    return false;
}

void TreeViewModel::setFilter(std::string text) {
    if (text != m_filter) {
        m_filter = std::move(text);
        m_dirty = true;
    }
}

void TreeViewModel::setExpanded(std::uint64_t key, bool expanded) {
    const bool changed = expanded ? m_collapsed.erase(key) > 0 : m_collapsed.insert(key).second;
    m_dirty = m_dirty || changed;
}

void TreeViewModel::reveal(std::uint64_t key) {
    const std::optional<std::size_t> item = find(key);
    if (!item) {
        return;
    }
    for (std::optional<std::size_t> at = parentOf(*item); at.has_value(); at = parentOf(*at)) {
        setExpanded(m_items[*at].key, true);
    }
}

const std::vector<TreeRow>& TreeViewModel::rows() {
    if (m_dirty) {
        rebuild();
    }
    return m_rows;
}

std::optional<std::size_t> TreeViewModel::rowOf(std::uint64_t key) {
    const std::optional<std::size_t> item = find(key);
    if (!item) {
        return std::nullopt;
    }
    const std::vector<TreeRow>& shown = rows();
    const auto found = std::ranges::find_if(shown, [&](const TreeRow& row) { return row.item == *item; });
    return found != shown.end() ? std::optional<std::size_t>(static_cast<std::size_t>(found - shown.begin()))
                                : std::nullopt;
}

std::vector<std::uint64_t> TreeViewModel::range(std::uint64_t from, std::uint64_t to) {
    const std::optional<std::size_t> first = rowOf(from);
    const std::optional<std::size_t> last = rowOf(to);
    std::vector<std::uint64_t> keys;
    if (!first || !last) {
        return keys;
    }
    const std::size_t low = std::min(*first, *last);
    const std::size_t high = std::max(*first, *last);
    for (std::size_t row = low; row <= high; ++row) {
        keys.push_back(m_items[m_rows[row].item].key);
    }
    return keys;
}

bool TreeViewModel::setSelected(std::span<const std::uint64_t> keys) {
    if (std::ranges::equal(keys, m_selected)) {
        return false;
    }
    m_selected.assign(keys.begin(), keys.end());
    m_selectedSet = std::unordered_set<std::uint64_t>(keys.begin(), keys.end());
    return true;
}

void TreeViewModel::rebuild() {
    m_dirty = false;
    ++m_rebuilds;
    m_rows.clear();
    const std::size_t count = m_items.size();
    const auto hasChildItems = [&](std::size_t i) { return i + 1 < count && m_items[i + 1].depth > m_items[i].depth; };
    // Skips the subtree of item i: the next item at its depth or above.
    const auto afterSubtree = [&](std::size_t i) {
        std::size_t next = i + 1;
        while (next < count && m_items[next].depth > m_items[i].depth) {
            ++next;
        }
        return next;
    };

    if (m_filter.empty()) {
        for (std::size_t i = 0; i < count;) {
            const bool children = hasChildItems(i);
            const bool expanded = children && isExpanded(m_items[i].key);
            m_rows.push_back(
                TreeRow{.item = i, .depth = m_items[i].depth, .hasChildren = children, .expanded = expanded});
            i = expanded ? i + 1 : afterSubtree(i);
        }
        return;
    }

    const std::string needle = toLowerAscii(m_filter);
    std::vector<bool> matches(count);
    std::vector<bool> matchBelow(count); // a strict descendant matches
    for (std::size_t i = count; i-- > 0;) {
        matches[i] = toLowerAscii(m_items[i].label).find(needle) != std::string::npos;
        if ((matches[i] || matchBelow[i]) && m_parents[i] != count) {
            matchBelow[m_parents[i]] = true;
        }
    }
    for (std::size_t i = 0; i < count;) {
        if (!matches[i] && !matchBelow[i]) {
            i = afterSubtree(i);
            continue;
        }
        m_rows.push_back(TreeRow{.item = i,
                                 .depth = m_items[i].depth,
                                 .hasChildren = matchBelow[i],
                                 .expanded = matchBelow[i],
                                 .matches = matches[i]});
        i = matchBelow[i] ? i + 1 : afterSubtree(i);
    }
}

} // namespace ith
