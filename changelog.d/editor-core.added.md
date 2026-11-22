- New `editor_core` module, the editor without its UI: an `ith::EditorSession` with the editor
  document and a registry of commands with stable ids and JSON arguments; the built-in commands to
  create, rename, attach, detach, move, delete, duplicate and select entities, add and remove
  components and set properties; transactions that apply live, nest and cancel; and an undo history
  that names entities by stable id, carries the selection, keeps 500 entries or 256 MiB, and
  tracks whether the level has unsaved changes. Its tests run headless, under sanitizers too.
