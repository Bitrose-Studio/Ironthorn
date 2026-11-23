# The editor's core (ADR-0029 rule 1, ADR-0037, ADR-0040): the editor document and its world,
# commands and their registry, transactions and the undo history, selection, change counters, the
# keymap, the automation runner, the panel registry and layouts, and the view models of the Outliner
# and Details panels. Headless: no ImGui, no window, no
# GPU, so it is tested on its own and under sanitizers. editor_ui draws its state and issues its
# commands.
ith_add_module(editor_core
    LAYER editor
    KIND editor
    SOURCES
        Automation.h
        Automation.cpp
        CommandArgs.h
        CommandArgs.cpp
        CommandRegistry.h
        CommandRegistry.cpp
        DetailsModel.h
        DetailsModel.cpp
        EditCommands.cpp
        EditorChanges.h
        EditorDocument.h
        EditorDocument.cpp
        EditorLayout.h
        EditorLayout.cpp
        EditorLabel.h
        EditorPreferences.h
        EditorSession.h
        EditorSession.cpp
        FileCommands.cpp
        Keymap.h
        Keymap.cpp
        LayoutCommands.cpp
        LayoutSession.h
        LayoutSession.cpp
        OutlinerModel.h
        OutlinerModel.cpp
        PanelRegistry.h
        PanelRegistry.cpp
        QueryCommands.cpp
        Selection.h
        Selection.cpp
        StoredPatches.h
        StoredPatches.cpp
        Transactions.h
        Transactions.cpp
        TreeViewModel.h
        TreeViewModel.cpp
        UndoHistory.h
        UndoHistory.cpp
    DEPENDS
        foundation
        math
        file_system
        platform_events
        jobs
        reflection
        serialization
        config
        ecs
        level
        assets
        input
        asset_database
    REFLECT)
