- The editor core has view models for the Outliner and Details panels: a reusable virtualized tree,
  the Outliner's order, search, selection, rename, Delete, Enabled and drag and drop, and Details
  rows generated from reflection with multi-select, Min and Max clamping, Reset to Default, lists,
  optionals, Add and Remove Component, and slider drags that preview live and make one undo entry.
- New commands: `ith.entity.reset_property`, `ith.entity.insert_element`,
  `ith.entity.erase_element` and `ith.entity.set_enabled`. `ith.entity.attach` and
  `ith.entity.detach` take an optional `before` to place the entities among their new siblings.
- Shortcuts and menus invoke commands through prompts, so F2 can start renaming in the Outliner.
