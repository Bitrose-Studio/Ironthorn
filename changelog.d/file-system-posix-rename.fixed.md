- On Windows, `fs::writeFileAtomic` no longer fails with `fs.busy` while another thread or process
  reads the target: it renames through the temporary file's handle with POSIX semantics, which
  replaces a file that readers hold open with delete sharing, and falls back to `MoveFileExW` where
  the file system refuses that. `fs::readFile` looks again for a file it doesn't find, so a read
  that races such a replacement doesn't report `fs.not_found`.
