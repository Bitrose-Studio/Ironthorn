# The editor's core (ADR-0029 rule 1, ADR-0037): the editor document and its world, commands and
# their registry, transactions and the undo history, selection, change counters, the keymap and
# the automation runner. Headless: no ImGui, no window, no GPU, so it is tested on its own and
# under sanitizers. editor_ui draws its state and issues its commands.
ith_add_module(editor_core
    LAYER editor
    KIND editor
    SOURCES
        CommandRegistry.h
        CommandRegistry.cpp
        EditCommands.cpp
        EditorChanges.h
        EditorDocument.h
        EditorDocument.cpp
        EditorLabel.h
        EditorSession.h
        EditorSession.cpp
        Selection.h
        Selection.cpp
        StoredPatches.h
        StoredPatches.cpp
        Transactions.h
        Transactions.cpp
        UndoHistory.h
        UndoHistory.cpp
    DEPENDS
        foundation
        math
        reflection
        serialization
        ecs
        level
    REFLECT)
