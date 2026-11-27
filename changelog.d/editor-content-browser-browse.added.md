- The editor has a Content Browser, docked at the bottom, and a content drawer on Ctrl + Space that
  shows the same browser over the bottom of the window. Both show the project's `Content` folder as
  the asset database last scanned it: a folder tree, a path bar, and the folder's items in a grid or
  a list, with type icons, problem entries and badges for what the scan found wrong, a search and a
  type filter (Level, Model, Texture, Audio, Data, Other, Problems) over the folder and its
  subfolders, sorting by name in natural order or by type (also by the list's column headers), and
  a status line. Click, Ctrl, Shift and the arrow keys select; double-click or Enter goes into a
  folder or opens a level, asking about unsaved changes first; Rescan reads the disk again after
  outside changes, and the open level follows its file when it was moved. A new level saved into
  `Content` shows at once. File operations and import come next.
- While the Content Browser has the keyboard focus, Ctrl + A, Escape, Enter (also on the keypad) and
  Alt + Up act on the browser instead of the level (the keymap's new Content context), and the
  arrows, Home, End and Ctrl + F only move in the browser; elsewhere they work as before. F2, Delete
  and Ctrl + D do nothing while the browser has the focus, until its file operations arrive. A click
  into the browser counts at once, also for a key pressed right after it.
- While a menu or a dialog is open, its keys never also run a shortcut: Escape closes the menu only.
- The keymap id `ith.content_browser.open_drawer` is now `ith.content.toggle_drawer`. Input methods
  that take Ctrl + Space (fcitx5, IBus, the Chinese IMEs on Windows) hide it from the editor; rebind
  it in your `Keymap.json` then.
- The `ith.content.*` commands (navigate, navigate up, select, select all, dismiss, view, filter,
  Rescan, open, toggle drawer) and the `ith.query.content`, `ith.query.content_tree` and
  `ith.query.content_problems` queries let automation scripts drive the browser.
- The browser remembers the current folder, the view mode and sort, the expanded folders, the
  tree's width and the drawer's height per project in `Saved/Editor/ContentBrowser.json`.
- `AssetDatabase::folders()` lists every folder below `Content`, empty ones included; a scan goes at
  most `kMaxFolderDepth` (64) folders deep and reports deeper ones as problems.
  `EditorSessionOptions::assets` is now a mutable `AssetDatabase*`.
