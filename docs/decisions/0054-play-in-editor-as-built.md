# ADR-0054: Play In Editor as built (amends 0027, 0033, 0037, 0053)

- **Status:** Accepted (2026-11-28)
- **Date:** 2026-11-28
- **Related:** amends [ADR-0053](0053-play-in-editor-and-simulate.md) rules 1, 2, 4 to 13, 18, 20 to 22
  and 24 to 28, and [ADR-0033](0033-reflection-property-paths-and-patches.md) rule 16, which
  ADR-0053 rules 24 to 27 replace; the [Play In Editor spec](../specs/play-in-editor.md) sections
  3.2, 3.5, 3.7, 3.12 and 4; [ADR-0027](0027-input-rules-completed.md) rule 3 (capture on click);
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rules 1 and 10 (command
  kinds, the history while playing); the [editor core](../development/editor-core.md#play-in-editor)
  and [editor UI](../development/editor-ui.md#play-in-editor) guides

## Context

ADR-0053's five slices landed (`ecs/World`, `gameplay/{Time, GameWorld}`,
`editor_core/{PlaySession, PlayCommands, PlayInput, KeepSimulationChanges, CommandRegistry,
Transactions, DetailsModel, Keymap, EditorSession}`, `input/Input`,
`editor_ui/{EditorShell, ViewportPanel, PlayIndicators, ImGuiPlatform}`, `editor/Main.cpp`).
Building and testing them showed gaps in the rules as written:

- a new game world may reuse a destroyed world's id, so a view that cached a structure generation
  could take the new world for the old one;
- the play commands were refused during the very drag that Escape and Stop must end;
- Stop "cancels" an open game-world drag, replaying an inverse into a world about to be destroyed,
  which may no longer apply after the systems changed it;
- rule 22's "a mouse press while possessed" passes presses on any panel to the game;
- Keep Simulation Changes, run from inside another command, joined that command's transaction;
- ADR-0033 rule 16 (Adds and Removes, Transient skipped) was replaced by ADR-0053 rules 24 to 27
  but never marked as replaced.

Items 1 to 36 restate the affected rules as built. Rules not listed stand. Requirement numbers are
the spec's (section 4).

## Options considered

Structure generations across worlds (item 1):

- **A counter per world from 0.** A new game world that reuses an id and reaches the same count
  looks unchanged to a cached view. Rejected.
- **Key every cache on the active world's identity as well** (done anyway, item 13). It covers the
  editor's view models, but not other code that caches a generation.
- **Seed each world's counter 2^32 above the previous world's (chosen).** One atomic add per world.
  Values then differ across worlds unless a world makes 2^32 structural changes.

An open game-world drag at Stop (item 10):

- **Cancel it** (ADR-0053 rule 10 as written). This replays the inverse into the world that Stop then
  destroys, and the inverse may fail after the systems despawned what it restores. Rejected.
- **Abandon it (chosen).** The transaction ends without replay and records nothing. The world goes
  with it.

Showing Keep Simulation Changes' result (item 28):

- **Each caller formats the result.** K reaches the command through the keymap consumer, which has
  no UI, so there would be three paths. Rejected.
- **The command's prompt runs it and reports (chosen).** K, the Edit menu and the Outliner all
  invoke it as a user's gesture, so one registration covers them. Automation runs `execute` and
  gets the plain result.

A reparented entity's `LocalTransform` (item 24):

- **Keep it.** It is relative to the game parent, so under the editor parent it moves the entity.
  Rejected.
- **Convert it through both parents' world transforms.** This interprets a structural change, which
  ADR-0017 rule 13 excludes. Rejected.
- **Report it and don't keep it (chosen; owner, 2026-11-28).**

## Decision

### Generation, time and the game world

1. **Structure generations never repeat across worlds** (rule 2). Each `World` starts its counter
   2^32 above the one created before it in the process. The counter only grows, possibly by more
   than one per call, so compare values for equality only. *Reason:* a cached value can't match a
   new world that reuses an old world's id. Requirements 29a, 45c.
2. **There is no bump for a whole command buffer** (rule 2's "applied command"). An applied
   `Commands` buffer moves the generation only through the structural operations it holds. These
   leave it unchanged: a buffer of value writes or of calls on stale handles, a refused call, and a
   schedule run without structural commands. *Reason:* a tick that only writes values must not
   rebuild the Outliner. Requirements 29, 48.
3. **`tickGameWorld`** (rule 1):
   - A `dilation` that is negative or not finite asserts and counts as 0.
   - A `dilation` or `Time::paused` that a system writes applies from the next tick, since `Time` is
     advanced before any system runs.
   - A world that is iterated or runs a system asserts, and nothing happens: `Time` is unchanged.
   - A schedule that isn't built or is running asserts after `Time` has advanced.
   - All `Time` fields except `dilation` and `paused` are ReadOnly in reflection.
   - In `PlaySession::tick`, a clock that goes back counts as 0 s.

   *Reason:* game data that is wrong never stops the editor in a release build, and every system of
   a tick reads the same `Time`. Requirements 13, 45b.
4. **Resources at start** (rule 7, requirement 12). `copyWorld` copies every resource of the editor
   world that isn't EditorOnly. Each session therefore starts with the editor world's resources as
   they are, plus a default `Time`, and Stop discards what play changed. Requirement 12 now reads
   "the editor world's value" where it said "default value". App-scoped resources (ADR-0016 rule 6) don't
   exist yet. *Reason:* the editor world is never simulated, so its resources are the level's
   starting state. **Owner's answer (2026-11-28):** the game world starts with the level's world
   resources, and app-level resources, once they exist, stay fresh for each session.

### Registry and refusals

5. **`CommandDesc::runsDuringInteraction`** (Actions only, asserted; added to ADR-0037 rule 1's
   descriptor beside ADR-0053 rule 13's `playEditable`) lets a command run while an interaction
   holds a transaction open. It is set on the seven play commands, not on Keep Simulation Changes:
   - Stop drops a game-world drag;
   - Pause and Step keep the game going during one;
   - Play and Simulate refuse through their `canExecute`.

   The registry's interaction check covers open transactions in either world. *Reason:* Escape and
   the toolbar must stop a session during a Details drag on the game world. Requirements 3, 25 and
   34.
6. **`editor.command.nested`.** Play, Simulate and Stop refuse to run from inside another command
   (`CommandRegistry::depth() > 0`), and `startPlay` and `stopPlay` assert a depth of at most 1.
   *Reason:* an outer command's transaction scope could point into the game world they create or
   destroy.
7. **Error codes pass through.** A `canExecute` error becomes `editor.command.disabled`, except
   these, which keep their code: `editor.command.playing`, `editor.command.interaction_open`,
   `editor.command.nested`, `editor.document.busy` and every `editor.play.*` code. *Reason:* the
   spec's codes are the automation contract (3.14). Requirements 2, 3.
8. **Refusal order for Play and Simulate** (rule 7):
   1. the registry refuses every Action while a level opens;
   2. their `canExecute` refuses with `editor.command.nested`;
   3. `canStartPlay` refuses with `editor.play.running`, then `editor.document.busy`, then
      `editor.command.interaction_open` (an editor-world transaction).

   The toolbar disables them through the same check. *Reason:* one order for the toolbar, the
   keymap and automation. Requirements 2, 3.

### Transactions and Details

9. **Play and Stop drop a cut-off Details drag.** A drag that Play or Stop cut off is dropped at the
   model's next call, and its release writes into neither world. *Reason:* the drag's handles belong
   to the world it started in.
10. **Stop abandons, it doesn't cancel** (rule 10). `Transactions::abandonOpen()` ends the open
    game-world transaction without applying its inverse and records nothing. A scope still held on
    it must be dropped with `TransactionScope::release()`, never ended. *Reason:* the world goes with
    the transaction. Requirements 34, 35.
11. **Foreign writers.** The game world's `Transactions` has `setForeignWriters(true)`. When a
    cancelled drag's inverse no longer applies (a system changed or despawned the entity), the
    inverse is dropped and logged as a warning instead of asserting. *Reason:* systems change the
    game world outside the transactions, so this is not a bug. Requirement 25.
12. **Details after a tick.** `bumpAllValues` leaves no log, so Details compares the shown values with
    its working copy and refreshes only when one differs. The comparison allocates nothing.
    *Reason:* a tick that changes nothing shown costs no refresh. Requirements 47, 48.
13. **`editor.details.stale`.** Between Play or Stop and the next `update()`, Details reports nothing
    from its handles: no addable components, and "None" for references. In that window `beginEdit`,
    `addComponent` and `removeComponent` fail with `editor.details.stale`. The view models also key
    their caches on the active world's identity. *Reason:* the handles may belong to a destroyed
    world whose id was recycled. Requirement 29a.

### Input

14. **The press that passes the keymap gate** (rule 22). The keymap gate passes a mouse press on only
    when all of these hold: the session is possessed, capture on click is on for the event's window,
    and the press is on the play viewport's image (`Viewports::imageAt`). While possessed, the
    viewport consumer leaves presses, motion and the wheel over that image alone. *Reason:* as
    written, presses on other panels and viewports reached the game. Requirements 22, 24a.
15. **Possess at the current pose only until the first Eject** (rule 18). Possess with no Eject yet in
    the session leaves the camera where it is. After an Eject, Possess resumes the saved pose,
    whichever mode started the session. *Reason:* a Simulate session that possessed and ejected has a
    spectator pose to resume. Requirement 20.
16. **`Input` checks capture on click again after the route** (rule 20; refines ADR-0027 rule 3). A
    consumer above the game may turn capture on click off while it handles a press (a Stop or an
    Eject). The mouse is then not captured, and the press still reaches no `Game` consumer.
    *Reason:* a press that ends possession must not capture for it. Requirements 22, 35.
17. **Only the editor window's capture is released** (rules 10 and 20). Stop, Eject and Shift + F1
    release the capture only when the bound window holds it, and another window's capture stays.
    *Reason:* a game window's capture isn't Play In Editor's. Requirement 35.
18. **Binding the input** (rule 20):
    - Bind with `setPlayInput(input, window)` before any session starts.
    - Unbind with `setPlayInput(nullptr, kNoWindow)` after stopping the session and before the
      `Input` is destroyed. `setPlayInput` asserts that no session runs.
    - Without a binding (headless), nothing is ever captured.
    - `~EditorSession` stops a running session through the bound `Input`.
    - The application stops play at every close request and at exit, before it unbinds.

    *Reason:* Stop uses the `Input`, also from the destructor. Requirement 6.
19. **ImGui while captured** (rule 21). ImGui gets no key press and no text, and the UI consumes no
    key. Releases and modifiers still reach ImGui, and the keys ImGui held when the capture started
    are cleared. A Play In Editor key is consumed even when its command refuses. *Reason:* the key-up
    goes to the game, so ImGui would otherwise keep the key held after the capture. Requirements 23,
    26.

### Keep Simulation Changes

20. **ADR-0033 rule 16 is replaced** by ADR-0053 rules 24 to 27 as amended here:
    - no Add is made for a component only the game world has, and no Remove for one only the editor
      world has;
    - EditorOnly types, `StableId` and `ChildOf` are excluded as well as Transient ones;
    - the undo step is the entry of one transaction.

    *Reason:* ADR-0017 rule 13 keeps values only.
21. **Refusals beyond rule 6:**
    - `editor.command.invalid_args` when `ids` is not an array of stable-id strings;
    - `editor.command.interaction_open` during an interaction in either world (from the registry:
      Keep doesn't run during interactions);
    - `editor.command.interaction_open` when it is called from inside another command while an
      editor-world transaction is open (its `canExecute`);
    - `editor.document.busy` while a level opens.

    Repeated ids count once, and an empty play selection keeps nothing. *Reason:* a nested call would
    join the outer command's transaction. Requirements 2, 42a.
22. **An Action that records its own entry** (rule 27). Keep is an `Action`, because `Edit` commands
    are refused while playing. It opens one document transaction named "Keep
    Simulation Changes" only when something differs. A failed apply cancels that transaction and
    returns its error. *Reason:* this is ADR-0037 rule 10's one exception, so it gets no new command
    kind. Requirements 37, 41.
23. **Matching references.**
    - A game entity with a `StableId` maps to the editor entity with that id.
    - A game entity without one maps to the editor entity in the same slot with the same generation,
      when that entity is alive and also has no `StableId`.
    - Any other reference becomes null.

    `editor.play.reference_dropped`, a warning whose message starts with the entity's stable id, is
    added only for a component that is kept: not for a value that then equals the editor's, and not
    for `ChildOf`. Types that can't hold entities are compared in place and copied only when they
    differ. *Reason:* `copyWorld` keeps slots and generations, so only entities copied from the
    editor world match, and a warning is useful only when the level changes. Requirement 40.
24. **A reparented entity** (owner, 2026-11-28). The parent counts as changed when the `ChildOf` link
    is in one world only, names another mapped parent, or has another `despawnWithParent`. Then
    `ChildOf` is reported as "parent changed". A differing `LocalTransform` is also not kept and is
    reported with the same reason. *Reason:* the transform is relative to the game parent.
    Requirement 39.
25. **Adds before sets.** Within an entity, the Adds of immutable types come before the whole-value
    sets. *Reason:* a hook that an Add runs (`onReplace`, `onInsert`) can't overwrite a kept value.
26. **Hooks run in the editor world.** Those hooks run in the editor world, which has none of the
    game's resources (no `Time`), and the commands they record are applied there too. *Reason:* Keep
    writes the editor world. Hooks must not rely on game resources.
27. **Reason strings are a stable contract.** These strings change only through an ADR:
    - `skipped` reasons: `editor.play.not_found` and "not in the level";
    - `ignored` reasons: "added during play", "removed during play", "parent changed" and "enabled
      state changed";
    - components are named by their type name.

    *Reason:* automation scripts and the UI's notice match on them. Requirements 38, 39.

### UI and the application

28. **The Keep notice is the command's prompt** (rule 4).
    - `registerKeepSimulationChanges(session, shell)` sets the prompt. While the returned
      `KeepChangesPrompt` lives, the prompt runs the command on the play selection. The prompt is
      RAII, neither copied nor moved, and its destructor removes the prompt.
    - The prompt puts the notice ("Kept 2 components of 1 entity, 1 ignored") in the status bar and
      the Output Log (category `Play`), and logs each skipped entry, ignored entry and warning as a
      warning.
    - `runMenuCommand` keeps a result that a prompt reported.

    The prompt here gathers no input; it runs the command at once. *Reason:* K, the menu and the
    Outliner share one path.
29. **The selected viewport's fallback** (rule 5). `EditorSession::startPlay`, not the UI, plays in
    `ith.viewport.1` when the Viewport panel is registered and the selected instance is not open.
    The selection itself stays. The shell opens the play viewport's panel once per session; closed
    during the session, it stays closed. Headless, the selected viewport plays. *Reason:* the session
    and its result name the same viewport in headless runs and in the editor. Requirement 1.
30. **The toolbar** (spec 3.2) is left-aligned, and every button has one width, so Stop lands where
    Play was. Each button invokes its command and is disabled by its `canExecute`. *Reason:* the
    spec's "controls keep their place" with the least layout code. Requirement 5.
31. **The capture hint is fixed text.** `kPlayCaptureHint` names Shift + F1 even when the binding is
    changed. This is a known limit. Requirement 43.
32. **Automation with a running session.** After an `--automation` script, the editor exits once it
    is idle. While a session the script left running keeps it drawing, it exits after 30 frames
    (`kFramesAfterScript`). *Reason:* a ticking session is never idle. Requirement 44.
33. **Picks across Play and Stop.** A viewport pick carries the active world's identity from when it
    was copied, and a read after Play or Stop changed that identity is dropped. Stop also drops the
    play viewport's waiting click. *Reason:* the texel names an entity of a world no longer drawn
    there. Requirement 32.
34. **Elapsed time** in the status bar comes from `PlaySession::elapsedSeconds`. *Reason:* `editor_ui`
    then doesn't depend on `gameplay`.

### Deferred

35. **`WorldStarted` and `WorldEnding`** stay as marked points in `PlaySession`, as rule 11 says.
    Requirement 36 is enabled when ECS events land.
36. **Performance on the Low tier.** Requirements 46, 47 and 49 have not been measured on the
    Low-tier reference machine. The play session benchmark prints "met" or "MISSED" and always exits
    0. Under ThreadSanitizer it reports allocation counts as unavailable. Requirement 47's "allocates
    nothing" holds only while no shown value changes: a tick that changes one rebuilds Details'
    working copy and rows. Updating them in place is deferred.

## Consequences

- ADR-0053, ADR-0033 rule 16 and the spec now match the code and its tests
  (`tests/ecs/StructureGenerationTests`, `tests/gameplay/GameWorldTests`,
  `tests/editor_core/{PlaySessionTests, PlayInputTests, KeepSimulationChangesTests, KeymapTests}`,
  `tests/input/InputTests`, `tests/editor_ui/PlayInEditorUiTests`, the `play_in_editor` automation
  script).
- Stop can't fail on a drag that systems invalidated, and a game-world cancel that can't be put back
  is a logged warning, not an assert.
- Keep Simulation Changes can't be undone as part of another command's entry. A user who reparents
  during play keeps neither the parent nor the local transform.
- Hooks of immutable components must work without game resources, since Keep runs them in the
  editor world.
- A rebound Shift + F1 shows a stale hint. Automation that leaves a session running ends after 30
  frames.
- ADR-0027, ADR-0033, ADR-0037 and ADR-0053 list this ADR under "Amended by" (ADR-0027 for
  item 16, ADR-0037 for items 5 and 22).
- **Revisit when:**
  - ECS events land (item 35, requirement 36);
  - the Low-tier machine measures requirements 46, 47 or 49 as missed;
  - Details shows values a game changes every tick (item 36);
  - app-scoped resources exist (item 4);
  - the keymap exposes a chord's display text (item 31);
  - the gameplay framework brings a player's view (item 15).
