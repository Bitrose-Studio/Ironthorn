# ADR-0036: File system: Windows atomic replace corrected after the first implementation

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) rules 2
  (`readFile`) and 3 (`writeFileAtomic` on Windows); [ADR-0001](0001-cpp-language-and-safety-rules.md)
  (no exceptions, `ith::Expected`), [file system guide](../development/file-system.md)

## Context

ADR-0035 rule 3 replaces the target on Windows with `MoveFileExW(MOVEFILE_REPLACE_EXISTING |
MOVEFILE_WRITE_THROUGH)`, and rule 2 opens readers with delete sharing so that "a reader never
blocks an atomic replacement". The first implementation showed that this doesn't hold: on CI
(windows-2025) and in a local stress test, `MoveFileExW` fails with `ERROR_ACCESS_DENIED` while
any other handle holds the target open, even with `FILE_SHARE_DELETE`. After the retry of rule 3
this becomes `fs.busy`. In the editor, a save would fail whenever anything reads the file.

The intent of ADR-0035 stands: readers see the old file or the new one, never a partial one, and
a reader doesn't stop a save. This ADR changes only how Windows does the replacement, and adds the
one rule for readers that the new way needs. Everything else in ADR-0035 stands, POSIX included.

## Options considered

- **Keep `MoveFileExW` and retry longer.** A reader that holds the file for seconds (a viewer, a
  file loaded in a loop) still blocks the save. Rejected.
- **`ReplaceFileW`.** ADR-0035 already rejected it (fails when the target is missing, has
  partial-failure states, merges attributes and ACLs); it also doesn't remove the sharing
  problem. Rejected.
- **Rename through the open handle with POSIX semantics** (chosen):
  `SetFileInformationByHandle(FileRenameInfoEx)` with `FILE_RENAME_FLAG_POSIX_SEMANTICS` replaces
  the name while readers holding the old file with delete sharing keep reading it, as `rename`
  does on POSIX. It needs Windows 10 or later and a file system that supports it (NTFS), so the
  old path stays as a fallback.

## Decision

1. **Rule 3, Windows main path:**
   - The temporary file is created with `GENERIC_WRITE | DELETE` access and
     `FILE_SHARE_READ | FILE_SHARE_DELETE` sharing, because the handle stays open for a moment
     after the rename, when readers may already open the name.
   - It is written, then flushed with `FlushFileBuffers` when `Durable`.
   - It is renamed over the target through its open handle with
     `SetFileInformationByHandle(FileRenameInfoEx)` and
     `FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS`, then closed.
   - A sharing violation or access denial is still retried for about 200 ms, then fails with
     `fs.busy`.
2. **Rule 3, fallback:** when the POSIX rename is refused (`ERROR_INVALID_PARAMETER`,
   `ERROR_NOT_SUPPORTED`, `ERROR_INVALID_FUNCTION`, `ERROR_CALL_NOT_IMPLEMENTED`: FAT, some network
   shares, older Windows), the handle is closed and the ADR-0035 path is used: `MoveFileExW` with
   `MOVEFILE_REPLACE_EXISTING`, plus `MOVEFILE_WRITE_THROUGH` when `Durable`, with the same retry.
   On that path a reader still blocks a replacement, as before.
3. **Rule 3, durability:** the data is flushed before the rename. The rename itself is not forced
   to the disk: Windows documents no such guarantee for either path, and none is claimed.
4. **Rule 2, readers on Windows:** a POSIX rename removes the old name a moment before the new
   file takes it, so a reader opening the name at that instant can find nothing. `readFile`
   therefore retries `ERROR_FILE_NOT_FOUND` up to 3 times, yielding with `SwitchToThread` between
   attempts, before it returns `fs.not_found`.
5. **Supported:** Windows 10 and later on NTFS take the main path. Older systems and other file
   systems take the fallback.
6. **Tests:** a deterministic test replaces a file while a reader holds it open and expects
   success; a stress test runs readers against repeated replacements and checks they never see a
   partial file.

## Consequences

- An editor save succeeds while other code or another program reads the file, on NTFS, as it
  does on POSIX.
- On FAT, some network shares and older Windows, a reader holding the file still makes a save
  fail with `fs.busy` after the retry.
- A `readFile` of a file that is truly missing pays up to three yields before `fs.not_found`.
- Windows durability is unchanged: data flushed, rename not guaranteed on disk.
- **Revisit when:**
  - the not-found retry shows up in CI flakes or in profiles (for example, a reader that still
    misses the name after three yields, or a hot path that probes for missing files);
  - a supported Windows target or file system refuses the POSIX rename often enough that the
    fallback matters;
  - Windows documents a way to force a rename to the disk.
