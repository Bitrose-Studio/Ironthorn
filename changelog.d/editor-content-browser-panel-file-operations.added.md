- The Content Browser panel and the content drawer now do the file operations themselves:
  - F2, Rename in a context menu and New Folder open a rename field in place of the item's name.
    It shows the extension after the field and says why a name is invalid as you type. Enter
    renames, Escape cancels, and clicking away keeps a valid name.
  - Delete asks first. The dialog lists the items, the assets and other files inside folders, every
    asset that is still used with its users (the open level and its unsaved changes included), and
    the files that couldn't be read. Cancel has the keyboard focus.
  - Dragging items or tree folders onto a folder, a tree folder or a path bar segment moves them. A
    folder can't be dropped into itself, and the drag's tooltip says why.
  - Context menus offer Rename, Duplicate, Delete, Reimport (greyed when only native assets are
    selected), and Import or Assign New Id for problem entries. Tree folders offer New Folder,
    Rename and Delete; the empty space offers New Folder and Import.
- Import opens the system's file dialog and imports the chosen files into the current folder. Files
  dragged from the file manager onto the browser are imported into the folder under the pointer,
  which is highlighted while the drag hovers. When no file dialog can be shown, the status line says
  to drop files instead.
- Name conflicts during an import are asked one file at a time, with Replace, Keep Both or Skip and
  "Apply to all remaining". Cancel imports nothing.
- The browser's status line shows what the last file operation did or why it was refused.
- The status bar has a Content Drawer button that opens and closes the drawer.
- `UiHost::showOpenFileDialog`, `EditorUi::handleEvent` and `EditorUiDesc::contentBrowser` hand the
  open-file dialog, drops and the dialog's results between the editor application and the Content
  Browser. `ImporterRegistry::extensions()` lists the extensions the registered importers read.
- `UiHost` has a new pure virtual function, `showOpenFileDialog`: every `UiHost` implementation
  outside the engine (a test host, a tool) must add it. Return the dialog's error, or an error
  when the tool has no dialog.
- Files that arrive while a question of the browser is open, or while another import waits to run,
  are not imported: the status line says why. Files dropped where Windows reports no position go
  into the browser's current folder.
- Known gap: with the keyboard focus in the folder tree, F2 and Delete still act on the selected
  items, not on the folder; rename or delete a tree folder through its context menu.
