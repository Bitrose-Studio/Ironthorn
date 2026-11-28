#include "ith/editor_core/Transactions.h"

#include "ith/ecs/Hierarchy.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorChanges.h"
#include "ith/editor_core/EditorLabel.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/UndoHistory.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/reflection/TypeOf.h"

#include <algorithm>
#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(EditorTransactions);

// True when `change`, a Patch of only sets, sets nothing that `older` doesn't also set.
bool coveredBy(const ComponentChange& change, const ComponentChange& older) {
    if (older.kind != ComponentChange::Kind::Patch || older.type == nullptr || change.type == nullptr ||
        older.type->id != change.type->id) {
        return false;
    }
    return std::ranges::all_of(change.patch.entries(), [&](const PatchEntry& entry) {
        return entry.op == PatchOp::Set && std::ranges::any_of(older.patch.entries(), [&](const PatchEntry& old) {
                   return old.op == PatchOp::Set && old.path == entry.path;
               });
    });
}

} // namespace

TransactionScope::TransactionScope(TransactionScope&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)), m_epoch(other.m_epoch) {}

Transactions* TransactionScope::live() const noexcept {
    return m_owner != nullptr && m_owner->m_epoch == m_epoch ? m_owner : nullptr;
}

TransactionScope& TransactionScope::operator=(TransactionScope&& other) noexcept {
    if (this != &other) {
        if (m_owner != nullptr) {
            cancel();
        }
        m_owner = std::exchange(other.m_owner, nullptr);
        m_epoch = other.m_epoch;
    }
    return *this;
}

TransactionScope::~TransactionScope() {
    if (m_owner != nullptr) {
        cancel();
    }
}

Expected<void> TransactionScope::apply(std::span<const EntityPatch> patches) {
    if (live() == nullptr) {
        return makeError("editor.transaction.ended", "the transaction scope was committed or cancelled");
    }
    return m_owner->apply(patches);
}

void TransactionScope::commit() {
    Transactions* owner = live();
    m_owner = nullptr;
    if (owner != nullptr) {
        owner->end(false);
    }
}

void TransactionScope::cancel() {
    Transactions* owner = live();
    m_owner = nullptr;
    if (owner != nullptr) {
        owner->end(true);
    }
}

TransactionScope Transactions::begin(std::string name) {
    if (m_depth == 0) {
        m_name = std::move(name);
        m_cancelled = false;
        m_ids = {};
        m_inverse.clear();
        m_selectionBefore = m_selection->entities();
    }
    ++m_depth;
    return TransactionScope(*this, m_epoch);
}

void Transactions::cancelOpen() {
    if (m_depth == 0) {
        return;
    }
    m_depth = 1;
    end(true);
    ++m_epoch;
}

void Transactions::abandonOpen() noexcept {
    if (m_depth == 0) {
        return;
    }
    m_depth = 0;
    ++m_epoch;
    m_cancelled = false;
    m_name.clear();
    m_ids = {};
    m_inverse.clear();
    m_selectionBefore.clear();
}

Expected<void> Transactions::apply(std::span<const EntityPatch> patches) {
    ITH_ASSERT(m_depth > 0);
    // Check the targets before changing anything: toStored of the inverse would find a target
    // without a stable id only after the change.
    for (const EntityPatch& patch : patches) {
        if (patch.op != EntityOp::Spawn &&
            (patch.entity.worldId() != m_world->id() || m_world->get<const StableId>(patch.entity) == nullptr)) {
            const bool spawnedHere =
                patch.entity.worldId() == 0 && std::ranges::any_of(patches, [&](const EntityPatch& other) {
                    return other.op == EntityOp::Spawn && other.entity == patch.entity;
                });
            if (!spawnedHere) {
                return makeError("editor.no_stable_id", "the editor changes only entities that have a StableId");
            }
        }
    }
    Expected<std::vector<EntityPatch>> inverse = m_world->applyPatches(patches);
    if (!inverse) {
        return makeUnexpected(std::move(inverse).error());
    }
    Expected<std::vector<EntityPatch>> stored = toStored(*m_world, std::move(*inverse), m_ids, m_dropped);
    // The targets were checked, and every entity a Spawn creates carries its StableId's Add.
    ITH_ASSERT_MSG(stored.hasValue(), "editor: the inverse of a checked change has no stable ids");
    if (!stored) {
        return makeUnexpected(std::move(stored).error());
    }
    noteChanges(*stored, &m_ids);
    merge(std::move(*stored));
    if (m_selection->prune(*m_world)) {
        m_changes->bumpSelection();
    }
    return {};
}

void Transactions::merge(std::vector<EntityPatch> newer) {
    // Drop the newer sets an older inverse already restores, so only the first old value stays.
    for (EntityPatch& patch : newer) {
        if (patch.op != EntityOp::Modify) {
            continue;
        }
        std::erase_if(patch.changes, [&](const ComponentChange& change) {
            if (change.kind != ComponentChange::Kind::Patch) {
                return false;
            }
            return std::ranges::any_of(m_inverse, [&](const EntityPatch& older) {
                return older.op == EntityOp::Modify && older.entity == patch.entity &&
                       std::ranges::any_of(older.changes,
                                           [&](const ComponentChange& old) { return coveredBy(change, old); });
            });
        });
    }
    std::erase_if(newer, [](const EntityPatch& patch) {
        return patch.op == EntityOp::Modify && patch.changes.empty() && !patch.placeBefore.has_value();
    });
    newer.insert(newer.end(), std::make_move_iterator(m_inverse.begin()), std::make_move_iterator(m_inverse.end()));
    m_inverse = std::move(newer);
}

