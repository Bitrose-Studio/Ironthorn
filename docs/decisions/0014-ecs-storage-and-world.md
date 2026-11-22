# ADR-0014: ECS storage and the world

- **Status:** Accepted
- **Amended by:** [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md),
  [ADR-0031](0031-ecs-hierarchy-enabled-state-and-transform-propagation.md)
- **Date:** 2026-11-20
- **Related:** [ADR-0001](0001-cpp-language-and-safety-rules.md) (no exceptions, no RTTI,
  generational handles, jobs), [ADR-0004](0004-modules-and-layering.md) (`core` layer, no
  third-party types in public headers), [ADR-0008](0008-editor-familiar-to-unreal-developers.md)
  (Outliner, Actor, Play In Editor), [ADR-0010](0010-serialization.md) (reflection-driven
  serialization), [ADR-0013](0013-input.md) (input stage),
  [architecture: World model](../architecture.md#world-model)

## Context

Every gameplay system, the renderer's extraction, replication and the editor read and write the
world, so its storage and API are fixed before gameplay code exists. Requirements:

- **Scale:** 100k+ entities with cache-friendly iteration; low-end machines are CPU-bound.
- **Editor:** an ordered hierarchy of entities in the Outliner, where an Actor is the editor's
  view of an entity with a transform; reflection-driven Details; undo; a Play In Editor world
  next to the editor world.
- **Networking:** per-entity, per-component change detection, so replicated collections need no
  manual "mark dirty" call.
- **Determinism:** a defined order of systems and of structural changes.
- No exceptions, no RTTI, no third-party types in gameplay code; one reflection system.

Lessons from earlier engine prototypes by the same maintainer:

- A world built on a third-party ECS exposed that library's registry and handle types in its
  public headers, so every system was bound to its storage model and the world had to be
  rewritten.
- An archetype ECS with hooks, relations, deferred commands and stages was designed, built and
  measured: spawning 100k entities took 5.1 ms, iterating two components over 100k entities
  0.34 ms with 1 to 100 archetypes, and propagating transforms through 111,100 entities four
  levels deep 5.2 ms (desktop CPU, release build). Its open points were whether handles carry a
  world identity, and how component ids are shared between worlds.
- In the earlier C++ world, every system had to check an "enabled" flag up the hierarchy
  itself; world transforms were recomputed for every entity every frame; and after a level
  change, new entities could get the very handles old ones had, so systems holding per-entity
  state had to be told. The editor referred to entities by stable id or by name path, never by
  handle, for undo, selection, clipboard and commands, and that worked. Loading checked
  everything before creating anything, so a failed load left the world untouched.

Facts below were checked on 2026-11-20 against release tags and sources, and both libraries
were compiled and run on Windows with the engine flags.

## Options considered

**A. Our own archetype ECS** (chosen). Every type is ours; component ids and type operations
come from our reflection; change ticks per component value; hierarchy and scheduling designed
for the editor and the job system. Cost: size L, roughly 5–7k lines plus tests, in slices.

**B. flecs v4.2.0** (MIT). The richest option (archetypes, ordered hierarchy, prefabs,
observers, pipeline, threads), and it builds cleanly with our flags. But it is a framework that
duplicates our reflection, JSON, scheduler, job system, OS layer and logging. Its change
detection is per table and misses writes made without an explicit "modified" call, which is the
bug replication must not have. Its 4.2.0 minor release lists 13 breaking changes, and one
author has almost all commits. Wrapping it behind engine types discards most of its value;
exposing it repeats the lesson above.

**C. EnTT v4.0.0** (MIT). Excellent sparse sets and clean with our flags (it needs MSVC 19.44 or
newer). But it has no archetypes, relations, hierarchy or scheduler; reactivity is signals
rather than change ticks; its type ids come from compiler-specific names, so they differ between
MSVC and Clang; and its runtime reflection would be a second reflection system. It provides
storage only, and everything else would be ours anyway.

## Decision

### Placement

1. The ECS is the **`ecs` module in the `core` layer**: entities, components, queries, commands,
   hooks, relations, resources, the schedule and the `World`. No storage type appears in its
   public API, so storage can change on benchmark evidence without breaking callers. No ECS
   library is used.

### Entities and worlds

2. **An entity is a 64-bit generational handle: 32-bit index, 24-bit generation, 8-bit world
   id.** A stale handle fails lookups and never aliases a new entity; an index whose generation
   would wrap is retired. A handle used with a world it doesn't belong to is an assert, so
   editor and Play In Editor handles can't be mixed. Up to 255 worlds exist at once. Clearing a
   world never resets generations, so handles from before a level change stay stale.
3. **Handles are runtime-only.** Persistent identity in files, prefabs and networking is a
   stable object id component (the asset identity ADR), never the handle value. The editor's
   undo history, selection, clipboard and commands refer to entities by that id (or by name
   path for people), so they survive despawn and respawn.
4. **A `World` has a stable address** for its lifetime and is owned by one thread at a time.
   Loading a level clears and refills it; everything is validated before anything is created,
   so a failed load leaves the world as it was. Play In Editor creates a second world and copies
   the editor world into it through the component type records.

### Components and storage

5. **Component ids are reflection's stable type ids**, never compiler type names. One
   process-wide registry, shared by all worlds, maps them to dense indices and to the type
   record (size, alignment, construct, move, copy, destroy). A minimal reflection core with
   these records therefore lands before or with slice 2.
6. **Archetype tables** with one aligned, type-erased column per component; cached edges for
   adding and removing components; `has` in constant time through a per-archetype bitset.
   Sparse-set storage is an opt-in per component type, added only when a benchmark shows
   add/remove churn dominating.

### Queries

7. **Typed queries** for C++ (`const T&`, `T&`, filters `With`, `Without`, `Added`, `Changed`)
   and **dynamic queries by component id** for the editor, serialization and the visual-script
   VM, on the same matching code, with a per-query cache of matching archetypes. A query that
   writes a component and also accesses it another way is an assert.

### Commands

8. **Structural changes are deferred.** Spawn, despawn, add and remove are recorded as commands;
   spawn returns a reserved handle at once. Commands apply at the end of each stage in a
   deterministic order: system by system in run order, each system's commands in recording
   order. That order doesn't depend on how systems were executed.

### Change detection

9. **Each component value has an added tick and a changed tick** (32-bit, clamped periodically
   so wraparound never misleads). Getting mutable access to a value marks it changed (simple,
   and it may over-report a write that changed nothing); there is no separate "mark dirty"
   call. A write-tracking proxy is added only if profiles show the false positives matter.
   Queries filter by added or changed since the system last ran. Replication, render
   extraction and the editor's refresh build on these ticks.

### Hooks

10. **Per-component-type hooks:** `onAdd`, `onInsert`, `onReplace`, `onRemove`. They run as
    changes are applied, may read and write data and queue commands, but never change structure
    directly. The commands they queue run right after the change that caused them. A hook only
    touches its own layer; to inform a higher layer (rendering, physics) it records the change in
    a resource that the other layer drains in its own stage. Observers and gameplay events come
    with the gameplay framework ADR.
11. A component type can be **immutable**: it changes only by being replaced, so its hooks always
    run. Relation links are immutable.

### Hierarchy

12. **`ChildOf(parent)`** is an immutable link with a **despawn policy per link**: by default the
    child is **detached and keeps its world placement** when the parent is despawned;
    despawning with the parent is opt-in, for the parts of one object. Hooks maintain an
    **ordered `Children` list** on the parent, which is the Outliner's order. Despawn runs hooks
    while the entity is whole, so children that despawn with their parent go before it.
    The world also keeps the **order of root entities**. Reparenting keeps the world placement
    by default (shear from a rotated, non-uniformly scaled parent can't be kept). The editor's
    Delete command removes the selected entities with everything below them, as an explicit
    command, whatever the links' policy.
13. Transform propagation is a scheduled system in the `PostUpdate` stage that updates **only
    subtrees whose local transforms or parent links changed**, using the change ticks. Tools that
    need a world matrix before the next propagation compute it on demand.
14. **Enabled state.** Disabling an entity adds a `Disabled` tag; hierarchy hooks keep a
    `DisabledInHierarchy` tag on it and every entity below it. Queries skip entities with either
    tag unless they ask for them (the editor does), so systems never walk the hierarchy to check.

### Prefabs

15. **Prefabs are not an ECS feature.** A prefab is an asset; instantiating it copies its
    entities into the world and records per-property overrides, and changes to the prefab reach
    instances that haven't overridden them. There is no runtime component inheritance. The
    details belong to the asset identity ADR.

### Resources and schedule

16. **Resources** are typed singletons in the world (time, input state, settings), accessed
    through the same declared access as components.
17. **Stages** run in order, each ending with a sync point:
    `Input → PrePhysics → DuringPhysics → PostPhysics → PostUpdate → Extract`.
    Ordinary gameplay runs in `PrePhysics`; `Input` runs input routing (ADR-0013) first;
    `Extract` fills the renderer's data.
18. **Systems are explicit descriptors:** a unique name, declared component and resource access
    (read or write), and `before`/`after` constraints within their stage. Unknown names,
    duplicates and cycles are errors reported before anything runs; touching undeclared data is
    an assert. The first executor is **serial and deterministic**. A parallel executor on the job
    system follows once the job system exists and a profile shows a gain, using the declared
    access, with no API change.

### Evidence and slices

19. **Benchmarks** ship with slice 1 and run in CI: spawn and despawn 100k entities; iterate two
    components over 100k entities with 1, 10 and 100 archetypes; transform propagation over 100k
    entities four levels deep; add/remove churn of a tag on 10k entities; random access by
    handle. The earlier prototype's numbers are the initial targets. A randomized model test
    checks structural invariants, and the dynamic paths are fuzzed.
20. **Implementation slices, in order:**
    1. entities, archetype tables, typed queries, `has`, benchmarks;
    2. component ids from reflection (its minimal core lands first), dynamic queries,
       resources;
    3. commands and the schedule: stages, declared access, serial executor;
    4. hooks and immutable components;
    5. hierarchy: `ChildOf`, ordered `Children` and roots, despawn policy, enabled state,
       transform propagation;
    6. change ticks with `Added` and `Changed` filters;

    then, when needed: world copy for Play In Editor, the parallel executor, sparse-set storage.
    flecs is not linked, not even by tests; it may be added as a test-only benchmark baseline
    only if our numbers need an outside reference.

## Consequences

- One ECS shaped for the editor, replication and the job system, with nothing third-party
  leaking into gameplay code; every slice is ours to maintain and debug.
- Type-erased storage is the riskiest code in the engine; the sanitizer jobs, the model test,
  fuzzing and the benchmarks guard it.
- Marking values changed on mutable access over-reports writes that didn't change anything;
  consumers that care (replication's delta compression) compare values anyway.
- The 8-bit world id limits a process to 255 live worlds and the 24-bit generation retires a
  slot after 16.7 million reuses; both are far beyond expected use.
- The ECS waits on reflection's minimal core from slice 2 on.
- flecs remains a design reference only.
- **Revisit when:**
  - benchmarks miss their targets, or a component's add/remove churn dominates (sparse sets);
  - profiles show the serial executor limiting frame time (parallel executor);
  - the gameplay framework needs observers or events beyond hooks;
  - more than 255 worlds are needed at once.
