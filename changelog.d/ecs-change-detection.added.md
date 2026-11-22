- ECS change detection (ADR-0014 rule 9): every component value has an added and a changed tick.
  Spawning and adding stamp both; replacing, `get<T>` with a non-const `T`, the non-const
  `get(entity, type)` and `forEachComponent`, written query terms and `HookContext::value<T>` stamp
  the changed tick. Inside systems, the query filters `ith::Added<T>` and `ith::Changed<T>`
  (`ith::FilterKind::Added` and `Changed` for dynamic queries) keep the values added or changed since
  that system last ran on the world; they need read access to `T` and assert outside systems.
  Ticks are 32-bit, compared relative to the current tick and clamped, so wraparound is safe.
- Transform propagation writes only the subtrees whose `LocalTransform` or `ChildOf` changed, or
  that lost one; the first propagation on a world, or one after most of it changed, writes
  everything. Each `WorldTransform` written is marked changed.
