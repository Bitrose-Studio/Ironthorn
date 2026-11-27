- The Content Browser's file operations are commands that automation scripts can run:
  `ith.content.new_folder`, `rename`, `move`, `duplicate`, `delete`, `import`, `reimport` and `fix`.
  None of them can be undone, and none makes the level dirty. Each one waits until a level save in
  progress has finished.
- Names follow one rule: no other file or folder in the folder may have the same name in any letter
  case. New folders, duplicates and imports kept with Keep Both are numbered `<name>_2`, `<name>_3`,
  and so on. Renamed and moved assets keep their ids, and the open level's path follows its file.
- A delete without `confirmed` deletes nothing. It returns a preview of what would go, including
  the files inside folders that the browser doesn't show, and each asset that other assets or the
  open level's unsaved state still use, with its users. Once confirmed, the delete goes ahead and
  the references keep the deleted id. The open level and the folders that hold it can't be deleted.
- Import copies files from outside the project into a folder, then imports them. The files their
  importer reads and the files they refer to are copied along.
  - Dropped folders keep their subfolders, and merge into a folder of the same name in another
    letter case.
  - Links inside a dropped folder are skipped; a link dropped on its own is imported.
  - Files already in `Content` are imported where they are, and files no importer reads are skipped.
  - A taken name stops the import with a list of the conflicts until each one is answered with
    Replace, Keep Both or Skip.

  Reimport runs the importer again; Fix imports a file that has no sidecar, or gives an unresolved
  duplicate a new id.
- With the Content Browser focused, F2 and Delete start a rename or a delete, and Ctrl + D
  duplicates. Escape cancels a pending rename or dialog before it clears the selection. These keys
  never reach the level while the browser has the focus, even when a keymap layer unbinds the
  content commands.
- On Windows, a file that another program holds open can't be renamed, moved, replaced or deleted.
  Such an operation fails with an error that names the file.
- `AssetDatabase::importers()` returns the importer registry that the database uses.
