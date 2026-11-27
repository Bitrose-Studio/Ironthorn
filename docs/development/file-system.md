# File system

The `file_system` module (`engine/ith/file_system`) is the engine's one way to files and
directories. The design is [ADR-0035](../decisions/0035-file-system-configuration-and-main-thread-queue.md)
rules 1 to 10. It is a `runtime` module in the `platform` layer, depends only on `foundation`, and
works on the operating system's own APIs: the Win32 wide functions on Windows, POSIX elsewhere.
The editor, the cooker, players and dedicated servers can all link it.

```cpp
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"

namespace fs = ith::fs;

const std::string file = fs::joinPath(projectDirectory, "Content/Maps/Harbour.ithmap");
ith::Expected<void> saved = fs::writeFileAtomic(file, levelText);   // durable by default
ith::Expected<std::string> text = fs::readFile(file, ith::serialization::kMaxTextSize);
if (!text) {
    log(text.error().code, text.error().message);   // "fs.not_found", "can't open '...': ..."
}
```

## Paths

- Paths are UTF-8 `std::string_view` with `/` separators; `\` is accepted on input. A path that
  isn't valid UTF-8 or holds a NUL byte fails with `fs.invalid_path`. `std::filesystem` is not
  part of the API.
- On Windows the module converts paths to UTF-16 and gives absolute ones the `\\?\` prefix after
  normalization, so paths longer than 260 characters work without the long-path manifest.
  Relative paths resolve against the working directory as they are.
- The module never folds case. The asset database keeps ADR-0015's case rules on top of
  `listDirectory`.

The lexical helpers in `Path.h` touch no disk and behave the same on every platform:

| Function | Result |
|---|---|
| `normalizePath("C:\\Game\\.\\Content\\..\\Config\\")` | `"C:/Game/Config"` |
| `normalizePath("/a/../../b")` | `"/b"` (never above the root) |
| `normalizePath("../a/../../b")` | `"../../b"` (leading `..` of a relative path are kept) |
| `joinPath("/home/ann", "Saved/Editor")` | `"/home/ann/Saved/Editor"`; an absolute second path wins |
| `parentPath("Content/Ships/Hull.gltf")` | `"Content/Ships"`; a root is its own parent |
| `fileName("Content/Ships/Hull.gltf")` | `"Hull.gltf"` |
| `extension("archive.tar.gz")` | `".gz"`; `".gitignore"` has none |

Three roots are recognized: `/`, a drive root `C:/` and a UNC root `//server/share`.

## Operations

| Function | What it does |
|---|---|
| `readFile(path, maxSize)` | The whole file. The size is checked before anything is allocated (`fs.too_large`). Only a regular file is read, at once, without waiting for a FIFO's writer: on POSIX a FIFO, a socket or a device fails with `fs.unsupported`, and an open held up by another process's write lease with `fs.busy`; on Windows a handle that is not a disk file (a pipe, a console, a device) fails with `fs.unsupported`, while an AF_UNIX socket file already fails to open, with `fs.io`. On Windows the file is opened with read, write and delete sharing, so a reader never blocks a replacement, and an open that races a replacement in progress is tried again (see below). |
| `writeFileAtomic(path, bytes, options)` | Readers see the old file or the new one, never a partial one. See below. |
| `fileInfo(path)` | Kind, size and modification time (nanoseconds since 1970 UTC), or `nullopt` when nothing is there. A final link is reported as a link, with its target's kind and size. |
| `listDirectory(path)` | One level: name, kind, size and `validName`. |
| `createDirectories(path)` | The directory and its missing parents; an existing one is fine. |
| `remove(path)` | A file, a link or an empty directory (`fs.not_empty` otherwise). |
| `move(from, to)` | Never replaces an existing target (`fs.already_exists`), except a rename that changes only the case of the same file. |
| `executableDirectory()`, `userConfigDirectory()`, `userCacheDirectory()` | Known folders, absolute and normalized. |

Kinds are `File`, `Directory`, `FileLink`, `DirectoryLink` (symbolic links; on Windows junctions
too) and `Other` (devices, pipes, sockets and links whose target is missing). Listings don't
follow links.

