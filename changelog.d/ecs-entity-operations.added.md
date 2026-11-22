- `ith::World::applyPatches` also spawns and despawns whole entities and places them among their
  siblings (`ith::EntityOp::Spawn` under placeholder handles, `ith::EntityOp::Despawn`,
  `ith::EntityPatch::placeBefore`), all in one checked step. A Despawn is refused while the entity
  has children, and is undone by a Spawn of all its components in its old place. Inverses now also
  restore what hooks changed on the same entity, so undoing and redoing a reparent gives the same
  values bit for bit. `ith::remapEntities` translates the handles of an `ith::EntityPatch`.
