- Files and folders dragged from the file manager onto a window arrive as `DropBegin`,
  `DropPosition`, `DropFile` (the path in `text`) and `DropComplete` events, with the window and the
  position in window pixels. Dropped text is not delivered.
- `Platform::showOpenFileDialog` shows the operating system's open-file dialog, with file type
  filters, a starting folder and the choice of several files, for the editor and tools. A later
  pump delivers a `FileDialogFile` event per chosen file and then `FileDialogClosed`, which holds
  the error when the dialog couldn't be shown (on Linux without the desktop portal or `zenity`).
  One dialog is open at a time; another call meanwhile fails with `platform.dialog_busy`.