A name in a listing that isn't valid Unicode (an unpaired surrogate on Windows, bytes that aren't
UTF-8 on Linux) comes with `validName == false` and a display name with U+FFFD in place of the bad
parts, so callers can report it; it can't be opened by that name. A Linux name that holds `\` is
listed as it is but with `validName == false` too: every function here takes `\` for a
separator, so joining that name into a path would address another entry, possibly outside the
folder (`..\..`). Never join a name whose `validName` is false into a path.

### Atomic writes

`writeFileAtomic` writes the bytes to `<name>.tmp-<uuid>` in the target's folder and renames that
over the target. Missing parents are created unless `createParents` is false. The temporary file
is removed on every failure; `isTemporaryFileName()` lets scans report the ones a crash left.

| Step | POSIX | Windows |
|---|---|---|
| Create | `open` with `O_CREAT \| O_EXCL \| O_CLOEXEC` | `CreateFileW` with `CREATE_NEW`, `GENERIC_WRITE \| DELETE` access, read and delete sharing |
| Write | a loop that retries `EINTR` and short writes | `WriteFile` in a loop |
| Keep permissions | the existing target's permission bits (`fchmod`) | (attributes are not copied) |
| Flush (`Durable`) | `fsync` | `FlushFileBuffers` |
| Replace | `rename` | `SetFileInformationByHandle` with `FileRenameInfoEx`, `FILE_RENAME_FLAG_REPLACE_IF_EXISTS \| FILE_RENAME_FLAG_POSIX_SEMANTICS`, through the open handle; where that is refused, `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`, plus `MOVEFILE_WRITE_THROUGH` when durable |
| After (`Durable`) | `fsync` of the folder, and of the parent of every folder the call created | nothing: Windows documents no directory flush |

- `Durability::Durable` (the default) is for what the user made: levels, sidecars, settings.
  `Durability::Atomic` skips the flushes, for data that can be rebuilt, such as derived-data cache
  entries. The module claims no more than these calls give; durability across a real power loss
  isn't tested.
- **Readers don't hold up a replacement on Windows.** `MoveFileExW` can't replace a file that
  another handle has open, even with delete sharing; it fails with access denied for as long as a
  reader holds the file, so a file that is read in a loop could not be saved. The rename with POSIX
  semantics (Windows 10 and later, on NTFS) replaces the name while readers hold the old file,
  and they keep reading the old content, as on POSIX. Where it is refused (`ERROR_INVALID_PARAMETER`,
  `ERROR_NOT_SUPPORTED`, `ERROR_INVALID_FUNCTION` or `ERROR_CALL_NOT_IMPLEMENTED`: FAT, some
  network shares, older Windows), `MoveFileExW` is used, with the old behaviour.
- **The POSIX rename takes steps a reader can see.** Measured: an open of the name can fail with
  not found (`STATUS_OBJECT_NAME_NOT_FOUND`) about 0.3 ms after the writer's rename returned, when
  the temporary file is already gone; CI also saw access denied, which is what Windows reports for
  a delete-pending file (`STATUS_DELETE_PENDING`). Under load the window can last milliseconds. So
  when `readFile` fails with not found, access denied or a sharing violation, it opens once more,
  then keeps trying with growing sleeps of up to 10 ms: for up to a second while a temporary file
  `<name>.tmp-<uuid>` exists next to the name, and for up to 50 ms after an access denial or a
  sharing violation without one (a delete-pending file, or a scanner opening the new file). A file
  that is really missing is reported after the one extra open and one directory lookup, a real
  permission error after 50 ms. A temporary file left by a crash makes reads of a missing target
  wait the full second; scans report such files (`isTemporaryFileName`).
- **Durability on Windows:** with `Durable` the temporary file's data is flushed before the
  rename. The rename itself is not forced to the disk: Windows documents no directory flush, and
  `MOVEFILE_WRITE_THROUGH` (on the fallback path) is documented only for moves done as copy and
  delete.
- On Windows, a virus scanner or the search indexer may hold a new file for a moment. A sharing
  violation or access denial on the replacement is retried with a growing delay for about
  200 ms, then fails with `fs.busy`. A read-only target that the file system refuses to replace
  fails with `fs.access_denied` at once.
- `ReplaceFileW` is not used: it fails when the target is missing, has partial-failure states,
  and merges attributes and ACLs.

### Move

`move` uses `MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` on Windows and `renameat2` with
`RENAME_NOREPLACE` on Linux, falling back to check-then-rename where the file system refuses the
flag. Source and target must be on the same volume. A case-only rename (`Ship.txt` to `ship.txt`)
is allowed when both names are the same file (same volume and file id on Windows, same device and
inode on POSIX); on POSIX it goes through a temporary name, because `rename` does nothing for two
names of one file.

### Known folders

| Function | Windows | Linux |
|---|---|---|
| `executableDirectory()` | `GetModuleFileNameW` | `/proc/self/exe` |
| `userConfigDirectory()` | the roaming application data folder | `$XDG_CONFIG_HOME` when absolute, else `~/.config` |
| `userCacheDirectory()` | the local application data folder | `$XDG_CACHE_HOME` when absolute, else `~/.cache` |

Callers append `<Organization>/<Application>`. Other platforms fail with `fs.unsupported` until
they implement the functions.

## Errors

| Code | When |
|---|---|
| `fs.not_found` | The file, or a directory on the way, doesn't exist. |
| `fs.already_exists` | `move` onto an existing name. |
| `fs.access_denied` | Permissions, a read-only medium. |
| `fs.busy` | Another program holds the file (Windows), after the retries of an atomic write; on POSIX, `readFile` of a file under another process's write lease. |
| `fs.not_a_directory`, `fs.is_a_directory` | The path names the other kind. |
| `fs.not_empty` | `remove` of a directory with entries. |
| `fs.invalid_path` | Not UTF-8, a NUL byte, an empty path, a path without a file name, a name too long. |
| `fs.too_large` | `readFile` above `maxSize`. |
| `fs.no_space` | The disk or quota is full. |
| `fs.unsupported` | The platform or file system can't do it; `readFile` of something other than a regular file (on Windows an AF_UNIX socket file is `fs.io`). |
| `fs.io` | Anything else, and the test faults. |

The message names the path and gives the operating system's text, for example
`can't open 'C:/Game/Config/Editor.json': The system cannot find the file specified`.