void Transactions::end(bool cancel) {
    ITH_ASSERT(m_depth > 0);
    m_cancelled = m_cancelled || cancel;
    if (--m_depth > 0) {
        return;
    }
    if (m_cancelled) {
        if (!m_inverse.empty()) {
            if (!replay(m_inverse, m_ids)) {
                m_history->clear();
            }
        }
        if (m_selection->replace(m_selectionBefore)) {
            m_changes->bumpSelection();
        }
        if (m_selection->prune(*m_world)) {
            m_changes->bumpSelection();
        }
    } else if (!m_inverse.empty()) {
        const bool wasDirty = m_history->dirty();
        m_history->push(UndoEntry{.name = std::move(m_name),
                                  .ids = std::move(m_ids),
                                  .patches = std::move(m_inverse),
                                  .selectionBefore = std::move(m_selectionBefore),
                                  .selectionAfter = m_selection->entities()});
        if (m_history->dirty() != wasDirty) {
            m_changes->bumpDocument();
        }
    }
    m_name.clear();
    m_ids = {};
    m_inverse.clear();
    m_selectionBefore.clear();
    m_cancelled = false;
}

Expected<void> Transactions::replay(std::vector<EntityPatch>& patches, StableIdTable& ids) {
    const std::vector<EntityPatch> live = toLive(*m_world, std::move(patches), ids);
    Expected<std::vector<EntityPatch>> inverse = m_world->applyPatches(live);
    patches.clear();
    ITH_ASSERT_MSG(inverse.hasValue() || m_foreignWriters,
                   "editor: an undo entry no longer applies; the world changed outside the editor's "
                   "transactions");
    if (!inverse) {
        if (m_foreignWriters) {
            ITH_LOG_WARNING(EditorTransactions, "a change of a running game world can't be put back: {}",
                            inverse.error().message);
        }
        return makeError("editor.undo.failed", "the change no longer applies: " + inverse.error().message);
    }
    StableIdTable newIds;
    Expected<std::vector<EntityPatch>> stored = toStored(*m_world, std::move(*inverse), newIds, m_dropped);
    if (!stored) {
        return makeError("editor.undo.failed", stored.error().message);
    }
    noteChanges(*stored, &newIds);
    patches = std::move(*stored);
    ids = std::move(newIds);
    return {};
}

Expected<void> Transactions::undo() {
    if (isOpen()) {
        return makeError("editor.transaction.open", "can't undo while a transaction is open");
    }
    UndoEntry* entry = m_history->undoEntry();
    if (entry == nullptr) {
        return makeError("editor.undo.nothing", "there is nothing to undo");
    }
    const bool wasDirty = m_history->dirty();
    if (Expected<void> replayed = replay(entry->patches, entry->ids); !replayed) {
        m_history->clear();
        return replayed;
    }
    const bool restored = m_selection->replace(entry->selectionBefore);
    if (m_selection->prune(*m_world) || restored) {
        m_changes->bumpSelection();
    }
    m_history->stepBack();
    if (m_history->dirty() != wasDirty) {
        m_changes->bumpDocument();
    }
    return {};
}

Expected<void> Transactions::redo() {
    if (isOpen()) {
        return makeError("editor.transaction.open", "can't redo while a transaction is open");
    }
    UndoEntry* entry = m_history->redoEntry();
    if (entry == nullptr) {
        return makeError("editor.redo.nothing", "there is nothing to redo");
    }
    const bool wasDirty = m_history->dirty();
    if (Expected<void> replayed = replay(entry->patches, entry->ids); !replayed) {
        m_history->clear();
        return replayed;
    }
    const bool restored = m_selection->replace(entry->selectionAfter);
    if (m_selection->prune(*m_world) || restored) {
        m_changes->bumpSelection();
    }
    m_history->stepForward();
    if (m_history->dirty() != wasDirty) {
        m_changes->bumpDocument();
    }
    return {};
}

void Transactions::noteChanges(std::span<const EntityPatch> patches, const StableIdTable* ids) {
    const std::uint64_t childOf = typeOf<ChildOf>().id;
    const std::uint64_t label = typeOf<EditorLabel>().id;
    bool structure = false;
    for (const EntityPatch& patch : patches) {
        if (patch.op != EntityOp::Modify || patch.placeBefore.has_value()) {
            structure = true;
            continue;
        }
        for (const ComponentChange& change : patch.changes) {
            const bool isStructural = change.kind != ComponentChange::Kind::Patch || change.type == nullptr ||
                                      change.type->id == childOf || change.type->id == label;
            if (isStructural) {
                structure = true;
            } else if (ids != nullptr && patch.entity.index() < ids->size()) {
                m_changes->bumpValues(ids->ids()[patch.entity.index()]);
            }
        }
    }
    if (structure) {
        m_changes->bumpStructure();
    }
}

} // namespace ith
