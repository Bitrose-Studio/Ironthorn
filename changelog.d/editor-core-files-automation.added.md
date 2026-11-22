- The editor core saves and opens levels (`ith.file.new_level`, `open_level` by path or asset id,
  `save`, `save_as`), writing and reading files on the background lane and asking what to do
  with unsaved changes. Query commands report the document, the selection, an entity, entities by
  label, the undo history and a hash of the level. `ith::Automation` runs commands from C++ and
  from `ith.automation_script` JSON scripts with expected results and errors, and
  `ith::KeymapConsumer` runs commands from the shortcuts of `Keymap.json`, which now binds
  undo, redo, delete, rename, select all and none, the file commands, Play and Simulate.
- Duplicating a selected parent together with its selected children copies them as a hierarchy:
  references between the copied entities point at the copies.