## Threading

Every function is thread-safe and blocks until the operating system answers. Large reads and
writes (level saves, imports) run on the job system's background lane and return their results
through the main-thread queue ([jobs guide](jobs.md)).

## One place for file I/O

The `file_io_confined` test refuses `<filesystem>`, `<fstream>`, `fopen` and `CreateFileW` in
every file under `engine/ith` outside `engine/ith/file_system` (`cmake/CheckFileIo.cmake`).
Tests, tools and samples are not checked.

## Tests

`file_system` (`tests/file_system`), each test in its own temporary folder:

- the path helpers: roots, normalization (idempotent), join, parent, name and extension;
- reads and writes: round trips, binary data, created parents, `createParents = false`, writes
  onto directories and bad paths, the size limit, missing files;
- the fault points: a test-only, thread-local `fs::testing::setWriteFault` (not in Shipping
  builds) fails a write after the temporary write, after the flush and before the rename, with
  both durabilities; the old content stays whole and no temporary file remains;
- a replacement that goes through at once while a reader holds the file open (the reader keeps
  the old content), and two reader threads that never see a partial file while another thread
  replaces it 100 times;
- a missing file reported at once, and on Windows after the one-second wait when a temporary file
  is next to it;
- long paths (over 400 characters) and names outside ASCII; a busy target on Windows (`fs.busy`
  after the retries); permission bits kept and names that are not UTF-8 in listings on Linux;
- `fileInfo`, `listDirectory`, links (skipped on Windows without the symbolic-link privilege),
  `createDirectories`, `remove`, and `move` with an existing target refused and a case-only
  rename;
- the known folders, with and without the XDG variables on Linux.

The Linux cases run in CI and in a Linux container; durability across a real power loss is not
tested.

## Not there yet

- A file watcher (after P2; P2 uses a Rescan command).
- A user-data folder for save games (with the save system).
- Console implementations (they implement the same functions or fail with `fs.unsupported`).
