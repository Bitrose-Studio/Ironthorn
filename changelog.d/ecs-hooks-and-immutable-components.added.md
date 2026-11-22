- `ith/ecs/Hooks.h`: component hooks (ADR-0030). `ith::setComponentHooks<T>({.onAdd, .onInsert,
  .onReplace, .onRemove})` sets a component type's hooks once per process, before any world stores
  the type, and every `ith::World` then runs them as it adds, replaces and removes the type's
  values: spawn runs `onAdd` then `onInsert` in the order the components were given; despawn and
  `clear()` run `onReplace` then `onRemove` while the entity is whole, sorted by stable type id.
  A hook gets an `ith::HookContext` with the entity, the type, the value, component and resource
  access and a command buffer. Direct structural changes inside a hook are refused; its commands
  are applied right after the change that ran it, depth-first, also between the commands of
  `World::apply`. The World's destructor runs no hooks.
- Immutable components: `ITH_TYPE(Component, Immutable)` sets the new reflection flag
  `ith::ReflectionFlags::Immutable`, which the generator accepts only on component types. An
  immutable component changes only by being replaced through `add`, so its hooks always run:
  non-const `get` (typed and by record), writable query terms (typed and dynamic) and the non-const
  `forEachComponent` assert and are refused, and a system that declares write access to one fails
  `Schedule::build()` with `ecs.schedule.invalid_access`.
- `World::add(entity, type, value)` adds a value by reflection record, moved from a value the
  caller built, so hooks see it; a const `forEachComponent` visits every component read-only.
