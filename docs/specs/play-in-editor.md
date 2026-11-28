# Spec: Play In Editor and Simulate

- **Status:** Decided in [ADR-0053](../decisions/0053-play-in-editor-and-simulate.md), not
  implemented yet. The owner decided every open question and conflict on 2026-11-28 (section 6);
  ADR-0053 records them, settles ADR-0042 rule 1 and makes the amendments they need (ADR-0013
  rule 10, ADR-0014, ADR-0016 rule 15, ADR-0017 rule 10, ADR-0029 rule 7, ADR-0037 rules 1, 4, 10
  and 17, ADR-0042 rules 1 and 3). Where this spec and ADR-0053 differ, the ADR wins; the spec has
  been brought in line with it
- **Date:** 2026-11-28
- **Decided by:** [ADR-0053](../decisions/0053-play-in-editor-and-simulate.md)
- **Builds on:** [ADR-0003](../decisions/0003-clean-room.md) (clean room),
  [ADR-0004](../decisions/0004-modules-and-layering.md) (layers),
  [ADR-0005](../decisions/0005-reference-hardware.md) (the Low tier reference machine),
  [ADR-0008](../decisions/0008-editor-familiar-to-unreal-developers.md) (rules 2 to 5: the toolbar
  with Play and Simulate, the vocabulary, Esc, Shift + F1 and F8, editing during play),
  [ADR-0013](../decisions/0013-input.md) and [ADR-0027](../decisions/0027-input-rules-completed.md)
  (consumer stacks, mouse capture, capture on click, focus loss),
  [ADR-0014](../decisions/0014-ecs-storage-and-world.md) (world ids, schedules),
  [ADR-0016](../decisions/0016-gameplay-model-on-the-ecs.md) (rules 6, 9, 12 and 15: app-scoped
  resources, possess and eject as data operations, the `Time` resource, `WorldStarted` and
  `WorldEnding`),
  [ADR-0017](../decisions/0017-runtime-and-editor-separation.md) (rules 8 and 10 to 14: editor-only
  data, the in-process world copy, game systems only in game worlds, fresh app resources per
  session, where it plays, Simulate, Keep Simulation Changes),
  [ADR-0029](../decisions/0029-editor-ui-toolkit.md) (rules 7 and 12: the input stack, the idle
  policy),
  [ADR-0032](../decisions/0032-entity-references-stable-ids-and-world-copy.md) (`StableId`,
  `copyWorld`, `remapEntities`),
  [ADR-0037](../decisions/0037-editor-commands-transactions-selection-and-automation.md) (rules 1
  to 6, 10 to 13 and 17 to 20: commands, transactions, nothing undoable while playing, selection,
  change counters, automation; Play Alt + P, Simulate Alt + S),
  [ADR-0042](../decisions/0042-editor-viewport.md) and its amendments
  [ADR-0045](../decisions/0045-editor-viewport-amendments-after-the-viewport-panel.md) to
  [ADR-0048](../decisions/0048-editor-viewport-translate-gizmo-as-built.md) (the viewport, its
  camera, redraw rule, picking and the gizmo hidden while playing),
  [ADR-0049](../decisions/0049-editor-content-browser-v1.md) (rule 3, keymap contexts; content
  file operations during play)

This is a clean-room behavioural spec (ADR-0003). It was written from public documentation and the
observable behaviour of game editors; the sources are listed at the end. The implementation is
built from this spec and Ironthorn's architecture, never from another engine's code.

## 1. Purpose and scope

The editor world is never simulated (ADR-0017 rule 10). To see a level run, a user needs
**Play In Editor**: the level, copied into a separate game world, ticking with the same game
systems a player runs, inside the editor process, in a viewport, with the editor still open
around it. And **Simulate**: the same game world running without a player, watched through the
editor camera, with selection and Details available. A user switches between the two with
**Possess** and **Eject**, edits the running world through Details to try values, and keeps the
values worth keeping with **Keep Simulation Changes**. Stop throws the game world away, and the
editor is exactly as it was.

P2 has no gameplay framework yet: no `Player` component, no `Controls` relation, no camera
component and no game modules (ADR-0016 rules 7 to 9 are contracts, not code). This spec makes
Play In Editor useful and testable before them, and defines where they plug in.

### In scope for P2

- The main toolbar's play controls: Play, Simulate, Pause and Resume, Step, Stop, Possess and
  Eject, with their states (3.2).
- One game world at a time, in process, in the **selected viewport** (3.3).
- The game world: a copy without editor-only data, the game-world systems, fresh change ticks and
  a fresh `Time` (3.4, 3.5).
- The view: the editor camera in Simulate and after Eject; a built-in **spectator view** for Play
  while no game provides a view (3.6).
- Input routing between game and editor, freeing the mouse, Esc to stop (3.7).
- The Outliner, Details and viewport showing the game world, edits to it, picking in it (3.8, 3.9).
- What is refused while playing, and with which error (3.10).
- Stop restoring the editor exactly (3.11).
- Keep Simulation Changes (3.12).
- A visible play state in the viewport, the toolbar and the status bar (3.13).
- Every action as a command and query, scriptable headless (3.14).

### Out of scope for P2

| Out of scope | Why, or where it goes |
|---|---|
| Playing in a **new editor window** | ADR-0017 rule 12 names it as an option; ADR-0017's consequences schedule only the world copy, Simulate and Keep Simulation Changes for P2. Decision 1 |
| Several players, client and server worlds, networked play | P5 with networking (ADR-0017 rule 14) |
| **Standalone game** (a separate player process on cooked data) | Needs the cooker (ADR-0017 rule 15) |
| A player, pawn, player start, game mode, `ViewTarget` camera | The gameplay framework (ADR-0016 rules 7 to 10); P2 uses the spectator view (3.6) |
| Running the input action layer (`InputActions`) for local players | Comes with the first consumer of game input in a game world (decision 6) |
| Play From Here (start at the cursor) | Needs a player start; with the gameplay framework |
| Structural edits of the game world (create, delete, duplicate, reparent, add or remove components) | P2 edits values only (3.9; decision 8) |
| Selecting entities spawned during play | They have no stable id, and the selection holds stable ids (ADR-0037 rules 5 and 11; decision 9) |
| The translate gizmo on the game world | Hidden while playing (ADR-0048 item 6); Details edits transforms |
| A visible in-editor console or stat overlays for the game | Development features of their own (ADR-0017 rule 7) |
| Hot reload of game code while playing | ADR-0016 rule 19, its own ADR in P5 |
| Crash isolation of game code | In-process by decision (ADR-0017 rule 10 and its rejected IPC option) |
| Keeping structural changes (entities spawned or removed, components added or removed) | ADR-0017 rule 13: values of existing entities only |

## 2. Concepts and terminology

| Ironthorn name | What it is |
|---|---|
| **Play In Editor**, **play** | Running a game world in the editor process. "Playing" covers both Play and Simulate |
| **Play session** | One run, from Play or Simulate to Stop. At most one exists |
| **Editor world** | The open level's world (`EditorDocument`), never simulated, frozen while a session runs |
| **Game world** | The session's world: a copy of the editor world under a new world id, ticked by the game-world schedule, discarded at Stop |
| **Active world** | The world panels and selection commands act on: the game world while a session runs, the editor world otherwise. `EditorSession::active()` gives it with its `Transactions`, `Selection`, `EditorChanges` and entity mapping (ADR-0053 rule 12) |
| **Game-world systems** | The systems a game world runs: engine runtime systems and, later, the game modules' systems (ADR-0017 rule 11). The editor world never runs them |
| **Tick** | One call of `gameplay`'s `tickGameWorld`: it advances the game world's `Time`, then runs the game-world schedule on it once |
| **Possessed** | The session gives the game the player's view and input (Play) |
| **Ejected** | The session runs without a player; the viewport uses the editor camera (Simulate, or Play after Eject) |
| **Paused** | The session exists but doesn't tick; the view, selection and Details still work |
| **Play viewport** | The viewport instance the session shows; the other viewports keep showing the editor world |
| **Selected viewport** | The viewport instance a session starts in: the one the last mouse press on a viewport image went to, kept in the `Viewports` store, else `ith.viewport.1` (ADR-0053 rule 5) |
| **Spectator view** | P2's built-in view for Play while no game provides one: a free camera driven by game input while the mouse is captured (3.6) |
| **Play camera** | The play viewport's own camera, used as it is during a session; the session saves it at start and restores it bit for bit at Stop (ADR-0053 rules 7 and 10) |
| **Captured** | The mouse is captured for the game (ADR-0013 rule 11): cursor hidden and locked, all mouse input to the `Game` layer |
| **Free the mouse** | Ending capture without ending the session (Shift + F1) |
| **Play selection** | The selection of the game world, by stable id, separate from the editor selection |
| **Keep Simulation Changes** | Copying the component values of selected game-world entities back to the same entities of the editor world, as one undo entry |

