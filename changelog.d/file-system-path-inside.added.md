- `fs::pathInside(path, folder)` tells whether a path is a folder or inside it the way the operating
  system sees it, and returns the rest of the path with the case that is on disk.
  - It resolves the longest existing part of each path first: the final path on Windows, so other
    cases, 8.3 short names, junctions and links compare right, and `realpath` on POSIX.
  - The part that doesn't exist yet is appended as given.
  - On Windows, names compare without case.
- `fs::hasUnusableName(path)` is true for a raw path that names something the module can't address:
  on POSIX, a name that holds `\`.
