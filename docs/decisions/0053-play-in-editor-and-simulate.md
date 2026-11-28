# ADR-0053: Play In Editor and Simulate (amends 0013, 0014, 0016, 0017, 0029, 0037, 0042)

- **Status:** Accepted
- **Amended by:** [ADR-0054](0054-play-in-editor-as-built.md)
- **Date:** 2026-11-28
- **Related:** the [Play In Editor spec](../specs/play-in-editor.md) (behaviour and acceptance
  tests; this ADR wins where they differ); amends [ADR-0013](0013-input.md) rule 10,
  [ADR-0014](0014-ecs-storage-and-world.md) (a structure generation on `World`),
  [ADR-0016](0016-gameplay-model-on-the-ecs.md) rule 15,
  [ADR-0017](0017-runtime-and-editor-separation.md) rule 10,
  [ADR-0029](0029-editor-ui-toolkit.md) rule 7,
  [ADR-0037](0037-editor-commands-transactions-selection-and-automation.md) rules 1, 4, 10 and 17,
  and [ADR-0042](0042-editor-viewport.md) rules 1 and 3;
  [ADR-0004](0004-modules-and-layering.md) (layers), [ADR-0008](0008-editor-familiar-to-unreal-developers.md)
  rules 2 to 5 (toolbar, vocabulary, Esc, Shift + F1, F8),
  [ADR-0027](0027-input-rules-completed.md) rule 3 (capture on click),
  [ADR-0032](0032-entity-references-stable-ids-and-world-copy.md) (`StableId`, `copyWorld`,
  `remapEntities`), [ADR-0045](0045-editor-viewport-amendments-after-the-viewport-panel.md) to
  [ADR-0048](0048-editor-viewport-translate-gizmo-as-built.md) (viewport as built),
  [ADR-0049](0049-editor-content-browser-v1.md) (keymap contexts; content file operations during
  play)

## Context

P2 step 13 adds Play In Editor and Simulate. The behaviour is specified in the spec above; the
owner decided its open questions on 2026-11-28. Accepted ADRs already fix the model:

- the game world is an in-process copy without editor-only data, under a new world id, discarded
  at Stop; the editor world is never simulated (ADR-0017 rules 10 and 11);
- Simulate, Possess and Eject, Details edits during play, Keep Simulation Changes by stable id
  (ADR-0017 rule 13, ADR-0016 rule 9);
- nothing is undoable while playing, and Keep Simulation Changes is the one entry recorded
  (ADR-0037 rule 10);
- Esc, Shift + F1 and F8 on top of each editor window's input stack (ADR-0029 rule 7), capture on
  click (ADR-0027 rule 3).

What the code has today, and what this step builds on:

- `copyWorld(source, target, {.excludeEditorOnly = true})` into a fresh `World`: same slots under
  the target's id, entity references remapped, change ticks started over, no system runs copied.
- `Schedule` with stages, `build()` errors `ecs.schedule.*`, per-world last runs, and
  `transformPropagationSystem()` (`PostUpdate`).
- `EditorDocument::setPlaying`, checked by `CommandRegistry` (every `Edit` command), `DetailsModel`,
  Undo, Redo, New Level and Open Level. Save and Save As are allowed today.
- `Transactions(World&, UndoHistory&, Selection&, EditorChanges&)`: any world can have one.
- `Input::setCaptureOnClick`, `captureMouse`, `releaseMouse`, `mouseCaptured`; capture sends the
  mouse only to the `Game` layer, keyboard events still walk the whole stack.
- `Keymap` has the Play In Editor context (`kPlayInEditorCommands`), but no consumer handles it;
  `Keymap.json` binds the five play chords, and K is free.
- `TypeInfo::equals` and `remapEntities` (reflection), `Patch::set` with the empty path replacing a
  whole value.
- No `Time` resource, no main toolbar, no "selected viewport", no world-level structure counter,
  and no ECS events.

