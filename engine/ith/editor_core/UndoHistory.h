#pragma once

// ith::UndoHistory: the undo entries of the editor document (ADR-0037 rules 5, 9, 10 and 15).
//
// An entry is a name, stored entity patches (StoredPatches.h) with the stable ids they name, and
// the selection before and after. It holds one direction only: undoing applies its patches and
// keeps the inverse World::applyPatches returns, for redo, and redoing does the reverse. Entries
// before the cursor are undoable, entries after it redoable.
//
// Limits: at most `maxEntries` entries and `maxBytes` estimated bytes (Editor Preferences, 500 and
// 256 MiB by default). The oldest undoable entries are dropped first, then the furthest redoable
// ones; the newest entry is always kept, even alone over the budget. Pushing an entry drops the
// redoable ones.
//
// The saved marker: the document is clean while the history stands where it stood at the last
// save, so undoing back to it makes the document clean again. Each entry has a serial, never
// reused; the position is the serial of the last undoable entry, or, with none, of the last entry
// dropped from the front. A saved position that can't come back (dropped, or in a discarded redo
// tail) leaves the document dirty until the next save. Main thread only.

#include "ith/ecs/EntityPatch.h"
#include "ith/editor_core/StoredPatches.h"
#include "ith/foundation/Uuid.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ith {

struct UndoEntry {
    // Shown in Edit > Undo and the history, e.g. "Rename".
    std::string name;
    // The stable ids the placeholders of `patches` index.
    StableIdTable ids;
    // What undoing (or redoing) the entry applies, in the stored form.
    std::vector<EntityPatch> patches;
    std::vector<Uuid> selectionBefore;
    std::vector<Uuid> selectionAfter;
    // The estimated memory of the entry; set by the history.
    std::size_t bytes = 0;
    // Unique in the history's lifetime; set by the history.
    std::uint64_t serial = 0;
};

struct UndoLimits {
    std::size_t maxEntries = 500;
    std::size_t maxBytes = std::size_t{256} * 1024 * 1024;
};

class UndoHistory {
public:
    explicit UndoHistory(const UndoLimits& limits = {}) : m_limits(limits) {}

    // Applies new limits, dropping the entries that no longer fit.
    void setLimits(const UndoLimits& limits);

    [[nodiscard]] const UndoLimits& limits() const noexcept { return m_limits; }

    // Adds `entry` as the newest undoable one, after dropping the redoable ones, then enforces the
    // limits.
    void push(UndoEntry entry);

    [[nodiscard]] bool canUndo() const noexcept { return m_cursor > 0; }

    [[nodiscard]] bool canRedo() const noexcept { return m_cursor < m_entries.size(); }

    // The entry undo applies, or null. After applying it the caller stores the inverse in it and
    // calls stepBack.
    [[nodiscard]] UndoEntry* undoEntry() noexcept { return canUndo() ? &m_entries[m_cursor - 1] : nullptr; }

    // The entry redo applies, or null; then stepForward.
    [[nodiscard]] UndoEntry* redoEntry() noexcept { return canRedo() ? &m_entries[m_cursor] : nullptr; }

    // Move the cursor over the entry just undone or redone, re-estimating its size.
    void stepBack();
    void stepForward();

    // Drops every entry. The saved position stays, so the document stays dirty when it was.
    void clear();

    // Clears the entries and makes the current state the saved one: a new or opened level.
    void reset();

    // The entries, oldest first; the first `cursor()` are undoable.
    [[nodiscard]] const std::deque<UndoEntry>& entries() const noexcept { return m_entries; }

    [[nodiscard]] std::size_t cursor() const noexcept { return m_cursor; }

    [[nodiscard]] std::size_t bytes() const noexcept { return m_bytes; }

    // The current position, which markSaved records.
    [[nodiscard]] std::uint64_t position() const noexcept;
    // Records the current position as saved and returns the saved position it replaces.
    std::uint64_t markSaved() noexcept;

    // Sets the saved position back, for a save that failed.
    void setSavedPosition(std::uint64_t position) noexcept { m_saved = position; }

    [[nodiscard]] std::uint64_t savedPosition() const noexcept { return m_saved; }

    // True when the history is not at the saved position.
    [[nodiscard]] bool dirty() const noexcept { return position() != m_saved; }

private:
    void refresh(UndoEntry& entry);
    void enforceLimits();

    UndoLimits m_limits;
    std::deque<UndoEntry> m_entries;
    std::size_t m_cursor = 0;
    std::size_t m_bytes = 0;
    std::uint64_t m_nextSerial = 1;
    // The position when nothing is undoable.
    std::uint64_t m_base = 0;
    std::uint64_t m_saved = 0;
};

// The estimated memory of an entry: its patches' values through reflection (type sizes, container
// elements, string capacities), its id table and its selections.
[[nodiscard]] std::size_t estimateBytes(const UndoEntry& entry);

} // namespace ith
