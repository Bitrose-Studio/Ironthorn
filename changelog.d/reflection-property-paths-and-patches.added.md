- Property paths (`ith/reflection/PropertyPath.h`): `ith::PropertyPath::parse(type, "items[3].name")`
  and `parseQualified(registry, "game::Light.intensity")` name a value inside a reflected value,
  with its type, field attributes and flags known before resolving it. Optionals are transparent,
  fields are found by their `FormerName` too, and `resolve` reaches the value.
- Patches (`ith/reflection/Patch.h`): `ith::Patch` with `set`, `insert` and `erase` entries,
  `preparePatch` and `applyPatch`, which check the whole patch on a copy, change nothing on
  failure and return the inverse patch for undo, and `remapEntities` for the entities in a patch.
  `ith::OwnedValue` holds a value of any reflected type.
- `ith::serialization::toValue` and `fromValue` convert one reflected value to and from a `Value`
  tree with the text archive's rules, without a document around it.
- `ith::TypeInfo::count` gives the number of elements of a `std::array` type.
