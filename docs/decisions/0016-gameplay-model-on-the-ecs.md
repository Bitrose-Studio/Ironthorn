# ADR-0016: Gameplay model on the ECS

- **Status:** Accepted
- **Amended by:** [ADR-0053](0053-play-in-editor-and-simulate.md)
- **Date:** 2026-11-20
- **Related:** [ADR-0004](0004-modules-and-layering.md) (layers),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (Actor is an editor word),
  [ADR-0010](0010-serialization.md) (config and data files), [ADR-0013](0013-input.md) (players
  and devices), [ADR-0014](0014-ecs-storage-and-world.md) (ECS, hooks, stages, resources, change
  ticks), [ADR-0015](0015-asset-identity-and-reimport.md) (prefabs)

## Context

ADR-0014 fixed the ECS and left events and observers to this ADR. Gameplay code, the abilities
plugin, visual scripting, replication and the editor all need one answer to "how is a game
written on this engine?" Requirements:

- **One runtime data model.** Everything a game simulates lives in the ECS, so the scheduler,
  replication (change ticks), saving (reflection) and the editor see all of it.
- **Familiar to Unreal developers in the editor** (ADR-0008), without importing Unreal's runtime
  class model: the owner's direction is that gameplay is designed ECS-natively, and Unreal
  concepts appear only where they are genuinely useful, expressed as data.
- **Scale and parallelism:** thousands of agents and projectiles, systems run by declared access.
- **Networking** with server authority and client prediction; dedicated servers without the
  renderer.
- Gameplay in C++ first, a visual graph on a bytecode VM for designers, C# possibly later.

Lessons from earlier engine prototypes by the same maintainer:

- A design that mapped Unreal's framework onto an ECS, with actor "behaviours" that had
  begin-play, tick and end-play callbacks, was replaced before it shipped: per-object callbacks
  hid their data access from the scheduler, duplicated what systems do, and needed a second
  execution model next to systems. Its successor made gameplay **capabilities** (components),
  **relations** and **systems**, with no runtime Actor type, and gated growth on a vertical-slice
  prototype.
- A hand-bound scripting language with about a hundred bindings, mandatory in every runtime and
  unwinding through C++ exceptions, cost more than it gave.

## Options considered

**A. An Unreal-style class framework on top of the ECS** (Actor, Pawn, Character, Controller,
GameMode as classes with virtual begin-play and tick).
- Two object models: classes hold state the ECS can't see, so replication, saving, the editor
  and the scheduler each need a bridge.
- A virtual tick per object defeats batched iteration and parallel execution; access isn't
  declared, so nothing can run in parallel safely.
- Inheritance fights composition: a flying, swimming, possessable thing doesn't fit one branch
  of a class tree, and the tree's choices leak into every game.
- It brings the costs of the original (large base classes, implicit lifecycles) without its
  tooling.

**B. Plain object-oriented actors without an ECS.** Ruled out by ADR-0014's scale and
parallelism requirements.

**C. Gameplay only in a scripting language.** Slower in hot loops, a large binding surface to
maintain, and a second debugging world; the visual graph serves designers without making it the
only way.

**D. ECS-native gameplay** (chosen): components are data, systems are logic, resources are
world-wide state, relations link entities, events carry what happened. Unreal concepts become
data where useful, and a mapping table helps Unreal developers find them.

## Decision

### The model

1. **The ECS is the only runtime gameplay model.** There is no runtime Actor class, no
   inheritance hierarchy of gameplay types and no per-entity begin-play or tick callback.
   - **Components are data** that say what an entity is or can do (`Health`, `Interactable`,
     `CharacterMovement`). "Is this a pawn?" is a component test.
   - **Systems hold the logic**, as ADR-0014 descriptors with declared access, in stages.
   - **Resources** hold world-wide state.
   - **Relations** link entities (control, ownership, attachment), kept consistent by hooks.
   - **"Actor"** is how the editor presents an entity with a transform (ADR-0008), nothing more.
2. **Gameplay code is C++ systems in game modules or plugins.** A module registers its reflected
   components, resources, events and systems in one registration function. Systems go into the
   engine's stages; a module groups its systems into **named sets** that can be ordered
   (`before`/`after`) as a whole. Modules don't add stages.

### Events and observers

3. **Events are typed values**, sent by systems (sending is declared access) or recorded by
   engine hooks into the world. They are kept for the frame they are sent in and the next
   frame, then dropped. Each reading system keeps its own cursor, so it sees every event exactly
   once, in send order. With the serial executor and ADR-0014's command order, event order is
   deterministic.
4. **Observers are systems with a run condition** "events of type T are pending"; they run in
   their stage like any system, with declared access. There are no immediate callbacks that run
   inside the call that sent the event, and gameplay never registers hooks: hooks stay engine
   invariants (ADR-0014).
5. **Entity-targeted events** (damage to an entity, an interaction) carry the target entity as a
   field. Engine events include `Spawned` and `Despawned` (sent once the commands are applied),
   control changes, and the lifecycle events of rule 15. Additions and changes of components
   are read with ADR-0014's `Added` and `Changed` filters; a component type may opt in to a
   `Removed` event.

### State, players and control

6. **Game-level state is resources** with one of two scopes: **app** (lives across worlds: save
   system, online services, settings) or **world** (lives and dies with a world: match rules,
   score). Per-player state is components on the player's entity.
7. **A player is an entity** with a `Player` component: local (with its device set from ADR-0013)
   or remote (with its connection). Local players are created by the game, as ADR-0013 assigns
   devices.
8. **Control is a relation.** A controller entity (a player or an AI) has an immutable
   `Controls(target)` link to at most one entity; the target gets the inverse `ControlledBy`.
   Other links (owned vehicles, summoned units) are separate relations.
   **Possessing** replaces the link through a command and sends a control-changed event. Input
   reaches the controlled entity through the player's input contexts (ADR-0013). The view a local
   player sees is a `ViewTarget` link from the player to an entity with a camera.
