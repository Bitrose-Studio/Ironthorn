- `isValidContentPath` accepts file and folder names of at most `kMaxContentNameSize` (209) bytes
  instead of 255, so that every accepted name can be written with its sidecar and the temporary
  name of an atomic write.
- The asset database refuses paths whose parent folder is a link to a folder
  (`asset.invalid_path`) and names the file system takes for an existing entry, such as another
  case of a letter outside ASCII on Windows (`asset.path_conflict`); `import` no longer finds a
  file through a link to a folder.
