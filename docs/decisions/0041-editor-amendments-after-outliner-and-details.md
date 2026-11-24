# ADR-0041: Editor amendments after the Outliner and Details (amends 0035, 0037)

- **Status:** Accepted
- **Date:** 2026-11-24
- **Related:** amends [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md)
  rule 11 (component selection) with a note to
  [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) rule 26 (the wake-up
  bound); [ADR-0040](0040-editor-docking-panels-and-layouts.md) (the Outliner is a closable
  panel)

## Context

P2 steps 9 and 10 landed the Outliner and Details (view models in `editor_core`, panels in
`editor_ui`, registered per ADR-0040 rule 3). Three small items were left open and are
settled here.

1. Component selection. ADR-0037 rule 11 describes component selection (`{entity, component
   type}`) as coming with the Details step. The Details step is done and it isn't built:
   `Selection` (`editor_core/Selection.h`) holds entity stable ids only, and `DetailsModel`
   (`editor_core/DetailsModel.cpp`) lists every component the selected entities share, with
   no way to select one.
2. Wake latency. ADR-0035 rule 26 asks for the wake-up test to end the wait within 50 ms.
   The check as written in `tests/platform/WakeTests.cpp` allows 500 ms, because a shared
   CI runner can deschedule the main thread for longer. The two numbers need one wording.
3. Rename with the Outliner closed. F2 (`ith.edit.rename`) gathers its input through a
   command prompt: the Outliner starts the inline rename of the primary selection
   (`editor_ui/OutlinerPanel.cpp` sets the prompt). The pending rename lives in the
   `OutlinerModel`, so with the panel closed the rename simply waits until the panel is
   shown again.

## Options considered

Component selection:

- **Build it in P2** (`Selection` gains `{entity, component}` pairs, Details keeps a selected
  component). Nothing in the P2 scope needs it: the gizmo works on entities, and the undo
  story stays simpler with entities only. Rejected for now.
- **Defer it** (chosen): selection holds entities for all of P2. Revisit with the gizmo on
  components (sockets, child transforms) or with prefabs, which give the feature its real
  requirements.

Wake latency:

- **Keep 50 ms as the CI bound.** Failed on shared CI runners; the failure is the runner,
  not the engine. Rejected.
- **50 ms design target, 500 ms test bound** (chosen): the target is measured on the
  reference machine (ADR-0005); the test asserts that a post wakes the wait, and 500 ms is
  still far below the 10 s a missed wake takes. Written as a note to ADR-0035's rule 26,
  not a new rule: the rule's intent is unchanged.

Rename with the Outliner closed:

- **Refuse F2 when the Outliner is closed.** Surprising: the selection exists, the command
  exists, nothing tells the user why nothing happens. Rejected.
- **Leave the rename pending until the panel shows** (chosen for P2): the state already
  lives in the `OutlinerModel`; opening the panel completes the gesture. The Rename command
  opening the Outliner first is the follow-up.

## Decision

1. **Component selection is deferred.** For all of P2 the selection holds entities only;
   ADR-0037 rule 11's component-selection sentence does not apply. Details shows every
   component the selected entities share. Revisit with the gizmo on components (sockets,
   child transforms) or with prefabs. Undo is unaffected: selection still rides along with
   each entry and is never undoable on its own (ADR-0037 rule 12).
2. **Note to ADR-0035 rule 26:** the 50 ms wake-up bound is a design target measured on the
   reference machine, not a CI assertion. The CI test (`tests/platform/WakeTests.cpp`)
   allows 500 ms, because a shared runner can deschedule the main thread; that is still far
   below the 10 s a missed wake costs.
3. **Rename with the Outliner closed** leaves the rename pending in the Outliner view model
   until the panel is shown; accepted for P2. Follow-up: the Rename command
   (`ith.edit.rename`) opens the Outliner first.

## Consequences

- Selection stays entity-only through P2; Details' component list stays a display and edit
  surface, never a selection.
- The wake test's bound is honest about CI noise and keeps its coverage; a regression that
  adds a real delay above 500 ms still fails.
- F2 with a closed Outliner shows nothing until the user opens the panel; accepted for P2,
  fixed when the Rename command opens the Outliner first.
- **Revisit when:** the gizmo edits components (sockets, child transforms); prefabs need
  component-level selection; the Rename command's follow-up lands.
