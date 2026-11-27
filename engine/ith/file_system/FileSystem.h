#pragma once

// Files and directories on the operating system's own APIs (ADR-0035 rules 1 to 10): the Win32
// wide functions on Windows, POSIX elsewhere. The one place for file I/O under engine/ith: a lint
// test refuses <filesystem>, <fstream>, fopen and CreateFileW in every other module.
//
//   const ith::Expected<std::string> text = ith::fs::readFile(path, ith::fs::kMaxConfigSize);
//   ith::Expected<void> saved = ith::fs::writeFileAtomic(levelPath, bytes); // durable by default
//   ith::Expected<void> cached = ith::fs::writeFileAtomic(entryPath, blob,
//                                                         {.durability = ith::fs::Durability::Atomic});
//
// Paths are UTF-8 with '/' separators (Path.h); '\' is accepted on input. A path that is not valid
// UTF-8 or holds a NUL byte fails with "fs.invalid_path". On Windows an absolute path is used with
// the "\\?\" prefix after normalization, so paths longer than 260 characters work without the
// long-path manifest; relative paths are passed as they are and resolve against the working
// directory. The module never folds case.
//
// Errors have the codes "fs.not_found", "fs.already_exists", "fs.access_denied", "fs.busy",
// "fs.not_a_directory", "fs.is_a_directory", "fs.not_empty", "fs.invalid_path", "fs.too_large",
// "fs.no_space", "fs.unsupported", and "fs.io" for the rest. The message names the path and gives
// the operating system's text.
//
// Every function is thread-safe and blocks until the operating system has answered. Large reads
// and writes belong on the job system's background lane (ADR-0035 rule 8).

#include "ith/foundation/Expected.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ith::fs {

// What a path names.
enum class FileKind : std::uint8_t {
    File,
    Directory,
    // A symbolic link (or, on Windows, a junction) to a file or to a directory.
    FileLink,
    DirectoryLink,
    // Anything else: a device, a pipe, a socket, or a link whose target is missing.
    Other,
};

// What fileInfo() reports.
struct FileInfo {
    FileKind kind = FileKind::Other;
    // The size in bytes of the file, or of a link's target file; 0 for anything else.
    std::uint64_t size = 0;
    // The last modification, in nanoseconds since 1970-01-01 UTC (of a link's target).
    std::int64_t modificationTime = 0;
};

// One entry of listDirectory().
struct DirectoryEntry {
    // The name in UTF-8. When the name is not valid Unicode (an unpaired surrogate on Windows,
    // bytes that aren't UTF-8 elsewhere) this is a lossy display name with U+FFFD in place of the
    // bad parts, and `validName` is false: the entry can be reported but not opened by this name.
    // A POSIX name holding '\' is kept as it is, with `validName` false too: every function here
    // takes '\' for a separator, so the name can't address the entry (and could lead elsewhere).
    std::string name;
    FileKind kind = FileKind::Other;
    // The size in bytes of a file or of a link's target file; 0 for anything else.
    std::uint64_t size = 0;
    // False when `name` can't be used to open the entry: never join it into a path.
    bool validName = true;
};

// How hard writeFileAtomic() works to keep the new content through a power loss.
enum class Durability : std::uint8_t {
    // The data is flushed to the disk before the rename. On POSIX the rename and the directory
    // entries are flushed too; Windows documents no way to force the rename itself to the disk
    // (ADR-0036). For documents the user made: levels, sidecars, settings.
    Durable,
    // Readers still see the old or the new file, never a partial one, but nothing is flushed, so a
    // power loss can lose the latest write. For data that can be rebuilt, such as cache entries.
    Atomic,
};

struct WriteOptions {
    Durability durability = Durability::Durable;
    // Create missing parent directories. When false, a missing parent fails with "fs.not_found".
    bool createParents = true;
};

// The limit configuration readers pass to readFile() (ADR-0035 rule 14).
inline constexpr std::size_t kMaxConfigSize = std::size_t{1} << 20;

// The marker in the names of writeFileAtomic()'s temporary files: "<name>.tmp-<uuid>".
inline constexpr std::string_view kTemporaryMarker = ".tmp-";

// True when the last segment of `path` is a temporary file name writeFileAtomic() uses,
// "<name>.tmp-<uuid>" with a valid UUID, so scans can report the ones a crash left behind.
[[nodiscard]] bool isTemporaryFileName(std::string_view path) noexcept;

