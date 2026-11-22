- `ith::LocalTransform` now carries an engine `onRemove` hook, like `ChildOf` and `Disabled`, so
  `setComponentHooks<LocalTransform>` is refused. `transformPropagationSystem()` also declares read
  access to `ChildOf`.
