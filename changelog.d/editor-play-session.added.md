- The editor core runs Play In Editor and Simulate sessions headless: `ith.play.play` and
  `ith.play.simulate` copy the open level in memory, without editor-only data, into a game world
  with its own `ith::Time` and the game-world schedule (`EditorSessionOptions::gameSystems`);
  `ith.play.pause`, `ith.play.step` (exact ticks for scripts and tests), `ith.play.possess_or_eject`,
  `ith.play.free_mouse` and `ith.play.stop` drive it, and `ith.query.play` reports its state, time
  and entities. Until the views and input arrive, `ith.play.possess_or_eject` only switches the
  possessed flag and `ith.play.free_mouse` only reports that the mouse is free. Stop throws the
  game world away and leaves the level, its history, dirty state and selection as they were. The
  play commands also run while a Details drag is open: Stop drops it, Step keeps the game going.
- While a session runs, the selection commands, the Details value edits (`ith.entity.set_property`,
  `reset_property`, `insert_element`, `erase_element`), the Outliner and Details panels' models and
  the `ith.query.selection`, `entity`, `find` and `world_hash` queries act on the game world; the
  queries take `world: "editor" | "game"`. Edits of the game world record no undo entry. Entities
  spawned during play show in the Outliner as "Spawned <n>" and can't be selected.
- While playing, Save and Save As are refused like Undo, Redo, New Level, Open Level and every
  structural edit, `ith.entity.set_enabled` included, all with `editor.command.playing`, which
  commands now report as such instead of `editor.command.disabled`. The Content Browser's file
  operations stay available.
