- `ith::StableId` (`ith/ecs/StableId.h`): an opt-in, immutable component holding an entity's
  persistent `Uuid`. `World::findByStableId` finds the entity that has an id; engine hooks keep the
  lookup, and a duplicate or nil id asserts and is refused before anything is stored.
- `ith::copyWorld(source, target, options)` (`ith/ecs/WorldCopy.h`) copies a whole world into a
  fresh one, as Play In Editor will: entities keep their slots under the target's world id, every
  component and resource is copied with its reflected `Entity` values remapped, the hierarchy,
  its order and the enabled state are kept, no hooks run and change ticks start over.
  `excludeEditorOnly` leaves out `EditorOnly` components and resources.
- `ChildOf::parent`, `Children::entities` and `InputPlayer::controlled` (Transient) are reflected,
  so world copy remaps them.
