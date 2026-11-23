#include "ith/editor_core/OutlinerModel.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Hash.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

Value idsValue(std::span<const Uuid> ids) {
    Value list = Value::array();
    for (const Uuid& id : ids) {
        list.elements().push_back(Value::string(id.toString()));
    }
    return list;
}

Expected<void> run(EditorSession& session, std::string_view command, const Value& args) {
    Expected<Value> result = session.execute(command, args);
    if (!result) {
        return makeUnexpected(std::move(result).error());
    }
    return {};
}

bool contains(std::span<const Uuid> ids, const Uuid& id) {
    return std::ranges::any_of(ids, [&id](const Uuid& other) { return other == id; });
}

void appendEntries(const World& world, Entity entity, std::uint32_t depth, bool parentDisabled,
                   std::vector<TreeItem>& items, std::vector<OutlinerEntry>& entries) {
    const auto* stable = world.get<const StableId>(entity);
    if (stable == nullptr) {
        return;
    }
    const auto* label = world.get<const EditorLabel>(entity);
    OutlinerEntry entry{.id = stable->id,
                        .label = label != nullptr ? label->name : stable->id.toString(),
                        .disabled = world.has<Disabled>(entity)};
    entry.disabledInHierarchy = parentDisabled || entry.disabled;
    items.push_back(TreeItem{.key = OutlinerModel::keyOf(stable->id), .depth = depth, .label = entry.label});
    const bool disabled = entry.disabledInHierarchy;
    entries.push_back(std::move(entry));
    if (const auto* children = world.get<const Children>(entity)) {
        for (const Entity child : children->entities) {
            appendEntries(world, child, depth + 1, disabled, items, entries);
        }
    }
}

} // namespace

OutlinerModel::OutlinerModel(EditorSession& session) : m_session(&session) {}

std::uint64_t OutlinerModel::keyOf(const Uuid& id) noexcept {
    return hashCombine(id.high(), id.low());
}

void OutlinerModel::update() {
    const EditorChanges& changes = m_session->document().changes();
    if (changes.structure() != m_seenStructure || changes.document() != m_seenDocument) {
        m_seenStructure = changes.structure();
        m_seenDocument = changes.document();
        rebuild();
        // Rebuilding drops keys; the selection is mirrored again below.
        m_seenSelection = std::numeric_limits<std::uint64_t>::max();
    }
    if (changes.selection() != m_seenSelection) {
        m_seenSelection = changes.selection();
        syncSelection();
    }
}

void OutlinerModel::rebuild() {
    const World& world = m_session->document().world();
    std::vector<TreeItem> items;
    m_entries.clear();
    world.forEachRoot([&](Entity root) { appendEntries(world, root, 0, false, items, m_entries); });
    m_tree.setItems(std::move(items));
    if (m_renaming.has_value() && find(*m_renaming) == nullptr) {
        cancelRename();
    }
}

void OutlinerModel::syncSelection() {
    std::vector<std::uint64_t> keys;
    for (const Uuid& id : m_session->document().selection().entities()) {
        if (find(id) != nullptr) {
            keys.push_back(keyOf(id));
            // A selected entity's row is shown, as when it is selected in the viewport.
            m_tree.reveal(keyOf(id));
        }
    }
    m_tree.setSelected(keys);
}

std::optional<std::size_t> OutlinerModel::itemOf(const Uuid& id) const {
    const std::optional<std::size_t> item = m_tree.find(keyOf(id));
    if (!item || !(m_entries[*item].id == id)) {
        return std::nullopt;
    }
    return item;
}

const OutlinerEntry* OutlinerModel::find(const Uuid& id) const {
    const std::optional<std::size_t> item = itemOf(id);
    return item ? &m_entries[*item] : nullptr;
}

Expected<void> OutlinerModel::click(const Uuid& id, OutlinerClick how) {
    std::vector<Uuid> ids;
    std::string_view mode = "replace";
    if (how == OutlinerClick::Range && m_anchor.has_value() && find(*m_anchor) != nullptr) {
        for (const std::uint64_t key : m_tree.range(keyOf(*m_anchor), keyOf(id))) {
            const std::optional<std::size_t> item = m_tree.find(key);
            if (item.has_value() && !(m_entries[*item].id == id)) {
                ids.push_back(m_entries[*item].id);
            }
        }
    } else if (how == OutlinerClick::Toggle) {
        mode = "toggle";
        m_anchor = id;
    } else {
        m_anchor = id;
    }
    // The clicked entity last: it becomes primary.
    ids.push_back(id);
    Value args = Value::object();
    args.set("entities", idsValue(ids));
    args.set("mode", Value::string(std::string(mode)));
    return run(*m_session, "ith.selection.set", args);
}

bool OutlinerModel::beginRename() {
    const std::optional<Uuid> primary = m_session->document().selection().primary();
    return primary.has_value() && beginRename(*primary);
}

bool OutlinerModel::beginRename(const Uuid& id) {
    const OutlinerEntry* entry = find(id);
    if (entry == nullptr) {
        return false;
    }
    m_renaming = id;
    m_renameText = entry->label;
    m_tree.reveal(keyOf(id));
    return true;
}

