- Save Current Level As to a new file gives the saved copy a new level id, and the editor goes on
  editing the copy; the original file keeps its id, so references to it stay on it. Saving over
  another level file takes over that file's id, so references to the overwritten level keep
  working. A file at the target that can't be read (busy, locked, too large, a directory, a FIFO or
  a device) fails the save without replacing it. A failed save changes neither the id nor the file.
  The copy shows in the Content Browser at once, without a Rescan, and a later Rescan finds no
  duplicate id. The viewport cameras go on with the saved level.
- `ith.file.save_as` replaces an existing file without asking; the Save As dialog will ask.
- A level copied outside the editor and opened takes the new id that a Rescan, or its first save,
  gives its file and stays on its own file, so the open level and its file keep one id.
