- `ith/ecs/Query.h`: query filters (ADR-0014 rule 7). `ith::With<T>` requires a component and
  `ith::Without<T>` excludes it, anywhere among a typed query's terms; they hand out no values and
  need no declared access in a system. `world.query<ith::With<Selected>>()` visits entities by
  filters alone. Dynamic queries take the same filters as `ith::QueryFilter{.type, .kind}` with
  `ith::FilterKind::With` or `Without`: `world.dynamicQuery(terms, filters)`.
