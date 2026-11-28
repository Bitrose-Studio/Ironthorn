#pragma once

// Transactions: how the editor changes its world (ADR-0037 rules 5 to 7, 10 and 12).
//
//   ith::TransactionScope scope = transactions.begin("Move");
//   if (!scope.apply(patches)) { return; }          // applied at once: a live preview
//   ...                                            // more apply calls while the drag goes on
//   scope.commit();                                // one undo entry; or scope.cancel()
//
// One transaction per interaction: a Details drag or a gizmo drag begins one, applies patches
// every frame and commits when it ends; Esc cancels it, which applies the collected inverse and
// records nothing. A scope destroyed without commit() cancels too, so an early return can't leave
// a half-made change in the history.
//
// Scopes nest: begin() while a transaction is open joins it. An inner commit() does nothing, an
// inner cancel() cancels the whole transaction when the outermost scope ends.
//
// The inverse of each apply goes in front of the ones before (StoredPatches.h, stable ids); then a
// `set` of an entity, component and path that an earlier inverse also sets is dropped, so a drag
// of a thousand frames keeps one old value per property. A transaction that changed nothing
// records nothing; the selection alone never makes an entry.
//
// Every apply, cancel, undo and redo bumps EditorChanges by what it touched, and prunes the
// selection of entities that are gone. Main thread only.

#include "ith/ecs/EntityPatch.h"
#include "ith/editor_core/StoredPatches.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Uuid.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ith {

class EditorChanges;
class Selection;
class Transactions;
class UndoHistory;
class World;

// A handle on the open transaction; move-only.
class TransactionScope {
public:
    TransactionScope(const TransactionScope&) = delete;
    TransactionScope& operator=(const TransactionScope&) = delete;
    TransactionScope(TransactionScope&& other) noexcept;
    TransactionScope& operator=(TransactionScope&& other) noexcept;
    // Cancels when neither commit() nor cancel() was called. Precondition: the Transactions it came
    // from still exists, unless release() was called.
    ~TransactionScope();

    // Applies `patches` to the world now, as one World::applyPatches, and adds their inverse to the
    // transaction. Fails with applyPatches' errors (the world is then unchanged and the transaction
    // goes on), "editor.no_stable_id" (a target without a StableId; nothing changes), and
    // "editor.transaction.ended" after commit or cancel.
    Expected<void> apply(std::span<const EntityPatch> patches);

    // Ends this scope; the outermost one records the transaction.
    void commit();

    // Ends this scope and cancels the transaction.
    void cancel();

    // True until commit() or cancel(). Precondition: the Transactions it came from still exists.
    [[nodiscard]] bool active() const noexcept { return live() != nullptr; }

    // Forgets the transaction without touching it: for a scope whose Transactions is gone, as a
    // game world's is after Stop (EditorSession::stopPlay), which cancelled it first. The scope is
    // then inactive and its destructor does nothing.
    void release() noexcept { m_owner = nullptr; }

private:
    friend class Transactions;

    TransactionScope(Transactions& owner, std::uint64_t epoch) noexcept : m_owner(&owner), m_epoch(epoch) {}

    // The owner while this scope still belongs to the open transaction: not after commit or cancel,
    // nor after Transactions::cancelOpen.
    [[nodiscard]] Transactions* live() const noexcept;

    Transactions* m_owner = nullptr;
    std::uint64_t m_epoch = 0;
};

class Transactions {
public:
    Transactions(World& world, UndoHistory& history, Selection& selection, EditorChanges& changes) noexcept
        : m_world(&world), m_history(&history), m_selection(&selection), m_changes(&changes) {}

    Transactions(const Transactions&) = delete;
    Transactions& operator=(const Transactions&) = delete;
    Transactions(Transactions&&) = delete;
    Transactions& operator=(Transactions&&) = delete;
    ~Transactions() = default;

    // Opens a transaction named `name` (shown as "Undo <name>"), or joins the open one, whose name
    // stays.
    [[nodiscard]] TransactionScope begin(std::string name);

    // True while a transaction is open.
    [[nodiscard]] bool isOpen() const noexcept { return m_depth > 0; }

    // Undoes the newest undoable entry, restoring the selection it records before. Fails with
    // "editor.transaction.open" while a transaction is open, "editor.undo.nothing" without an
    // entry, and "editor.undo.failed" when the entry no longer applies, a bug: the history is then
    // cleared.
    Expected<void> undo();
    // Redoes the next entry, restoring its selection after; as undo.
    Expected<void> redo();

    // Cancels the open transaction from outside its scopes, as Esc does during a drag: the collected
    // inverse is applied and nothing is recorded. The scopes of that transaction become inactive;
    // their commit and cancel do nothing. Does nothing when no transaction is open.
    void cancelOpen();

    // Ends the open transaction without applying its inverse and records nothing, for a world that
    // is about to be destroyed (a game world at Stop). Its scopes become inactive as with cancelOpen.
    // Does nothing when no transaction is open.
    void abandonOpen() noexcept;

    // Whether something besides these transactions changes the world: a game world's systems
    // (PlaySession sets it). Then an inverse that no longer applies when a transaction is cancelled
    // (an entity it restores was despawned by a system) is dropped and logged as a warning instead
    // of asserting as a bug; the world keeps what the systems made of it.
    void setForeignWriters(bool foreign) noexcept { m_foreignWriters = foreign; }

    // References into stored values that named entities without a stable id, made null (StoredPatches.h).
    [[nodiscard]] std::size_t droppedReferences() const noexcept { return m_dropped; }

private:
    friend class TransactionScope;

    Expected<void> apply(std::span<const EntityPatch> patches);
    void end(bool cancel);
    // Applies stored patches; returns their inverse in the stored form with a new table.
    Expected<void> replay(std::vector<EntityPatch>& patches, StableIdTable& ids);
    // Bumps the changes for what `patches` (live or stored) touch.
    void noteChanges(std::span<const EntityPatch> patches, const StableIdTable* ids);
    void merge(std::vector<EntityPatch> newer);

    World* m_world;
    UndoHistory* m_history;
    Selection* m_selection;
    EditorChanges* m_changes;

    // The open transaction.
    std::size_t m_depth = 0;
    // Changes on every cancelOpen, so the scopes it ended know.
    std::uint64_t m_epoch = 0;
    bool m_cancelled = false;
    std::string m_name;
    StableIdTable m_ids;
    std::vector<EntityPatch> m_inverse;
    std::vector<Uuid> m_selectionBefore;
    std::size_t m_dropped = 0;
    bool m_foreignWriters = false;
};

} // namespace ith
