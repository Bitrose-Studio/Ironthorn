#pragma once

// ith::EditorDocument: the level open in the editor (ADR-0037 rules 10 and 14 to 16).
//
// It owns the editor world, which is never simulated (ADR-0017 rule 10), the open level's asset id,
// the undo history, the selection, the change counters and the transactions that change the world.
// One level is open at a time. Every change to the world goes through transactions(); systems
// that run in the editor world write only Transient data (ADR-0037 rule 8), so the history always
// matches the world.
//
// The document is dirty while its history is not where it was at the last save or load.
// While Play In Editor runs (setPlaying), nothing in the editor world changes: commands that
// would change it are refused and undo and redo are off. Main thread only.

#include "ith/ecs/World.h"
#include "ith/editor_core/EditorChanges.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/Transactions.h"
#include "ith/editor_core/UndoHistory.h"
#include "ith/foundation/Uuid.h"

namespace ith {

class EditorDocument {
public:
    // An untitled, empty, clean level with a new asset id.
    EditorDocument();

    EditorDocument(const EditorDocument&) = delete;
    EditorDocument& operator=(const EditorDocument&) = delete;
    EditorDocument(EditorDocument&&) = delete;
    EditorDocument& operator=(EditorDocument&&) = delete;
    ~EditorDocument() = default;

    // The editor world, read-only: changes go through transactions().
    [[nodiscard]] const World& world() const noexcept { return m_world; }

    // The world for code that owns its consistency with the history: loading a level, and tests.
    [[nodiscard]] World& mutableWorld() noexcept { return m_world; }

    [[nodiscard]] Transactions& transactions() noexcept { return m_transactions; }

    [[nodiscard]] UndoHistory& history() noexcept { return m_history; }

    [[nodiscard]] const UndoHistory& history() const noexcept { return m_history; }

    [[nodiscard]] Selection& selection() noexcept { return m_selection; }

    [[nodiscard]] const Selection& selection() const noexcept { return m_selection; }

    [[nodiscard]] EditorChanges& changes() noexcept { return m_changes; }

    [[nodiscard]] const EditorChanges& changes() const noexcept { return m_changes; }

    // The open level's asset id (ADR-0034 rule 1).
    [[nodiscard]] const Uuid& levelId() const noexcept { return m_levelId; }

    // True when the level has changes that are not saved.
    [[nodiscard]] bool dirty() const noexcept { return m_history.dirty(); }

    // Empties the world and starts an untitled level with a new asset id: history cleared,
    // selection cleared, clean. Precondition (asserted): no transaction is open.
    void reset();

    // True while Play In Editor or Simulate runs (ADR-0037 rule 10).
    [[nodiscard]] bool playing() const noexcept { return m_playing; }

    void setPlaying(bool playing) noexcept { m_playing = playing; }

private:
    World m_world;
    UndoHistory m_history;
    Selection m_selection;
    EditorChanges m_changes;
    Transactions m_transactions{m_world, m_history, m_selection, m_changes};
    Uuid m_levelId;
    bool m_playing = false;
};

} // namespace ith
