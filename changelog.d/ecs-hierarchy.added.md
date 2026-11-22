- `ith/ecs/Hierarchy.h`: the ECS hierarchy (ADR-0031). `ith::ChildOf{.parent, .despawnWithParent}`
  links a child to its parent; the World keeps each parent's ordered `ith::Children` and an ordered
  list of roots (`World::forEachRoot`), and `placeBefore` on `World` and `Commands` reorders
  siblings. Reparenting and detaching keep the entity's world placement. Despawning a parent
  despawns its `despawnWithParent` children first and detaches the others in place;
  `despawnRecursive` on `World` and `Commands` removes a whole subtree. A parent that is the entity
  itself, a descendant, dead or of another world asserts and is refused.
- Enabled state: `ith::Disabled` turns an entity and its descendants off; the World keeps
  `ith::DisabledInHierarchy` on them, and every query skips them unless it names either tag or
  includes `ith::IncludeDisabled` (`ith::FilterKind::IncludeDisabled` for dynamic queries).
- `ith::LocalTransform` and `ith::WorldTransform`, with `ith::propagateTransforms(world)` and the
  `PostUpdate` system `ith::transformPropagationSystem()` (`ith/ecs/TransformPropagation.h`),
  which compute every world matrix from the local transforms down the hierarchy.
- `ith::decompose(matrix)` in `ith/math/Transform.h`: the transform closest to an affine matrix,
  exact for matrices `toMatrix` makes, with shear dropped.
