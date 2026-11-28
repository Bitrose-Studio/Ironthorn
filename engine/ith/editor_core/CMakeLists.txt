# The editor's core (ADR-0029 rule 1, ADR-0037, ADR-0040, ADR-0042, ADR-0049): the editor document
# and its world, commands and their registry, transactions and the undo history, selection, change
# counters, the keymap, the automation runner, the panel registry and layouts, the viewports (cameras,
# input routing, click picking, the translate gizmo, tools, Focus, Snap to Floor, Add Shape), and the
# view models of the Outliner, Details and Content Browser panels, and the Play In Editor session
# (ADR-0053: the game world, its schedule and tick, Pause and Step, the play camera, the spectator,
# the Play In Editor input rules and Keep Simulation Changes). Headless: no ImGui, no window, no
# GPU, so it is tested on its own and under sanitizers. editor_ui draws its state and issues its
# commands.
ith_add_module(editor_core
    LAYER editor
    KIND editor
    SOURCES
        AddShape.h
        AddShape.cpp
        Automation.h
        Automation.cpp
        CommandArgs.h
        CommandArgs.cpp
        CommandRegistry.h
        CommandRegistry.cpp
        ContentBrowserModel.h
        ContentBrowserModel.cpp
        ContentCommandSupport.h
        ContentCommands.cpp
        ContentFileCommands.cpp
        ContentImportCommands.cpp
        ContentPaths.h
        DetailsModel.h
        DetailsModel.cpp
        EditCommands.cpp
        EditorCamera.h
        EditorCamera.cpp
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
        Gizmo.h
        Gizmo.cpp
        KeepSimulationChanges.cpp
        Keymap.h
        Keymap.cpp
        LayoutCommands.cpp
        LayoutSession.h
        LayoutSession.cpp
        OutlinerModel.h
        OutlinerModel.cpp
        PanelRegistry.h
        PanelRegistry.cpp
        PlayCommands.cpp
        PlayInput.h
        PlayInput.cpp
        PlaySession.h
        PlaySession.cpp
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
        ViewportCommands.cpp
        ViewportGizmo.h
        ViewportGizmo.cpp
        ViewportInput.h
        ViewportInput.cpp
        ViewportPick.h
        ViewportPick.cpp
        Viewports.h
        Viewports.cpp
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
        gameplay
        level
        assets
        input
        asset_database
        shapes
    REFLECT)