**Unreal prior art** (public documentation only, listed in the spec's sources): Play in the
selected viewport by default, the toolbar's Play and Simulate replaced while running by Pause,
frame step, Stop and Possess or Eject, click to control and Shift + F1 to free the mouse, Esc to
stop, and Keep Simulation Changes from the Outliner or K. The lesson kept: one duplicate world,
thrown away at Stop, so the editor is never restored by hand.

## Options considered

Only the choices the spec left to the implementation have real alternatives.

### Advancing `Time`

- **A function that advances `Time`, then runs the schedule** (chosen):
  `tickGameWorld(world, schedule, seconds)`. `Time` is final before any system runs, the player
  and server loops call the same function, and nothing needs ordering against it.
- **A time system, the first of the `Input` stage** (the spec's first draft). Rejected: it still needs the real
  delta from the driver, through a second resource or a field the driver writes, and every system
  that reads `Time` in `Input` must order itself after it. More parts, same result.

### The camera during a session

- **The play viewport's own camera serves as the play camera; the session saves it at start and
  restores it at Stop** (chosen). The viewport consumer, the camera controller, the redraw key and
  the renderer work unchanged. Stop cannot fail, and the file is written only at level close and
  exit, both of which stop the session first, so the saved camera never leaks.
- **A separate session camera**, threaded through the viewport consumer, the panel and the redraw
  key. Same behaviour, every viewport path gains a branch.

### The world commands and panels act on

- **One accessor for the active world** (chosen): `EditorSession::active()` gives the world, its
  `Transactions`, `Selection`, `EditorChanges` and entity mapping, those of the game world while a
  session runs and of the document otherwise. Commands and view models that act on "the world"
  call it; file, history and Keep Simulation Changes keep using `document()`.
- **A second `EditorDocument` for the game world.** Brings a path, level id, dirty state and play
  flag that mean nothing for a game world.
- **A `world` argument on every command.** Every caller must pass it; the panels still need a
  default.

### Escape above or below the UI

- **The Play In Editor consumer stays on top and passes Escape on when something else uses it**
  (chosen). One place decides; the UI reports, each frame, whether ImGui has an active text field
  or an open popup (the same one-frame lag as ADR-0029 rule 7's want-capture flags).
- **Stop on Escape from the keymap consumer, below the UI.** The viewport consumer swallows
  unmodified keys while flying, popups still need a report, and Escape is the Editor context's
  Select None there; three special cases instead of one.

### Following the game world's structure

The structure generation on `World` (decision 15) is chosen over rebuilding the Outliner every
tick (fails requirement 48 at 10,000 entities) and over ECS observers (rejected by ADR-0037
rule 13). It is one integer.

## Decision

### Modules

1. **`gameplay`**, new: layer `framework`, kind `runtime`, depends on `foundation`, `reflection`
   and `ecs`, with `REFLECT`. It holds:
   - `ith::Time`, a reflected world resource (`ITH_TYPE(Resource)`; ADR-0016 rule 12): `delta`
     and `unscaledDelta` (float seconds), `elapsed` and `unscaledElapsed` (double seconds),
     `frame` (`std::uint64_t`, 0 before the first tick), `dilation` (float, default 1) and
     `paused` (the game's own pause: `delta` is 0, the systems still run);
   - `kMaxTickDelta` = 0.1 s;
   - `buildGameSchedule(schedule, systems)`: adds the engine's game-world systems (in P2,
     `transformPropagationSystem()`), then `systems` in order, then `build()`; its errors are the
     schedule's;
   - `tickGameWorld(world, schedule, seconds)`: clamps `seconds` to [0, `kMaxTickDelta`] (a
     non-finite value asserts and counts as 0), advances the world's `Time` (inserting a default one
     when it has none), then `schedule.run(world)`.

   Game modules, the player and the dedicated server link it; later it also holds the lifecycle
   events and the game-world system list. `editor_core` gains a dependency on it.
2. **`ecs`**: `World::structureGeneration()`, a 64-bit counter that moves on every spawn, despawn,
   component add that adds a type, remove, `ChildOf` write, `placeBefore`, `clear` and applied
   command, and through `applyPatches` and `copyWorld`. Value writes never move it.
3. **`editor_core`**: a `PlaySession`, owned by `EditorSession` through `std::unique_ptr` while a
   session exists. It holds the game `World`, its `Schedule`, a `Transactions` over the game world
   with a private `UndoHistory` that is never undone, the play `Selection`, the game world's
   `EditorChanges` and entity mapping, the state flags, the saved editor camera and the spectator
   pose. `EditorSessionOptions` gains `gameSystems` (`std::vector<SystemDescriptor>`), which the
   editor application fills from the game modules' registration (ADR-0017 rule 5); empty in P2.
   It stays headless.
4. **`editor_ui`**: the main toolbar, the play viewport's border, label and capture hint, the status
   bar chip, the Outliner and Details notes and the Outliner's Keep Simulation Changes entry, the
   theme tokens `playActive`, `playSimulate` and `playPaused`, and the per-frame Escape report
   (rule 19).

### Session and commands

5. **One session at a time, in the selected viewport.** No new window and no multi-client in P2
   (ADR-0017 rule 12 keeps the new window for later). The selected viewport is the instance the
   last mouse press on a viewport image went to, kept in the `Viewports` store (amends ADR-0042
   rule 3), `ith.viewport.1` by default. If its panel is closed, the UI opens `ith.viewport.1` and
   plays there.
6. **Commands**, all of kind `Action`, refused while a level opens (`editor.document.busy`):

   | Id | Chord (context) | Refused with |
   |---|---|---|
   | `ith.play.play`, `ith.play.simulate` `{paused?}` | Alt + P, Alt + S (Editor) | `editor.play.running`; `editor.command.interaction_open` |
   | `ith.play.stop` | Escape (Play In Editor) | `editor.play.not_running` |
   | `ith.play.pause` `{paused?}` (toggles without it) | none | `editor.play.not_running` |
   | `ith.play.step` `{frames: 1..1000, deltaSeconds: (0, 0.1]}` | none | `editor.play.not_running`, `editor.play.not_paused`, `editor.command.invalid_args` |
   | `ith.play.possess_or_eject` `{possess?}` | F8 (Play In Editor) | `editor.play.not_running` |
   | `ith.play.free_mouse` | Shift + F1 (Play In Editor) | `editor.play.not_running` |
   | `ith.play.keep_simulation_changes` `{ids?}` | K (Editor) | `editor.play.not_running` |
   | `ith.query.play` | — | never |

   The results and the query's fields are the spec's (3.14). K amends ADR-0037 rule 4's list of
   commands registered without a chord; `Keymap.json` gains it, and its header comment drops "the
   Play In Editor commands are registered by the editor work that brings them".
7. **Start**, in this order, each failure changing nothing: the checks of rule 6;
   `buildGameSchedule` with `gameSystems` (its `ecs.schedule.*` error, also logged); a new
   `World`; `copyWorld` with `excludeEditorOnly` (a refusal is `editor.play.copy_failed`); a default
   `Time` inserted; the play selection set to the editor selection; the play viewport's camera
   saved; `setPlaying(true)`. With `paused`, the session starts paused. Nothing is read from or
   written to disk, so an untitled or unsaved level plays as it is.
8. **Ticking.** The application calls `PlaySession::tick(nowNs)` once per frame after input and
   the main-thread jobs and before the UI, with `Input::frameTimeNs()`. Running and not paused, a
   call runs `tickGameWorld` with the time since the previous call. The first call after a start
   or a resume only takes its time and runs nothing, so time spent paused never reaches the game
   and no command needs a clock. A minimized window (no pixel area) calls no tick. While a session
   ticks, the application requests a frame every frame (ADR-0029 rule 12); paused, the idle policy
   applies again. Focus loss doesn't pause. Headless sessions tick only through `ith.play.step`,
   which runs `frames` ticks of exactly `deltaSeconds` and stays paused.
9. **After every tick** the session bumps the game world's `EditorChanges`: `structure` when
   `World::structureGeneration()` moved, and `values` as "everything may have changed" (a new
   `EditorChanges::bumpAllValues()`, after which `touchedSince` of an older generation returns
   nullopt). The Outliner then rebuilds only on structure, and Details re-reads only its shown
   entities.
10. **Stop** cannot fail: it cancels an open game-world interaction, releases the mouse and turns
    capture on click off, drops the play viewport's waiting pick, restores the saved camera bit for
    bit, destroys the session (world, schedule, `Time`, play selection, spectator), and clears the
    play flag. The editor selection, history, dirty state and every camera are as before Play,
    except for Keep Simulation Changes entries. Quitting with a session running stops it first.
11. **Lifecycle events are deferred** (amends ADR-0016 rule 15 and ADR-0017 rule 10): no ECS
    events are built in this step. `PlaySession` marks the two points in code, after the copy
    ("`WorldStarted` goes here") and first in Stop ("`WorldEnding` goes here"), and the spec's
    requirement 36 is enabled when events land.

### The active world

12. **`EditorSession::active()`** gives the game world's set while a session exists, the document's
    otherwise (Options). While playing, these act on it: `ith.selection.set`,
    `ith.edit.select_all`, `ith.edit.select_none`, the value commands of rule 13, and the queries `ith.query.selection`, `ith.query.entity`, `ith.query.find` and `ith.query.world_hash`,
    which take an optional `world: "editor" | "game"` (amends ADR-0037 rule 17).
    `ith.query.document` and `ith.query.history` always describe the document. A view model keys
    its cache on the active world's id as well as on its counters, so switching worlds always
    rebuilds it.
13. **Editing the game world.** `CommandDesc` gains `playEditable` (amends ADR-0037 rule 1). While
    playing, the registry refuses `Edit` commands without it with `editor.command.playing` as
    today, and runs those with it in a scope of the active `Transactions`. It is set on
    `ith.entity.set_property`, `reset_property`, `insert_element` and `erase_element` only:
    `set_enabled` adds or removes `Disabled`, a structural change, and stays refused (values only in
    P2). Details drags on the game world preview live and Esc cancels them through the same scope;
    nothing reaches the editor history or the dirty state. `DetailsModel` drops its own playing
    refusal for these.
14. **Also refused while playing** with `editor.command.playing`: Undo, Redo, every other `Edit`
    command, New Level, Open Level, opening a level from the Content Browser, and **Save and Save
    As** (amends ADR-0037 rule 10, which refused only commands that change the editor world). The
    Content Browser's file operations stay allowed (ADR-0049). Entities spawned during play have no
    stable id, so they can't be selected; the Outliner shows them greyed as "Spawned <n>".
15. **Labels in the game world** come from the editor twin with the same stable id; the game world
    has no `EditorLabel`.

### Views

16. **The play viewport draws the game world; the others keep drawing the editor world.** Every
    viewport tints the active selection, and a pick in any viewport is validated against the world
    that viewport draws, then applied to the active selection by stable id; Focus frames the
    active selection in the world its viewport draws. Transform propagation
    runs for each drawn world before the first instance that draws it, as for the editor world
    (ADR-0045 item 7). The gizmo stays hidden (ADR-0048 item 6). This settles ADR-0042 rule 1.
17. **Simulate and ejected:** the play viewport's camera, with every editor camera control.
    **Play:** a spectator view, a free camera that turns with captured mouse motion and flies with
    W/A/S/D and Q/E at the viewport's speed step, fed by a `Game`-layer consumer in `editor_core`
    through the camera controller, so it gets input exactly when a game would. Possessed, the play
    viewport draws as game view whatever its flag says.
18. **Possess and Eject** keep one saved pose: Eject leaves the camera where the spectator was and
    saves that pose; Possess resumes the spectator at the saved pose, or, in a session that started
    with Simulate, at the camera's current pose. When the gameplay framework brings a player with a
    view, that view replaces the spectator and Possess and Eject become ADR-0016 rule 9's operations
    on the `Controls` link.

### Input

19. **The Play In Editor consumer** (`editor_core`, `Editor` layer, added last so it is on top)
    handles the Play In Editor context while a session runs, and passes everything else on.
    **Escape** stops play unless something else uses it; then it passes it on, and that thing takes
    it (amends ADR-0029 rule 7 and ADR-0037 rule 4, **for Escape only**). Escape passes on while
    the mouse is not captured and an interaction holds a transaction open in either world (the
    keymap consumer cancels it), or the UI reported in its last frame an active ImGui text field or
    an open popup (`WantTextInput`, `IsPopupOpen` with `ImGuiPopupFlags_AnyPopup`). This covers
    inline renames, which are text fields. While a popup is open, the ImGui consumer consumes
    Escape. While the mouse is captured, Escape always stops. Shift + F1 and F8 always run.
20. **Capture.** While possessed, the application turns capture on click on for the window; the
    viewport consumer passes on presses over the play viewport's image, so capture takes them
    (ADR-0027 rule 3). Capture ends on Shift + F1, Eject, Stop and focus loss; focus loss also
    releases held keys and keeps the session running.
21. **While captured, the keyboard is the game's except the three Play In Editor keys**: the
    ImGui consumer neither forwards keys to ImGui nor consumes them (a text field left active by
    the click that captured gets no typing), and the keymap consumer runs and consumes no chord (it reads
    `Input::mouseCaptured` for the event's window through an `Input` it is given). This clarifies
    ADR-0013 rule 10: "editor shortcuts" sit above the game only while the mouse is free.
22. **While a session runs and the mouse is free**, input is the editor's: the keymap consumer, the
    lowest `Editor`-layer consumer, consumes every keyboard and mouse event that reaches it, bound
    or not, so the `Ui` and `Game` layers get none. The one exception is a mouse press while
    possessed, which it passes on so capture on click takes it (rule 20). Gamepad events pass to
    the game while possessed and are consumed while ejected.
23. **No Pause chord, no whole-editor tint** in P2; the viewport border and label, the toolbar and
    the status bar show the state (spec 3.13).

### Keep Simulation Changes

24. `ith.play.keep_simulation_changes {ids?}` (default: the play selection) matches each id in both
    worlds by stable id. An id missing from the game world is skipped as `editor.play.not_found`,
    one missing from the editor world as "not in the level".
25. **Compared:** each component type the entity has in both worlds that is neither Transient nor
    EditorOnly, except `StableId` and `ChildOf`. The game value is copied, its entity references
    mapped game entity → stable id → editor entity (`remapEntities`; one without a stable id or an
    editor twin becomes null, with a warning), and compared with `TypeInfo::equals`.
26. **Kept:** each differing component, as a `set` of the whole value (the empty path), or an Add
    replacing it for an immutable type (ADR-0030). **Reported, not applied:** components only in
    one world, a changed parent, and a changed enabled state.
27. **One editor transaction** named "Keep Simulation Changes", recorded while the session runs
    (ADR-0037 rule 10's exception); it makes the level dirty, keeps the editor selection, and is
    undone after Stop like any entry. Nothing to keep records nothing. The viewports that draw the
    editor world show the kept values at once; the game world doesn't change.

### Tests and automation

28. The spec's requirements 1 to 49 are the acceptance tests, with these adjustments: requirement
    45b runs `tickGameWorld` on a plain `World`; requirement 36 waits for events (rule 11); the
    `ith.entity.set_enabled` part of requirement 31 is refused (rule 13); requirement 15's first
    delta after a resume is measured from the first tick call after it (rule 8). The automation
    script `tests/editor_core/scripts/play_in_editor.json` runs every play command and query
    (ADR-0037 rule 20).

### Implementation slices

29. Each slice builds, passes `verify` and is committed on its own:
    1. **`ecs` and `gameplay`:** the structure generation with its tests; the `gameplay` module
       (`Time`, `kMaxTickDelta`, `buildGameSchedule`, `tickGameWorld`), its tests and its
       `architecture.md` row.
    2. **The session in `editor_core`:** `PlaySession`, start, tick, pause, step, stop,
       `active()` and the commands and view models moved onto it, `playEditable`, the refusals
       of rule 14, `bumpAllValues`, spawned-entity rows, `ith.query.play`, the automation script,
       and the start and tick benchmarks of requirements 46 and 47.
    3. **Views and input in `editor_core`:** the selected viewport, the camera save and restore,
       the spectator consumer, Possess and Eject, the Play In Editor consumer, the keymap's capture
       rules (rules 19 to 22).
    4. **Keep Simulation Changes** and the K binding.
    5. **`editor_ui` and the application:** the toolbar, indicators, theme tokens, the Viewport
       panel drawing the game world, the Outliner and Details notes and menu entry, the Escape
       report, and in `editor/Main.cpp` the tick, capture on click, the idle rule, Quit stopping
       the session, the consumer order, and `registerReflection_gameplay`.

### Architecture entry

30. `docs/architecture.md` gains a row: **`gameplay`** | framework | runtime | the `Time` world
    resource and its advance, the game-world schedule setup and tick (`buildGameSchedule`,
    `tickGameWorld`); later the lifecycle events and the game-world system list | depend on the
    editor, platform or client modules; hold mutable global state (ADR-0017 rule 11); evaluate
    input. Edges: `gameplay` → `foundation`, `reflection`, `ecs`; `editor_core` → `gameplay`. The
    `ecs` and `editor_core` rows gain the structure generation and the Play In Editor session.

## Consequences

- Play and Simulate work on any level, saved or not, and Stop restores the editor by throwing the
  game world away; no restore code exists for anything but the play viewport's camera.
- Headless tests drive the game world deterministically through `ith.play.step`; the same session
  code runs in the editor.
- Game modules get a runtime home for time and the game schedule that the player and server
  share, before the gameplay framework exists.
- About sixty call sites move from `document()` to `active()`; a missed one acts on the editor
  world during play, and the registry's refusal catches the `Edit` ones. The panel tests during
  play cover the rest.
- The spectator, the camera save and restore, and the "first tick takes the baseline" rule are P2
  placeholders: the gameplay framework replaces the spectator with the player's view.
- `WorldStarted` and `WorldEnding` are not sent until ECS events exist; no game module needs them
  in P2.
- ADR-0013, 0014, 0016, 0017, 0029, 0037 and 0042 are marked as amended by this ADR in their
  headers and in the index.
- Follow-up: the spec follows this ADR where they differ (the Time advance, the first tick,
  `set_enabled`, the camera, the Escape report, kept values shown in editor viewports).
- **Owner's answers at acceptance (2026-11-28):** Save and Save As are refused while playing; the
  `gameplay` module holds `Time` and the game-world schedule setup, with `Time` advanced by
  `tickGameWorld` before the schedule runs rather than by a system; Escape yields to text fields,
  popups, inline renames and interactions; `ith.entity.set_enabled` is refused during play; every
  other open question takes the spec's recommendation.
- **Revisit when:**
  - ECS events land (rule 11; requirement 36);
  - the gameplay framework brings players and view targets (rules 17 and 18);
  - users ask for structural edits or selecting spawned entities during play;
  - networked Play In Editor needs several game worlds and windows (P5, ADR-0017 rule 14);
  - starting Play on 10,000 entities takes more than 100 ms on the Low-tier machine.