9. **Play In Editor** possess and eject (F8) are the same data operations: eject removes the
   player's `Controls` link and hands the view to the editor camera; possess restores it.
10. **Match rules** ("game mode") are a set of systems plus a rules resource registered by the
    game, running only where the world has authority (rule 13). Replicated game-wide state is
    a game-state entity with replicated components; replicated per-player state lives on the
    player entity.

### Spawning, time and tags

11. **Spawning uses prefabs** (ADR-0015): a command spawns a prefab at a transform under an
    optional parent and returns the reserved root handle at once. The prefab must be loaded; a
    soft reference is loaded first, asynchronously. In networked games the server spawns and
    replication creates the entities on clients.
12. **Time and timers.** A world `Time` resource gives the frame delta, unscaled time, pause and
    time dilation. Timers are components (duration, repeat) or resource entries, advanced by a
    timer system that sends a timer-finished event with the entity. Gameplay runs at the frame
    rate; fixed-step simulation belongs to physics and to networked movement, defined by their
    ADRs.
13. **Gameplay tags live in core.** Hierarchical names (`Status.Stunned`) are declared in config
    files, interned to compact process-local ids, and stored in a tag-container component.
    `hasTag(Status)` matches `Status.Stunned`; `hasTagExact` does not. Files store tag names,
    never ids.

### Authority and networking (contract)

14. **Authority is data.** The world has a network role resource (standalone, server, client,
    listen server). Replicated entities carry a role component (authoritative, locally
    predicted, simulated proxy). Systems use run conditions such as "server only", "has
    authority" or "locally controlled". A dedicated server simply doesn't link client-only
    modules (rendering, audio, UI). Replicated components and fields are marked in reflection;
    replication reads change ticks (ADR-0014); remote calls are networked events. Details belong
    to the networking ADR.

### Lifecycle

15. **World lifecycle is events:** `WorldStarted` after a level is loaded and before the first
    gameplay frame (also when Play In Editor starts its world), and `WorldEnding` with a reason
    (level change, Play In Editor stop, quit) before teardown. Level streaming sends cell-loaded
    and cell-unloading events, defined by the open-world ADR.

### Contracts for later ADRs

16. **Abilities, attributes and effects** are a framework plugin made of components, systems and
    events, using core tags and the replicated collections; it needs no new ECS feature.
17. **Visual scripting:** a graph asset is referenced by a component, its state is a component,
    and **executor systems** run graphs in a stage with access declared as at least the union of
    what their nodes can touch. Nodes call the same reflected functions and send the same events
    as C++. Nothing ticks itself. C#, if added, plugs into the same contracts.

### Determinism and hot reload

18. **Determinism:** on one build and platform, the same inputs in the same order give the same
    simulation: serial executor, deterministic command and event order, no iteration of
    unordered containers that affects results, and a seeded random-number resource per world.
    Bit-identical results across platforms and lockstep networking are not goals; networked
    games rely on server correction.
19. **Hot reload boundary** (editor and development builds only): gameplay modules may reload at
    a frame boundary; their systems are re-registered and the world stays. A component whose
    layout changed is carried over through reflection (saved and restored), or the reload is
    refused with a message. Engine modules never reload. Assets and graphs reload at any time.
    The reload mechanism has its own ADR in P5: reloading game modules as shared libraries, with
    a commercial live-patching tool evaluated as the alternative.

### If you know Unreal

20. Where an Unreal developer would look for something:

    | Unreal | Ironthorn |
    |---|---|
    | Actor | An entity with a transform, shown as an Actor in the editor |
    | Actor component, scene component | A component; attachment is a child entity |
    | Pawn, Character | Components: something controllable, plus movement, collider and mesh |
    | Controller, possession | A controller entity with a `Controls` relation |
    | PlayerController, local player | A player entity with a `Player` component and input contexts |
    | GameMode | Match-rule systems plus a rules resource, authority only |
    | GameState, PlayerState | A replicated game-state entity; components on the player entity |
    | GameInstance, subsystems | App-scoped and world-scoped resources |
    | Cast, interfaces | A component test or a query filter |
    | Blueprint class | A prefab, optionally with a graph component |
    | BeginPlay, EndPlay, delegates | Events read by systems |
    | Tick, tick groups | Systems in stages |
    | Timers | Timer components and a timer system |
    | Gameplay tags | Core gameplay tags |

    This table is documentation; none of these Unreal types exists in the engine.

### Gate

21. **A vertical slice comes before the gameplay plugins grow:** a controlled character with
    movement and health, a door driven by a state machine, a thousand AI-controlled agents,
    projectiles and damage events, and a level run in Play In Editor, kept in samples. Its
    benchmarks (event dispatch, structural changes, iteration) confirm or amend this ADR.

## Consequences

- One data model for simulation, editor, saving and replication; systems can run in parallel
  because all access is declared.
- Unreal developers find familiar concepts through the editor and the mapping table, but write
  systems rather than subclasses; documentation and samples must teach that.
- No per-entity callbacks means "this door has its own logic" is data (a state machine or graph)
  run by a system.
- Event lifetime of two frames means a system that doesn't run for two frames misses events;
  systems that must see every event run every frame.
- Follow-up work: events and run conditions in the ECS; players, control and view target;
  timers; gameplay tags; the vertical slice; the abilities, visual scripting, networking and hot
  reload ADRs.
- **Revisit when:**
  - the vertical slice shows event dispatch or the run-condition model too slow or awkward;
  - visual scripting needs finer access declarations than "union of nodes";
  - networking needs fixed-step gameplay beyond movement.