// The whole file as bytes, following symbolic links. Fails with "fs.too_large" when the file is
// larger than `maxSize` (checked before anything is allocated), "fs.is_a_directory", or the codes
// above. Only a regular file is read, at once, without waiting for a FIFO's writer: on POSIX a FIFO,
// a socket or a device fails with "fs.unsupported", and an open held up by another process's write
// lease with "fs.busy"; on Windows a handle that is not a disk file (a pipe, a console, a device)
// fails with "fs.unsupported", while an AF_UNIX socket file already fails to open, with "fs.io".
// On Windows the file is opened with read, write and delete sharing, so a reader never blocks an
// atomic replacement or a rename by another thread or process. The POSIX rename
// of a concurrent writeFileAtomic can make the name delete-pending or missing for a moment, so an
// open that fails with not found, access denied or a sharing violation is tried once more, then for
// up to a second while the writer's temporary file "<name>.tmp-<uuid>" exists, and for up to 50 ms
// after an access denial or a sharing violation without one. A missing file is reported after the
// one extra try, a real permission error after 50 ms.
[[nodiscard]] Expected<std::string> readFile(std::string_view path, std::size_t maxSize);

// Replaces the file at `path` with `bytes` so that readers see the old file or the new one, never
// a partial one: the bytes go to "<name>.tmp-<uuid>" in the same directory, which is then renamed
// over the target. With Durability::Durable the temporary file is flushed before the rename, and on
// POSIX the directory (and every directory the call created) is flushed after it; Windows documents
// no flush of a rename. On Windows the rename goes through the temporary file's handle with POSIX
// semantics, so readers holding the target open don't hold it up; where the file system refuses
// that, MoveFileExW is used, which fails while a reader holds the target (and is retried). On
// POSIX an existing target's permission bits are kept. On Windows a replacement refused by a sharing violation or access
// denial (a scanner or indexer holding the file) is retried for about 200 ms before failing with
// "fs.busy"; a read-only target whose file system refuses the replacement fails with
// "fs.access_denied" at once. Fails with "fs.is_a_directory" when `path` is a directory,
// "fs.invalid_path" when it has no file name, or the codes above. The temporary file is removed
// on every failure, and a failure before the rename leaves the target unchanged; on POSIX a
// failure to flush the directory after the rename is reported with the new content in place.
[[nodiscard]] Expected<void> writeFileAtomic(std::string_view path, std::string_view bytes,
                                             const WriteOptions& options = {});

// What is at `path`, without following a final symbolic link (but reporting its target's kind and
// size), or nullopt when nothing is there (also when a parent is missing or is a file). Fails
// with the codes above, for example "fs.access_denied".
[[nodiscard]] Expected<std::optional<FileInfo>> fileInfo(std::string_view path);

// The entries of the directory at `path`, one level, without "." and "..", in no particular
// order. Links are reported, not followed. Fails with "fs.not_found", "fs.not_a_directory" or the
// codes above.
[[nodiscard]] Expected<std::vector<DirectoryEntry>> listDirectory(std::string_view path);

// Creates the directory at `path` and its missing parents. Succeeds when it already exists (a
// link to a directory counts); fails with "fs.not_a_directory" when it or a parent is something
// else, or the codes above.
[[nodiscard]] Expected<void> createDirectories(std::string_view path);

// Removes the file, symbolic link or empty directory at `path` (a link is removed, not its
// target). Fails with "fs.not_found", "fs.not_empty" or the codes above.
[[nodiscard]] Expected<void> remove(std::string_view path);

// Renames `from` to `to`, which must be on the same volume, and never replaces an existing target:
// that fails with "fs.already_exists". A rename that changes only the case of a name is allowed
// when source and target are the same file (same volume and file id, same device and inode).
// Fails with "fs.not_found" when `from` or the parent of `to` is missing, or the codes above.
[[nodiscard]] Expected<void> move(std::string_view from, std::string_view to);

// The directory of the running executable, absolute and normalized.
[[nodiscard]] Expected<std::string> executableDirectory();

// The user's configuration directory, absolute and normalized: the roaming application data folder
// on Windows, $XDG_CONFIG_HOME on Linux when it is set to an absolute path, else ~/.config. Callers
// append "<Organization>/<Application>". Fails with "fs.not_found" when the folder can't be
// determined, or "fs.unsupported" on a platform without one.
[[nodiscard]] Expected<std::string> userConfigDirectory();

// The user's cache directory, absolute and normalized: the local application data folder on
// Windows, $XDG_CACHE_HOME on Linux when it is set to an absolute path, else ~/.cache. Fails as
// userConfigDirectory().
[[nodiscard]] Expected<std::string> userCacheDirectory();

#ifndef ITH_SHIPPING
namespace testing {

// A point inside writeFileAtomic() where a test can make it fail, to check that the old content
// stays whole and no temporary file remains. Not in Shipping builds.
enum class WriteFault : std::uint8_t {
    None,
    // After the bytes were written to the temporary file, before the flush.
    AfterTemporaryWrite,
    // After the flush (the place of it with Durability::Atomic).
    AfterFlush,
    // Right before the rename (on POSIX after the temporary file was closed; on Windows the rename
    // goes through its open handle).
    BeforeRename,
};

// Makes every writeFileAtomic() call on the calling thread fail with "fs.io" at `fault` until it is
// set back to WriteFault::None. Thread-local, so tests running in parallel don't see each other's.
void setWriteFault(WriteFault fault) noexcept;

} // namespace testing
#endif

} // namespace ith::fs
