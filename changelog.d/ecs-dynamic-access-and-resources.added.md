- `ith::World` reaches components by reflection record as well as by C++ type: `has`, `get`,
  `add` (default-constructs) and `remove` take a `const ith::TypeInfo&`, and
  `forEachComponent(entity, function)` visits every component of an entity with its record.
  This is the access the editor, serialization and scripting use.
- `ith/ecs/DynamicQuery.h`: `world.dynamicQuery(terms)` iterates the entities that have a set of
  component types known only at run time, given as `ith::QueryTerm` records with read or write
  access, with the same matching and archetype cache as typed queries. A query without terms
  visits every entity.
- World resources: typed singletons with the `Resource` reflection role, through
  `insertResource`, `resource<T>()` and `removeResource<T>()`, or by record. A resource keeps its
  address until it is removed, survives `clear()`, and may change while a query iterates.
- `ith_ecs_dynamic_fuzz` fuzzes the record-based paths in the Clang sanitizer builds. See
  `docs/development/ecs.md`.
