- `fs::readFile` reads only regular files and never waits for a FIFO's writer. On POSIX a FIFO, a
  socket or a device fails at once with `fs.unsupported`, and a file under another process's write
  lease with `fs.busy`. On Windows a handle that is not a disk file (a pipe, a console, a device)
  fails with `fs.unsupported`; an AF_UNIX socket file fails to open, with `fs.io`.
