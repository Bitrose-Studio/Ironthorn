# ADR-0030: ECS: component hooks registration and immutable components

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0014](0014-ecs-storage-and-world.md) (ECS) and
  [ADR-0018](0018-reflection.md) (reflection); [ADR-0016](0016-gameplay-model-on-the-ecs.md)
  rule 4 (hooks are engine invariants), [ADR-0017](0017-runtime-and-editor-separation.md)
  (Play In Editor), [ADR-0019](0019-reflection-rules-corrected.md),
  [ADR-0020](0020-serialization-rules-completed.md) (serialization flags)

## Context

ECS slice 4 (ADR-0014 rule 20) adds hooks and immutable components. ADR-0014 rules 10 and 11
name them but leave open:

- how a component type is marked immutable, and what "changes only by being replaced" refuses;
- where hooks are registered: per world or once per type;
- when each hook runs relative to the others, during spawn, despawn and `clear()`, and what a
  hook may do.

This ADR settles only those points. Everything else in ADR-0014 and ADR-0018 stands.

Prior art, from public documentation: Unreal's Mass observers are processors that run when a
fragment or tag is added to or removed from entities. Bevy has per-type `on_add`, `on_insert`,
`on_replace` and `on_remove` hooks, registered per world, and immutable components declared on
the type.

## Options considered

How immutability is declared:

- **A reflection flag** (chosen). Every path that handles the type reads its record, so all of
  them see it.
- **A C++ trait or marker on the type.** Typed calls see it; calls by type record, records from
  another binary, level loads and the editor don't.

Where hooks are registered:

- **Per world**, like Bevy's `register_component_hooks`. Every world, each Play In Editor copy
  included, must register the engine's hooks; forgetting once silently breaks an invariant such
  as the hierarchy.
- **Once per process, per component type, in the component registry** (chosen).

## Decision

1. **Immutable is a reflection flag** (adds to ADR-0018 rule 4 and ADR-0014 rule 11).
   - Written `ITH_TYPE(Component, Immutable)`, stored as `ReflectionFlags::Immutable`.
   - Valid only with the `Component` role; on any other role, a field or an enum the generator
     rejects it with a `file:line` error.
   - **Why a flag:** typed calls, calls by type record, records from another binary, level loads
     and the editor's Details panel all see it. Details offers "replace" for an immutable
     component instead of editing it in place.
2. **Immutable components are never written in place.** Every mutable access asserts and is
   refused:
   - non-const `get`, typed and by record;
   - writable query terms, typed and dynamic;
   - the writable pointer from `forEachComponent`;
   - write access through `SystemContext`.

   A system that declares write access to an immutable type fails `Schedule::build()` with
   `ecs.schedule.invalid_access`. Replacing through `add` or `Commands::add` stays allowed, so
   the hooks always run.
3. **Serialization is unaffected.** Immutable values are saved and loaded like any component
   (ADR-0020 rule 5 lists no effect for it); loading creates them through `add`, the replace
   path, never by writing in place.
4. **Hooks are registered once per process, per component type,** in the process-wide component
   registry (ADR-0014 rule 5).
   - `ith::setComponentHooks<T>(ith::ComponentHooks{...})`, and an overload taking a
     `TypeInfo`. Each hook is a plain function pointer `void (*)(ith::HookContext&)`, with no
     state.
   - Set once, before any world stores the type. Setting them again, or after first use,
     asserts.
   - **Why:** rule 10 says "per component type"; ADR-0016 rule 4 makes hooks engine invariants;
     and Play In Editor world copies get the same invariants without registering anything.
5. **When hooks run** (completes ADR-0014 rule 10):
   - `onAdd`: the entity gains a type it didn't have.
   - `onInsert`: after every add or replace, after `onAdd`.
   - `onReplace`: before a value is overwritten or removed, with the old value still present.
   - `onRemove`: before removal or despawn, with the value still present.
   - **Despawn** runs `onReplace` then `onRemove` for every component while the entity is still
     whole (ADR-0014 rule 12).
   - **Order:** spawn runs hooks in the order the components were given; despawn and `clear()`
     run them sorted by stable type id.
   - `clear()` runs remove hooks; the `World` destructor doesn't.
6. **What a hook may do.**
   - `HookContext` exposes the entity, the `TypeInfo`, a pointer to the value, `get`, `has`,
     resource access and `commands()`. Structural changes made directly inside a hook are
     refused.
   - Commands a hook queues are applied right after the change that caused them, depth-first,
     with a fatal depth limit. Applying a command buffer (`World::apply`) applies the hook
     commands of each command before the next command.

## Consequences

- An engine invariant (hierarchy, enabled state) is registered once and holds in every world,
  Play In Editor copies included.
- Immutability can't be bypassed by type-erased code or another binary; editing an immutable
  component in the editor is always a replace, with its hooks.
- `ReflectionFlags` gains one bit (bit 6 of its 8).
- Hooks can't carry per-world state; a hook that needs it uses a world resource.
- Deep hook chains end in a fatal error rather than running unbounded.
- Follow-up work: the `Immutable` attribute in `ith_reflect` with tests; `ComponentHooks`,
  `HookContext` and the checks above in `ecs` (slice 4).
- **Owner's answers at acceptance (2026-11-22):** immutability is the reflection flag
  `Immutable`, valid only on components, enforced on every mutable path; hooks are registered
  once per process per type, not per world.
- **Revisit when:**
  - a real hook needs per-world state that a resource can't hold;
  - the gameplay framework needs observers beyond ADR-0016's run conditions.
