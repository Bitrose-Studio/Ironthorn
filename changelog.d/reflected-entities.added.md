- `ith::Entity` moved to `ith/foundation/Entity.h` and is a reflected leaf type
  (`ith::TypeKind::Entity`). `ith::remapEntities(type, value, map)` in `ith/reflection/Entities.h`
  replaces every `Entity` that a value's reflected fields hold, at any depth, and
  `TypeInfo::containsEntities` tells which types can hold one.
- The text and binary archives refuse an `Entity` field (`serialization.no_entity_mapping`)
  unless the caller passes an `ith::serialization::EntityMapping`. With one, they write the
  entity as the `Uuid` that the mapping gives and read it back through the mapping.
