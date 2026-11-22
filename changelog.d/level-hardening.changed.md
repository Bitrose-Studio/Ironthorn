- `ith::saveLevel` takes the `TypeRegistry`, so it leaves out an unresolved component whose name
  now resolves, also through a `FormerName`, to a component the entity has; such a save loads again.
- `ith::loadLevel` is linear in the size of a level, also for very deep and very wide hierarchies;
  every record and component value it reads draws from one allocation budget; its warnings are
  built only when collected and capped at `kMaxLevelWarningsPerCode` per code, with a count of the
  rest; and names from the file are shortened and escaped in messages.
- `ith::serialization::ValueReadOptions::sharedBudget` lets several `fromValue` reads share one
  allocation budget, and `ith::serialization::printable` makes text from data safe for messages.
