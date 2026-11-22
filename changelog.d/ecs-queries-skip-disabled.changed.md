- ECS queries, typed and dynamic, skip disabled entities (those with `ith::DisabledInHierarchy`)
  unless they ask for them, so `world.dynamicQuery({})` visits every enabled entity; add an
  `ith::FilterKind::IncludeDisabled` filter to visit every entity, as level saving must.
