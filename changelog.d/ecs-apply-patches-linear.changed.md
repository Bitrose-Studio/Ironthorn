- `World::applyPatches` plans a batch in linear time: earlier changes to a component are found
  through a hash map instead of a scan, so batches of tens of thousands of changes stay fast.
