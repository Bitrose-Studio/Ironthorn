- A `level` module (`ith/level/Level.h`) saves and loads levels: `ith::saveLevel(world, levelId)`
  writes the world's entities as an `ith.level` text document, and `ith::loadLevel(text, world,
  registry)` replaces the world's entities with a document's, all or nothing. Records are in
  Outliner order with their stable id, components are keyed by type name as full values,
  `EditorOnly` components go into an `editor` section, entity references are saved as stable ids,
  and the same world always gives the same bytes. Loading checks the whole document before it
  changes the world, then adds every component through `World::add` with `ChildOf` first, so
  hooks run. Entities without a `StableId` are left out with a warning, and components whose type
  is unknown or whose value doesn't read are kept in `ith::UnresolvedComponents` and saved back.
- `ith::kLevelFormat` moved from the asset database to `ith/level/LevelFormat.h`.
- `World::isIterating()` tells whether structural changes are refused right now.
