# ADR-0031: ECS: hierarchy, enabled state and transform propagation completed

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0014](0014-ecs-storage-and-world.md) (ECS) rules 12–14, 19 and 20;
  [ADR-0006](0006-math-and-coordinate-conventions.md) (math, glm configuration),
  [ADR-0008](0008-editor-familiar-to-unreal-developers.md) (Outliner, Delete),
  [ADR-0016](0016-gameplay-model-on-the-ecs.md) (relations kept by hooks),
  [ADR-0017](0017-runtime-and-editor-separation.md) (editor and Play In Editor worlds),
  [ADR-0030](0030-ecs-hooks-registration-and-immutable-components.md) (hooks, immutable components)

## Context

ECS slice 5 (ADR-0014 rule 20.5) adds the hierarchy, enabled state and transform propagation.
ADR-0014 rules 12–14 state the intent but leave open:

- where the transform components live and what the world transform is;
- how propagation is split between slice 5 and slice 6 (change ticks);
- how reparenting keeps the world placement, and how a caller keeps the local one instead;
- how roots and sibling order are stored and changed;
- what `World::despawn` does to children, and what the editor's Delete does;
- which parents are invalid, and how queries opt in to disabled entities.

This ADR settles only those points, and changes rule 12's editor Delete. Everything else in
ADR-0014 stands.

Prior art, from public documentation:

- **Unreal:** attaching a component or actor takes attachment rules per location, rotation and
  scale, among them "Keep Relative" and "Keep World"; deleting an actor in the editor's level
  detaches the actors attached to it, which stay in the level.
- **Bevy:** the hierarchy is a `ChildOf` relationship component with a `Children` list kept by
  hooks; a `Disabled` component is skipped by queries through default query filters unless a
  query names it.
- **flecs:** the hierarchy is the `ChildOf` relationship, whose cleanup policy by default deletes
  children with their parent; queries skip entities with the `Disabled` tag unless they match it
  explicitly.

## Options considered

How reparenting treats the placement:

- **Keep the world placement** (chosen, the owner's decision): what the user sees doesn't move,
  as Unreal's "Keep World". Costs a decomposition, and shear is lost.
- **Keep the local transform:** simplest, but the entity jumps. Callers who want it set
  `LocalTransform` after reparenting, so no flag is needed.

What the editor's Delete does to children:

- **Remove the whole subtree** (ADR-0014 rule 12 as written).
- **Remove the selection and detach the other children in place** (chosen, the owner's
  decision), as in Unreal's editor, with a separate "Delete with children".

How the world transform is stored:

- **`Transform`:** smaller, but can't represent the shear that rotation with non-uniform scale
  under a parent produces.
- **`Mat4`** (chosen): exact for any hierarchy, and what rendering consumes.

## Decision

1. **Transform components live in `ecs`,** which may depend on `math` (a lower layer).
   - `ith::LocalTransform { ith::Transform value; }` is written by users.
   - `ith::WorldTransform { ith::Mat4 matrix; }` is Transient and written only by propagation.
     It is a `Mat4` because rotation combined with non-uniform scale under a parent produces
     shear.
2. **Propagation** (completes ADR-0014 rules 13 and 19).
   - Slice 5 ships a full propagation system in `PostUpdate`: every root, then down the tree.
   - An entity in the hierarchy without `LocalTransform` counts as identity: its `WorldTransform`, if it has one, equals its parent's.
   - Slice 6 adds the changed-subtree check with change ticks (ADR-0014 rule 13).
   - The rule 19 propagation benchmark moves to slice 5: 100k entities four levels deep, target
     5.2 ms.
3. **Reparent and detach keep the world placement by default.**
   - The `ChildOf` hooks compute the entity's current world matrix on demand by walking up its
     ancestors, then set `local = inverse(newParentWorld) * world`. Shear is dropped.
   - `math` gains a translation-rotation-scale decomposition of a `Mat4`. `glm::decompose` needs
     `GLM_ENABLE_EXPERIMENTAL`, which ADR-0006 rule 3 forbids.
   - To keep the local transform instead, the caller sets `LocalTransform` after reparenting, in
     the same command buffer. There is no flag.
4. **Roots.** Every entity without `ChildOf` is a root. The root order is an intrusive linked
   list in the world's entity records, constant time per operation, iterated with
   `world.forEachRoot`.
5. **Order.**
   - Children are appended when they gain the parent.
   - `placeBefore(entity, sibling or end)` on `World` and `Commands` reorders children and roots.
   - The Outliner shows this order.
6. **`Children`.**
   - `Children { std::vector<Entity> }` is Immutable to users and written by the hierarchy hooks
     through an internal path.
   - It is Transient. The level format saves entities in Outliner order and rebuilds `Children`
     by appending; the level-documents ADR must confirm this.
7. **Despawn policy** (completes ADR-0014 rule 12).
   - `ChildOf { Entity parent; bool despawnWithParent = false; }`.
   - `World::despawn` handles the hierarchy itself. First it despawns the children marked
     `despawnWithParent`, depth-first, children before parents. Then it detaches the other
     children, which become roots and keep their world placement.
   - `despawnRecursive` on `World` and `Commands` removes a whole subtree.
   - **The editor's Delete** (replaces the last sentence of ADR-0014 rule 12) removes the
     selected entities; their children not marked `despawnWithParent` are detached in place and
     become roots. A separate "Delete with children" command uses `despawnRecursive`.
8. **Invalid parents.** A `ChildOf` whose parent is the entity itself, one of its descendants, a
   dead entity or an entity of another world asserts and is refused before the value is stored.
9. **Enabled state** (completes ADR-0014 rule 14).
   - `Disabled` is a user tag. `DisabledInHierarchy` is Transient and kept by hooks on the
     disabled entity and every descendant.
   - Every query skips `DisabledInHierarchy` unless it names `Disabled` or `DisabledInHierarchy`
     as a term or filter, or opts in with `ith::IncludeDisabled` (typed) or
     `FilterKind::IncludeDisabled` (dynamic).
   - The editor and the level saver opt in.
   - `dynamicQuery({})` now visits every enabled entity; with `IncludeDisabled` it visits every
     entity. This changes the documented rule that it visits every entity.

## Consequences

- Systems read `WorldTransform` without walking the hierarchy, and never check enabled state
  themselves.
- Reparenting never moves what the user sees, except for dropped shear. Keeping the local
  transform costs the caller one extra command.
- Every world pays a full propagation each frame until slice 6.
- `Children` and `DisabledInHierarchy` are derived data: never saved, rebuilt by hooks, so a
  level file can't contradict them.
- Code that relied on `dynamicQuery({})` reaching disabled entities must opt in.
- The editor's Delete differs from ADR-0014 rule 12 as written; Delete with children keeps the
  old behaviour as a separate command.
- Follow-up work: the decomposition in `math` with tests; slice 5 in `ecs` (components, hooks,
  roots list, `placeBefore`, `despawnRecursive`, `IncludeDisabled`, propagation and its
  benchmark); the level-documents ADR confirms rebuilding `Children` from saved order.
- **Owner's answers at acceptance (2026-11-22):** reparent and detach keep the world placement
  by default; the editor's Delete detaches children in place, with a separate Delete with
  children.
- **Revisit when:**
  - the propagation benchmark misses its target after slice 6;
  - a real case needs shear kept through reparenting;
  - the level-documents ADR chooses to save `Children` instead of rebuilding it.