"Play In Editor", "Simulate", "Possess", "Eject" and "Keep Simulation Changes" are generic editor
vocabulary kept for familiarity (ADR-0008 rule 3).

## 3. Behaviour

### 3.1 States and lifecycle

A session has one **state** and, while it runs, two flags:

| State | `possessed` | `paused` | Meaning |
|---|---|---|---|
| `stopped` | — | — | No session; the editor edits the editor world |
| `running` | true | false | Play: ticking, the game has the view and (when captured) the input |
| `running` | false | false | Simulate, or Play after Eject: ticking, the editor camera views it |
| `running` | either | true | Paused: no ticks |

Transitions, each a command (3.14):

| From | Command | To | Notes |
|---|---|---|---|
| stopped | Play (`ith.play.play`) | running, possessed, not paused | Starts a session (3.3) |
| stopped | Simulate (`ith.play.simulate`) | running, ejected, not paused | Starts a session |
| running | Pause (`ith.play.pause`) | paused | Toggles: on a paused session it resumes |
| paused | Step (`ith.play.step`) | paused | Runs exactly the ticks asked for (3.5) |
| running | Eject / Possess (`ith.play.possess_or_eject`) | the other `possessed` value | Paused or not; 3.6 |
| running | Free the mouse (`ith.play.free_mouse`) | unchanged | Ends capture only (3.7) |
| running | Stop (`ith.play.stop`) | stopped | Discards the session and restores the editor (3.11) |

- Play and Simulate are refused while a session runs (`editor.play.running`); a running session
  switches with Possess and Eject (ADR-0017 rule 13).
- Stop, Pause, Step, Possess and Eject, Free the mouse and Keep Simulation Changes are refused with
  no session (`editor.play.not_running`). Step is refused unless paused (`editor.play.not_paused`).
- Quitting the editor while a session runs stops it first, then follows the usual quit rules.

### 3.2 The main toolbar

The main toolbar is a strip under the main menu bar, over the whole width of the main window
(ADR-0008 rule 2). The editor has none yet (the guide's "Not here yet"); this step adds it with
the play controls only. The place-actor menu and build come later.

| Control | stopped | running, possessed | running, ejected | paused |
|---|---|---|---|---|
| **Play** | shown | hidden | hidden | hidden |
| **Simulate** | shown | hidden | hidden | hidden |
| **Pause** / **Resume** | hidden | Pause | Pause | Resume |
| **Step** | hidden | shown, disabled | shown, disabled | shown |
| **Stop** | hidden | shown | shown | shown |
| **Possess** / **Eject** | hidden | Eject | Possess | as the `possessed` flag |

- Every control runs its command through the registry and changes nothing itself. A control whose
  command can't run now is disabled, with the command's reason as its tooltip (ADR-0037 rule 1).
  The tooltip also names the chord the keymap binds ("Play (Alt+P)").
- Play and Simulate are disabled with their reason while a level opens (`editor.document.busy`)
  and while an interaction holds a transaction open (`editor.command.interaction_open`).
- The controls keep their place: the buttons that appear while running take the place of Play and
  Simulate, so the pointer lands on Stop where Play was. Icons come from the editor's icon font
  (Lucide); the colours are theme tokens.
- The toolbar is drawn in every state, including while the mouse is captured; it can't be clicked
  then (the cursor is hidden and locked).

### 3.3 Starting a session

Play and Simulate do the same work; only the `possessed` flag differs.

1. **Checks**, in this order, each failing with nothing changed:
   - a session already runs: `editor.play.running`;
   - a level is opening: `editor.document.busy`;
   - an interaction holds a transaction open: `editor.command.interaction_open`;
   - the game-world schedule fails to build: `gameplay`'s `buildGameSchedule` adds the engine's
     game-world systems, then `EditorSessionOptions::gameSystems` (empty in P2), then builds; a name
     used twice, a cycle or undeclared access gives its `ecs.schedule.*` error, also logged.
2. **The play viewport** is the selected viewport (2). In the UI, if that panel is closed, the
   viewport panel `ith.viewport.1` is opened and used. A headless session has no panels; the play
   viewport is still the instance id in the `Viewports` store, and nothing draws it.
3. **The copy.** A new `World` is created with a new world id, and `copyWorld(editorWorld,
   gameWorld, {.excludeEditorOnly = true})` fills it (ADR-0032 rule 9; ADR-0017 rule 10). The copy
   is taken from the editor world in memory: an untitled or unsaved level plays as it is, and
   nothing is saved or read from disk. A refused copy (a broken precondition, which is a bug)
   fails with `editor.play.copy_failed`.
4. **Fresh session state:** the app-scoped resources a game registers (ADR-0016 rule 6) are created
   for this session (none exist in P2); a default `Time` resource is inserted into the game world
   (3.5); the play selection starts as the editor selection (every id exists in the copy).
5. **Views:** the play viewport's camera is saved, to be restored at Stop; during the session that
   same camera is the play camera. With `possessed`, the spectator view starts at its pose (3.6).
6. **The document's play flag** is set (`EditorDocument::setPlaying(true)`), so commands that
   change the editor world are refused from here on (3.10), and the change counters are bumped so
   every panel rebuilds from the game world.
7. **`WorldStarted`** is sent to the game world before its first tick (ADR-0016 rule 15), once the
   engine has events (decision 5).
8. With `possessed`, the editor window's capture on click is turned on for the play viewport
   (3.7); the mouse is not captured until the user clicks into it.

The first tick runs in the next editor frame (3.5).

### 3.4 What the game world holds

- **Every entity of the editor world, in the same slots under the game world's id**, with every
  component that is not `EditorOnly` (so no `EditorLabel`, no editor icons, no notes) and every
  world resource that is not `EditorOnly`; entity references inside values point at the copies
  (ADR-0032 rules 10 to 12). The copy looks like what the cooker would produce (ADR-0017 rule 10).
- **Stable ids are kept** (`StableId` is neither Transient nor EditorOnly, ADR-0032 rule 6), so an
  entity of the game world is matched to its editor twin by id, never by handle.
- **Fresh ticks:** the game world's clock starts over and every component value counts as added and
  changed (ADR-0032 rule 12), so game systems see a world that was just loaded. No per-system last
  run is copied.
- **Game-world systems only.** The session builds its own `Schedule` with `buildGameSchedule`
  (3.3). In P2 the engine's only game-world system is transform propagation
  (`ith::propagate_transforms`, `PostUpdate`); `Time` is not a system but is advanced by
  `tickGameWorld` before the schedule runs (3.5). Game modules add theirs through
  `EditorSessionOptions::gameSystems` once they exist (ADR-0016 rule 2, ADR-0017 rule 5). The editor world keeps running only what it runs
  today.
- **Entities spawned during play** get no `StableId` (ADR-0032 rule 7). They live and die with the
  game world.
- **Asset edits are real.** Saving or reimporting an asset during play changes it on disk and it
  stays changed after Stop (ADR-0017 rule 13).

### 3.5 Ticking, time, pause and step

- **One tick call per editor frame**: the application calls `PlaySession::tick(nowNs)` with
  `Input::frameTimeNs()` after the frame's input and main-thread jobs and before the UI, so the
  panels and the viewport show this frame's game state. Running and not paused, the call runs
  `tickGameWorld(world, schedule, seconds)` with the time since the previous call (ADR-0053 rule 8).
  While a session ticks, the editor requests a frame every frame (ADR-0029 rule 12); paused, it
  follows the idle policy again.
- **How a tick advances time.** `tickGameWorld` (module `gameplay`) clamps `seconds` to
  [0, `kMaxTickDelta`] (a non-finite value asserts and counts as 0), advances the world's `Time`
  (inserting a default one when it has none), and only then runs the schedule. So `Time` is final
  before any system runs, nothing has to be ordered against it, and the player and dedicated server
  loops call the same function.
- **After every tick** the session bumps the game world's `EditorChanges`: `structure` when
  `World::structureGeneration()` moved, and `values` as "everything may have changed"
  (`EditorChanges::bumpAllValues()`, after which `touchedSince` of an older generation returns
  nullopt). The Outliner rebuilds only on structure; Details re-reads only its shown entities.
- **The `Time` resource** (ADR-0016 rule 12, ADR-0053 rule 1) is a reflected world resource of the
  `gameplay` module:

  | Field | Meaning |
  |---|---|
  | `delta` | Float seconds of game time for this tick: `unscaledDelta × dilation`, 0 while `paused` |
  | `unscaledDelta` | Float real seconds for this tick, clamped to `kMaxTickDelta` (0.1 s) |
  | `elapsed`, `unscaledElapsed` | Double sums of the above since the session started (0 at start) |
  | `frame` | `std::uint64_t` ticks run so far; 0 before the first tick, 1 in it |
  | `dilation` | Float, game-controlled time scale, default 1 |
  | `paused` | Game-controlled pause (a pause menu): `delta` is 0, the systems still run |

  A `dilation` or `paused` that a system writes takes effect in the next tick, since `Time` is
  advanced before the systems run.

  The editor's Pause is not `Time::paused`: an editor pause runs no systems at all, and the game's
  own pause is left to the game.
- **Delta rules:** the first tick call after a start or a resume only records its time and runs
  nothing (no `Time` advance, no systems); the next call ticks with the time since it. So time spent
  paused never reaches the game, and no command needs a clock. Any longer gap (a stall, a
  breakpoint, a window drag) is clamped to 0.1 s.
- **Step** (`ith.play.step {frames: 1, deltaSeconds: 1/60}`) runs `frames` ticks (1 to 1000)
  immediately, each with `unscaledDelta = deltaSeconds` (greater than 0, at most 0.1), and stays
  paused. It is the deterministic way to advance a headless session (3.14).
- **While paused**, values edited in Details still show: transform propagation runs for the game
  world before the viewport renders, as it does for the editor world (ADR-0045 item 7); it writes
  only the Transient `WorldTransform`.
- A minimized editor window (no pixel area) calls no tick, so the game doesn't advance; on restore
  the clamp above applies. Losing window focus doesn't pause the game (decision 10).
- Headless sessions have no frame loop and tick only through Step.

### 3.6 Cameras and the view

- **Simulate and Ejected:** the play viewport uses its **own camera**, with every editor camera
  control (fly, orbit, dolly, pan, focus, ADR-0008 rule 4); the viewport consumer, camera
  controller, redraw key and renderer work unchanged. The session saved that camera at start and
  restores it at Stop (ADR-0053 rules 7 and 10), so starting Simulate doesn't move the view and
  flying during it leaves no trace afterwards.
- **Play with no game view (P2):** the play viewport shows the **spectator view**, a free camera
  that starts at the play camera's pose. While the mouse is captured, mouse motion turns it (no
  button held) and the fly keys move it at the viewport's speed step (W/A/S/D along its axes, Q/E
  down and up), as RMB fly does in the editor (ADR-0042 rule 7). It is fed by a consumer in the
  `Game` layer, so it gets input exactly when a game would. When the gameplay framework brings a
  local player with a `ViewTarget`, that view replaces the spectator, and Possess and Eject become
  ADR-0016 rule 9's data operations on the `Controls` link.