Expected<void> OutlinerModel::commitRename() {
    if (!m_renaming.has_value()) {
        return makeError("editor.outliner.not_renaming", "no rename is in progress");
    }
    Value args = Value::object();
    args.set("id", Value::string(m_renaming->toString()));
    args.set("label", Value::string(m_renameText));
    cancelRename();
    return run(*m_session, "ith.edit.rename", args);
}

void OutlinerModel::cancelRename() {
    m_renaming.reset();
    m_renameText.clear();
}

Expected<void> OutlinerModel::deleteSelection(bool withChildren) {
    return run(*m_session, withChildren ? "ith.edit.delete_with_children" : "ith.edit.delete", Value::object());
}

Expected<void> OutlinerModel::setEnabled(const Uuid& id, bool enabled) {
    const Selection& selection = m_session->document().selection();
    Value args = Value::object();
    args.set("ids", selection.contains(id) ? idsValue(selection.entities()) : idsValue(std::span(&id, 1)));
    args.set("enabled", Value::boolean(enabled));
    return run(*m_session, "ith.entity.set_enabled", args);
}

std::vector<Uuid> OutlinerModel::dragSet(const Uuid& grabbed) const {
    const Selection& selection = m_session->document().selection();
    if (!selection.contains(grabbed)) {
        return {grabbed};
    }
    std::vector<std::size_t> items;
    for (const Uuid& id : selection.entities()) {
        if (const std::optional<std::size_t> item = itemOf(id)) {
            items.push_back(*item);
        }
    }
    std::ranges::sort(items);
    std::vector<Uuid> dragged;
    for (const std::size_t item : items) {
        const bool belowAnother = std::ranges::any_of(
            items, [&](std::size_t other) { return other != item && m_tree.isInSubtree(item, other); });
        if (!belowAnother) {
            dragged.push_back(m_entries[item].id);
        }
    }
    return dragged;
}

Expected<void> OutlinerModel::canDrop(std::span<const Uuid> dragged, const std::optional<Uuid>& target,
                                      DropZone zone) const {
    if (dragged.empty()) {
        return makeError("editor.outliner.invalid_drop", "nothing is dragged");
    }
    for (const Uuid& id : dragged) {
        if (find(id) == nullptr) {
            return makeError("editor.outliner.invalid_drop", "a dragged entity is gone");
        }
    }
    if (zone == DropZone::End) {
        return {};
    }
    const std::optional<std::size_t> targetItem = target.has_value() ? itemOf(*target) : std::nullopt;
    if (!targetItem) {
        return makeError("editor.outliner.invalid_drop", "a drop on a row needs the row's entity");
    }
    for (const Uuid& id : dragged) {
        const std::optional<std::size_t> item = itemOf(id);
        if (!item || m_tree.isInSubtree(*targetItem, *item)) {
            return makeError("editor.outliner.invalid_drop", "an entity can't move into itself or below itself");
        }
    }
    return {};
}

std::optional<std::size_t> OutlinerModel::nextSibling(std::size_t item, std::span<const Uuid> skipped) const {
    const std::vector<TreeItem>& items = m_tree.items();
    std::size_t next = item + 1;
    while (next < items.size()) {
        while (next < items.size() && items[next].depth > items[item].depth) {
            ++next;
        }
        if (next >= items.size() || items[next].depth < items[item].depth) {
            return std::nullopt;
        }
        if (!contains(skipped, m_entries[next].id)) {
            return next;
        }
        ++next;
    }
    return std::nullopt;
}

Expected<void> OutlinerModel::drop(std::span<const Uuid> dragged, const std::optional<Uuid>& target, DropZone zone) {
    if (Expected<void> allowed = canDrop(dragged, target, zone); !allowed) {
        return allowed;
    }
    const std::optional<std::size_t> targetItem =
        zone != DropZone::End && target.has_value() ? itemOf(*target) : std::nullopt;
    // The new parent (null: the roots) and the sibling to place before (null: the end).
    std::optional<std::size_t> parent;
    std::optional<std::size_t> before;
    if (targetItem.has_value()) {
        if (zone == DropZone::Inside) {
            parent = targetItem;
        } else {
            parent = m_tree.parentOf(*targetItem);
            before = zone == DropZone::Before ? targetItem : nextSibling(*targetItem, dragged);
        }
    }

    // One entity dropped where it already is changes nothing.
    if (const std::optional<std::size_t> item = itemOf(dragged.front());
        dragged.size() == 1 && zone != DropZone::Inside && item.has_value() && m_tree.parentOf(*item) == parent &&
        nextSibling(*item, {}) == before) {
        return {};
    }

    Value args = Value::object();
    args.set("ids", idsValue(dragged));
    if (zone != DropZone::Inside) {
        args.set("before", before ? Value::string(m_entries[*before].id.toString()) : Value());
    }
    if (parent.has_value()) {
        args.set("parent", Value::string(m_entries[*parent].id.toString()));
        return run(*m_session, "ith.entity.attach", args);
    }
    return run(*m_session, "ith.entity.detach", args);
}

DropZone OutlinerModel::zoneAt(float fraction) noexcept {
    if (fraction < 0.25f) {
        return DropZone::Before;
    }
    if (fraction > 0.75f) {
        return DropZone::After;
    }
    return DropZone::Inside;
}

} // namespace ith
