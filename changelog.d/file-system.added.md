- A `file_system` module (`ith/file_system/FileSystem.h`, `Path.h`) is the engine's one way to
  files: UTF-8 paths with `/` separators (long paths and names outside ASCII work on Windows),
  lexical path helpers, `fs::readFile` with a size limit, `fs::writeFileAtomic` that replaces a
  file through a temporary file and, with `Durability::Durable` (the default), flushes it to the
  disk before it returns, `fs::fileInfo`, `fs::listDirectory`, `fs::createDirectories`,
  `fs::remove`, an `fs::move` that never replaces an existing file, and the executable, user
  configuration and user cache folders. Errors carry `fs.*` codes and the operating system's text.
  It is a runtime module, so servers and tools can use it.