- **Possess and Eject keep one saved pose** (ADR-0053 rule 18). **Eject** leaves the viewport's
  camera where the spectator was ("the camera stays where the player was") and saves that pose.
  **Possess** resumes the spectator at the saved pose; in a session that started with Simulate, at
  the camera's current pose (there is no player start in P2).
- **Game view while possessed:** the play viewport draws as game view (no grid, no selection tint,
  no gizmo, ADR-0042 rule 20) whatever its flag says. Ejected, it follows the viewport's own
  game-view flag (G).
- **Field of view:** the spectator and the viewport's camera use the editor camera's `fovX` (ADR-0046
  item 7; the game camera's own rule comes with P3).
- **No session pose leaks.** Stop restores the saved camera bit for bit, and `Viewports.json` is
  written only when a level closes and at exit, both of which stop the session first, so it never
  holds a pose from a session. The spectator pose is session state. The other viewports keep their
  own cameras and keep showing the editor world.

### 3.7 Input routing

The editor window's stack (ADR-0029 rule 7, ADR-0042 rule 8), top first: the **Play In Editor
consumer** (`editor_core`, `Editor` layer, added last so it is on top; it handles the Play In Editor
context while a session runs and passes everything else on), the ImGui consumer, the viewport
consumer, the keymap consumer (the lowest `Editor`-layer consumer), then the game consumers (`Ui`,
`Game`; in P2 the spectator). ADR-0053 rules 19 to 22 add these rules for a running session:

1. **Capture.** While possessed and not captured, a mouse press on the play viewport's image that
   the panels leave unconsumed captures the mouse and is consumed by the capture; it never reaches
   the game (ADR-0027 rule 3). The viewport consumer passes such presses on (no pick, no camera
   gesture). Presses elsewhere (panels, other viewports) behave as in the editor. An overlay in the
   play viewport says "Click to control the game · Shift+F1 frees the mouse" while possessed and not
   captured.
2. **While captured, the keyboard is the game's except the Play In Editor keys** (Esc, Shift + F1,
   F8), and all mouse input goes to the `Game` layer (ADR-0013 rule 11). The ImGui consumer neither
   forwards keys to ImGui nor consumes them, so a text field left active by the click that captured
   gets no typing; ImGui is also told the mouse is gone (ADR-0029 rule 7). The keymap consumer runs
   and consumes no chord (it reads `Input::mouseCaptured` for the event's window), so W moves the
   spectator instead of choosing the translate tool, and Ctrl + Z reaches the game. This clarifies
   ADR-0013 rule 10: "editor shortcuts" sit above the game only while the mouse is free (section 6,
   C4).
3. **While a session runs and the mouse is free** (Simulate, ejected, or after Shift + F1), input is
   the editor's: the keymap, panels and viewports work as when editing, with the refusals of 3.10.
   **The keymap gate:** the keymap consumer, the lowest `Editor`-layer consumer, consumes every
   keyboard and mouse event that reaches it, bound to a command or not, so the `Ui` and `Game`
   layers get none. The one exception is a mouse press while possessed, which it passes on so
   capture on click takes it (rule 1). Gamepad events pass to the game while possessed and are
   consumed while ejected (ADR-0053 rule 22).
4. **Shift + F1** (`ith.play.free_mouse`) ends capture (ADR-0013 rule 11) and keeps the session
   running; clicking into the play viewport captures again. Capture also ends when the editor
   window loses focus, and when Eject or Stop runs.
5. **F8** (`ith.play.possess_or_eject`) ejects a possessed session (ending capture) or possesses an
   ejected one (the next click captures).
6. **Esc** first ends whatever in the editor is using it, and only then stops play (ADR-0053
   rule 19). The Play In Editor consumer stays on top and decides: **while the mouse is captured,
   Escape always stops play**, since ImGui gets no keys then (rule 2). While the mouse is free, it
   passes Escape on, and the user of it takes it, when:
   - an interaction holds a transaction open in either world (a Details drag; the keymap consumer
     cancels it, ADR-0037 rule 6); or
   - the UI reported in its last frame an active ImGui text field or an open popup (the same
     one-frame lag as ADR-0029 rule 7's want-capture flags). This covers the Content Browser's
     inline rename, which is a text field, and dialogs, which are popups. **While a popup is open,
     the ImGui consumer consumes Escape**, so it closes the popup and reaches nothing below.

   Otherwise Escape runs `ith.play.stop`, possessed or not. This amends ADR-0029 rule 7 and ADR-0037
   rule 4 for Escape only (section 6, C2); Shift + F1 and F8 always run.
7. **Focus loss** releases every held key and button (ADR-0013 rule 9) and ends capture; the session
   keeps running.

**The free-the-mouse chord stays Shift + F1.** It is not an Ironthorn choice to make again here:
ADR-0008 rule 4, ADR-0013 (context and rule 11), ADR-0017 rule 12 and ADR-0029 rule 7 all fix it,
and `engine/config/Keymap.json` already binds `ith.play.free_mouse` to it. A default key chord is a
workflow convention, which ADR-0008 rule 8 lets the editor share for day-one familiarity; it isn't
a name or a piece of expression. It also suits the job: F1 with Shift is rarely a game binding, it
sits away from the WASD hand, and every binding is remappable (ADR-0008 rule 4). The owner
confirmed it (section 6, C1).

### 3.8 Panels while playing

**Outliner.**

- Lists the **active world**: the game world's entities in Outliner order, refreshed when the game
  world's structure changes (spawns, despawns, reparenting, order, component sets), not every tick.
- **Labels** come from the editor world: the game world has no `EditorLabel` (editor-only), so an
  entity shows the label of the editor entity with its stable id. An entity spawned during play has
  no stable id; it shows "Spawned <n>" (its slot index), greyed, and can't be selected
  (decision 9).
- **Read-only structure:** rename, delete, drag and drop, Enabled toggles and Add Shape are
  refused (3.10); the context menu shows Keep Simulation Changes instead (3.12).
- A note at the top of the panel reads "Game world · changes are lost on Stop".

**View models follow the world, not only its counters.** Every view model (Outliner, Details, the
viewport's selection tint) keys its cache on the active world's id as well as on its change
counters, so switching worlds at Play and at Stop always rebuilds it, even when the counters happen
to match (ADR-0053 rule 12).

**Details.**

- Shows the **primary entity of the play selection** in the game world, its values refreshed every
  tick while running (only the shown entities are read, so the cost follows the selection, not the
  world).
- **Editable values** (ADR-0008 rule 5, ADR-0017 rule 13): Set Property, Reset to Default, and list
  element insert and erase (3.9).
- The Enabled toggle, Add Component and Remove Component are refused (3.10): they add or remove
  components, which is structural.
- The same note as the Outliner's, plus "Keep Simulation Changes keeps them".

**Viewport.**

- The play viewport draws the game world through the same preview pass as the editor world; the
  other viewports keep drawing the editor world, which changes only through Keep Simulation
  Changes (3.12). Transform propagation runs for each drawn world before the first instance that
  draws it (ADR-0045 item 7). Every viewport tints the active selection.
- The play viewport re-renders every frame while the session ticks; paused, it follows the redraw
  rule (ADR-0045 item 8) with the game world's changes as its world key.
- **Picking** (while the mouse is free): a click in any viewport is validated against the world that
  viewport draws (the play viewport's id target holds game-world handles, the others editor-world
  handles), mapped to a stable id and applied to the **active selection**, the play selection, with
  the click's modifiers (ADR-0047 item 9). A hit on a spawned entity changes nothing (ADR-0047
  item 10).
- **The gizmo is hidden** while playing (ADR-0048 item 6).
- Focus frames the active selection in the world its viewport draws; Snap to Floor is refused
  (3.10).

**Content Browser.** Works as when editing (ADR-0049, content spec 3.19), except that opening a
level is refused (`editor.command.playing`): its file operations (new folder, rename, move,
duplicate, delete, import, reimport, fix) stay allowed during play, as ADR-0049 says (section 6,
C3), and change assets for real (ADR-0017 rule 13).

### 3.9 Editing the game world

- The four Details value commands (`ith.entity.set_property`, `ith.entity.reset_property`,
  `ith.entity.insert_element`, `ith.entity.erase_element`) carry the `playEditable` flag and act on
  the **game world** while a session runs, in a scope of the active `Transactions`, naming entities
  by stable id as always (ADR-0053 rule 13).
- `ith.entity.set_enabled` (the Outliner's and Details' Enabled toggle) is **refused** while
  playing with `editor.command.playing`: it adds or removes the `Disabled` tag, a structural change,
  and P2 edits values only.
- They change the game world at once and are **never recorded**: no undo entry, no dirty state,
  nothing in the editor history (ADR-0037 rule 10). A Details drag still previews live, and Esc
  during it puts the value back, as when editing (ADR-0037 rule 6); its commit records nothing.
- They work paused and running. A running game system may overwrite an edited value in the next
  tick; that is the game's behaviour, not an editor error.
- Every other command of kind `Edit` (create, rename, attach, detach, move, add and remove
  component, set enabled, delete, duplicate, Snap to Floor) is refused while playing (3.10).
- Changes go through the same `applyPatches` validation as editing, so an invalid value is refused
  with the same error and changes nothing.

### 3.10 What is refused while playing

| Refused | Error | Why |
|---|---|---|
| Undo, Redo | `editor.command.playing` ("nothing is undoable while playing") | ADR-0037 rule 10 |
| Every `Edit` command other than the four value commands of 3.9 (create, rename, attach, detach, move, add and remove component, set enabled, delete, delete with children, duplicate, Snap to Floor, Add Shape) | `editor.command.playing` | The editor world is frozen; structural edits of the game world are out of scope for P2 |
| New Level, Open Level, opening a level from the Content Browser | `editor.command.playing` | The level can't be replaced under a session (as today) |
| **Save, Save As** | `editor.command.playing` | Owner's decision (section 6, decision 4): nothing a user did while playing is in the editor world except what Keep Simulation Changes put there, so a save during play is almost always a mistake; Stop, then Save |
| Play, Simulate | `editor.play.running` | One session at a time |

Allowed while playing: queries; selection commands (on the play selection); Keep Simulation
Changes; the play commands; viewport tools, game view and Focus; layouts and windows; the content
drawer; the Content Browser's file operations (per ADR-0049; section 6, C3).

### 3.11 Stopping

Stop runs these steps in order; none can fail:

1. An open interaction on the game world (a Details drag) is cancelled.
2. Capture ends and capture on click is turned off for the window; relative mouse mode turns off
   (the UI backend's rule, ADR-0045 item 5); the play viewport's waiting pick is dropped.
3. **`WorldEnding`** with the reason "Play In Editor stop" is sent to the game world, and the
   systems that read it run once (ADR-0016 rule 15), once the engine has events (decision 5; the
   point is marked first in Stop, ADR-0053 rule 11).
4. The play viewport's camera saved at start is restored bit for bit (3.6).
5. The game world, its schedule state, `Time`, the session's app-scoped resources, the play
   selection and the spectator pose are destroyed.
6. The document's play flag is cleared; the view models see the editor world's id again (3.8), so
   every panel rebuilds from the editor world.

**The editor is exactly as before Play**, because nothing but Keep Simulation Changes changed it,
and the one thing the session did change, the play viewport's camera, is restored:

- the editor world (`ith.query.world_hash` equal), its history (same entries and cursor) and its
  dirty state, **unless Keep Simulation Changes ran**, which added its entries (3.12);
- the editor selection, the one from before Play (the play selection is not carried back;
  decision 3);
- the play viewport's camera, restored bit for bit, and every other viewport's, untouched, in Play
  and in Simulate alike (decision 13);
- the panels' own state (Outliner expansion and scroll, Details folding, the Content Browser).

### 3.12 Keep Simulation Changes

`ith.play.keep_simulation_changes {ids?}` (ids by stable id; default: the play selection). It is
available while playing, possessed or not, paused or not, from the Outliner's context menu, the
Edit menu and the **K** key (Editor context, decision 7). K runs it only while the mouse is free:
while captured the keymap yields K to the game (3.7 rule 2), as it does every chord but the Play In
Editor keys.

For each id:

1. The entity is found in the game world and, by the same stable id, in the editor world. An id
   not in the game world is `skipped` (`editor.play.not_found`); one not in the editor world is
   `skipped` with "not in the level" (ADR-0017 rule 13 says spawned entities are reported and
   skipped; spawned entities can't be selected anyway, 3.8).
2. **What is compared:** each component type the entity has **in both worlds** that is neither
   Transient nor EditorOnly, except `StableId` (immutable identity) and `ChildOf` (the hierarchy is
   structure). The game value is copied, its entity references mapped to the editor world (game
   entity, its stable id, the editor entity with that id; `remapEntities`, ADR-0032 rule 4), and
   compared with the editor value through `TypeInfo::equals`. A reference to an entity without a
   stable id, or with no editor twin, becomes the null entity and adds a warning.
3. **What is kept:** every compared component whose value differs is set in the editor world to the
   mapped game value, as a `set` of the whole value (the empty path), or, for an immutable type
   (ADR-0030), an Add that replaces it. The values may differ because the user edited them or because a game system
   changed them (an object that fell and came to rest keeps its resting place).
4. **What is reported, not kept** (`ignored`, per entity and component, with the reason): components
   only in the game world (added during play), components only in the editor world (removed during
   play), a changed parent, and a changed enabled state (the `Disabled` tag is a component added or
   removed). These are structural, and ADR-0017 rule 13 keeps values only.

All the changes of one call are **one transaction** in the editor history, named "Keep Simulation
Changes", recorded at once even though a session runs (ADR-0037 rule 10's one exception). It makes
the level dirty, carries the editor selection as both its before and after selection (the editor
selection doesn't change), and is undone after Stop like any entry. The kept values show at once
in the viewports that draw the editor world (every viewport but the play viewport); the game world
doesn't change. Calling it twice with no new differences records
nothing the second time. A call that finds nothing to keep records no entry and returns empty
`kept`.

Result: `{kept: [{id, components: [<type name>]}], skipped: [{id, reason}], ignored: [{id,
component, reason}], warnings: [...]}`.

### 3.13 Showing the play state

- **Play viewport:** a 2-pixel border (at display scale 1) around its image in the theme token
  `playActive` while running possessed, `playSimulate` while running ejected and `playPaused` while
  paused, and a label in its top-left corner: "Playing", "Simulating" or "Paused". The hint of 3.7
  rule 1 shows below it while possessed and not captured. In immersive or game view the border
  stays: it is how a user tells the game world from the editor world.
- **Toolbar:** the controls of 3.2; the state is readable from which controls show.
- **Status bar:** a state chip with the same word and colour as the label, and the game's
  `elapsed` time (`m:ss.t`), next to the level's name. Clicking it does nothing.
- **Outliner and Details:** the note of 3.8.
- No tint over the whole editor in P2 (decision 11).

### 3.14 Commands and queries

Every action is a registered command (ADR-0037 rules 1 and 20), so menus, the toolbar, the keymap
and automation share one path. All play commands are of kind **Action**: they record no undo entry
(Keep Simulation Changes records its own), are refused while a level opens
(`editor.document.busy`), and Play and Simulate are refused while an interaction is open
(`editor.command.interaction_open`).

| Id | Chord (context) | Args | Result |
|---|---|---|---|
| `ith.play.play` | Alt + P (Editor) | `{paused?: false}` | `{state, possessed, paused, viewport}` |
| `ith.play.simulate` | Alt + S (Editor) | `{paused?: false}` | as `play` |
| `ith.play.stop` | Escape (Play In Editor) | `{}` | `{state: "stopped"}` |
| `ith.play.pause` | none (decision 12) | `{paused?}`: without it, toggles | `{paused}` |
| `ith.play.step` | none | `{frames?: 1, deltaSeconds?: 1/60}` | `{frame, elapsed}` |
| `ith.play.possess_or_eject` | F8 (Play In Editor) | `{possess?}`: without it, toggles | `{possessed}` |
| `ith.play.free_mouse` | Shift + F1 (Play In Editor) | `{}` | `{captured: false}` |
| `ith.play.keep_simulation_changes` | K (Editor; decision 7) | `{ids?}` | 3.12 |
| `ith.query.play` | — | `{}` | `{state, possessed, paused, captured, viewport, frame, elapsed, entities, spawned}` (`spawned`: entities without a stable id) |

- `paused: true` on Play or Simulate starts the session paused, so a script can step from the
  first tick. Headless sessions never tick by themselves: there is no frame loop, so automation
  pauses (or starts paused) and steps.
- **The active world in existing commands and queries.** While a session runs,
  `ith.selection.set`, `ith.edit.select_all`, `ith.edit.select_none`, `ith.query.selection`,
  `ith.query.entity`, `ith.query.find` and `ith.query.world_hash`, and the four value commands of
  3.9, act on the game world and the play selection through `EditorSession::active()`; the queries
  take an optional `world: "editor" | "game"` to read the other one.
  `ith.query.document` and `ith.query.history` always describe the editor document;
  `ith.query.document`'s `playing` is already there. `ith.query.find` matches labels through the
  editor twins (3.8).
- **Error codes:** `editor.play.running`, `editor.play.not_running`, `editor.play.not_paused`,
  `editor.play.not_found`, `editor.play.copy_failed`, plus `editor.command.playing`,
  `editor.command.invalid_args` (`frames` out of 1 to 1000, `deltaSeconds` out of (0, 0.1]),
  `editor.command.interaction_open`, `editor.document.busy` and the schedule's `ecs.schedule.*`.
- **Keymap:** five chords are already in `engine/config/Keymap.json`; this step adds
  `"ith.play.keep_simulation_changes": ["Keyboard.K"]` (decision 7, amending ADR-0037 rule 4's list
  of commands registered without a chord). `ith.play.stop`, `ith.play.free_mouse` and
  `ith.play.possess_or_eject` are the Play In Editor context (`kPlayInEditorCommands`), active only
  while a session runs; Play, Simulate and Keep Simulation Changes are Editor context, and K with no
    session runs nothing that changes state (the command fails with `editor.play.not_running`). The
  header comment of `Keymap.json` drops "the Play In Editor commands are registered by the editor
  work that brings them" (ADR-0053 rule 6).
- An automation script `tests/editor_core/scripts/play_in_editor.json` runs every play command and
  query (ADR-0037 rule 20).

### 3.15 Failure behaviour

- A failed start changes nothing: no world, no flag, no capture (3.3 lists the checks).
- **Game code runs in the editor process.** A programmer error in a game system asserts as anywhere
  in the editor (ADR-0001); there is no isolation (ADR-0017's accepted consequence). Expected
  failures in game systems are the game's to report.
- Stop can't fail; it never asks a question and never depends on the game world being consistent.
- A Details edit of the game world that `applyPatches` refuses changes nothing and reports its error
  as when editing.
- Keep Simulation Changes is all or nothing per call: if the editor transaction fails, nothing is
  kept and the error is returned (a bug; ADR-0037 rule 6).
- The game world counts against ADR-0014 rule 2's limit of 255 worlds; a session uses one.

### 3.16 Multiplayer

None in P2: the game world is standalone (its network role, when ADR-0016 rule 14's resource
exists, is standalone). Networked Play In Editor with several client worlds and a server world in
one process is ADR-0017 rule 14, in P5; this spec's session, schedule and view rules are written so
that each of those worlds is one more game world with its own window.

## 4. Requirements

Each requirement is an acceptance test. Unless stated otherwise, tests are headless: an
`EditorSession` on a temporary project folder, commands and queries through `Automation::run`,
sessions started paused and advanced with `ith.play.step`.

### 4.1 States and toolbar

1. `ith.query.play` reports `stopped` before Play; after `ith.play.play` it reports `running`,
   `possessed: true`, `paused: false`, `viewport: "ith.viewport.1"`; after `ith.play.simulate` from
   stopped, `possessed: false`.
2. Play or Simulate during a session fails with `editor.play.running`; Stop, Pause, Step, Possess or
   Eject, Free the mouse and Keep Simulation Changes with no session fail with
   `editor.play.not_running`; Step while running unpaused fails with `editor.play.not_paused`.
3. Play while a level opens fails with `editor.document.busy`; Play while a Details drag holds a
   transaction fails with `editor.command.interaction_open`; neither starts anything.
4. A game-world system list with a cycle makes Play fail with `ecs.schedule.cycle`, logged, and
   `ith.query.play` stays `stopped`, the document's `playing` false.
5. In the panel test (ImGui null backend), the toolbar shows exactly the controls of 3.2's table in
   each of the four states, Step is disabled unless paused, and each control issues its command.
6. Quit with a session running stops it, then applies the unsaved-changes rule.

### 4.2 The game world

7. After Play on a level with 3 entities (one with a child, one disabled, all with `EditorLabel`,
   `LocalTransform` and `ShapeRenderer`), the game world has the same entities by stable id, the
   same hierarchy, order and enabled state, no `EditorLabel` anywhere, and another world id than
   the editor world.
8. A component field holding an `Entity` points at the game world's copy of the referenced entity.
9. An unsaved, untitled level plays: Play reads nothing from disk and writes nothing.
10. The editor world never runs a game-world system: a test system registered for game worlds that
    counts its runs reports 3 after `step {frames: 3}` and 0 runs on the editor world.
11. A game-world system's first run sees every component as added (`Added` filter), as after a level
    load.
12. Two sessions in a row each start with `Time.elapsed` 0, `frame` 0, and fresh app-scoped
    resources: a test resource changed in the first session has its default value in the second.

### 4.3 Time, pause and step

13. `step {frames: 2, deltaSeconds: 0.02}` gives `frame` 2, `elapsed` 0.04, `delta` 0.02; with
    `dilation` 0.5 written by a test system in one tick, the next tick has `delta` 0.01 and
    `unscaledDelta` 0.02 (`Time` is advanced before the systems run). Every system sees the tick's
    final `Time`, whatever its stage.
14. `deltaSeconds` 0 or 0.2, and `frames` 0 or 1001, fail with `editor.command.invalid_args`.
15. With a fake clock (`PlaySession::tick(nowNs)`): the first tick call after a start runs nothing
    (`frame` stays 0, a counting test system doesn't run) and records its time; a 2-second gap
    before the next call gives `unscaledDelta` 0.1; after 5 seconds paused and a Resume, the first
    call again only records its time, and the call after it has a delta measured from that first
    call, not from the pause (ADR-0053 rule 28).
16. While paused, `ith.play.pause` resumes; running, every tick call after the first advances
    `frame` by one; paused, tick calls advance nothing. A minimized window calls no tick.
17. While paused, a Details edit of `LocalTransform` shows in the game world's `WorldTransform`
    after the viewport's propagation, and `frame` doesn't change.

### 4.4 Cameras

18. Simulate leaves the play viewport's view where its camera was; flying during Simulate moves
    that viewport's own camera; after Stop the camera equals its value before Simulate bit for bit,
    and a level close after Stop writes the pre-Simulate pose to `Viewports.json`, never a session
    pose.
19. Play starts the spectator at the viewport camera's pose; injected captured mouse motion turns
    it and W moves it at the viewport's speed step, through a `Game`-layer consumer.
20. Eject leaves the viewport's camera at the spectator's pose and saves it; Possess resumes the
    spectator at the saved pose; a session started with Simulate possesses at the camera's current
    pose.
21. The other viewport instances keep their cameras and draw the editor world during a session.

### 4.5 Input

22. Through a real `Input` with the editor's consumers: while possessed and not captured, a press on
    the play viewport image captures the mouse, picks nothing and reaches no `Game` consumer; a
    press on another panel doesn't capture.
23. While captured, W, K, Delete, Ctrl + Z and Ctrl + S reach the `Game` layer and run no editor
    command (section 6, C4); a text field left active by the capturing click receives no key and
    ImGui sees none; Esc runs `ith.play.stop`, even with that text field active; Shift + F1 runs
    `ith.play.free_mouse`; F8 runs `ith.play.possess_or_eject`.
24. After Shift + F1 the session still runs, keys run editor commands again (W selects the translate
    tool), and the next press on the play viewport captures again.
24a. The keymap gate (ADR-0053 rule 22): while a session runs and the mouse is free, a key bound to
    nothing (J) and an unbound mouse event reach no `Ui` or `Game` consumer; while possessed, a
    mouse press on the play viewport still reaches capture on click; a gamepad button reaches the
    `Game` layer while possessed and doesn't while ejected.
25. Escape order with the mouse free (section 6, C2): Esc in an active text field ends the editing,
    Esc with a popup or dialog open closes it and is consumed by the ImGui consumer (it reaches no
    consumer below), Esc during a Content Browser inline rename cancels it, and Esc during a Details
    drag on the game world cancels the drag; in each case the session keeps running. The next Esc,
    with nothing left taking it, stops the session. The decision uses the UI's report from its last
    frame. Shift + F1 and F8 act even while a text field is active.
26. Focus loss while captured ends capture, releases held keys, and keeps the session running.
27. In Simulate, LMB, RMB fly and the Alt gestures on the play viewport work as when editing, and no
    input reaches the `Game` layer.

### 4.6 Panels and editing the game world

28. During a session, `ith.query.find` and the Outliner view model list game-world entities with
    the labels of their editor twins; an entity spawned by a test system appears as "Spawned <n>",
    is not selectable, and `ith.query.play`'s `spawned` counts it.
29. The Outliner model rebuilds when a game system spawns an entity (`World::structureGeneration()`
    moved) and not on a tick that only changes values.
29a. Every view model keys its cache on the active world's id: at Play and at Stop the Outliner and
    Details rebuild even when the game world's change counters equal the editor world's.
30. `ith.entity.set_property` on `LocalTransform` during play changes the game world and leaves the
    editor world's hash, the history (entries, cursor) and the dirty state unchanged.
31. Every Edit command other than the four value commands of 3.9, `ith.entity.set_enabled`
    included (the Outliner's and Details' Enabled toggle), and Undo, Redo, New Level, Open Level,
    `ith.content.open` on a level, Save and Save As, fail with `editor.command.playing` during play
    and change neither world; no file is written by Save or Save As (decision 4, ADR-0053 rules 13
    and 14).
31a. During play, the content file operations (new folder, rename, move, duplicate, delete, import,
    reimport, fix) succeed as when editing and are not refused with `editor.command.playing`
    (ADR-0049; section 6, C3).
32. A click on a game-world shape in the play viewport (ejected) selects its stable id in the play
    selection; a click in another viewport on an editor-world shape also changes the play
    selection, after a check against the editor world; the editor selection is unchanged
    (`ith.query.selection {world: "editor"}`), and every viewport tints the play selection.
33. No gizmo is drawn while playing, possessed or not (panel test with `drawnGizmo`).

### 4.7 Stop

34. Play, step 10 ticks with a system that moves every entity, edit values in Details, change the
    play selection, Eject and fly, then Stop: the editor world's hash, history, dirty state,
    selection and every viewport's camera are equal to their values before Play.
35. After Stop, no world with the game world's id exists, relative mouse mode is off, capture and
    capture on click are off, and the next Play gets a fresh world.
36. Enabled when ECS events land (decision 5; not part of this step's acceptance): a test system that reads `WorldEnding` runs once at
    Stop with the reason "Play In Editor stop", and one that reads `WorldStarted` runs once before
    the first tick.

### 4.8 Keep Simulation Changes

37. Simulate, move a selected entity's `LocalTransform` in the game world and change a
    `ShapeRenderer` colour, Keep Simulation Changes: `kept` names both components; at once, before
    Stop, the editor world has both values and a viewport other than the play viewport re-renders
    with them (its world key moved), while the game world is unchanged; after Stop the editor world
    still has both values, the history has one more entry "Keep Simulation Changes", the
    level is dirty, and one Undo restores both values.
38. An id that only the game world has (a test system spawns an entity and gives it a new
    `StableId`) is `skipped` with "not in the level"; the id of a level entity that a system
    despawned from the game world is `skipped` with `editor.play.not_found`; neither changes the
    editor world.
39. A component added during play, one removed during play, a reparent and a `Disabled` change are
    listed in `ignored` and not applied.
40. An `Entity` field referring to another level entity is kept as a reference to that entity's
    editor twin; one referring to a spawned entity is kept as null with a warning.
41. A second call with no new differences records no entry; a call on an unchanged entity returns
    an empty `kept` and records no entry.
42. Keep Simulation Changes without `ids` uses the play selection.
42a. With the mouse free during a session, K runs `ith.play.keep_simulation_changes` on the play
    selection; with the mouse captured, K reaches the `Game` layer and keeps nothing; with no
    session, K fails with `editor.play.not_running` and changes nothing (decision 7).

### 4.9 Indicators

43. In the panel test, the play viewport's border uses `playActive`, `playSimulate` and `playPaused`
    in the matching states, its label reads "Playing", "Simulating" or "Paused", the capture hint
    shows only while possessed and not captured, and the status bar shows the state and elapsed
    time; nothing is drawn after Stop.

### 4.10 Automation

44. Every `ith.play.*` command and `ith.query.play` appears in
    `tests/editor_core/scripts/play_in_editor.json`, which runs headless in CTest and in the
    editor's `--automation` run (ADR-0037 rule 20).
45. The test that `engine/config/Keymap.json` names only registered commands passes with the six
    play bindings (the five existing ones and K for `ith.play.keep_simulation_changes`), no
    `editor.keymap.unknown_command` warning remains for them, and K conflicts with no other Editor
    context chord.

### 4.10a The `gameplay` module (decision 2)

45a. `ith::Time` is a reflected world resource in the new `gameplay` module (framework layer,
    runtime kind, depending on `foundation`, `reflection` and `ecs`); the `module_layering` test
    accepts it, `editor_core` depends on it, and the sample player and server targets may link it
    (ADR-0017 rule 2).
45b. `tickGameWorld(world, schedule, seconds)` on a plain `World` (no editor) gives the `Time` values
    of requirement 13 for given deltas, inserts a default `Time` into a world without one, clamps
    0.5 to `kMaxTickDelta` and a negative value to 0, and runs the schedule after `Time` is
    advanced; `buildGameSchedule` adds transform propagation, then the given systems, and returns
    the schedule's error for a cycle (ADR-0053 rules 1 and 28).
45c. `World::structureGeneration()` moves on spawn, despawn, adding a new component type, remove,
    a `ChildOf` write, `placeBefore`, `clear`, applied commands, `applyPatches` and `copyWorld`, and
    doesn't move on a value write (ADR-0053 rule 2).

### 4.11 Performance (Low tier, ADR-0005)

46. Starting Play on a level of 10,000 entities (each with `StableId`, `EditorLabel`,
    `LocalTransform`, `WorldTransform` and `ShapeRenderer`, a quarter of them children) takes at
    most 100 ms to the end of the copy and schedule build, and Stop at most 50 ms, on the Low-tier
    reference machine, measured by a benchmark.
47. The session's own work per tick (time, schedule overhead, view model polling), excluding the
    game systems and rendering, is at most 0.25 ms with that level, and allocates nothing in steady
    state.
48. With 10,000 entities and a running session that only changes values, the Outliner does no
    rebuild per tick (requirement 29), and Details reads only the shown entities.
49. A level of 1,000 shapes plays in the play viewport at 1280 x 720 at 60 frames per second or
    more on the Low-tier reference machine (report-only; CI has no GPU timing).

## 5. Fit with Ironthorn

### Layer and modules (ADR-0004, ADR-0017)

- **`editor_core`** (layer `editor`, kind `editor`): a `PlaySession` owned by `EditorSession`:
  - the game world, its `Schedule`, the play selection, the game world's change counters and a
    `Transactions` over the game world whose private history is never undone and is dropped with
    the session (this gives Details drags their live preview and Esc cancel with no new mechanism);
  - the state machine of 3.1, `PlaySession::tick(nowNs)` with the first-call rule of 3.5 (it calls
    `tickGameWorld`), the saved camera and the spectator pose, the selected-viewport id in the
    `Viewports` store; `PlaySession` is owned by `EditorSession` through `std::unique_ptr` while a
    session exists;
  - `EditorSession::active()` (the active world's `World`, `Transactions`, `Selection`,
    `EditorChanges` and entity mapping), which commands and view models that act on "the world"
    call, while file, history and Keep Simulation Changes keep using `document()`; view models key
    their caches on the active world's id; `EditorChanges::bumpAllValues()`;
  - Keep Simulation Changes, built on `remapEntities`, `TypeInfo::equals` and one editor
    transaction;
  - the `ith.play.*` commands and `ith.query.play`;
  - a `playEditable` flag on `CommandDesc` for the four value commands of 3.9 (not `set_enabled`):
    the registry's "Edit refused while playing" check (today in `CommandRegistry.cpp`) refuses only
    Edit commands without it, and runs those with it in a scope of the active `Transactions`;
    `DetailsModel` drops its own playing refusal for them;
  - the Play In Editor consumer (on top of the stack) with the Escape rule of 3.7 rule 6 (C2), fed
    by the UI's per-frame report of an active text field or open popup; the keymap consumer's
    capture rule (3.7 rule 2, C4; it reads `Input::mouseCaptured` through an `Input` it is given)
    and its gate while the mouse is free (3.7 rule 3); the spectator's `Game`-layer consumer;
  - Save and Save As refusing with `editor.command.playing` (decision 4) and the K binding of Keep
    Simulation Changes (decision 7);
  - `EditorSessionOptions::gameSystems` (`std::vector<SystemDescriptor>`), filled by the editor
    application from the game modules' registration functions (ADR-0017 rule 5); empty in P2.

  It stays free of ImGui and the GPU, so everything above tests headless.
- **`editor_ui`**: the main toolbar strip in `drawEditorShell`, the play viewport's border, label
  and hint, the status bar chip, the Outliner and Details notes, the Outliner's Keep Simulation
  Changes entry, the theme tokens `playActive`, `playSimulate`, `playPaused`, and the per-frame
  Escape report (whether ImGui has an active text field or an open popup). The Viewport panel
  draws whichever world its instance shows.
- **The editor application** (`editor/Main.cpp`): calls `PlaySession::tick` in the frame order of
  3.5 (none while minimized), turns capture on click on for the window while possessed (ADR-0027
  rule 3), requests a frame every frame while a session ticks, stops the session on Quit, orders the
  consumers of 3.7, and registers the `gameplay` module's reflection.
- **`ecs`** gains `World::structureGeneration()` (decision 15, ADR-0053 rule 2), a 64-bit counter
  that moves on every spawn, despawn, add of a new component type, remove, `ChildOf` write,
  `placeBefore`, `clear` and applied command, and through `applyPatches` and `copyWorld`; value
  writes never move it. The Outliner follows the game world with it, without rebuilding every tick
  and without the ECS observers ADR-0037 rule 13 rejected.
- **`gameplay`** (new; layer `framework`, kind `runtime`, depending on `foundation`, `reflection`
  and `ecs`; decision 2, ADR-0053 rule 1): the reflected `ith::Time` world resource,
  `kMaxTickDelta`, `buildGameSchedule(schedule, systems)` and `tickGameWorld(world, schedule,
  seconds)`, which advances `Time` and then runs the schedule (3.5), with their tests. Game modules,
  the player and the dedicated server link it and call the same tick; `editor_core` depends on it,
  which its layer allows. Later it also holds the lifecycle events and the game-world system list.
  It is created with real code and tests (ADR-0004), and gets its row in `docs/architecture.md`
  when it is built (ADR-0053 rule 30).

### Mapping onto the ECS

| Concept | ECS form |
|---|---|
| Game world | A second `World`, filled by `copyWorld(..., {.excludeEditorOnly = true})` |
| Matching entities across worlds | `StableId` and `World::findByStableId`, never handles |
| Game-world systems | `SystemDescriptor`s in the session's own `Schedule` (`buildGameSchedule`), run once per tick |
| Game time | The `Time` world resource of the `gameplay` module, advanced by `tickGameWorld` before the schedule runs |
| Following structure | `World::structureGeneration()`, read after each tick to bump the game world's `structure` counter |
| Play selection | A `Selection` of stable ids, like the editor's |
| Details edits of the game world | `World::applyPatches` through a game-world `Transactions` whose history is discarded |
| Keep Simulation Changes | Per component: `remapEntities` game to editor through stable ids, `TypeInfo::equals`, one whole-value `set` (or a replacing Add for an immutable type) per differing component, in one editor transaction |
| Possess and Eject | P2: whether the spectator or the viewport's own camera feeds the play viewport. Later: the `Controls` link (ADR-0016 rule 9) |
| Lifecycle | `WorldStarted` and `WorldEnding` events (ADR-0016 rule 15), once events exist |

### Why this design is the simple one

- The game world is a copy, so Stop restores by throwing it away; there is no snapshot, no diff and
  no restore code (ADR-0017's rejected snapshot option), except for the play viewport's camera,
  saved and restored so the viewport paths need no session branch.
- The editor world is frozen by the flag that already exists (`EditorDocument::playing`), so the
  editor's history and selection need no special cases.
- Panels read "the active world" and keep their view models; the game world gets the same
  `Selection`, change counters and `Transactions` types, not parallel ones.
- Every action is a command, so the toolbar, keymap and automation can't drift apart, and
  headless tests drive the game world deterministically with Step.
- The spectator view reuses the editor camera controller; it is a placeholder with a clear
  replacement (the player's `ViewTarget`).
- Keep Simulation Changes copies whole component values of existing entities, the rule ADR-0017
  rule 13 states, with one walker that already exists.

### Patents

No patent claims on in-editor play, world duplication or copy-back of values are known to the
author. Nothing here relies on a specific vendor technique.

## 6. Decisions (ADR-0053)

The owner answered every question and conflict on 2026-11-28, and
[ADR-0053](../decisions/0053-play-in-editor-and-simulate.md) records them with the ADR amendments
they need (C2, C4 and decision 7 among them). Each item keeps the question and records the answer;
the sections above follow them. Where ADR-0053 settled a point the spec had left open or worded
differently, the spec now follows the ADR:

- `Time` is advanced by `gameplay`'s `tickGameWorld` before the schedule runs, not by a system
  (3.5; ADR-0053 rule 1);
- the first tick call after a start or a resume only records its time and runs nothing (3.5;
  rule 8);
- `ith.entity.set_enabled` is refused while playing (3.9, requirement 31; rule 13);
- kept values show at once in the viewports that draw the editor world (3.12; rule 27);
- while the mouse is captured, ImGui receives no keys, so Escape always stops play (3.7; rules 19
  and 21);
- the play camera is the play viewport's own camera, saved at start and restored at Stop (3.6;
  rules 7, 10 and 17);
- added from the ADR: the keymap gate while the mouse is free (3.7 rule 3; rule 22), view models
  keyed on the world id (3.8; rule 12), Escape consumed by the ImGui consumer while a popup is open
  (3.7 rule 6; rule 19), picks validated against the drawn world and applied to the active
  selection (3.8; rule 16), and `World::structureGeneration()` with `bumpAllValues` (3.5; rules 2
  and 9).

### Conflicts with accepted ADRs or with the task

- **C1. A new free-the-mouse chord.** The task asked for "our own binding". ADR-0008 rule 4,
  ADR-0013, ADR-0017 rule 12, ADR-0029 rule 7 and `Keymap.json` already fix Shift + F1.
  **Decided:** keep Shift + F1 (3.7). No ADR changes.
- **C2. Escape above the text field.** ADR-0029 rule 7 puts the Play In Editor keys above the
  ImGui consumer, and ADR-0037 rule 4 says Escape reaches `ith.play.stop` first, so Esc in a
  Details text field or during a drag would stop the session and lose the edit. **Decided:** an
  active text field, an open popup or dialog, an inline rename, and an open drag or interaction take
  Escape first while the mouse is free; only an Escape none of them uses stops play, and while the
  mouse is captured Escape always stops (3.7 rule 6). ADR-0053 rule 19 amends ADR-0029 rule 7 and
  ADR-0037 rule 4 **for Escape only**; Shift + F1 and F8 stay above everything.
- **C3. Content file operations while playing.** The task listed file operations among the commands
  refused while playing; ADR-0049 (content spec 3.19) allows them except opening a level, and
  ADR-0017 rule 13 says asset edits during play persist. **Decided:** allowed during play, as
  ADR-0049 says (3.8, 3.10). No ADR changes.
- **C4. Editor shortcuts and the game.** ADR-0013 rule 10 lists "editor shortcuts" in the top layer
  above the game. **Decided:** while the mouse is captured, the keymap yields every chord to the
  game except the three Play In Editor keys (Esc, Shift + F1, F8), and ImGui gets no keys (3.7
  rule 2). ADR-0053 rule 21 records it as a clarification of ADR-0013 rule 10; rule 22 adds the
  keymap gate while the mouse is free (3.7 rule 3).
- **C5. Lifecycle events.** ADR-0017 rule 10 says Stop sends `WorldEnding`; the ECS has no events
  yet (ADR-0016 rules 3 to 5 are not built). **Decided:** as decision 5.
- **C6. Code facts that change.** Not ADR conflicts, but the implementation changes them: the
  registry refuses every Edit command while playing (`CommandRegistry.cpp`), and so does
  `DetailsModel`; 3.9 needs the `playEditable` exception for four commands. Save and Save As are allowed while playing
  today (`FileCommands.cpp`); decision 4 refuses them.

### Decisions

1. **New editor window in P2?** **Decided:** out of P2. The selected viewport covers the P2 exit
   criterion, and the session design allows one window per game world for P5's networked play.
2. **Where `Time` lives.** **Decided:** in a new framework-layer, runtime-kind module **`gameplay`**,
   created with `Time`, `kMaxTickDelta`, `buildGameSchedule` and `tickGameWorld` (which advances
   `Time`, then runs the schedule) and their tests (real code from day one, ADR-0004). It is also
   where the lifecycle events and the game-world system list go later.
3. **The selection after Stop.** **Decided:** the editor selection from before Play is restored
   (3.11); the play selection is not carried back.
4. **Save during play.** **Decided:** Save and Save As are refused while playing with
   `editor.command.playing` (3.10). Keep Simulation Changes, Stop, then Save is the path to keep
   values.
5. **`WorldStarted` and `WorldEnding` without an event system.** **Decided:** no ECS events in this
   step. The session marks the two points (3.3 step 7, 3.11 step 3); requirement 36 is enabled when
   events land.
6. **Running `InputActions` in game worlds.** **Decided:** not in P2; it plugs into the game-world
   system list when the first game uses it.
7. **A chord for Keep Simulation Changes.** **Decided:** K, in the Editor context (3.12, 3.14). It
   fires only when the mouse is free (C4). This amends ADR-0037 rule 4, which registered the command
   without a chord, and `engine/config/Keymap.json` gains the binding.
8. **Structural edits of the game world.** **Decided:** values only in P2 (3.9); ADR-0053 rule 13
   counts the Enabled toggle (`ith.entity.set_enabled`) as structural, so it is refused too.
9. **Selecting entities spawned during play.** **Decided:** not in P2. A session-local id for
   spawned entities of game worlds is the later route.
10. **Losing window focus.** **Decided:** the game keeps ticking (3.5); a pause-on-focus-loss
    preference can come later.
11. **A tint over the whole editor while playing.** **Decided:** no; the viewport border and label,
    the toolbar and the status bar show the state (3.13).
12. **A chord for Pause.** **Decided:** none in P2; toolbar and command only.
13. **The camera after Stop in Simulate.** **Decided:** the play viewport's camera, which the session
    uses as it is, is saved at start and restored at Stop (3.6, 3.11; ADR-0053 rule 10).
14. **The view in Play without a gameplay framework.** **Decided:** the spectator view of 3.6.
15. **The world structure generation in `ecs`.** **Decided:** added (5), so the Outliner follows the
    game world without rebuilding every tick (requirements 29 and 48).

## 7. Sources

Public documentation and observed editor behaviour only. Epic Games' editor is named here only to
identify the inspiration.

**Unreal Engine (Epic Games):**

- [Playing and Simulating](https://dev.epicgames.com/documentation/en-us/unreal-engine/playing-and-simulating-in-unreal-engine):
  the toolbar's Play and Simulate, the controls that replace them while running (Pause, frame
  step while paused, Stop, Eject, Possess), the play modes (selected viewport by default, new
  window, standalone), clicking into the viewport for mouse control, Shift + F1, Esc to stop, edits
  during play discarded unless kept.
- [In-Editor Testing: Play and Simulate](https://dev.epicgames.com/documentation/unreal-engine/ineditor-testing-play-and-simulate-in-unreal-engine):
  Play with a player and Simulate without one, switching by Possess and Eject, where possessing
  resumes, that the session runs on a duplicate of the level, and that only the viewport mode
  switches.
- [Playing and Simulating, 4.27](https://docs.unrealengine.com/4.27/Basics/PIE) and
  [In-Editor Testing, 4.26](https://docs.unrealengine.com/4.26/BuildingWorlds/LevelEditor/InEditorTesting):
  the same features in earlier versions.
- [Keep Simulation Changes, 4.27](https://docs.unrealengine.com/4.27/BuildingWorlds/LevelEditor/InEditorTesting/KeepSimulationChanges)
  (index entry; the page itself refused automated access) and the public issue reports
  [UE-237560](https://issues.unrealengine.com/issue/UE-237560) and
  [UE-5265](https://issues.unrealengine.com/issue/UE-5265): Keep Simulation Changes acts on the
  selected actors, from the Outliner's context menu or K, and copying only some changes back is a
  known source of user confusion, one reason this spec copies whole component values.

**Unity documentation:**

- [Colors preferences](https://docs.unity3d.com/Manual/preferences-colors.html): a tint over the
  whole editor in Play mode, to make the mode obvious (decision 11).
- [Configurable Enter Play Mode](https://docs.unity3d.com/2020.1/Documentation/Manual/ConfigurableEnterPlayMode.html):
  changes made in the editor during Play mode are reset when it ends; the cost of entering Play
  mode as an iteration concern (requirement 46).

**Godot Engine documentation:**

- [Game embedding](https://docs.godotengine.org/en/stable/tutorials/editor/game_embedding.html):
  the running game shown inside the editor, selecting its nodes with game input disabled, and
  changes made there not kept after the game stops.
- [Overview of debugging tools, 3.5](https://docs.godotengine.org/en/3.5/tutorials/scripting/debug/overview_of_debugging_tools.html):
  the Remote scene tree that inspects and edits the running game's nodes, separate from the Local
  tree (the Outliner showing the game world, 3.8).

**Ironthorn:** ADR-0001, ADR-0003, ADR-0004, ADR-0005, ADR-0008, ADR-0013, ADR-0014, ADR-0016,
ADR-0017, ADR-0027, ADR-0029, ADR-0032, ADR-0037, ADR-0042, ADR-0045, ADR-0046, ADR-0047, ADR-0048,
ADR-0049 and ADR-0053 in [docs/decisions](../decisions/README.md); the
[Content Browser spec](content-browser.md); the [editor core](../development/editor-core.md) and
[editor UI](../development/editor-ui.md) guides.
